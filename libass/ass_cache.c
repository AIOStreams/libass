/*
 * Copyright (C) 2006 Evgeniy Stepanov <eugeni.stepanov@gmail.com>
 * Copyright (C) 2011 Grigori Goronzy <greg@chown.ath.cx>
 *
 * This file is part of libass.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"
#include "ass_compat.h"

#include <inttypes.h>
#include <ft2build.h>
#include FT_OUTLINE_H
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>

#include "ass_utils.h"
#include "ass_font.h"
#include "ass_outline.h"
#include "ass_cache.h"

// Always enable native-endian mode, since we don't care about cross-platform consistency of the hash
#define WYHASH_LITTLE_ENDIAN 1
#include "wyhash.h"

// With wyhash any arbitrary 64 bit value will suffice
#define ASS_HASH_INIT 0xb3e46a540bd36cd4ULL

static inline ass_hashcode ass_hash_buf(const void *buf, size_t len, ass_hashcode hval)
{
    return wyhash(buf, len, hval, _wyp);
}

// type-specific functions
// create hash/compare functions for bitmap, outline and composite cache
#define CREATE_HASH_FUNCTIONS
#include "ass_cache_template.h"
#define CREATE_COMPARISON_FUNCTIONS
#include "ass_cache_template.h"

// font cache
static bool font_key_move(void *dst, void *src)
{
    ASS_FontDesc *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    d->family.str = ass_copy_string(s->family);
    return d->family.str;
}

static void font_key_destruct(void *key)
{
    ASS_FontDesc *k = key;
    free((char *) k->family.str);
}

static void font_destruct(void *value)
{
    ass_font_clear(value);
}

size_t ass_font_construct(void *key, void *value, void *priv);

const CacheDesc font_cache_desc = {
    .hash_func = font_hash,
    .compare_func = font_compare,
    .key_move_func = font_key_move,
    .construct_func = ass_font_construct,
    .key_destruct_func = font_key_destruct,
    .value_destruct_func = font_destruct,
    .key_size = sizeof(ASS_FontDesc),
    .value_size = sizeof(ASS_Font)
};


// bitmap cache
static bool bitmap_key_move(void *dst, void *src)
{
    BitmapHashKey *d = dst, *s = src;
    if (d) {
        *d = *s;
        ass_cache_inc_ref(d->outline);
    }
    return true;
}

static void bitmap_key_destruct(void *key)
{
    BitmapHashKey *k = key;
    ass_cache_dec_ref(k->outline);
}

static void bitmap_destruct(void *value)
{
    ass_free_bitmap(value);
}

size_t ass_bitmap_construct(void *key, void *value, void *priv);

const CacheDesc bitmap_cache_desc = {
    .hash_func = bitmap_hash,
    .compare_func = bitmap_compare,
    .key_move_func = bitmap_key_move,
    .construct_func = ass_bitmap_construct,
    .key_destruct_func = bitmap_key_destruct,
    .value_destruct_func = bitmap_destruct,
    .key_size = sizeof(BitmapHashKey),
    .value_size = sizeof(Bitmap)
};


// composite cache
static ass_hashcode composite_hash(void *key, ass_hashcode hval)
{
    CompositeHashKey *k = key;
    hval = filter_hash(&k->filter, hval);
    for (size_t i = 0; i < k->bitmap_count; i++)
        hval = bitmap_ref_hash(&k->bitmaps[i], hval);
    return hval;
}

static bool composite_compare(void *a, void *b)
{
    CompositeHashKey *ak = a;
    CompositeHashKey *bk = b;
    if (!filter_compare(&ak->filter, &bk->filter))
        return false;
    if (ak->bitmap_count != bk->bitmap_count)
        return false;
    for (size_t i = 0; i < ak->bitmap_count; i++)
        if (!bitmap_ref_compare(&ak->bitmaps[i], &bk->bitmaps[i]))
            return false;
    return true;
}

static bool composite_key_move(void *dst, void *src)
{
    CompositeHashKey *d = dst, *s = src;
    if (d) {
        *d = *s;
        for (size_t i = 0; i < d->bitmap_count; i++) {
            ass_cache_inc_ref(d->bitmaps[i].bm);
            ass_cache_inc_ref(d->bitmaps[i].bm_o);
        }
        return true;
    }

    free(s->bitmaps);
    return true;
}

static void composite_key_destruct(void *key)
{
    CompositeHashKey *k = key;
    for (size_t i = 0; i < k->bitmap_count; i++) {
        ass_cache_dec_ref(k->bitmaps[i].bm);
        ass_cache_dec_ref(k->bitmaps[i].bm_o);
    }
    free(k->bitmaps);
}

static void composite_destruct(void *value)
{
    CompositeHashValue *v = value;
    ass_free_bitmap(&v->bm);
    ass_free_bitmap(&v->bm_o);
    ass_free_bitmap(&v->bm_s);
}

size_t ass_composite_construct(void *key, void *value, void *priv);

const CacheDesc composite_cache_desc = {
    .hash_func = composite_hash,
    .compare_func = composite_compare,
    .key_move_func = composite_key_move,
    .construct_func = ass_composite_construct,
    .key_destruct_func = composite_key_destruct,
    .value_destruct_func = composite_destruct,
    .key_size = sizeof(CompositeHashKey),
    .value_size = sizeof(CompositeHashValue)
};


// outline cache
static ass_hashcode outline_hash(void *key, ass_hashcode hval)
{
    OutlineHashKey *k = key;
    switch (k->type) {
    case OUTLINE_GLYPH:
        return glyph_hash(&k->u, hval);
    case OUTLINE_DRAWING:
        return drawing_hash(&k->u, hval);
    case OUTLINE_BORDER:
        return border_hash(&k->u, hval);
    default:  // OUTLINE_BOX
        return hval;
    }
}

static bool outline_compare(void *a, void *b)
{
    OutlineHashKey *ak = a;
    OutlineHashKey *bk = b;
    if (ak->type != bk->type)
        return false;
    switch (ak->type) {
    case OUTLINE_GLYPH:
        return glyph_compare(&ak->u, &bk->u);
    case OUTLINE_DRAWING:
        return drawing_compare(&ak->u, &bk->u);
    case OUTLINE_BORDER:
        return border_compare(&ak->u, &bk->u);
    default:  // OUTLINE_BOX
        return true;
    }
}

static bool outline_key_move(void *dst, void *src)
{
    OutlineHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    if (s->type == OUTLINE_DRAWING) {
        d->u.drawing.text.str = ass_copy_string(s->u.drawing.text);
        return d->u.drawing.text.str;
    }
    if (s->type == OUTLINE_BORDER)
        ass_cache_inc_ref(s->u.border.outline);
    else if (s->type == OUTLINE_GLYPH)
        ass_cache_inc_ref(s->u.glyph.font);
    return true;
}

static void outline_key_destruct(void *key)
{
    OutlineHashKey *k = key;
    switch (k->type) {
    case OUTLINE_GLYPH:
        ass_cache_dec_ref(k->u.glyph.font);
        break;
    case OUTLINE_DRAWING:
        free((char *) k->u.drawing.text.str);
        break;
    case OUTLINE_BORDER:
        ass_cache_dec_ref(k->u.border.outline);
        break;
    default:  // OUTLINE_BOX
        break;
    }
}

static void outline_destruct(void *value)
{
    OutlineHashValue *v = value;
    ass_outline_free(&v->outline[0]);
    ass_outline_free(&v->outline[1]);
}

size_t ass_outline_construct(void *key, void *value, void *priv);

const CacheDesc outline_cache_desc = {
    .hash_func = outline_hash,
    .compare_func = outline_compare,
    .key_move_func = outline_key_move,
    .construct_func = ass_outline_construct,
    .key_destruct_func = outline_key_destruct,
    .value_destruct_func = outline_destruct,
    .key_size = sizeof(OutlineHashKey),
    .value_size = sizeof(OutlineHashValue)
};


// font-face size metric cache
static bool face_size_metrics_key_move(void *dst, void *src)
{
    FaceSizeMetricsHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    ass_cache_inc_ref(s->font);
    return true;
}

static void face_size_metrics_key_destruct(void *key)
{
    FaceSizeMetricsHashKey *k = key;
    ass_cache_dec_ref(k->font);
}

size_t ass_face_size_metrics_construct(void *key, void *value, void *priv);

const CacheDesc face_size_metrics_cache_desc = {
    .hash_func = face_size_metrics_hash,
    .compare_func = face_size_metrics_compare,
    .key_move_func = face_size_metrics_key_move,
    .construct_func = ass_face_size_metrics_construct,
    .key_destruct_func = face_size_metrics_key_destruct,
    .key_size = sizeof(FaceSizeMetricsHashKey),
    .value_size = sizeof(FT_Size_Metrics)
};


// glyph metric cache
static bool glyph_metrics_key_move(void *dst, void *src)
{
    GlyphMetricsHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    ass_cache_inc_ref(s->font);
    return true;
}

static void glyph_metrics_key_destruct(void *key)
{
    GlyphMetricsHashKey *k = key;
    ass_cache_dec_ref(k->font);
}

size_t ass_glyph_metrics_construct(void *key, void *value, void *priv);

const CacheDesc glyph_metrics_cache_desc = {
    .hash_func = glyph_metrics_hash,
    .compare_func = glyph_metrics_compare,
    .key_move_func = glyph_metrics_key_move,
    .construct_func = ass_glyph_metrics_construct,
    .key_destruct_func = glyph_metrics_key_destruct,
    .key_size = sizeof(GlyphMetricsHashKey),
    .value_size = sizeof(FT_Glyph_Metrics)
};


// shaped run cache
static bool shape_key_move(void *dst, void *src)
{
    ShapeHashKey *d = dst, *s = src;
    if (!d)
        return true;

    *d = *s;
    d->text.str = ass_copy_string(s->text);
    if (!d->text.str)
        return false;
    ass_cache_inc_ref(s->font);
    return true;
}

static void shape_key_destruct(void *key)
{
    ShapeHashKey *k = key;
    free((char *) k->text.str);
    ass_cache_dec_ref(k->font);
}

static void shape_destruct(void *value)
{
    ShapeHashValue *v = value;
    free(v->glyphs);
}

size_t ass_shape_construct(void *key, void *value, void *priv);

const CacheDesc shape_cache_desc = {
    .hash_func = shape_hash,
    .compare_func = shape_compare,
    .key_move_func = shape_key_move,
    .construct_func = ass_shape_construct,
    .key_destruct_func = shape_key_destruct,
    .value_destruct_func = shape_destruct,
    .key_size = sizeof(ShapeHashKey),
    .value_size = sizeof(ShapeHashValue)
};



// Cache data
//
// Lookups may run on several threads at once. A bucket's chain only grows while
// they do: new items go in at its head with a compare-and-swap, so a lookup
// needs no lock. Items leave only between frames, while no lookup runs. A mapped
// item holds a reference for the cache, so it can't be freed under a lookup
// either. Items that other items or images hold when they'd be evicted are
// parked instead: out of the eviction order until they're used again, or
// removed once only the cache holds them, which can only happen between frames.
typedef struct cache_item {
    Cache *cache;   // NULL once ass_cache_empty() has let go of the item
    const CacheDesc *desc;
    ass_hashcode hash;
    size_t size;
    struct cache_item *_Atomic next;    // in the bucket chain
    struct cache_item *queue_next, **queue_prev;    // eviction order, oldest first
    struct cache_item *recent_next;     // in the client's list of this frame
    atomic_size_t ref_count;
    atomic_uint frame;  // the frame the item was last used in
    atomic_uint state;
    bool parked;
} CacheItem;

enum {
    ITEM_CONSTRUCTING,
    ITEM_AWAITED,       // constructing, and other threads wait for it
    ITEM_READY,
};

struct cache {
    unsigned buckets;   // a power of two
    CacheItem *_Atomic *map;
    CacheItem *queue_first, **queue_last;

    const CacheDesc *desc;

    atomic_size_t cache_size;
    unsigned frame;

    pthread_mutex_t wait_lock;
    pthread_cond_t wait_cond;
};

#define CACHE_ALIGN 8
#define CACHE_ITEM_SIZE ((sizeof(CacheItem) + (CACHE_ALIGN - 1)) & ~(CACHE_ALIGN - 1))

static inline size_t align_cache(size_t size)
{
    return (size + (CACHE_ALIGN - 1)) & ~(CACHE_ALIGN - 1);
}

static inline CacheItem *value_to_item(void *value)
{
    return (CacheItem *) ((char *) value - CACHE_ITEM_SIZE);
}

static inline size_t item_cost(size_t size)
{
    return size + (size == 1 ? 0 : CACHE_ITEM_SIZE);
}


// Create a cache with type-specific hash/compare/destruct/size functions
Cache *ass_cache_create(const CacheDesc *desc)
{
    Cache *cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;
    cache->buckets = 1 << 16;
    cache->queue_last = &cache->queue_first;
    cache->desc = desc;
    cache->map = calloc(cache->buckets, sizeof(*cache->map));
    if (!cache->map)
        goto fail;
    if (pthread_mutex_init(&cache->wait_lock, NULL))
        goto fail;
    if (pthread_cond_init(&cache->wait_cond, NULL)) {
        pthread_mutex_destroy(&cache->wait_lock);
        goto fail;
    }
    return cache;

fail:
    free(cache->map);
    free(cache);
    return NULL;
}

static CacheItem *find_item(CacheItem *item, CacheItem *end, ass_hashcode hash,
                            const CacheDesc *desc, void *key)
{
    size_t key_offs = CACHE_ITEM_SIZE + align_cache(desc->value_size);
    for (; item != end; item = atomic_load_explicit(&item->next, memory_order_acquire))
        if (item->hash == hash && desc->compare_func(key, (char *) item + key_offs))
            return item;
    return NULL;
}

static void wait_until_ready(Cache *cache, CacheItem *item)
{
    if (atomic_load_explicit(&item->state, memory_order_acquire) == ITEM_READY)
        return;

    pthread_mutex_lock(&cache->wait_lock);
    unsigned state = ITEM_CONSTRUCTING;
    atomic_compare_exchange_strong(&item->state, &state, ITEM_AWAITED);
    while (atomic_load_explicit(&item->state, memory_order_acquire) != ITEM_READY)
        pthread_cond_wait(&cache->wait_cond, &cache->wait_lock);
    pthread_mutex_unlock(&cache->wait_lock);
}

static void mark_ready(Cache *cache, CacheItem *item)
{
    if (atomic_exchange(&item->state, ITEM_READY) != ITEM_AWAITED)
        return;

    pthread_mutex_lock(&cache->wait_lock);
    pthread_cond_broadcast(&cache->wait_cond);
    pthread_mutex_unlock(&cache->wait_lock);
}

// Records the item's first use this frame in the client's list.
static void use_item(Cache *cache, CacheClient *client, CacheItem *item)
{
    unsigned frame = cache->frame;
    if (atomic_load_explicit(&item->frame, memory_order_relaxed) == frame ||
            atomic_exchange_explicit(&item->frame, frame, memory_order_relaxed) == frame)
        return;
    item->recent_next = client->recent;
    client->recent = item;
}

// Retrieve a value corresponding to a particular cache key,
// creating one if it does not already exist.
// The returned item is guaranteed to be valid until the next ass_cache_cut call;
// to extend its lifetime further, call ass_cache_inc_ref().
void *ass_cache_get(Cache *cache, CacheClient *client, void *key, void *priv)
{
    const CacheDesc *desc = cache->desc;
    ass_hashcode hash = desc->hash_func(key, ASS_HASH_INIT);
    CacheItem *_Atomic *bucket = &cache->map[hash & (cache->buckets - 1)];

    CacheItem *head = atomic_load_explicit(bucket, memory_order_acquire);
    CacheItem *item = find_item(head, NULL, hash, desc, key);
    if (item) {
        desc->key_move_func(NULL, key);
        use_item(cache, client, item);
        wait_until_ready(cache, item);
        return (char *) item + CACHE_ITEM_SIZE;
    }

    size_t key_offs = CACHE_ITEM_SIZE + align_cache(desc->value_size);
    item = malloc(key_offs + desc->key_size);
    if (!item) {
        desc->key_move_func(NULL, key);
        return NULL;
    }
    void *new_key = (char *) item + key_offs;
    if (!desc->key_move_func(new_key, key)) {
        free(item);
        return NULL;
    }
    item->cache = cache;
    item->desc = desc;
    item->hash = hash;
    item->queue_prev = NULL;
    item->parked = false;
    atomic_init(&item->next, head);
    atomic_init(&item->ref_count, 1);
    atomic_init(&item->frame, cache->frame);
    atomic_init(&item->state, ITEM_CONSTRUCTING);

    // If another thread inserted the same key meanwhile, use its item instead.
    CacheItem *searched = head;
    while (!atomic_compare_exchange_weak_explicit(bucket, &head, item,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
        CacheItem *other = find_item(head, searched, hash, desc, new_key);
        if (other) {
            desc->key_destruct_func(new_key);
            free(item);
            use_item(cache, client, other);
            wait_until_ready(cache, other);
            return (char *) other + CACHE_ITEM_SIZE;
        }
        searched = head;
        atomic_store_explicit(&item->next, head, memory_order_relaxed);
    }

    item->recent_next = client->recent;
    client->recent = item;

    void *value = (char *) item + CACHE_ITEM_SIZE;
    item->size = desc->construct_func(new_key, value, priv);
    assert(item->size);
    atomic_fetch_add_explicit(&cache->cache_size, item_cost(item->size),
                              memory_order_relaxed);
    mark_ready(cache, item);
    return value;
}

void *ass_cache_key(void *value)
{
    CacheItem *item = value_to_item(value);
    return (char *) value + align_cache(item->desc->value_size);
}

static inline void destroy_item(const CacheDesc *desc, CacheItem *item)
{
    assert(item->desc == desc);
    char *value = (char *) item + CACHE_ITEM_SIZE;
    if (desc->value_destruct_func)
        desc->value_destruct_func(value);
    desc->key_destruct_func(value + align_cache(desc->value_size));
    free(item);
}

void ass_cache_inc_ref(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    atomic_fetch_add_explicit(&item->ref_count, 1, memory_order_relaxed);
}

// Takes the item out of the cache, freeing it unless something else holds it.
static void remove_item(Cache *cache, CacheItem *item)
{
    CacheItem *_Atomic *link = &cache->map[item->hash & (cache->buckets - 1)];
    CacheItem *cur;
    while ((cur = atomic_load_explicit(link, memory_order_relaxed)) != item)
        link = &cur->next;
    atomic_store_explicit(link, atomic_load_explicit(&item->next, memory_order_relaxed),
                          memory_order_relaxed);

    atomic_fetch_sub_explicit(&cache->cache_size, item_cost(item->size), memory_order_relaxed);
    item->cache = NULL;
    ass_cache_dec_ref((char *) item + CACHE_ITEM_SIZE);
}

void ass_cache_dec_ref(void *value)
{
    if (!value)
        return;
    CacheItem *item = value_to_item(value);
    size_t left = atomic_fetch_sub_explicit(&item->ref_count, 1, memory_order_acq_rel) - 1;
    if (left == 1 && item->parked) {
        item->parked = false;
        remove_item(item->cache, item);
    } else if (!left) {
        assert(!item->cache);
        destroy_item(item->desc, item);
    }
}

static void unqueue_item(Cache *cache, CacheItem *item)
{
    if (item->queue_next)
        item->queue_next->queue_prev = item->queue_prev;
    else
        cache->queue_last = item->queue_prev;
    *item->queue_prev = item->queue_next;
    item->queue_prev = NULL;
}

void ass_cache_flush(CacheClient *client)
{
    CacheItem *item = client->recent;
    client->recent = NULL;
    while (item) {
        Cache *cache = item->cache;
        if (item->queue_prev)
            unqueue_item(cache, item);
        item->parked = false;
        item->queue_next = NULL;
        item->queue_prev = cache->queue_last;
        *cache->queue_last = item;
        cache->queue_last = &item->queue_next;
        item = item->recent_next;
    }
}

// Evicts the least recently used items, parking those others hold.
void ass_cache_cut(Cache *cache, size_t max_size)
{
    while (cache->queue_first &&
           atomic_load_explicit(&cache->cache_size, memory_order_relaxed) > max_size) {
        CacheItem *item = cache->queue_first;
        unqueue_item(cache, item);
        if (atomic_load_explicit(&item->ref_count, memory_order_relaxed) > 1)
            item->parked = true;
        else
            remove_item(cache, item);
    }
    cache->frame++;
}

void ass_cache_empty(Cache *cache)
{
    // Every item leaves the map before any is let go of, as letting go of one
    // can let go of others.
    CacheItem *items = NULL;
    for (unsigned i = 0; i < cache->buckets; i++) {
        CacheItem *item = atomic_load_explicit(&cache->map[i], memory_order_relaxed);
        atomic_store_explicit(&cache->map[i], NULL, memory_order_relaxed);
        while (item) {
            CacheItem *next = atomic_load_explicit(&item->next, memory_order_relaxed);
            item->cache = NULL;
            item->parked = false;
            atomic_store_explicit(&item->next, items, memory_order_relaxed);
            items = item;
            item = next;
        }
    }
    cache->queue_first = NULL;
    cache->queue_last = &cache->queue_first;
    atomic_store_explicit(&cache->cache_size, 0, memory_order_relaxed);

    while (items) {
        CacheItem *next = atomic_load_explicit(&items->next, memory_order_relaxed);
        ass_cache_dec_ref((char *) items + CACHE_ITEM_SIZE);
        items = next;
    }
}

void ass_cache_done(Cache *cache)
{
    ass_cache_empty(cache);
    pthread_cond_destroy(&cache->wait_cond);
    pthread_mutex_destroy(&cache->wait_lock);
    free(cache->map);
    free(cache);
}

// Type-specific creation function
Cache *ass_font_cache_create(void)
{
    return ass_cache_create(&font_cache_desc);
}

Cache *ass_outline_cache_create(void)
{
    return ass_cache_create(&outline_cache_desc);
}

Cache *ass_glyph_metrics_cache_create(void)
{
    return ass_cache_create(&glyph_metrics_cache_desc);
}

Cache *ass_shape_cache_create(void)
{
    return ass_cache_create(&shape_cache_desc);
}

Cache *ass_face_size_metrics_cache_create(void)
{
    return ass_cache_create(&face_size_metrics_cache_desc);
}

Cache *ass_bitmap_cache_create(void)
{
    return ass_cache_create(&bitmap_cache_desc);
}

Cache *ass_composite_cache_create(void)
{
    return ass_cache_create(&composite_cache_desc);
}
