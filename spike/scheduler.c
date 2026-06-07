#include "scheduler.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>

#define MAX_SYS    128
#define MAX_TASKS  4096
#define MAX_TERMS  16
#define GRAIN      4096   /* entities per chunk target for width selection */

/* ---- conflict analysis (mirrors flecs_pipeline_check_term) -------------- */
typedef struct {
    ecs_id_t r[MAX_TERMS], w[MAX_TERMS];
    int nr, nw, write_all;
} access_t;

static void add_id(ecs_id_t *a, int *n, ecs_id_t id) {
    for (int i = 0; i < *n; i++) if (a[i] == id) return;
    if (*n < MAX_TERMS) a[(*n)++] = id;
}

static void analyze(ecs_world_t *world, ecs_entity_t sys, access_t *o) {
    const ecs_query_t *q = ecs_system_get(world, sys)->query;
    o->nr = o->nw = o->write_all = 0;
    for (int t = 0; t < q->term_count; t++) {
        const ecs_term_t *term = &q->terms[t];
        int16_t io = term->inout, oper = term->oper;
        if (io == EcsInOutNone || io == EcsInOutFilter) continue;
        int from_any  = ecs_term_match_0((ecs_term_t*)term);
        int from_this = ecs_term_match_this((ecs_term_t*)term);
        int is_shared = !from_any && (!from_this || !(term->src.id & EcsSelf));
        if (io == EcsInOutDefault) {
            if (from_any) continue;
            io = is_shared ? EcsIn : EcsInOut;
        }
        int writes = (io == EcsOut || io == EcsInOut) ||
                     (oper == EcsNot && io == EcsOut);
        int reads  = (io == EcsIn || io == EcsInOut);
        if (writes) {
            if (term->id == EcsWildcard) o->write_all = 1;
            else add_id(o->w, &o->nw, term->id);
        }
        if (reads) add_id(o->r, &o->nr, term->id);
    }
}

static int id_overlap(ecs_id_t a, ecs_id_t b) {
    if (a == EcsWildcard || b == EcsWildcard || a == EcsAny || b == EcsAny) return 1;
    return ecs_id_match(a, b) || ecs_id_match(b, a);
}
static int hits(const ecs_id_t *arr, int n, ecs_id_t id) {
    for (int i = 0; i < n; i++) if (id_overlap(arr[i], id)) return 1;
    return 0;
}
static int conflicts(const access_t *x, const access_t *y) {
    if (x->write_all || y->write_all) return (x->nr || x->nw || y->nr || y->nw);
    for (int i = 0; i < x->nw; i++)
        if (hits(y->r, y->nr, x->w[i]) || hits(y->w, y->nw, x->w[i])) return 1;
    for (int i = 0; i < y->nw; i++)
        if (hits(x->r, x->nr, y->w[i])) return 1;
    return 0;
}

/* ---- scheduler state --------------------------------------------------- */
typedef struct { ecs_entity_t sys; int k, K; } task_t;

struct ecs_scheduler_t {
    ecs_world_t *world;
    int threads;

    ecs_entity_t systems[MAX_SYS];
    access_t     access[MAX_SYS];
    int          width[MAX_SYS];      /* K per system */
    int          nsystems;

    /* ordering derived from flecs DependsOn/phases */
    int depth[MAX_SYS];               /* longest DependsOn path length */
    int order[MAX_SYS];               /* system indices sorted by (depth, id) */
    int wave_of[MAX_SYS];             /* wave index assigned to each system */
    int nwaves;

    /* flat task list, grouped by wave */
    task_t tasks[MAX_TASKS];
    int    wave_task_start[MAX_SYS], wave_task_count[MAX_SYS];
    int    ntasks;

    /* per-frame run state */
    volatile int cursor[MAX_SYS];     /* atomic task cursor per wave */
    ecs_ftime_t  dt;
    pthread_barrier_t b_drained, b_merged;
};

ecs_scheduler_t* scheduler_new(ecs_world_t *world, int threads) {
    ecs_scheduler_t *s = calloc(1, sizeof *s);
    s->world = world;
    s->threads = threads < 1 ? 1 : threads;
    ecs_set_stage_count(world, s->threads);
    return s;
}

