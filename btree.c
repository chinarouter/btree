#define  _GNU_SOURCE

#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include "btree.h"
#include "bit/bit.h"

#include <fcntl.h>

#define DEFAULT_PAGE_SIZE 4096
/* POOL_SIZE MUST BE DIVISIBLE BY PAGE_SIZE */
#define DEFAULT_POOL_SIZE 134217728

#define DEBUG
#include "dbg.h"

int _btree_delete(struct PagePool *pp, struct BTreeNode *x, void *key);

/*
 * Get number of bitmask pages
 * */
inline size_t bitmask_pages(struct PagePool *pp) {
	size_t ans = pp->pageSize * CHAR_BIT;
	ans = ceil(((double )pp->nPages) / ans);
	return ans;
}

/*
 * Dump bitmask pages to the disk
 * */
inline int bitmask_dump(struct PagePool *pp) {
	log_info("Dump bitmask");
	return pwrite(pp->fd, pp->bitmask, pp->pageSize * bitmask_pages(pp),
			pp->pageSize);
}

/*
 * Load bitmask pages from the disk
 * */
inline int bitmask_load(struct PagePool *pp) {
	log_info("Load bitmask");
	return pread(pp->fd, pp->bitmask, pp->pageSize * bitmask_pages(pp),
			pp->pageSize);
}

/*
 * Populate bitmask
 * Mark metadata page and BM page
 * */
int bitmask_populate(struct PagePool *pp) {
	size_t pagenum = bitmask_pages(pp) + 1;
	pp->bitmask = (void *)calloc(bitmask_pages(pp) * pp->pageSize, 1);
	while (pagenum > 0)
		bit_set(pp->bitmask, --pagenum);
	bitmask_dump(pp);
	return 0;
}

/*
 * Find empty page
 * Returns 0 when can't find empty page
 * */
size_t page_find_empty(struct PagePool *pp) {
	size_t pos = 0;
	while (pos < pp->nPages && bit_test(pp->bitmask, pos))
		pos++;
	return (pos == pp->nPages ? 0 : pos);
}

/*
 * Allocates page
 * Returns 0 on error
 * */
size_t page_alloc(struct PagePool *pp) {
	size_t pos = page_find_empty(pp);
	if (pos == -1)
		return -1;
	log_info("Allocating page %zd", pos);
	bit_set(pp->bitmask, pos);
	bitmask_dump(pp);
	return pos;
}

/*
 * Free page
 * Returns 1 on error
 * */
int page_free(struct PagePool *pp, size_t pos) {
	log_info("Freeing page %zd", pos);
	if (!bit_test(pp->bitmask, pos))
		return -1;
	bit_clear(pp->bitmask, pos);
	bitmask_dump(pp);
	return 0;
}

/*
 * Write node to disk
 * Returns 0 on Error
 * */
inline size_t page_node_write(struct PagePool *pp, struct BTreeNode *node) {
	log_info("Dumping Node %zd", node->page);
	int retval = pwrite(pp->fd, node, sizeof(struct BTreeNode),
			node->page * pp->pageSize);
	if (retval == -1) {
		log_err("Writing %zd bytes to page %zd failed\n",
				pp->pageSize, node->page);
		log_err("Error while writing %d: %s\n", errno,
				strerror(errno));
		return 0;
	}
	return retval;
}

/*
 * Read node from disk
 * Cleanup in the caller
 * Returns NULL on error
 * */
inline struct BTreeNode *page_node_read(struct PagePool *pp, size_t num) {
	log_info("Reading Node %zd", num);
	struct BTreeNode *node = (struct BTreeNode*)calloc(pp->pageSize, 1);
	int retval = pread(pp->fd, node, pp->pageSize, num * pp->pageSize);
	if (retval == -1) {
		log_err("Reading %zd bytes from page %zd failed\n", pp->pageSize, num);
		log_err("Error while reading %d: %s\n", errno, strerror(errno));
		free(node);
		return NULL;
	}
	return node;
}

/*
 * Write data to the disk
 * Returns 1 on error
 * */
int page_data_write(struct PagePool *pp,
		void *data, size_t data_len, size_t num) {
	assert(data_len < BTREE_VAL_LEN);
	struct DataPageMeta meta = {
		.dataSize = data_len,
		.nextPage = 0
	};
	ssize_t ans = pwrite(pp->fd, &meta, sizeof(struct DataPageMeta), num * pp->pageSize);
	assert(ans == sizeof(struct DataPageMeta));
	ans = write(pp->fd, data, data_len);
	assert(ans == data_len);
	return 0;
}

