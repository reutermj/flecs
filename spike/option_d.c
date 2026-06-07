/*
 * Does flecs' OWN native multithreaded pipeline have the order_by race?
 *
 * Sets up a multi_threaded system whose query uses order_by, registers it in
 * the default pipeline, enables real worker threads (ecs_set_threads), and runs
 * many frames via ecs_progress -- exactly flecs' built-in scheduler. The sort
 * component is mutated every frame to keep the cache "dirty" so iter-init has to
 * re-sort. If flecs' workers concurrently sort the same cache, TSan will catch
 * it in flecs_query_cache_sort_tables / sort_table.
 *
 * Usage: ./option_d [threads] [frames]
 */
#include "flecs.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef N
#define N 20000
#endif

typedef struct { float v; } SortKey;
typedef struct { float v; } Work;

static ecs_entity_t ecs_id(SortKey), ecs_id(Work);

static int cmp_sortkey(ecs_entity_t e1, const void *p1,
                       ecs_entity_t e2, const void *p2) {
    (void)e1; (void)e2;
    float a = ((const SortKey*)p1)->v, b = ((const SortKey*)p2)->v;
    return (a > b) - (a < b);
}

/* multi_threaded system: reads SortKey (the order_by component), writes Work,
 * and perturbs SortKey so the table is dirty next frame -> forces re-sort. */
static void Sys_Sorted(ecs_iter_t *it) {
    SortKey *k = ecs_field(it, SortKey, 0);
    Work *w = ecs_field(it, Work, 1);
    for (int i = 0; i < it->count; i++) {
        w[i].v += k[i].v;
        k[i].v = k[i].v * 1.000001f + 0.5f;  /* mutate sort key -> dirties table */
    }
}

int main(int argc, char **argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 4;
    int frames  = (argc > 2) ? atoi(argv[2]) : 40;

    ecs_world_t *world = ecs_init();
    ecs_id(SortKey) = ecs_component(world, {.type.size=sizeof(SortKey),.type.alignment=ECS_ALIGNOF(SortKey)});
    ecs_id(Work)    = ecs_component(world, {.type.size=sizeof(Work),   .type.alignment=ECS_ALIGNOF(Work)});

    for (int i = 0; i < N; i++) {
        ecs_entity_t e = ecs_new(world);
        ecs_set_id(world, e, ecs_id(SortKey), sizeof(SortKey), &(SortKey){(float)(N - i)});
        ecs_set_id(world, e, ecs_id(Work),    sizeof(Work),    &(Work){0});
    }

    /* multi_threaded system with order_by, in the default pipeline (EcsOnUpdate) */
    ecs_system(world, {
        .entity = ecs_entity(world, {.name="Sorted",
            .add = ecs_ids(ecs_dependson(EcsOnUpdate))}),
        .query.terms = {{ecs_id(SortKey), .inout=EcsIn}, {ecs_id(Work), .inout=EcsOut}},
        .query.order_by = ecs_id(SortKey),
        .query.order_by_callback = cmp_sortkey,
        .callback = Sys_Sorted,
        .multi_threaded = true
    });

    ecs_set_threads(world, threads);   /* real flecs worker threads */
    printf("flecs native pipeline: threads=%d frames=%d N=%d, order_by multi_threaded system\n",
        threads, frames, N);

    for (int f = 0; f < frames; f++) {
        ecs_progress(world, 0);
    }

    printf("done (see TSan output for races in flecs' own sort path)\n");
    ecs_fini(world);
    return 0;
}
