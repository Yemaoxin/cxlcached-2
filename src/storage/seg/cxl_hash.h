#pragma once
//add by yemaoxin,2023-09-19 17:02:31 fix
#include "cxl_item.h"

struct cxl_hash_table {
    struct item_slh *table;
    uint32_t nhash_item;
    uint32_t hash_power;
};

#define HASHSIZE(_n) (1ULL << (_n))
#define HASHMASK(_n) (HASHSIZE(_n) - 1)

struct cxl_hash_table *cxl_hashtable_create(uint32_t hash_power);
void cxl_hashtable_destroy(struct cxl_hash_table **ht_p);

void cxl_hashtable_put(struct cxl_item *it, struct cxl_hash_table *ht);
void cxl_hashtable_delete(const char *key, uint32_t klen, struct cxl_hash_table *ht);
struct cxl_item *cxl_hashtable_get(const char *key, uint32_t klen, struct cxl_hash_table *ht);


struct cxl_hash_table *cxl_hashtable_double(struct cxl_hash_table *ht); /* best effort expansion */