/*
 * Read data from the disk
 * Cleanup in the caller
 * Return NULL on error
 * */
inline void *page_data_read(struct PagePool *pp, size_t num, size_t *data_len) {
	struct DataPageMeta meta;
	pread(pp->fd, &meta, sizeof(struct DataPageMeta), num * pp->pageSize);
	*data_len = meta.dataSize;
	void *data = calloc(meta.dataSize, 1);
	read(pp->fd, data, meta.dataSize);
	return data;
}

/*
 * Cleanup in the caller
 * Create PagePool
 * */
struct PagePool *pool_create(char *name) {
	struct PagePool *pp = (struct PagePool *)calloc(
		sizeof(struct PagePool),  1);
	pp->fd = open(name, O_CREAT | O_RDWR,
			S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH);
	assert(pp->fd != -1);
	pp->pageSize = DEFAULT_PAGE_SIZE;
	pp->nPages = ceil(DEFAULT_POOL_SIZE / DEFAULT_PAGE_SIZE);
	posix_fallocate(pp->fd, 0, DEFAULT_POOL_SIZE);
	bitmask_populate(pp);
		return pp;
}

/*
 * Find free page, reserve it and map Node with it
 * */
struct BTreeNode *node_create_reserve(struct PagePool *pp,
		int is_leaf, size_t parentPage) {
	size_t page_t = page_alloc(pp); assert(page_t != -1);
	struct BTreeNode *node  = (struct BTreeNode*)calloc(
			sizeof(struct BTreeNode), 1);
	node->nKeys = 0;
	node->flags = (is_leaf ? IS_LEAF : 0);
	node->page = page_t;
	node->parentPage = parentPage;
	page_node_write(pp, node);
	return node;
}

/*
 * Create DB object
 * Main function.
 * Cleanup in the caller.
 * */
struct DB *db_create(char *db_name, size_t size) {
	log_info("Creating DB with name %s and size %zd", db_name, size);
	struct DB *db =  (struct DB *)calloc(sizeof(struct DB), 1);
	db->db_name = db_name;

	db->pool = pool_create(db_name);
	db->top = node_create_reserve(db->pool, 1, 0);
	return db;
}

/*
 * Cleanup function for DB object
 * */
int db_free(struct DB *db) {
	if (db->top) free(db->top);
	if (db->pool) {
		if (db->pool->bitmask) free(db->pool->bitmask);
		if (db->pool->fd) close(db->pool->fd);
		free(db->pool);
	}
	free(db);
	return 0;
}

#define NODE_KEY_POS(NODE, POS)  NODE->keys + (POS) * BTREE_KEY_LEN
#define NODE_CHLD_POS(NODE, POS) NODE->chld + (POS)
#define NODE_VAL_POS(NODE, POS)  NODE->vals + (POS)

#define NODE_FULL(NODE) (NODE->nKeys == BTREE_KEY_CNT)

/*
 * Insert data into prepared node
 * */
int _btree_insert_data(struct PagePool *pp, struct BTreeNode *node,
		char *val, int val_len, size_t pos) {
	size_t page = page_alloc(pp); assert(page != -1);
	page_data_write(pp, val, val_len, page);
	node->vals[pos] = page;
	return 0;
}

/*
 * Replace data with the existed key
 * */
int _btree_replace_data(struct PagePool *pp, struct BTreeNode *node,
		char *val, int val_len, size_t pos) {
	page_free(pp, node->vals[pos]);
	_btree_insert_data(pp, node, val, val_len, pos);
	return 0;
}

/*
 * Prepare node for insertion
 * (move last (node.len - pos) elements to the right)
 * */
void _btree_insert_into_node_prepare(struct PagePool *pp, struct BTreeNode *node,
				     char *key, size_t pos, size_t child) {
	int isLeaf = node->flags & IS_LEAF;
	assert(!(isLeaf ^ !child));
	assert(!NODE_FULL(node));
	memmove(NODE_KEY_POS(node, pos + 1), NODE_KEY_POS(node, pos),
			(node->nKeys - pos) * BTREE_KEY_LEN);
	memmove(NODE_VAL_POS(node, pos + 1), NODE_VAL_POS(node, pos),
			(node->nKeys - pos) * sizeof(size_t));
	if (!isLeaf) {
		memmove(NODE_CHLD_POS(node, pos + 2), NODE_CHLD_POS(node, pos + 1),
				(node->nKeys - pos) * sizeof(size_t));
		node->chld[pos + 1] = child;
	}
	node->nKeys += 1;
	strncpy(NODE_KEY_POS(node, pos), key, BTREE_KEY_LEN);
}