void scheduler_add(ecs_scheduler_t *s, ecs_entity_t system) {
    ecs_assert(s->nsystems < MAX_SYS, ECS_INVALID_OPERATION, "too many systems");
    s->systems[s->nsystems++] = system;
}

/* Longest DependsOn path length to a root. Reuses flecs' relationship data:
 * a system's phase (DependsOn a phase entity) and any fine-grained
 * DependsOn(system, system) both contribute. Phase chains run ~10 deep, so
 * recomputation is cheap; `guard` catches accidental cycles. */
static int dep_depth(ecs_world_t *world, ecs_entity_t e, int guard) {
    ecs_assert(guard > 0, ECS_INVALID_OPERATION, "DependsOn cycle detected");
    int max = -1;
    for (int i = 0; ; i++) {
        ecs_entity_t t = ecs_get_target(world, e, EcsDependsOn, i);
        if (!t) break;
        int d = dep_depth(world, t, guard - 1);
        if (d > max) max = d;
    }
    return max + 1;   /* no targets -> 0 */
}

static int count_entities(ecs_world_t *world, ecs_entity_t sys) {
    const ecs_query_t *q = ecs_system_get(world, sys)->query;
    ecs_iter_t it = ecs_query_iter(world, q);
    int c = 0;
    while (ecs_query_next(&it)) c += it.count;
    return c;
}

void scheduler_build(ecs_scheduler_t *s) {
    ecs_world_t *world = s->world;

    /* 1. access sets + adaptive width */
    for (int i = 0; i < s->nsystems; i++) {
        analyze(world, s->systems[i], &s->access[i]);
        const ecs_system_t *sd = ecs_system_get(world, s->systems[i]);
        if (!sd->multi_threaded) {
            s->width[i] = 1;                 /* can't data-parallelize */
        } else {
            int n = count_entities(world, s->systems[i]);
            int k = (n + GRAIN - 1) / GRAIN; /* ceil(n / GRAIN) */
            if (k < 1) k = 1;
            if (k > s->threads) k = s->threads;
            s->width[i] = k;
        }
    }

    /* 2. ordering from flecs DependsOn/phases: depth + (depth, entity id) sort.
     *    Same depth => DependsOn-independent (a dep would deepen it). */
    for (int i = 0; i < s->nsystems; i++) {
        s->depth[i] = dep_depth(world, s->systems[i], 4096);
        s->order[i] = i;
    }
    /* insertion sort by (depth, entity id) -- entity id is flecs' within-phase
     * tiebreak, which also resolves direction for same-depth data conflicts */
    for (int i = 1; i < s->nsystems; i++) {
        int oi = s->order[i], j = i - 1;
        while (j >= 0) {
            int oj = s->order[j];
            int after = (s->depth[oj] > s->depth[oi]) ||
                (s->depth[oj] == s->depth[oi] && s->systems[oj] > s->systems[oi]);
            if (!after) break;
            s->order[j + 1] = oj; j--;
        }
        s->order[j + 1] = oi;
    }

    /* 3. wave assignment. Walking in (depth, id) order, a system must land in a
     *    strictly later wave than any earlier system P when P is in a previous
     *    stage (depth[P] < depth[S]) OR P data-conflicts with S. Otherwise it
     *    may share P's wave. This honors "run after all previous-stage systems"
     *    (broad phase ordering) and conflict direction (P precedes S in id
     *    order), while letting independent same-depth systems pack together. */
    s->nwaves = 0;
    for (int a = 0; a < s->nsystems; a++) {
        int si = s->order[a];
        int min_wave = 0;
        for (int b = 0; b < a; b++) {
            int pi = s->order[b];
            int must_after = (s->depth[pi] < s->depth[si]) ||
                             conflicts(&s->access[pi], &s->access[si]);
            if (must_after && s->wave_of[pi] + 1 > min_wave)
                min_wave = s->wave_of[pi] + 1;
        }
        s->wave_of[si] = min_wave;
        if (min_wave + 1 > s->nwaves) s->nwaves = min_wave + 1;
    }

    /* 4. flatten into (system, k, K) tasks grouped by wave, preserving
     *    (depth, id) order within each wave. */
    s->ntasks = 0;
    for (int w = 0; w < s->nwaves; w++) {
        s->wave_task_start[w] = s->ntasks;
        for (int a = 0; a < s->nsystems; a++) {
            int si = s->order[a];
            if (s->wave_of[si] != w) continue;
            int K = s->width[si];
            for (int k = 0; k < K; k++) {
                ecs_assert(s->ntasks < MAX_TASKS, ECS_INVALID_OPERATION, "too many tasks");
                s->tasks[s->ntasks++] = (task_t){ s->systems[si], k, K };
            }
        }
        s->wave_task_count[w] = s->ntasks - s->wave_task_start[w];
    }

    pthread_barrier_init(&s->b_drained, NULL, s->threads);
    pthread_barrier_init(&s->b_merged,  NULL, s->threads);
}

