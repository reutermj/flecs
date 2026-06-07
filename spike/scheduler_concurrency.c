/*
 * Measures how much data vs. system parallelism the scheduler actually achieves.
 *
 * Each system callback records, while it runs:
 *   - max_active[i]  : peak number of concurrent stripes of system i  (DATA parallelism)
 *   - max_distinct   : peak number of DISTINCT systems running at once (SYSTEM parallelism)
 *
 * Same graph/phases as scheduler_demo, but generic busy-work callbacks (we care
 * about timing, not results). K per system is driven by N (smaller N -> smaller
 * K -> more room for systems to run side by side).
 *
 * Usage: ./scheduler_concurrency [threads] [frames]   (compile-time -DN=...)
 */
#include "scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef N
#define N 100000
#endif
#ifndef BUSY
#define BUSY 60          /* inner-loop iterations per entity, to lengthen tasks */
#endif
#define NSYS 7

typedef struct { float v; } Mass,Vel,Pos,AIState,RenderData,Health,Energy,Sound;
static ecs_entity_t ecs_id(Mass),ecs_id(Vel),ecs_id(Pos),ecs_id(AIState),
                    ecs_id(RenderData),ecs_id(Health),ecs_id(Energy),ecs_id(Sound);

static volatile int active[NSYS];
static volatile int max_active[NSYS];
static volatile int max_distinct;

static void update_max(volatile int *slot, int v){
    int cur = __atomic_load_n(slot, __ATOMIC_SEQ_CST);
    while (v > cur && !__atomic_compare_exchange_n(slot, &cur, v, 0,
        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
}
static void enter(int i){
    int a = __atomic_add_fetch(&active[i], 1, __ATOMIC_SEQ_CST);
    update_max(&max_active[i], a);
    int distinct = 0;
    for (int j = 0; j < NSYS; j++) if (__atomic_load_n(&active[j], __ATOMIC_SEQ_CST) > 0) distinct++;
    update_max(&max_distinct, distinct);
}
static void leave(int i){ __atomic_sub_fetch(&active[i], 1, __ATOMIC_SEQ_CST); }

/* generic instrumented system: busy-work proportional to matched rows */
static void run_sys(ecs_iter_t *it){
    int idx = (int)(intptr_t)it->ctx;
    enter(idx);
    volatile double x = 0;
    for (int i = 0; i < it->count; i++)
        for (int b = 0; b < BUSY; b++) x += 1.0;
    (void)x;
    leave(idx);
}

static const char *names[NSYS] = {"Gravity","Move","AI","Render","Decay","Spin","Audio"};

int main(int argc, char **argv){
    int threads = (argc>1)?atoi(argv[1]):8;
    int frames  = (argc>2)?atoi(argv[2]):20;

    ecs_world_t *world = ecs_init();
    #define C(T) ecs_id(T)=ecs_component(world,{.type.size=sizeof(T),.type.alignment=ECS_ALIGNOF(T)})
    C(Mass);C(Vel);C(Pos);C(AIState);C(RenderData);C(Health);C(Energy);C(Sound);
    for (int i=0;i<N;i++){ ecs_entity_t e=ecs_new(world);
        ecs_set_id(world,e,ecs_id(Mass),sizeof(Mass),&(Mass){2});
        ecs_set_id(world,e,ecs_id(Vel),sizeof(Vel),&(Vel){0});
        ecs_set_id(world,e,ecs_id(Pos),sizeof(Pos),&(Pos){0});
        ecs_set_id(world,e,ecs_id(AIState),sizeof(AIState),&(AIState){0});
        ecs_set_id(world,e,ecs_id(RenderData),sizeof(RenderData),&(RenderData){0});
        ecs_set_id(world,e,ecs_id(Health),sizeof(Health),&(Health){0});
        ecs_set_id(world,e,ecs_id(Energy),sizeof(Energy),&(Energy){0});
        ecs_set_id(world,e,ecs_id(Sound),sizeof(Sound),&(Sound){0});
    }
    #define ONUPD  .add = ecs_ids(ecs_dependson(EcsOnUpdate))
    #define POSTUPD .add = ecs_ids(ecs_dependson(EcsPostUpdate))
    #define SYS(nm,phase,IDX,...) ecs_system(world,{.entity=ecs_entity(world,{.name=nm,phase}),\
        .multi_threaded=true,.ctx=(void*)(intptr_t)IDX,.callback=run_sys,.query.terms=__VA_ARGS__})
    ecs_entity_t g  = SYS("Gravity",ONUPD,0,  {{ecs_id(Mass),.inout=EcsIn},{ecs_id(Vel),.inout=EcsInOut}});
    ecs_entity_t m  = SYS("Move",ONUPD,1,     {{ecs_id(Vel),.inout=EcsIn},{ecs_id(Pos),.inout=EcsOut}});
    ecs_entity_t a  = SYS("AI",POSTUPD,2,     {{ecs_id(Pos),.inout=EcsIn},{ecs_id(AIState),.inout=EcsOut}});
    ecs_entity_t r  = SYS("Render",POSTUPD,3, {{ecs_id(Pos),.inout=EcsIn},{ecs_id(RenderData),.inout=EcsOut}});
    ecs_entity_t d  = SYS("Decay",ONUPD,4,    {{ecs_id(Health),.inout=EcsInOut}});
    ecs_entity_t s  = SYS("Spin",ONUPD,5,     {{ecs_id(Energy),.inout=EcsInOut}});
    ecs_entity_t au = SYS("Audio",POSTUPD,6,  {{ecs_id(Sound),.inout=EcsInOut}});
    ecs_add_pair(world, m, EcsDependsOn, g);

    ecs_scheduler_t *sched = scheduler_new(world, threads);
    ecs_entity_t all[NSYS]={g,m,a,r,d,s,au};
    for (int i=0;i<NSYS;i++) scheduler_add(sched, all[i]);
    scheduler_build(sched);
    scheduler_print(sched);

    for (int f=0; f<frames; f++) scheduler_run(sched, 1.0f);

    printf("\nthreads=%d N=%d BUSY=%d frames=%d\n", threads, N, BUSY, frames);
    printf("DATA parallelism  (peak concurrent stripes per system):\n");
    for (int i=0;i<NSYS;i++) printf("    %-8s K-achieved=%d\n", names[i], max_active[i]);
    printf("SYSTEM parallelism (peak distinct systems running at once): %d\n", max_distinct);
    printf("=> data parallelism: %s ; system parallelism: %s\n",
        ({int any=0; for(int i=0;i<NSYS;i++) if(max_active[i]>1) any=1; any;}) ? "YES" : "no",
        max_distinct > 1 ? "YES" : "no");

    scheduler_free(sched);
    ecs_fini(world);
    return 0;
}