/*
 * Insert into B-Tree by string and pointer
 * */
int _btree_insert_into_node_sp(struct PagePool *pp, struct BTreeNode *node,
		char *key, size_t data_page, size_t pos, size_t child) {
	_btree_insert_into_node_prepare(pp, node, key, pos, child);
	node->vals[pos] = data_page;
	return page_node_write(pp, node);
}

/*
 * Insert into B-Tree by string and string
 * */
int _btree_insert_into_node_ss(struct PagePool *pp, struct BTreeNode *node,
		char *key, char *val,
		int val_len, size_t pos, size_t child) {
	_btree_insert_into_node_prepare(pp, node, key, pos, child);
	_btree_insert_data(pp, node, val, val_len, pos);
	return page_node_write(pp, node);
}

int _btree_split_node(struct PagePool *pp,
		struct BTreeNode *parent,
		struct BTreeNode *node) {
	assert(!(parent && NODE_FULL(parent)));
	int isLeaf = node->flags & IS_LEAF;
	size_t middle = ceil((double )node->nKeys/2) - 1;
	struct BTreeNode *right = node_create_reserve(pp,
			isLeaf, node->page);
	memcpy(right->keys, NODE_KEY_POS(node, middle + 1),
			BTREE_KEY_LEN  * (node->nKeys - middle - 1));
	memcpy(right->vals, NODE_VAL_POS(node, middle + 1),
			sizeof(size_t) * (node->nKeys - middle - 1));
	memcpy(right->chld, NODE_CHLD_POS(node, middle + 1),
			sizeof(size_t) * (node->nKeys - middle));
#ifdef DEBUG
	memset(NODE_KEY_POS(node, middle + 1), 0,
			BTREE_KEY_LEN * (node->nKeys - middle - 1));
	memset(NODE_VAL_POS(node, middle + 1), 0,
			sizeof(size_t) * (node->nKeys - middle - 1));
	memset(NODE_CHLD_POS(node, middle + 1), 0,
			sizeof(size_t) * (node->nKeys - middle));
#endif
	right->nKeys = node->nKeys - middle - 1;
	node->nKeys = middle + 1;
	if (parent == NULL) { /* It's top node */
		struct BTreeNode *left = node_create_reserve(pp,
				isLeaf, node->page);
		if (isLeaf) node->flags -= IS_LEAF;
		memcpy(left->keys, node->keys, BTREE_KEY_LEN  * middle);
		memcpy(left->vals, node->vals, sizeof(size_t) * middle);
		memcpy(left->chld, node->chld, sizeof(size_t) * (middle + 1));
#ifdef DEBUG
		memset(node->keys, 0, BTREE_KEY_LEN * middle);
		memset(node->vals, 0, sizeof(size_t) * middle);
		memset(node->chld, 0, sizeof(size_t) * (middle + 1));
#endif
		left->nKeys = middle;
		memmove(node->keys, NODE_KEY_POS(node, middle), BTREE_KEY_LEN);
		node->vals[0] = node->vals[middle];
		node->chld[0] = left->page;
		node->chld[1] = right->page;
		node->nKeys = 1;
		page_node_write(pp, left);
		free(left);
	} else {
		size_t pos = 0;
		while (pos < parent->nKeys && node->page != parent->chld[pos++]);
		assert(pos == parent->nKeys);
		_btree_insert_into_node_sp(pp, parent, NODE_KEY_POS(node, middle),
				node->vals[middle], pos - 1, right->page);
		--node->nKeys;
	}
	page_node_write(pp, node);
	page_node_write(pp, right);
	free(right);
	return 0;
}

