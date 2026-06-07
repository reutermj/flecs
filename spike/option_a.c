/*
 * Option A spike: can two phases run concurrently on the same world,
 * on different OS threads, with no flecs-provided locking, given:
 *   - storage frozen once (readonly mode),
 *   - disjoint write sets,
 *   - structural changes deferred to per-thread stages,
 *   - a single serial merge at the end.
 *
 * Mode 0 (disjoint):  thread A writes Position, thread B writes Velocity.   -> expect clean
 * Mode 1 (conflict):  both threads write Position (same memory).            -> negative control, expect race
 *
 * Usage: ./option_a [0|1]
 */
#include "flecs.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef N
#define N 100000
#endif

typedef struct { float x, y; } Position;
typedef struct { float x, y; } Velocity;

static ecs_entity_t ecs_id(Position);
static ecs_entity_t ecs_id(Velocity);
static ecs_entity_t TagA;
static ecs_entity_t TagB;

static ecs_world_t *g_world;
static ecs_query_t *g_qa;   /* phase A query */
static ecs_query_t *g_qb;   /* phase B query */
static int g_mode;

/* Phase A: write Position values + defer structural add of TagA */
static void *phase_a(void *arg) {
    int stage_id = *(int*)arg;
    ecs_world_t *stage = ecs_get_stage(g_world, stage_id);
    ecs_iter_t it = ecs_query_iter(stage, g_qa);
    while (ecs_query_next(&it)) {
        Position *p = ecs_field(&it, Position, 0);
        for (int i = 0; i < it.count; i++) {
            p[i].x = 1.0f;           /* direct value write into frozen storage */
            ecs_add_id(stage, it.entities[i], TagA); /* deferred structural op */
        }
    }
    return NULL;
}

/* Phase B: in disjoint mode write Velocity; in conflict mode write Position too */
static void *phase_b(void *arg) {
    int stage_id = *(int*)arg;
    ecs_world_t *stage = ecs_get_stage(g_world, stage_id);
    ecs_iter_t it = ecs_query_iter(stage, g_qb);
    while (ecs_query_next(&it)) {
        if (g_mode == 0) {
            Velocity *v = ecs_field(&it, Velocity, 0);
            for (int i = 0; i < it.count; i++) {
                v[i].x = 2.0f;
                ecs_add_id(stage, it.entities[i], TagB);
            }
        } else {
            Position *p = ecs_field(&it, Position, 0); /* SAME component as A -> race */
            for (int i = 0; i < it.count; i++) {
                p[i].x = 2.0f;
                ecs_add_id(stage, it.entities[i], TagB);
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    g_mode = (argc > 1) ? atoi(argv[1]) : 0;
    printf("mode=%d (%s)\n", g_mode, g_mode ? "conflict/negative-control" : "disjoint");

    g_world = ecs_init();

    ecs_id(Position) = ecs_component(g_world, { .type.size = sizeof(Position),
        .type.alignment = ECS_ALIGNOF(Position) });
    ecs_id(Velocity) = ecs_component(g_world, { .type.size = sizeof(Velocity),
        .type.alignment = ECS_ALIGNOF(Velocity) });
    TagA = ecs_entity(g_world, {0});
    TagB = ecs_entity(g_world, {0});

    /* create N entities with Position + Velocity */
    for (int i = 0; i < N; i++) {
        ecs_entity_t e = ecs_new(g_world);
        ecs_set_id(g_world, e, ecs_id(Position), sizeof(Position), &(Position){0,0});
        ecs_set_id(g_world, e, ecs_id(Velocity), sizeof(Velocity), &(Velocity){0,0});
    }

    /* build queries before freezing the world */
    g_qa = ecs_query(g_world, { .terms = {{ .id = ecs_id(Position) }} });
    g_qb = ecs_query(g_world, { .terms = {{
        .id = (g_mode == 0) ? ecs_id(Velocity) : ecs_id(Position) }} });

    /* two stages, but WE manage the threads (no ecs_set_threads) */
    ecs_set_stage_count(g_world, 2);

    /* ---- concurrent section: freeze once, run both phases, merge once ---- */
    ecs_readonly_begin(g_world, true);

    int id0 = 0, id1 = 1;
    pthread_t ta, tb;
    pthread_create(&ta, NULL, phase_a, &id0);
    pthread_create(&tb, NULL, phase_b, &id1);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    ecs_readonly_end(g_world); /* serial merge of both command buffers */
    /* -------------------------------------------------------------------- */

    /* verification */
    int bad_pos = 0, bad_vel = 0, cnt_a = 0, cnt_b = 0;
    ecs_query_t *qall = ecs_query(g_world, { .terms = {{ .id = ecs_id(Position) }} });
    ecs_iter_t it = ecs_query_iter(g_world, qall);
    while (ecs_query_next(&it)) {
        Position *p = ecs_field(&it, Position, 0);
        for (int i = 0; i < it.count; i++) {
            if (g_mode == 0 && p[i].x != 1.0f) bad_pos++;
            if (ecs_has_id(g_world, it.entities[i], TagA)) cnt_a++;
            if (ecs_has_id(g_world, it.entities[i], TagB)) cnt_b++;
        }
    }
    if (g_mode == 0) {
        ecs_query_t *qv = ecs_query(g_world, { .terms = {{ .id = ecs_id(Velocity) }} });
        ecs_iter_t it2 = ecs_query_iter(g_world, qv);
        while (ecs_query_next(&it2)) {
            Velocity *v = ecs_field(&it2, Velocity, 0);
            for (int i = 0; i < it2.count; i++)
                if (v[i].x != 2.0f) bad_vel++;
        }
    }

    printf("verify: bad_pos=%d bad_vel=%d TagA_count=%d TagB_count=%d (expected %d each)\n",
        bad_pos, bad_vel, cnt_a, cnt_b, N);

    int ok = (bad_pos == 0) && (cnt_a == N) && (cnt_b == N) && (g_mode || bad_vel == 0);
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");

    ecs_fini(g_world);
    return ok ? 0 : 1;
}
