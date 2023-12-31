#include "cxl_slab.h"
#include "cxl_item.h"
#include <cc_debug.h>

#include <stdlib.h>
#include <stdio.h>

extern delta_time_i max_ttl;
proc_time_i cxl_flush_at = -1;
//  不区分cold和warm的代码，只通过位区分
static cxl_item_slru_link(cxl_item* it)
{
    // 一个九位值，通过移位实现
    uint16_t slru_id=GET_SLRU_ID(it->slru_id,it->id);
    //add by yemaoxin,2023-09-20 15:47:17 使用头插法
    pthread_mutex_lock(&lru_locks[slru_id]);
    if(heads[slru_id]!=NULL)
    {
        heads[slru_id]->prev = it;
    }
    it->next = heads[slru_id];
    it->prev = NULL;
    if(tails[slru_id]==NULL)
    {
        tails[slru_id] =it;
        it->next=NULL;
    }
    pthread_mutex_unlock(&lru_locks[slru_id]);
}
// 从slru上取下
static cxl_item_slru_unlink(cxl_item* it)
{
    // 一个九位值，通过移位实现
    uint16_t slru_id=GET_SLRU_ID(it->slru_id,it->id);
    //add by yemaoxin,2023-09-20 15:47:17 使用头插法
    pthread_mutex_lock(&lru_locks[slru_id]);
    cxl_item* before = it->prev;
    cxl_item* after =it->next;
    if(before!=NULL)
    {  // 隐含了after可能空
        before->next=after;
    }
    if(after!=NULL)
    {
        after->prev =before;
    }
    
    if(heads[slru_id]==it)
    {
        heads[slru_id]=after;
    }
    if(tails[slru_id]==it)
    {
        tails[slru_id]=before;
    }
    it->next=NULL;
    it->prev=NULL;
    pthread_mutex_unlock(&lru_locks[slru_id]);
}
static inline bool
_item_expired(struct cxl_item *it)
{
    return (it->expire_at < time_proc_sec() || it->create_at <= cxl_flush_at);
}
//add by yemaoxin,2023-09-18 21:14:11 slab机制原有的基于Segmented LRU的item复制
static inline void
_copy_key_item(struct cxl_item *nit, struct cxl_item *oit)
{
    nit->olen = oit->olen;
    cc_memcpy(cxl_item_key(nit), cxl_item_key(oit), oit->klen);
    nit->klen = oit->klen;
}

void
cxl_item_hdr_init(struct cxl_item *it, uint32_t offset, uint8_t id)
{
    ASSERT(offset >= SLAB_HDR_SIZE && offset < slab_size);

#if CC_ASSERT_PANIC == 1 || CC_ASSERT_LOG == 1
    it->magic = CXL_ITEM_MAGIC;
#endif
    it->offset = offset;
    it->id = id;
    it->is_linked = it->in_freeq = it->is_raligned = 0;
}

static inline void
_item_reset(struct cxl_item *it)
{
    it->is_linked = 0;
    it->in_freeq = 0;
    it->is_raligned = 0;
    it->vlen = 0;
    it->klen = 0;
    it->olen = 0;
    it->expire_at = 0;
    it->create_at = 0;
}

/*
 * Allocate an item. We allocate an item by consuming the next free item
 * from slab of the item's slab class.
 *
 * On success we return the pointer to the allocated item.
 */
item_rstatus_e
cxl_item_alloc(struct cxl_item **it_p, uint8_t klen, uint32_t vlen, uint8_t olen)
{
    uint8_t id = slab_id(cxl_item_ntotal(klen, vlen, olen));
    struct cxl_item *it;

    log_verb("allocate item with klen %u vlen %u", klen, vlen);

    *it_p = NULL;
    if (id == SLABCLASS_INVALID_ID) {
        return ITEM_EOVERSIZED;
    }

    it = slab_get_item(id);
    *it_p = it;
    if (it != NULL) {
        _item_reset(it);
        slab_ref(item_to_slab(it)); /* slab to be deref'ed in _item_link */
        INCR(slab_metrics, item_curr);
        INCR(slab_metrics, item_alloc);
        PERSLAB_INCR(id, item_curr);

        log_verb("alloc it %p of id %"PRIu8" at offset %"PRIu32, it, it->id,
                it->offset);

        return ITEM_OK;
    } else {
        INCR(slab_metrics, item_alloc_ex);
        log_warn("server error on allocating item in slab %"PRIu8, id);

        return ITEM_ENOMEM;
    }
}

