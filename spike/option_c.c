/*
 * Option C spike: data + system parallelism nested in one pool.
 *
 * W threads, each owns one stage (1:1, fixed). Several conflict-free
 * multi_threaded systems; each is split into K=W stripes. ALL stripes from ALL
 * systems go into one shared atomic work queue. A thread pulls (system, k, K)
 * tasks and runs each via ecs_run_worker(my_stage, sys, k, K) -- i.e. the
 * stage it defers to (its thread id) is decoupled from the slice index k.
 *
 * Validates:
 *   - data parallelism (each system split into stripes) AND
 *     system parallelism (different systems' stripes interleaved in the queue)
 *     running together in one readonly window.
 *   - the change-detection path: a system's K workers iterate the SAME query
 *     concurrently (caveat #2).
 *   - work-sharing: threads drain a shared queue, so a thread that finishes a
 *     cheap stripe picks up another system's stripe (per-thread task counts
 *     show the uneven draining).
 *
 * Modes:
 *   0  conflict-free wave {Move, Decay, Spin}      -> expect clean
 *   1  conflict control: add Move2 (also writes Pos), co-scheduled with Move -> race
 *
 * Usage: ./option_c [0|1]
 */
#include "flecs.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef N
#define N 200000
#endif
#define W 8                 /* pool threads / stages */

typedef struct { float x, y; } Pos;
typedef struct { float x, y; } Vel;
typedef struct { float v; } Health;
typedef struct { float v; } Energy;

static ecs_entity_t ecs_id(Pos), ecs_id(Vel), ecs_id(Health), ecs_id(Energy);
static ecs_entity_t Touched;

/* a little busy-work so stripes take non-trivial, uneven time */
static float burn(float x, int iters) { for (int i=0;i<iters;i++) x = x*1.000001f + 1.0f; return x; }

static void Sys_Move(ecs_iter_t *it) {           /* reads Vel, writes Pos (heavy) */
    Pos *p = ecs_field(it, Pos, 0); Vel *v = ecs_field(it, Vel, 1);
    for (int i=0;i<it->count;i++){ p[i].x = burn(p[i].x + v[i].x, 50); p[i].y += v[i].y;
        ecs_add_id(it->world, it->entities[i], Touched); }
}
static void Sys_Decay(ecs_iter_t *it) {          /* reads+writes Health (cheap) */
    Health *h = ecs_field(it, Health, 0);
    for (int i=0;i<it->count;i++) h[i].v *= 0.99f;
}
static void Sys_Spin(ecs_iter_t *it) {           /* reads+writes Energy (medium) */
    Energy *e = ecs_field(it, Energy, 0);
    for (int i=0;i<it->count;i++) e[i].v = burn(e[i].v, 10);
}
static void Sys_Move2(ecs_iter_t *it) {          /* conflicts with Move on Pos */
    Pos *p = ecs_field(it, Pos, 0);
    for (int i=0;i<it->count;i++) p[i].x = 123.0f;
}

/* ---- shared work queue ---- */
typedef struct { ecs_entity_t sys; int k, K; } task_t;
static ecs_world_t *g_world;
static task_t g_tasks[64];
static int g_ntasks;
static int g_next;                /* atomic cursor */
static int g_per_thread[W];

static void *worker(void *arg) {
    int tid = *(int*)arg;
    ecs_world_t *stage = ecs_get_stage(g_world, tid);
    for (;;) {
        int i = __atomic_fetch_add(&g_next, 1, __ATOMIC_SEQ_CST);
        if (i >= g_ntasks) break;
        task_t *t = &g_tasks[i];
        ecs_run_worker(stage, t->sys, t->k, t->K, 0, NULL); /* stage=tid, slice=k */
        g_per_thread[tid]++;
    }
    return NULL;
}

static void push_system(ecs_entity_t sys, int K) {
    for (int k=0;k<K;k++) g_tasks[g_ntasks++] = (task_t){sys, k, K};
}