int _btree_insert(struct PagePool *pp, struct BTreeNode *node,
		char *key, char *val, int val_len) {
	if (node->parentPage == 0 && NODE_FULL(node)) /* UNLIKELY */
		_btree_split_node(pp, NULL, node);
	size_t pos = 0;
	int cmp = -1;
	while (pos < node->nKeys && (cmp = strncmp(NODE_KEY_POS(node, pos),
					key, BTREE_KEY_LEN)) < 0)
		pos++;
	if (cmp == 0) {
		_btree_replace_data(pp, node, val, val_len, pos);
		return 0;
	}
	if (node->flags & IS_LEAF) {
		_btree_insert_into_node_ss(pp, node, key, val, val_len, pos, 0);
	} else {
		struct BTreeNode *child = page_node_read(pp, node->chld[pos]);
		if (NODE_FULL(child)) {
			_btree_split_node(pp, node, child);
			if (strncmp(NODE_KEY_POS(node, pos + 1),
						key, BTREE_KEY_LEN) > 0) {
				_btree_insert(pp, child, key, val, val_len);
			} else {
				struct BTreeNode *r = page_node_read(pp,
						node->chld[pos+2]);
				_btree_insert(pp, r, key, val, val_len);
				page_node_write(pp, r);
				free(r);
			}
		} else {
			_btree_insert(pp, child, key, val, val_len);
		}
		page_node_write(pp, child);
		free(child);
	}
	return 0;
}

int db_insert(struct DB *db, char *key, char *val, int val_len) {
	log_info("Inserting value into DB with key '%s'", key);
	return _btree_insert(db->pool, db->top, key, val, val_len);
}

/* Cleanup in the caller */
void *_btree_search(struct PagePool *pp, struct BTreeNode *node,
		void *key, size_t *val_len) {
	*val_len = 0;
	size_t pos = 0;
	int cmp = 0;
	while (pos < node->nKeys &&
			((cmp = strncmp(NODE_KEY_POS(node, pos), key, BTREE_KEY_LEN)) < 0))
		pos++;
	if (pos < node->nKeys && cmp == 0)
		return page_data_read(pp, node->vals[pos], val_len);
	if (node->flags & IS_LEAF)
		return NULL;
	struct BTreeNode *kid = page_node_read(pp, node->chld[pos]);
	void *ret = _btree_search(pp, node, key, val_len);
	free(kid);
	return ret;
}

inline void *db_search(struct DB *db, char *key, size_t *val_len) {
	log_info("Searching value in the DB with key '%s'", key);
	return _btree_search(db->pool, db->top, key, val_len);
}

int _btree_delete_replace_max(struct PagePool *pp, struct BTreeNode *x,
		size_t pos, struct BTreeNode *left) {
	struct BTreeNode *left_bkp = left;
	struct BTreeNode *left_new = NULL;
	while (!(left->flags & IS_LEAF)) {
		left_new = page_node_read(pp, left->chld[left->nKeys]);
		if (left != left_bkp) free(left);
		left = left_new;
	}
	memcpy(NODE_KEY_POS(left, left->nKeys-1), NODE_KEY_POS(x, pos), BTREE_KEY_LEN);
	x->vals[pos] = left->vals[left->nKeys-1];
	page_node_write(pp, left);
	page_node_write(pp, left_bkp);
	return 0;
}

int _btree_delete_replace_min(struct PagePool *pp, struct BTreeNode *x,
		size_t pos, struct BTreeNode *right) {
	struct BTreeNode *right_bkp = right;
	struct BTreeNode *right_new = NULL;
	while (!(right->flags & IS_LEAF)) {
		right_new = page_node_read(pp, right->chld[0]);
		if (right != right_bkp) free(right);
		right = right_new;
	}
	memcpy(NODE_KEY_POS(right, 0), NODE_KEY_POS(x, pos), BTREE_KEY_LEN);
	x->vals[pos] = right->vals[right->nKeys-1];
	page_node_write(pp, right);
	page_node_write(pp, right);
	return 0;
}

int _btree_merge_nodes(struct PagePool *pp, struct BTreeNode *x,
		size_t pos, struct BTreeNode *left, struct BTreeNode *right) {
	memcpy(NODE_KEY_POS(x, pos), NODE_KEY_POS(left, left->nKeys), BTREE_KEY_LEN);
	left->vals[left->nKeys] = x->vals[pos];
	memmove(NODE_KEY_POS(x, pos), NODE_KEY_POS(x, pos + 1),
			BTREE_KEY_LEN * (x->nKeys - pos - 1));
	memmove(NODE_VAL_POS(x, pos), NODE_VAL_POS(x, pos + 1),
			sizeof(size_t) * (x->nKeys - pos - 1));
	memmove(NODE_VAL_POS(x, pos + 1), NODE_VAL_POS(x, pos + 2),
			sizeof(size_t) *(x->nKeys - pos));
	--x->nKeys;
	++left->nKeys;
	memcpy(NODE_KEY_POS(left, left->nKeys), NODE_KEY_POS(right, 0),
			BTREE_KEY_LEN * right->nKeys);
	memcpy(NODE_VAL_POS(left, left->nKeys), NODE_VAL_POS(right, 0),
			sizeof(size_t) * right->nKeys);
	memcpy(NODE_VAL_POS(left, left->nKeys - 1), NODE_VAL_POS(right, 0),
			sizeof(size_t) * right->nKeys);
	left->nKeys += right->nKeys;
	right->nKeys = 0;
	return 0;
}