static inline void
_item_dealloc(struct cxl_item **it_p)
{
    uint8_t id = (*it_p)->id;

    DECR(slab_metrics, item_curr);
    INCR(slab_metrics, item_dealloc);
    PERSLAB_DECR(id, item_curr);

    slab_put_item(*it_p, id);
    cc_itt_free(slab_free, *it_p);
    *it_p = NULL;
}

/*
 * (Re)Link an item into the hash table and slru
 */
static void
_item_link(struct cxl_item *it, bool relink)
{
    ASSERT(it->magic == CXL_ITEM_MAGIC);
    ASSERT(!(it->in_freeq));
    ASSERT(it->prev==NULL&&it->next==NULL);
    if (!relink) {
        ASSERT(!(it->is_linked));

        it->is_linked = 1;
        slab_deref(item_to_slab(it)); /* slab ref'ed in _item_alloc */
    }

    log_verb("link it %p of id %"PRIu8" at offset %"PRIu32, it, it->id,
            it->offset);

    
    cxl_hashtable_put(it, cxl_hash_table);
    cxl_item_slru_link(it);

    INCR(slab_metrics, item_linked_curr);
    INCR(slab_metrics, item_link);
    /* TODO(yao): how do we track optional storage? Separate or treat as val? */
    INCR_N(slab_metrics, item_keyval_byte, it->klen + it->vlen);
    INCR_N(slab_metrics, item_val_byte, it->vlen);
    PERSLAB_INCR_N(it->id, item_keyval_byte, it->klen + it->vlen);
    PERSLAB_INCR_N(it->id, item_val_byte, it->vlen);
}

void
cxl_item_relink(struct cxl_item *it)
{
    _item_link(it, true);
}
// 原有的机制不合理，怎么就只有一个hashtable管理，并没有所谓的Segmented LRU来管理啊
void
cxl_item_insert(struct cxl_item *it, const struct bstring *key)
{
    ASSERT(it != NULL && key != NULL);

    cxl_item_delete(key);

    _item_link(it, false);
    log_verb("insert it %p of id %"PRIu8" for key %.*s", it, it->id, key->len,
        key->data);

    cc_itt_alloc(slab_malloc, it, item_size(it));
}

/*
 * Unlinks an item from the hash table. 这个不合理呀，没有LRU的管理机制
 */
static void
_item_unlink(struct cxl_item *it)
{
    ASSERT(it->magic == CXL_ITEM_MAGIC);

    log_verb("unlink it %p of id %"PRIu8" at offset %"PRIu32, it, it->id,
            it->offset);

    if (it->is_linked) {
        it->is_linked = 0;
        cxl_item_slru_unlink(it);
        cxl_hashtable_delete(cxl_item_key(it), it->klen, cxl_hash_table);
    }
    DECR(slab_metrics, item_linked_curr);
    INCR(slab_metrics, item_unlink);
    DECR_N(slab_metrics, item_keyval_byte, it->klen + it->vlen);
    DECR_N(slab_metrics, item_val_byte, it->vlen);
    PERSLAB_DECR_N(it->id, item_keyval_byte, it->klen + it->vlen);
    PERSLAB_DECR_N(it->id, item_val_byte, it->vlen);
}

/**
 * Return an item if it hasn't been marked as expired, lazily expiring
 * item as-and-when needed
 */
struct cxl_item *
cxl_item_get(const struct bstring *key,uint8_t read_flag)
{
    struct cxl_item *it;

    it = cxl_hashtable_get(key->data, key->len, cxl_hash_table);
    if (it == NULL) {
        log_verb("get it '%.*s' not found", key->len, key->data);
        return NULL;
    }

    log_verb("get it key %.*s val %.*s", key->len, key->data, it->vlen,
            cxl_item_data(it));

    if (_item_expired(it)) {
        log_verb("get it '%.*s' expired and nuked", key->len, key->data);
        _item_unlink(it);
        _item_dealloc(&it);
        return NULL;
    }
    if(read_flag==1)
    {  
        // 即使原本在warm中也会被刷新到表头
        cxl_item_slru_unlink(it);
        it->slru_id|=1;
        cxl_item_slru_link(it);
    }

    log_verb("get it %p of id %"PRIu8, it, it->id);

    return it;
}

