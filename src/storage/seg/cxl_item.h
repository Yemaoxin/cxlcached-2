#pragma once

#include "item.h"
#include "cxl_slabclass.h"

#include "time/time.h"

#include <cc_bstring.h>
#include <cc_metric.h>
#include <cc_queue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Every item chunk in the cache starts with a header (struct cxl_item)
 * followed by item data. An item is essentially a chunk of memory
 * carved out of a slab. Every item is owned by its parent slab.
 *
 * Items are either linked or unlinked. When item is first allocated and
 * has no data, it is unlinked. When data is copied into an item, it is
 * linked into the hash table (ITEM_LINKED). When item is deleted either
 * explicitly or due to item expiration, it is moved in the free q
 * (ITEM_FREEQ). The flags ITEM_LINKED and ITEM_FREEQ are mutually
 * exclusive and when an item is unlinked it has neither of these flags.
 *
 *   <-----------------------item size------------------>
 *   +---------------+----------------------------------+
 *   |               |                                  |
 *   |  item header  |          item payload            |
 *   | (struct item) |         ...      ...             |
 *   +---------------+-------+-------+------------------+
 *   ^               ^       ^       ^
 *   |               |       |       |
 *   |               |       |       |
 *   |               |       |       |
 *   |               |       |       \
 *   |               |       |       item_data()
 *   |               |       \
 *   \               |       item_key()
 *   item            \
 *                   item->end, (if enabled) item_get_cas(), metadata
 *
 * item->end is followed by:
 * - 8-byte cas, if ITEM_CAS flag is set
 * - other optional metadata
 * - key
 * - data
 */
typedef struct cxl_item { 
#if defined CC_ASSERT_PANIC || defined CC_ASSERT_LOG
    uint32_t          magic;         /* item magic (const) */
#endif
    //add by yemaoxin,2023-09-20 10:00:43 LRU链的指针，需要通过LRU lock上锁保护，主要目的是用于热度数据的识别
    struct cxl_item* next;
    struct cxl_item* prev;
    
    SLIST_ENTRY(cxl_item) i_sle;         /* link in hash/freeq */
    // 才能支持
    proc_time_i       expire_at;     /* expiry time in secs */
    proc_time_i       create_at;     /* time when this item was last linked */
    //add by yemaoxin,2023-09-20 09:58:38 使用了c语言的位域的机制用于减少空间的浪费
    uint32_t          is_linked:1;   /* item in hash */
    uint32_t          in_freeq:1;    /* item in free queue */
    uint32_t          is_raligned:1; /* item data (payload) is right-aligned */
    uint32_t          vlen:29;       /* data size (29 bits since uint32_t is 32
                                      * bits and we have 3 flags)
                                      * NOTE: need at least enough bits to
                                      * support the largest value size allowed
                                      * by the implementation, i.e. SLAB_MAX_SIZE
                                      */

    uint32_t          offset;        /* offset of item in slab */
    uint8_t           id;            /* slab class id */
    uint8_t           klen;          /* key length */
    uint8_t           olen;          /* optional length (right after cas) */
    uint8_t           slru_id:1;     /* cold =0 ; warm =1 */
    uint8_t           padding:7;     /* keep end 64-bit aligned */
    //add by yemaoxin,2023-09-20 10:04:36 应该是一个偏移量的计算，注意看看就是了
    char              end[1];        /* item data  */
}cxl_item;

#define ITEM_MAGIC      0xfeedface
#define ITEM_HDR_SIZE   offsetof(struct cxl_item, end)
#define ITEM_CAS_SIZE   sizeof(uint64_t)

#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 2
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#endif


//add by yemaoxin,2023-09-20 10:08:02 设置WARM LRU和COLD LRU
#define max_slru_id 512
#define GET_SLRU_ID(flag,id) (flag<<8)|id; 
static pthread_mutex_t lru_locks[max_slru_id];
static cxl_item *heads[max_slru_id];
static cxl_item *tails[max_slru_id];



// typedef enum item_rstatus {
//     ITEM_OK,
//     ITEM_EOVERSIZED,
//     ITEM_ENOMEM,
//     ITEM_ENAN, /* not a number */
//     ITEM_EOTHER,
// } item_rstatus_e;

