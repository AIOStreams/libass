/*
 * Copyright (C) 2026 Viren070
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

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "ass_render.h"

// A frame's events render on the calling thread, which wakes as few helper
// threads as are predicted to bring the frame under TARGET_US: more only cost
// more, as threads on the same cores slow each other down. An event is
// expected to cost what it did last frame, and a new one what new events of
// its layer and style have lately. Costly events go first, costliest first,
// so that no long one starts last. Threads that keep waiting for each other's
// cache items, as when their events need the same new glyphs, are worth less
// than they cost: after two such frames in a row, frames use half as many
// threads, and one more again after every CALM_FRAMES frames without.

#define MAX_DEFAULT_THREADS 8
#define TARGET_US 16000
#define NEW_EVENT_US TARGET_US  // a new event's expected cost, with nothing to go by
#define HEAVY_EVENT_US 500      // events this costly are taken one at a time
#define NEW_COST_SLOTS 256      // a power of two
#define CALM_FRAMES 8

typedef struct {
    int index;          // in the track's events
    int read_order;     // tells the event apart after the track changed
    uint32_t cost_us;
} EventCost;

// What new events of a layer and style have lately cost: a running mean that
// each new one moves a quarter of the way to its own cost.
typedef struct {
    bool used;
    int layer, style;
    uint32_t cost_us;
} NewEventCost;

typedef struct {
    uint32_t cost_us;   // expected
    int eimg;
} PlannedEvent;

typedef struct {
    pthread_t thread;
    RenderContext context;
    struct render_pool *pool;
} Helper;

struct render_pool {
    ASS_Renderer *renderer;

    pthread_mutex_t lock;
    pthread_cond_t work;    // idle helpers wait here
    pthread_cond_t done;    // the calling thread waits here for busy helpers
    Helper *helpers;
    int n_helpers;
    bool started;
    int wanted;             // helpers still to join the current frame
    int busy;               // helpers working on it
    int joined;             // helpers that worked on it
    bool quit;

    int max_threads;        // a frame may use, lowered while threads wait on each other
    int contended, calm;    // frames in a row with and without that

    // the current frame: its events in the order to render them, and which
    // are new, by index in eimg
    PlannedEvent *plan;
    bool *fresh;
    int count, n_heavy, batch;
    atomic_int next;

    // last frame's costs, in event order
    ASS_Track *costs_track;
    EventCost *costs;
    int n_costs;
    NewEventCost new_costs[NEW_COST_SLOTS];

    int max_events;
};

int ass_default_threads(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    long cores = info.dwNumberOfProcessors;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return FFMINMAX(cores, 1, MAX_DEFAULT_THREADS);
}

static void render_event(RenderContext *state, EventImages *event_images)
{
    int64_t start = ass_now_us(), waited = state->cache_client.wait_us;
    event_images->rendered = ass_render_event(state, event_images->event, event_images);
    int64_t cost = ass_now_us() - start - (state->cache_client.wait_us - waited);
    event_images->cost_us = FFMINMAX(cost, 0, UINT32_MAX);
}

static void wake_helpers(RenderPool *pool, int threads);

// The threads it takes to render the events not yet taken within TARGET_US
static int threads_for_rest(RenderPool *pool)
{
    int64_t rest = 0;
    for (int i = atomic_load_explicit(&pool->next, memory_order_relaxed); i < pool->count; i++)
        rest += pool->plan[i].cost_us;
    return FFMIN((rest + TARGET_US - 1) / TARGET_US, pool->max_threads);
}

// Renders the frame's events until none are left. Past check_at, which only
// the calling thread sets, helpers are woken if what remains needs them.
static void run_queue(RenderPool *pool, RenderContext *state, int64_t check_at)
{
    EventImages *eimg = pool->renderer->eimg;
    for (;;) {
        int i = atomic_load_explicit(&pool->next, memory_order_relaxed);
        int n = i < pool->n_heavy ? 1 : pool->batch;
        i = atomic_fetch_add_explicit(&pool->next, n, memory_order_relaxed);
        if (i >= pool->count)
            return;
        n = FFMIN(n, pool->count - i);
        for (int j = i; j < i + n; j++)
            render_event(state, eimg + pool->plan[j].eimg);

        if (check_at && ass_now_us() >= check_at) {
            wake_helpers(pool, threads_for_rest(pool));
            check_at = 0;
        }
    }
}

static void *run_helper(void *arg)
{
    Helper *helper = arg;
    RenderPool *pool = helper->pool;

    pthread_mutex_lock(&pool->lock);
    for (;;) {
        while (!pool->quit && !pool->wanted)
            pthread_cond_wait(&pool->work, &pool->lock);
        if (pool->quit)
            break;
        pool->wanted--;
        pool->busy++;
        pool->joined++;
        pthread_mutex_unlock(&pool->lock);

        ass_setup_shaper(helper->context.shaper, pool->renderer);
        run_queue(pool, &helper->context, 0);

        pthread_mutex_lock(&pool->lock);
        if (!--pool->busy)
            pthread_cond_signal(&pool->done);
    }
    pthread_mutex_unlock(&pool->lock);
    return NULL;
}

// Starts the helpers from the rendering thread, whose priority they inherit.
static void start_helpers(RenderPool *pool)
{
    pool->started = true;
    int n = pool->renderer->settings.threads - 1;
    pool->helpers = calloc(n, sizeof(*pool->helpers));
    if (!pool->helpers)
        return;

    for (int i = 0; i < n; i++) {
        Helper *helper = pool->helpers + i;
        helper->pool = pool;
        if (!ass_render_context_init(&helper->context, pool->renderer)) {
            ass_render_context_done(&helper->context);
            break;
        }
        if (pthread_create(&helper->thread, NULL, run_helper, helper)) {
            ass_render_context_done(&helper->context);
            break;
        }
#if defined(__linux__)
        pthread_setname_np(helper->thread, "libass/render");
#endif
        pool->n_helpers++;
    }
}

// Wakes helpers until, with the calling thread, that many work on the frame.
static void wake_helpers(RenderPool *pool, int threads)
{
    if (threads < 2)
        return;
    if (!pool->started)
        start_helpers(pool);

    pthread_mutex_lock(&pool->lock);
    int n = FFMIN(threads - 1, pool->n_helpers) - pool->busy - pool->wanted;
    if (n > 0)
        pool->wanted += n;
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < n; i++)
        pthread_cond_signal(&pool->work);
}

static void wait_for_helpers(RenderPool *pool)
{
    if (!pool->n_helpers)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->wanted = 0;
    while (pool->busy)
        pthread_cond_wait(&pool->done, &pool->lock);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->n_helpers; i++)
        ass_cache_flush(&pool->helpers[i].context.cache_client);
}

static bool reserve_events(RenderPool *pool, int count)
{
    if (count <= pool->max_events)
        return true;

    int size = FFMAX(count, 2 * pool->max_events);
    PlannedEvent *plan = realloc(pool->plan, size * sizeof(*plan));
    if (plan)
        pool->plan = plan;
    bool *fresh = realloc(pool->fresh, size * sizeof(*fresh));
    if (fresh)
        pool->fresh = fresh;
    EventCost *costs = realloc(pool->costs, size * sizeof(*costs));
    if (costs)
        pool->costs = costs;
    if (!plan || !fresh || !costs)
        return false;

    pool->max_events = size;
    return true;
}

// The estimate for new events like this one, or the unused slot for it
static NewEventCost *find_new_event_cost(RenderPool *pool, ASS_Event *event)
{
    unsigned slot = ((unsigned) event->Layer * 2654435761u ^ event->Style) & (NEW_COST_SLOTS - 1);
    for (int n = 0; n < NEW_COST_SLOTS; n++, slot = (slot + 1) & (NEW_COST_SLOTS - 1)) {
        NewEventCost *entry = pool->new_costs + slot;
        if (!entry->used || (entry->layer == event->Layer && entry->style == event->Style))
            return entry;
    }
    return NULL;
}

// Expects each event's cost, through cost_us.
static void predict_costs(RenderPool *pool, int count)
{
    ASS_Track *track = pool->renderer->track;
    EventImages *eimg = pool->renderer->eimg;
    if (pool->costs_track != track) {
        pool->n_costs = 0;
        memset(pool->new_costs, 0, sizeof(pool->new_costs));
    }

    for (int i = 0, j = 0; i < count; i++) {
        ASS_Event *event = eimg[i].event;
        int index = event - track->events;
        while (j < pool->n_costs && pool->costs[j].index < index)
            j++;
        pool->fresh[i] = j == pool->n_costs || pool->costs[j].index != index ||
                         pool->costs[j].read_order != event->ReadOrder;
        if (!pool->fresh[i]) {
            eimg[i].cost_us = pool->costs[j].cost_us;
            continue;
        }
        NewEventCost *estimate = find_new_event_cost(pool, event);
        eimg[i].cost_us = estimate && estimate->used ? estimate->cost_us : NEW_EVENT_US;
    }
}

static int compare_cost(const void *a, const void *b)
{
    uint32_t ca = ((const PlannedEvent *) a)->cost_us, cb = ((const PlannedEvent *) b)->cost_us;
    return (ca < cb) - (ca > cb);
}

// Orders the frame's events, the heavy ones first and costliest first, and
// returns how many threads to render them with.
static int plan_frame(RenderPool *pool, int count)
{
    EventImages *eimg = pool->renderer->eimg;
    predict_costs(pool, count);

    int64_t total = 0;
    uint32_t max_cost = 0;
    int n_heavy = 0;
    for (int i = 0; i < count; i++) {
        uint32_t cost = eimg[i].cost_us;
        total += cost;
        max_cost = FFMAX(max_cost, cost);
        if (cost >= HEAVY_EVENT_US)
            pool->plan[n_heavy++] = (PlannedEvent) { cost, i };
    }
    qsort(pool->plan, n_heavy, sizeof(*pool->plan), compare_cost);
    for (int i = 0, n = n_heavy; i < count; i++)
        if (eimg[i].cost_us < HEAVY_EVENT_US)
            pool->plan[n++] = (PlannedEvent) { eimg[i].cost_us, i };

    // A frame can't take less than its costliest event.
    int64_t share = FFMAX(max_cost, TARGET_US);
    int threads = FFMINMAX((total + share - 1) / share, 1, pool->max_threads);

    pool->count = count;
    pool->n_heavy = n_heavy;
    pool->batch = FFMINMAX((count - n_heavy) / (8 * threads), 1, 32);
    atomic_store_explicit(&pool->next, 0, memory_order_relaxed);
    return threads;
}

static int64_t take_wait(CacheClient *client)
{
    int64_t wait_us = client->wait_us;
    client->wait_us = 0;
    return wait_us;
}

static void adapt_threads(RenderPool *pool, int count)
{
    int64_t work = 0, waited = take_wait(&pool->renderer->state.cache_client);
    for (int i = 0; i < count; i++)
        work += pool->renderer->eimg[i].cost_us;
    for (int i = 0; i < pool->n_helpers; i++)
        waited += take_wait(&pool->helpers[i].context.cache_client);
    int used = pool->joined + 1;
    pool->joined = 0;

    if (used > 1 && waited * 4 > work) {
        pool->calm = 0;
        if (++pool->contended >= 2) {
            pool->max_threads = FFMAX(used / 2, 1);
            pool->contended = 0;
        }
        return;
    }
    pool->contended = 0;
    if (pool->max_threads < pool->renderer->settings.threads && ++pool->calm >= CALM_FRAMES) {
        pool->max_threads++;
        pool->calm = 0;
    }
}

static void save_costs(RenderPool *pool, int count)
{
    ASS_Track *track = pool->renderer->track;
    EventImages *eimg = pool->renderer->eimg;
    for (int i = 0; i < count; i++) {
        ASS_Event *event = eimg[i].event;
        uint32_t cost = eimg[i].cost_us;
        pool->costs[i] = (EventCost) { event - track->events, event->ReadOrder, cost };
        if (!pool->fresh[i])
            continue;
        NewEventCost *estimate = find_new_event_cost(pool, event);
        if (!estimate)
            continue;
        if (estimate->used)
            estimate->cost_us += ((int64_t) cost - estimate->cost_us) / 4;
        else
            *estimate = (NewEventCost) { true, event->Layer, event->Style, cost };
    }
    pool->n_costs = count;
    pool->costs_track = track;
}

static RenderPool *get_pool(ASS_Renderer *priv)
{
    if (!priv->pool && priv->settings.threads > 1) {
        RenderPool *pool = calloc(1, sizeof(*pool));
        if (!pool)
            return NULL;
        pool->renderer = priv;
        pool->max_threads = priv->settings.threads;
        pthread_mutex_init(&pool->lock, NULL);
        pthread_cond_init(&pool->work, NULL);
        pthread_cond_init(&pool->done, NULL);
        priv->pool = pool;
    }
    return priv->pool;
}

void ass_render_events(ASS_Renderer *priv, int count)
{
    RenderPool *pool = get_pool(priv);
    if (!pool || !reserve_events(pool, count)) {
        for (int i = 0; i < count; i++) {
            EventImages *event_images = priv->eimg + i;
            event_images->rendered = ass_render_event(&priv->state, event_images->event,
                                                      event_images);
        }
        return;
    }

    int threads = plan_frame(pool, count);
    wake_helpers(pool, threads);
    bool all_awake = pool->started && threads > pool->n_helpers;
    run_queue(pool, &priv->state, all_awake ? 0 : ass_now_us() + TARGET_US);
    wait_for_helpers(pool);
    adapt_threads(pool, count);
    save_costs(pool, count);
}

void ass_render_pool_free(RenderPool *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->quit = true;
    pthread_mutex_unlock(&pool->lock);
    pthread_cond_broadcast(&pool->work);
    for (int i = 0; i < pool->n_helpers; i++) {
        pthread_join(pool->helpers[i].thread, NULL);
        ass_render_context_done(&pool->helpers[i].context);
    }

    pthread_cond_destroy(&pool->done);
    pthread_cond_destroy(&pool->work);
    pthread_mutex_destroy(&pool->lock);
    free(pool->helpers);
    free(pool->plan);
    free(pool->fresh);
    free(pool->costs);
    free(pool);
}