/* TODO(yao): move this to memcache-specific location */
 void
cxl_item_define(struct cxl_item *it, const struct bstring *key, const struct bstring
        *val, uint8_t olen, proc_time_i expire_at)
{
    proc_time_i expire_cap = time_delta2proc_sec(max_ttl);

    it->create_at = time_proc_sec();
    it->expire_at = expire_at < expire_cap ? expire_at : expire_cap;
    cxl_item_set_cas(it);
    it->olen = olen;
    cc_memcpy(cxl_item_key(it), key->data, key->len);
    it->klen = key->len;
    if (val != NULL) {
        cc_memcpy(cxl_item_data(it), val->data, val->len);
    }
    it->vlen = (val == NULL) ? 0 : val->len;
}

item_rstatus_e
cxl_item_reserve(struct cxl_item **it_p, const struct bstring *key, const struct bstring
        *val, uint32_t vlen, uint8_t olen, proc_time_i expire_at)
{
    item_rstatus_e status;
    struct cxl_item *it;

    if ((status = cxl_item_alloc(it_p, key->len, vlen, olen)) != ITEM_OK) {
        log_debug("item reservation failed");
        return status;
    }

    it = *it_p;

    cxl_item_define(it, key, val, olen, expire_at);

    log_verb("reserve it %p of id %"PRIu8" for key '%.*s' optional len %"PRIu8,
            it, it->id,key->len, key->data, olen);

    return ITEM_OK;
}

void
cxl_item_release(struct cxl_item **it_p)
{
    slab_deref(item_to_slab(*it_p)); /* slab ref'ed in _item_alloc */
    _item_dealloc(it_p);
}

void
cxl_item_backfill(struct cxl_item *it, const struct bstring *val)
{
    ASSERT(it != NULL);

    cc_memcpy(cxl_item_data(it) + it->vlen, val->data, val->len);
    it->vlen += val->len;

    log_verb("backfill it %p with %"PRIu32" bytes, now %"PRIu32" bytes total",
            it, val->len, it->vlen);
}