int _btree_transfuse_to_left(struct PagePool *pp, struct BTreeNode *x,
		size_t pos, struct BTreeNode *to, struct BTreeNode *from) {
	/*
	 * x->pos_key -> append(to, key)
	 * from->begin_key -> x->pos_key
	 * from->begin_chld -> append(to, chld)
	 * */
	memcpy(NODE_KEY_POS(to, to->nKeys), NODE_KEY_POS(x, pos),
			BTREE_KEY_LEN);
	to->vals[to->nKeys] = x->vals[pos];
	memcpy(NODE_KEY_POS(x, pos), NODE_KEY_POS(from, 0), BTREE_KEY_LEN);
	x->vals[pos] = from->vals[0];
	to->chld[to->nKeys] = from->chld[0];
	memcpy(NODE_KEY_POS(from, 0), NODE_KEY_POS(from, 1),
			BTREE_KEY_LEN * (from->nKeys - 1));
	memcpy(NODE_VAL_POS(from, 0), NODE_VAL_POS(from, 1),
			sizeof(size_t)* (from->nKeys - 1));
	memcpy(NODE_CHLD_POS(from, 0), NODE_CHLD_POS(from, 1),
			sizeof(size_t)* (from->nKeys - 1));
	--from->nKeys;
	++to->nKeys;
	return 0;
}

int _btree_transfuse_to_right(struct PagePool *pp, struct BTreeNode *x,
		size_t pos, struct BTreeNode *to, struct BTreeNode *from) {
	/*
	 * x->pos_key -> append(to, key)
	 * from->begin_key -> x->pos_key
	 * from->begin_chld -> append(to, chld)
	 * */
	memcpy(NODE_KEY_POS(to, 1), NODE_KEY_POS(to, 0),
			BTREE_KEY_LEN * to->nKeys);
	memcpy(NODE_VAL_POS(to, 1), NODE_VAL_POS(to, 0),
			sizeof(size_t)* to->nKeys);
	memcpy(NODE_CHLD_POS(to, 1), NODE_CHLD_POS(to, 0),
			sizeof(size_t)* to->nKeys);

	memcpy(NODE_KEY_POS(to, 0), NODE_KEY_POS(x, pos),
			BTREE_KEY_LEN);
	to->vals[0] = x->vals[pos];

	memcpy(NODE_KEY_POS(x, pos), NODE_KEY_POS(from, from->nKeys-1), BTREE_KEY_LEN);
	x->vals[pos] = from->vals[from->nKeys-1];
	to->chld[0] = from->chld[from->nKeys-1];
	--from->nKeys;
	++to->nKeys;
	return 0;
}