static void run_pool(void) {
    g_next = 0;
    for (int i=0;i<W;i++) g_per_thread[i]=0;
    ecs_set_stage_count(g_world, W);
    ecs_readonly_begin(g_world, true);
    pthread_t th[W]; int ids[W];
    for (int i=0;i<W;i++){ ids[i]=i; pthread_create(&th[i],NULL,worker,&ids[i]); }
    for (int i=0;i<W;i++) pthread_join(th[i],NULL);
    ecs_readonly_end(g_world); /* serial merge of all W stages */
}

int main(int argc, char **argv) {
    int mode = (argc>1)?atoi(argv[1]):0;
    printf("mode=%d (%s), W=%d, N=%d\n", mode, mode?"conflict-control":"hybrid-wave", W, N);
    g_world = ecs_init();
    #define COMP(T) ecs_id(T)=ecs_component(g_world,{.type.size=sizeof(T),.type.alignment=ECS_ALIGNOF(T)})
    COMP(Pos);COMP(Vel);COMP(Health);COMP(Energy);
    Touched = ecs_entity(g_world,{.name="Touched"});

    for (int i=0;i<N;i++){ ecs_entity_t e=ecs_new(g_world);
        ecs_set_id(g_world,e,ecs_id(Pos),sizeof(Pos),&(Pos){(float)i,0});
        ecs_set_id(g_world,e,ecs_id(Vel),sizeof(Vel),&(Vel){1,1});
        ecs_set_id(g_world,e,ecs_id(Health),sizeof(Health),&(Health){100});
        ecs_set_id(g_world,e,ecs_id(Energy),sizeof(Energy),&(Energy){1});
    }

    /* all systems multi_threaded so ecs_worker_iter slicing engages */
    ecs_entity_t Move = ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="Move"}),
        .query.terms={{ecs_id(Pos),.inout=EcsOut},{ecs_id(Vel),.inout=EcsIn}},
        .callback=Sys_Move,.multi_threaded=true});
    ecs_entity_t Decay = ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="Decay"}),
        .query.terms={{ecs_id(Health),.inout=EcsInOut}},
        .callback=Sys_Decay,.multi_threaded=true});
    ecs_entity_t Spin = ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="Spin"}),
        .query.terms={{ecs_id(Energy),.inout=EcsInOut}},
        .callback=Sys_Spin,.multi_threaded=true});
    ecs_entity_t Move2 = ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="Move2"}),
        .query.terms={{ecs_id(Pos),.inout=EcsOut}},
        .callback=Sys_Move2,.multi_threaded=true});

    if (mode==0) { push_system(Move,W); push_system(Decay,W); push_system(Spin,W); }
    else         { push_system(Move,W); push_system(Move2,W); }   /* Pos write/write */

    run_pool();

    /* verify (mode 0): every entity Moved (Pos.x changed + Touched), Decay/Spin ran */
    int touched=0, bad=0;
    ecs_query_t *q = ecs_query(g_world,{.terms={{ecs_id(Pos)},{ecs_id(Health)},{ecs_id(Energy)}}});
    ecs_iter_t it = ecs_query_iter(g_world,q);
    while (ecs_query_next(&it)){
        Pos *p=ecs_field(&it,Pos,0); Health *h=ecs_field(&it,Health,1); Energy *e=ecs_field(&it,Energy,2);
        for (int i=0;i<it.count;i++){
            if (ecs_has_id(g_world,it.entities[i],Touched)) touched++;
            if (mode==0){
                if (h[i].v >= 100.0f) bad++;   /* Decay should have reduced it */
                if (e[i].v <= 1.0f)   bad++;    /* Spin should have increased it */
                (void)p;
            }
        }
    }
    printf("per-thread tasks pulled:");
    for (int i=0;i<W;i++) printf(" %d", g_per_thread[i]); printf("  (total %d)\n", g_ntasks);
    if (mode==0){
        printf("verify: Touched=%d/%d bad=%d\n", touched, N, bad);
        int ok = (touched==N && bad==0);
        printf("RESULT: %s\n", ok?"PASS":"FAIL");
        ecs_fini(g_world); return ok?0:1;
    }
    printf("RESULT: ran conflict control (see TSan for race on Pos)\n");
    ecs_fini(g_world); return 0;
}