item_rstatus_e
item_annex(struct cxl_item *oit, const struct bstring *key, const struct bstring
        *val, bool append)
{
    item_rstatus_e status = ITEM_OK;
    struct cxl_item *nit = NULL;
    uint8_t id;
    uint32_t ntotal = oit->vlen + val->len;

    id = item_slabid(oit->klen, ntotal, oit->olen);
    if (id == SLABCLASS_INVALID_ID) {
        log_info("client error: annex operation results in oversized item with"
                   "key size %"PRIu8" old value size %"PRIu32" and new value "
                   "size %"PRIu32, oit->klen, oit->vlen, ntotal);

        return ITEM_EOVERSIZED;
    }

    if (append) {
        /* if it is large enough to hold the extra data and left-aligned,
         * which is the default behavior, we copy the delta to the end of
         * the existing data. Otherwise, allocate a new item and store the
         * payload left-aligned.
         */
        if (id == oit->id && !(oit->is_raligned)) {
            cc_memcpy(cxl_item_data(oit) + oit->vlen, val->data, val->len);
            oit->vlen = ntotal;
            INCR_N(slab_metrics, item_keyval_byte, val->len);
            INCR_N(slab_metrics, item_val_byte, val->len);
            cxl_item_set_cas(oit);
        } else {
            status = cxl_item_alloc(&nit, oit->klen, ntotal, oit->olen);
            if (status != ITEM_OK) {
                log_debug("annex failed due to failure to allocate new item");
                return status;
            }
            _copy_key_item(nit, oit);
            nit->expire_at = oit->expire_at;
            nit->create_at = time_proc_sec();
            cxl_item_set_cas(nit);
            /* value is left-aligned */
            cc_memcpy(cxl_item_data(nit), cxl_item_data(oit), oit->vlen);
            cc_memcpy(cxl_item_data(nit) + oit->vlen, val->data, val->len);
            nit->vlen = ntotal;
            cxl_item_insert(nit, key);
        }
    } else {
        /* if oit is large enough to hold the extra data and is already
         * right-aligned, we copy the delta to the front of the existing
         * data. Otherwise, allocate a new item and store the payload
         * right-aligned, assuming more prepends will happen in the future.
         */
        if (id == oit->id && oit->is_raligned) {
            cc_memcpy(cxl_item_data(oit) - val->len, val->data, val->len);
            oit->vlen = ntotal;
            INCR_N(slab_metrics, item_keyval_byte, val->len);
            INCR_N(slab_metrics, item_val_byte, val->len);
            cxl_item_set_cas(oit);
        } else {
            status = cxl_item_alloc(&nit, oit->klen, ntotal, oit->olen);
            if (status != ITEM_OK) {
                log_debug("annex failed due to failure to allocate new item");
                return status;
            }
            _copy_key_item(nit, oit);
            nit->expire_at = oit->expire_at;
            nit->create_at = time_proc_sec();
            cxl_item_set_cas(nit);
            /* value is right-aligned */
            nit->is_raligned = 1;
            cc_memcpy(cxl_item_data(nit) - ntotal, val->data, val->len);
            cc_memcpy(cxl_item_data(nit) - oit->vlen, cxl_item_data(oit), oit->vlen);
            nit->vlen = ntotal;
            cxl_item_insert(nit, key);
        }
    }

    log_verb("annex to it %p of id %"PRIu8", new it at %p", oit, oit->id,
            nit ? oit : nit);

    return status;
}

// 在CXL上，item update就会导致进入warm-lru，一次更新就会进入warm-lru
void
cxl_item_update(struct cxl_item *it, const struct bstring *val)
{
    ASSERT(item_slabid(it->klen, val->len, it->olen) == it->id);

    it->vlen = val->len;
    cc_memcpy(cxl_item_data(it), val->data, val->len);
    cxl_item_slru_unlink(it);
    it->slru_id|=1;
    cxl_item_slru_link(it);
    cxl_item_set_cas(it);

    log_verb("update it %p of id %"PRIu8, it, it->id);
}

static void
_item_delete(struct cxl_item **it)
{
    log_verb("delete it %p of id %"PRIu8, *it, (*it)->id);

    _item_unlink(*it);
    _item_dealloc(it);
}

bool
cxl_item_delete(const struct bstring *key)
{
    struct cxl_item *it;

    it = cxl_item_get(key,0);
    if (it != NULL) {
        _item_delete(&it);

        return true;
    } else {
        return false;
    }
}

void
cxl_item_flush(void)
{
    time_update();
    cxl_flush_at = time_proc_sec();
    log_info("all keys flushed at %"PRIu32, cxl_flush_at);
}

/* this dumps all keys (matching a prefix if given) regardless of expiry status */
size_t
cxl_item_expire(struct bstring *prefix)
{
    uint32_t nbucket = HASHSIZE(cxl_hash_table->hash_power);
    size_t nkey, klen, vlen;

    log_info("start scanning all %"PRIu32" keys", cxl_hash_table->nhash_item);

    nkey = 0;
    for (uint32_t i = 0; i < nbucket; i++) {
        struct item_slh *entry = &cxl_hash_table->table[i];
        struct cxl_item *it;

        SLIST_FOREACH(it, entry, i_sle) {
            klen = it->klen;
            vlen = it->vlen;
            if (klen >= prefix->len &&
                    cc_bcmp(prefix->data, cxl_item_key(it), prefix->len) == 0) {
                nkey++;
                it->expire_at = time_proc_sec();
                log_verb("item %p flushed at %"PRIu32, it, it->expire_at);
            }
        }

        if (i % 1000000 == 0) {
            log_info("... %"PRIu32" out of %"PRIu32" buckets scanned ...", i,
                    nbucket);
        }
    }

    log_info("finish scanning all keys");

    return nkey;
}

