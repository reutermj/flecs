/*
 * Option E spike: the soundness precondition of term-based conflict analysis.
 *
 * The conflict analysis only sees a system's *declared* query terms. But a
 * callback can touch component data the terms don't mention:
 *   - ecs_get(world, e, T)      -> in-place READ of frozen storage (NOT deferred)
 *   - ecs_field(it, T, i)       -> in-place read/write (always declared: needs a term)
 *   - ecs_set/add/remove        -> DEFERRED to the stage, applied at the serial merge
 *
 * So the hazard is an *undeclared in-place read* (ecs_get) of a component that a
 * co-scheduled system writes in place. The analysis can't see it -> false
 * negative -> real race. This spike demonstrates it and the fix (declare it).
 *
 * Modes:
 *   0  B reads Position via ecs_get (UNDECLARED), co-scheduled with A writing
 *      Position. Analysis says conflict=0 (false negative) -> TSan shows a race.
 *   1  B additionally declares [in Position]. Analysis now says conflict=1, so a
 *      real scheduler would NOT co-schedule them. (Shown via the matrix.)
 *   2  Control: B does ecs_set(Other) UNDECLARED (deferred) instead of ecs_get.
 *      Analysis says conflict=0, and it is genuinely safe (deferred) -> clean.
 *
 * Usage: ./option_e [0|1|2]
 */
#include "flecs.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef N
#define N 50000
#endif

typedef struct { float x; } Position;
typedef struct { float x; } Velocity;
typedef struct { float x; } Other;

static ecs_entity_t ecs_id(Position), ecs_id(Velocity), ecs_id(Other);
static int g_mode;

/* A: declares [out Position], writes Position in place */
static void Sys_WritePos(ecs_iter_t *it) {
    Position *p = ecs_field(it, Position, 0);
    for (int i=0;i<it->count;i++) p[i].x += 1.0f;
}

/* B: declares only [in Velocity]; depending on mode also touches Position/Other
 * via the direct API (NOT through ecs_field, i.e. undeclared). */
static void Sys_ReadVel(ecs_iter_t *it) {
    Velocity *v = ecs_field(it, Velocity, 0);
    volatile float sink = 0;
    for (int i=0;i<it->count;i++) {
        sink += v[i].x;
        ecs_entity_t e = it->entities[i];
        if (g_mode == 2) {
            /* undeclared DEFERRED write -> goes to the stage, safe */
            ecs_set_id(it->world, e, ecs_id(Other), sizeof(Other), &(Other){sink});
        } else {
            /* undeclared IN-PLACE read of Position -> races with Sys_WritePos */
            const Position *p = ecs_get_id(it->world, e, ecs_id(Position));
            if (p) sink += p->x;
        }
    }
    (void)sink;
}

/* ---- minimal conflict analysis (declared terms only) ---- */
typedef struct { ecs_id_t r[8], w[8]; int nr, nw; } acc_t;
static void access_of(ecs_world_t *world, ecs_entity_t sys, acc_t *o) {
    const ecs_query_t *q = ecs_system_get(world, sys)->query;
    o->nr=o->nw=0;
    for (int t=0;t<q->term_count;t++){
        int16_t io=q->terms[t].inout; ecs_id_t id=q->terms[t].id;
        if (io==EcsInOutNone || io==EcsInOutFilter) continue;
        if (io==EcsInOutDefault) io=EcsInOut;
        if (io==EcsIn||io==EcsInOut) o->r[o->nr++]=id;
        if (io==EcsOut||io==EcsInOut) o->w[o->nw++]=id;
    }
}
static int has(const ecs_id_t*a,int n,ecs_id_t id){for(int i=0;i<n;i++)if(a[i]==id)return 1;return 0;}
static int conflict(const acc_t*x,const acc_t*y){
    for(int i=0;i<x->nw;i++) if(has(y->r,y->nr,x->w[i])||has(y->w,y->nw,x->w[i])) return 1;
    for(int i=0;i<y->nw;i++) if(has(x->r,x->nr,y->w[i])) return 1;
    return 0;
}

/* ---- co-schedule two systems in one wave ---- */
static ecs_world_t *g_world;
typedef struct { ecs_entity_t sys; int stage; } job_t;
static void *run_job(void *a){ job_t*j=a; ecs_run(ecs_get_stage(g_world,j->stage), j->sys, 0, NULL); return NULL; }
static void run_wave(ecs_entity_t a, ecs_entity_t b){
    ecs_set_stage_count(g_world,2);
    ecs_readonly_begin(g_world,true);
    pthread_t t1,t2; job_t j1={a,0}, j2={b,1};
    pthread_create(&t1,NULL,run_job,&j1); pthread_create(&t2,NULL,run_job,&j2);
    pthread_join(t1,NULL); pthread_join(t2,NULL);
    ecs_readonly_end(g_world);
}

int main(int argc,char**argv){
    g_mode=(argc>1)?atoi(argv[1]):0;
    printf("mode=%d (%s)\n", g_mode,
        g_mode==0?"undeclared ecs_get read (hazard)":
        g_mode==1?"declared [in Position] (fixed)":"undeclared ecs_set (deferred, safe)");
    g_world=ecs_init();
    #define C(T) ecs_id(T)=ecs_component(g_world,{.type.size=sizeof(T),.type.alignment=ECS_ALIGNOF(T)})
    C(Position);C(Velocity);C(Other);
    for(int i=0;i<N;i++){ecs_entity_t e=ecs_new(g_world);
        ecs_set_id(g_world,e,ecs_id(Position),sizeof(Position),&(Position){0});
        ecs_set_id(g_world,e,ecs_id(Velocity),sizeof(Velocity),&(Velocity){1});}

    ecs_entity_t A=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="WritePos"}),
        .query.terms={{ecs_id(Position),.inout=EcsOut}},.callback=Sys_WritePos,.multi_threaded=true});

    /* B declares [in Position] only in mode 1 (the fix) */
    ecs_system_desc_t bd = {.entity=ecs_entity(g_world,{.name="ReadVel"}),
        .callback=Sys_ReadVel,.multi_threaded=true};
    bd.query.terms[0] = (ecs_term_t){ecs_id(Velocity),.inout=EcsIn};
    if (g_mode==1) bd.query.terms[1] = (ecs_term_t){ecs_id(Position),.inout=EcsIn};
    ecs_entity_t B=ecs_system_init(g_world,&bd);

    acc_t ax,bx; access_of(g_world,A,&ax); access_of(g_world,B,&bx);
    printf("declared access: A(r=%d,w=%d) B(r=%d,w=%d) -> conflict=%d\n",
        ax.nr,ax.nw,bx.nr,bx.nw,conflict(&ax,&bx));

    if (g_mode==1){
        printf("analysis flags the conflict -> scheduler would NOT co-schedule. "
               "(not running them together)\n");
        ecs_fini(g_world); return 0;
    }

    printf("analysis says safe -> co-scheduling. %s\n",
        g_mode==0?"Expect a REAL race (TSan) despite conflict=0.":
                  "Deferred write is genuinely safe -> expect clean.");
    run_wave(A,B);
    printf("RESULT: ran\n");
    ecs_fini(g_world); return 0;
}