/* ---- execution --------------------------------------------------------- */
typedef struct { ecs_scheduler_t *s; int tid; } worker_arg_t;

static void worker_loop(ecs_scheduler_t *s, int tid) {
    ecs_world_t *stage = ecs_get_stage(s->world, tid);
    for (int w = 0; w < s->nwaves; w++) {
        int base = s->wave_task_start[w], n = s->wave_task_count[w];

        /* drain this wave's queue: any thread runs any task (all conflict-free) */
        for (;;) {
            int idx = __atomic_fetch_add(&s->cursor[w], 1, __ATOMIC_SEQ_CST);
            if (idx >= n) break;
            task_t *t = &s->tasks[base + idx];
            ecs_run_worker(stage, t->sys, t->k, t->K, s->dt, NULL);
        }

        pthread_barrier_wait(&s->b_drained);     /* all tasks of wave w done */
        if (tid == 0) {                          /* serial merge on main/stage 0 */
            ecs_readonly_end(s->world);
            if (w + 1 < s->nwaves) ecs_readonly_begin(s->world, true);
        }
        pthread_barrier_wait(&s->b_merged);      /* wait for merge before next wave */
    }
}

static void* worker_thread(void *arg) {
    worker_arg_t *wa = arg;
    worker_loop(wa->s, wa->tid);
    return NULL;
}

void scheduler_run(ecs_scheduler_t *s, ecs_ftime_t delta_time) {
    if (s->nwaves == 0) return;
    s->dt = delta_time;
    for (int w = 0; w < s->nwaves; w++) s->cursor[w] = 0;

    ecs_readonly_begin(s->world, true);          /* open wave 0 */

    int W = s->threads;
    pthread_t th[MAX_SYS];
    worker_arg_t args[MAX_SYS];
    for (int t = 1; t < W; t++) {
        args[t] = (worker_arg_t){ s, t };
        pthread_create(&th[t], NULL, worker_thread, &args[t]);
    }
    worker_loop(s, 0);                            /* main thread is worker 0 */
    for (int t = 1; t < W; t++) pthread_join(th[t], NULL);
    /* world is merged and out of readonly mode after the last wave */
}

void scheduler_print(const ecs_scheduler_t *s) {
    printf("schedule: %d systems, %d threads, %d waves, %d tasks\n",
        s->nsystems, s->threads, s->nwaves, s->ntasks);
    for (int w = 0; w < s->nwaves; w++) {
        printf("  wave %d:", w);
        for (int a = 0; a < s->nsystems; a++) {
            int si = s->order[a];
            if (s->wave_of[si] != w) continue;
            printf(" %s(d=%d,K=%d)", ecs_get_name(s->world, s->systems[si]),
                s->depth[si], s->width[si]);
        }
        printf("\n");
    }
}

void scheduler_free(ecs_scheduler_t *s) {
    if (!s) return;
    pthread_barrier_destroy(&s->b_drained);
    pthread_barrier_destroy(&s->b_merged);
    free(s);
}
