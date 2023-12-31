#include "cxl_slab.h"
//add by yemaoxin,2023-09-19 17:02:39 fix
#include <hash/cc_murmur3.h>
#include <cc_mm.h>

static uint32_t murmur3_iv = 0x3ac5d673;

/*
 * Allocate table given size
 */
static struct item_slh *
_cxl_hashtable_alloc(uint64_t size)
{
    struct item_slh *table;
    uint32_t i;

    table = cc_alloc(sizeof(*table) * size);

    if (table != NULL) {
        for (i = 0; i < size; ++i) {
            SLIST_INIT(&table[i]);
        }
    }

    return table;
}

struct cxl_hash_table *
cxl_hashtable_create(uint32_t hash_power)
{
    struct cxl_hash_table *ht;
    uint64_t size;

    ASSERT(hash_power > 0);

    /* alloc struct */
    ht = cc_alloc(sizeof(struct cxl_hash_table));

    if (ht == NULL) {
        return NULL;
    }

    /* init members */
    ht->table = NULL;
    ht->hash_power = hash_power;
    ht->nhash_item = 0;
    size = HASHSIZE(ht->hash_power);

    /* alloc table */
    ht->table = _cxl_hashtable_alloc(size);
    if (ht->table == NULL) {
        cc_free(ht);
        return NULL;
    }

    return ht;
}

void
cxl_hashtable_destroy(struct cxl_hash_table **ht_p)
{
    struct cxl_hash_table *ht = *ht_p;
    if (ht != NULL && ht->table != NULL) {
        cc_free(ht->table);
    }

    *ht_p = NULL;
}

static struct item_slh *
_get_bucket(const char *key, size_t klen, struct cxl_hash_table *ht)
{
    uint32_t hv;

    hash_murmur3_32(key, klen, murmur3_iv, &hv);

    return &(ht->table[hv & HASHMASK(ht->hash_power)]);
}

void
cxl_hashtable_put(struct cxl_item *it, struct cxl_hash_table *ht)
{
    struct item_slh *bucket;

    ASSERT(cxl_hashtable_get(cxl_item_key(it), it->klen, ht) == NULL);

    bucket = _get_bucket(cxl_item_key(it), it->klen, ht);
    SLIST_INSERT_HEAD(bucket, it, i_sle);

    ++(ht->nhash_item);
    INCR(slab_metrics, hash_insert);
}

void
cxl_hashtable_delete(const char *key, uint32_t klen, struct cxl_hash_table *ht)
{
    struct item_slh *bucket;
    struct cxl_item *it, *prev;

    ASSERT(cxl_hashtable_get(key, klen, ht) != NULL);

    bucket = _get_bucket(key, klen, ht);
    for (prev = NULL, it = SLIST_FIRST(bucket); it != NULL;
        prev = it, it = SLIST_NEXT(it, i_sle)) {
        INCR(slab_metrics, hash_traverse);

        /* iterate through bucket to find item to be removed */
        if ((klen == it->klen) && cc_memcmp(key, cxl_item_key(it), klen) == 0) {
            /* found item */
            break;
        }
    }

    if (prev == NULL) {
        SLIST_REMOVE_HEAD(bucket, i_sle);
    } else {
        SLIST_REMOVE_AFTER(prev, i_sle);
    }

    --(ht->nhash_item);
    INCR(slab_metrics, hash_remove);
}

struct cxl_item *
cxl_hashtable_get(const char *key, uint32_t klen, struct cxl_hash_table *ht)
{
    struct item_slh *bucket;
    struct cxl_item *it;

    ASSERT(key != NULL);
    ASSERT(klen != 0);

    INCR(slab_metrics, hash_lookup);

    bucket = _get_bucket(key, klen, ht);
    /* iterate through bucket looking for item */
    for (it = SLIST_FIRST(bucket); it != NULL; it = SLIST_NEXT(it, i_sle)) {
        INCR(slab_metrics, hash_traverse);

        if ((klen == it->klen) && cc_memcmp(key, cxl_item_key(it), klen) == 0) {
            /* found item */
            return it;
        }
    }

    return NULL;
}

/*
 * Expand the hashtable to the next power of 2.
 * This is an expensive operation and should _not_ be used in production or
 * during latency-related tests. It is included mostly for simulation around
 * the storage component.
 */
struct cxl_hash_table *
cxl_hashtable_double(struct cxl_hash_table *ht)
{
    struct cxl_hash_table *new_ht;
    uint32_t new_hash_power;
    uint64_t new_size;

    new_hash_power = ht->hash_power + 1;
    new_size = HASHSIZE(new_hash_power);

    new_ht = cxl_hashtable_create(new_size);
    if (new_ht == NULL) {
        return ht;
    }

    /* copy to new hash table */
    for  (uint32_t i = 0; i < HASHSIZE(ht->hash_power); ++i) {
        struct cxl_item *it, *next;
        struct item_slh *bucket, *new_bucket;

        bucket = &ht->table[i];
        SLIST_FOREACH_SAFE(it, bucket, i_sle, next) {
            new_bucket = _get_bucket(cxl_item_key(it), it->klen, new_ht);
            SLIST_REMOVE(bucket, it, cxl_item, i_sle);
            SLIST_INSERT_HEAD(new_bucket, it, i_sle);
        }
    }

    cxl_hashtable_destroy(&ht);

    return new_ht;
}