extern bool cxl_use_cas;
extern uint64_t cas_id;
extern size_t slab_profile[SLABCLASS_MAX_ID + 1];

static inline uint64_t
item_get_cas(struct cxl_item *it)
{
    return cxl_use_cas ? *((uint64_t *)it->end) : 0;
}

static inline void
cxl_item_set_cas(struct cxl_item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);

    if (cxl_use_cas) {
        *((uint64_t *)it->end) = ++cas_id;
    }
}

static inline size_t
item_cas_size(void)
{
    return cxl_use_cas * ITEM_CAS_SIZE;
}

#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 6
#pragma GCC diagnostic pop
#endif

static inline char *
cxl_item_key(struct cxl_item *it)
{
    return it->end + item_cas_size() + it->olen;
}

/* get key length */
static inline uint32_t
cxl_item_nkey(const struct cxl_item *it)
{
    return it->klen;
}

/* get payload length */
static inline uint32_t
cxl_item_nval(const struct cxl_item *it)
{
    return it->vlen;
}

static inline size_t
cxl_item_npayload(struct cxl_item *it)
{
    return item_cas_size() + it->olen + it->klen + it->vlen;
}

static inline size_t
cxl_item_ntotal(uint8_t klen, uint32_t vlen, uint8_t olen)
{
    return ITEM_HDR_SIZE + item_cas_size() + olen + klen + vlen;
}

static inline size_t
cxl_item_size(const struct cxl_item *it)
{
    ASSERT(it->magic == ITEM_MAGIC);

    return cxl_item_ntotal(it->klen, it->vlen, it->olen);
}

static inline char *
cxl_item_optional(struct cxl_item *it)
{
    return it->end + item_cas_size();
}

/*
 * Get start location of item value
 */
static inline char *
cxl_item_data(struct cxl_item *it)
{
    char *data;

    if (it->is_raligned) {
        data = (char *)it + slabclass[it->id].size - it->vlen;
    } else {
        data = it->end + item_cas_size() + it->olen + it->klen;
    }

    return data;
}

static inline item_rstatus_e
cxl_item_atou64(uint64_t *vint, struct cxl_item *it)
{
    rstatus_i status;
    struct bstring vstr;

    vstr.len = it->vlen;
    vstr.data = (char *)cxl_item_data(it);
    status = bstring_atou64(vint, &vstr);
    if (status == CC_OK) {
        return ITEM_OK;
    } else {
        return ITEM_ENAN;
    }
}
/* return true if item can fit len additional bytes in val in place, false otherwise */
static inline bool
item_will_fit(const struct cxl_item *it, uint32_t delta)
{
    return slab_profile[it->id] >= cxl_item_size(it) + delta;
}


/* Init header for given item */
void cxl_item_hdr_init(struct cxl_item *it, uint32_t offset, uint8_t id);

/* acquire an item */
struct cxl_item *cxl_item_get(const struct bstring *key,uint8_t read_flag);

/* TODO: make the following APIs protocol agnostic */

/* insert an item, removes existing item of the same key (if applicable) */
void cxl_item_insert(struct cxl_item *it, const struct bstring *key);

/* reserve an item, this does not link it or remove existing item with the same
 * key.
 * olen- optional data length, this can be used to reserve space for optional
 * data, e.g. flag in Memcached protocol) in payload, after cas.
 * */
item_rstatus_e cxl_item_reserve(struct cxl_item **it_p, const struct bstring *key, const
        struct bstring *val, uint32_t vlen, uint8_t olen, proc_time_i expire_at);
/* item_release is used for reserved item only (not linked) */
void cxl_item_release(struct cxl_item **it_p);

void cxl_item_backfill(struct cxl_item *it, const struct bstring *val);

/* Append/prepend */
item_rstatus_e item_annex(struct cxl_item *it, const struct bstring *key, const
        struct bstring *val, bool append);

/* In place item update (replace item value) */
void cxl_item_update(struct cxl_item *it, const struct bstring *val);

/* Remove item from cache */
bool cxl_item_delete(const struct bstring *key);

/* Relink item */
void cxl_item_relink(struct cxl_item *it);

size_t cxl_item_expire(struct bstring *prefix);

/* flush the cache */
void cxl_item_flush(void);