int _btree_delete(struct PagePool *pp, struct BTreeNode *x, void *key) {
	int pos = 0;
	int cmp = 0;
	while (pos < x->nKeys && ((cmp = \
			strncmp(NODE_KEY_POS(x, pos), key, BTREE_KEY_LEN)) < 0))
		++pos;
//	if (cmp < 0) --pos;
	int isLeaf = x->flags & IS_LEAF;
	if (isLeaf && pos < x->nKeys && !cmp) {
		memmove(NODE_KEY_POS(x, pos), NODE_KEY_POS(x, pos + 1),
				BTREE_KEY_LEN * (x->nKeys - pos - 1));
		memmove(NODE_VAL_POS(x, pos), NODE_VAL_POS(x, pos + 1),
				sizeof(size_t) * (x->nKeys - pos - 1));
	} else if (isLeaf && cmp) {
		return 0;
	} else if (!isLeaf && !cmp) {
		do {
			struct BTreeNode *kid_left = NULL;
			struct BTreeNode *kid_right = NULL;
			kid_left = page_node_read(pp, x->chld[pos]);
			if (kid_left->nKeys > floor(BTREE_KEY_CNT/2)) {
				_btree_delete_replace_max(pp, x, pos, kid_left);
				_btree_delete(pp, kid_left, key);
				free(kid_left);
				break;
			}
			kid_right = page_node_read(pp, x->chld[pos+1]);
			if (kid_right->nKeys > floor(BTREE_KEY_CNT/2)) {
				_btree_delete_replace_min(pp, x, pos, kid_right);
				_btree_delete(pp, kid_right, key);
				free(kid_left);
				free(kid_right);
				break;
			}

			/* TODO: May become empty*/
			_btree_merge_nodes(pp, x, pos, kid_left, kid_right);
			page_free(pp, kid_right->page);
			_btree_delete(pp, kid_left, key);
			free(kid_left);
			free(kid_right);
		} while (0);
	} else {
		struct BTreeNode *kid = page_node_read(pp, x->chld[pos]);
		if ((kid->nKeys) <= floor(BTREE_KEY_CNT/2)) {
			struct BTreeNode *kid_left = NULL;
			struct BTreeNode *kid_right = NULL;
			do {
				kid_left = page_node_read(pp, x->chld[pos]);
				if (kid_left->nKeys > floor(BTREE_KEY_CNT/2)) {
					_btree_transfuse_to_left(pp, x, pos,
							kid, kid_left);
					break;
				}
				kid_right = page_node_read(pp, x->chld[pos+1]);
				if (kid_right->nKeys > floor(BTREE_KEY_CNT/2)) {
					_btree_transfuse_to_right(pp, x, pos,
							kid, kid_right);
					free(kid_right);
					break;
				}
				_btree_merge_nodes(pp, x, pos, kid, kid_right);
				page_free(pp, kid_right->page);
				_btree_delete(pp, kid_left, key);
				free(kid_right);
			} while (0);
			if (kid_left->nKeys > floor(BTREE_KEY_CNT/2)) {
			} else if (kid_right->nKeys > floor(BTREE_KEY_CNT/2)) {
				_btree_transfuse_to_right(pp, x, pos, kid, kid_right);
			} else {
				_btree_merge_nodes(pp, x, pos, kid, kid_right);
				page_free(pp, kid_right->page);
				_btree_delete(pp, kid_right, key);
			}
			free(kid_left);
			free(kid_right);
		}
		_btree_delete(pp, kid, key);
		free(kid);
	}
	return 0;
}

int db_delete(struct DB *db, char *key) {
	log_info("Deleting value from DB with key '%s'", key);
	return _btree_delete(db->pool, db->top, key);
}

int print_node(struct BTreeNode *x) {
#ifdef DEBUG
	printf("--------------------------------------\n");
	printf("PageNo: %03zd, ParentPageNo: %03zd\n", x->page, x->parentPage);
	printf("Size: %d, Flags: ", x->nKeys);
	if (x->flags & IS_LEAF) printf("IS_LEAF");
	printf("\n");
	int i = 0;
	for (i = 0; i < x->nKeys; ++i) {
		printf("Key: %s, Value: %zd", NODE_KEY_POS(x, i), x->vals[i]);
		if (!(x->flags & IS_LEAF))
			printf(", Child: %zd", x->chld[i]);
		printf("\n");
	}
	if (!(x->flags & IS_LEAF))
		printf("Last Child: %zd\n", x->chld[x->nKeys]);
	printf("--------------------------------------\n");
#endif
	return 0;
}

int print_tree(struct PagePool *pp, struct BTreeNode *x) {
	print_node(x);
	if (x->flags & IS_LEAF)
		return 0;
	int i = 0;
	for (i = 0; i < x->nKeys + 1; ++i) {
		assert(x->chld[i] != 0);
		struct BTreeNode *n = page_node_read(pp, x->chld[i]);
		print_tree(pp, n);
	}
	return 0;
}

int db_print(struct DB *db) {
	printf("=====================================================\n");
	int retcode =  print_tree(db->pool, db->top);
	printf("=====================================================\n");
	return retcode;
}

int main() {
	struct DB *db = db_create("mydb", 128*1024*1024);
//	db_print(db);
	db_insert(db, "568", "4567890", 7);
//	db_print(db);
	db_insert(db, "567", "4567890", 7);
//	db_print(db);
	db_insert(db, "456", "4567890", 7);
//	db_print(db);
	db_insert(db, "345", "4567890", 7);
//	db_print(db);
	db_insert(db, "234", "4567890", 7);
//	db_print(db);
	db_insert(db, "123", "4567890", 7);
//	db_print(db);
	db_delete(db, "123");
	db_print(db);
	db_free(db);
	return 0;
}
