/*
 * Option B spike: DIY multi-system scheduler with conflict analysis.
 *
 * Goal: run *different* flecs systems concurrently on different OS threads
 * (true task parallelism), where safety comes from read/write conflict
 * analysis derived from each system's query terms.
 *
 * Pieces demonstrated:
 *   1. Extract each system's read/write component sets from its query terms,
 *      using the same resolution rules as flecs' own pipeline builder
 *      (owned default term => InOut; EcsIn => read; EcsOut/EcsInOut => write).
 *   2. Build a conflict matrix: two systems conflict iff one writes a
 *      component the other reads or writes (read/read is fine).
 *   3. Run a conflict-free wave concurrently: freeze world once, run each
 *      system on its own stage/thread via ecs_run(), merge once.
 *
 * Modes:
 *   0 (disjoint wave):  S1[inA,outB] S2[inC,outD] S3[inA,outE] in parallel -> clean
 *   1 (conflict ctrl):  S1[inA,outB] and SX[outB] in parallel -> real race on B
 *
 * Usage: ./option_b [0|1]
 */
#include "flecs.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef N
#define N 100000
#endif

typedef struct { float v; } A;
typedef struct { float v; } B;
typedef struct { float v; } C;
typedef struct { float v; } D;
typedef struct { float v; } E;

static ecs_entity_t ecs_id(A), ecs_id(B), ecs_id(C), ecs_id(D), ecs_id(E);
static ecs_entity_t Tag1, Tag2, Tag3;

/* ---- system callbacks: read inputs, write outputs, defer a structural op ---- */
static void Sys_ReadA_WriteB(ecs_iter_t *it) {
    A *a = ecs_field(it, A, 0); B *b = ecs_field(it, B, 1);
    for (int i = 0; i < it->count; i++) { b[i].v = a[i].v + 1.0f;
        ecs_add_id(it->world, it->entities[i], Tag1); }
}
static void Sys_ReadC_WriteD(ecs_iter_t *it) {
    C *c = ecs_field(it, C, 0); D *d = ecs_field(it, D, 1);
    for (int i = 0; i < it->count; i++) { d[i].v = c[i].v + 1.0f;
        ecs_add_id(it->world, it->entities[i], Tag2); }
}
static void Sys_ReadA_WriteE(ecs_iter_t *it) {
    A *a = ecs_field(it, A, 0); E *e = ecs_field(it, E, 1);
    for (int i = 0; i < it->count; i++) { e[i].v = a[i].v + 2.0f;
        ecs_add_id(it->world, it->entities[i], Tag3); }
}
static void Sys_WriteB_conflict(ecs_iter_t *it) {
    B *b = ecs_field(it, B, 0);
    for (int i = 0; i < it->count; i++) b[i].v = 99.0f;
}

/* ---- conflict analysis ------------------------------------------------- */
#define MAXC 16
typedef struct { ecs_id_t reads[MAXC], writes[MAXC]; int nr, nw; const char *name; } access_t;

/* Resolve a system's read/write sets from its query terms, mirroring
 * flecs_pipeline_check_term's resolution of EcsInOutDefault. */
static void system_access(ecs_world_t *w, ecs_entity_t sys, access_t *out) {
    const ecs_system_t *sd = ecs_system_get(w, sys);
    const ecs_query_t *q = sd->query;
    out->nr = out->nw = 0;
    out->name = ecs_get_name(w, sys);
    for (int t = 0; t < q->term_count; t++) {
        const ecs_term_t *term = &q->terms[t];
        int16_t inout = term->inout;
        if (inout == EcsInOutNone || inout == EcsInOutFilter) continue;
        if (inout == EcsInOutDefault) inout = EcsInOut; /* owned default */
        if (inout == EcsIn)                         out->reads[out->nr++]  = term->id;
        else if (inout == EcsInOut || inout == EcsOut) out->writes[out->nw++] = term->id;
        if (inout == EcsInOut) out->reads[out->nr++] = term->id; /* InOut also reads */
    }
}
static int has(const ecs_id_t *arr, int n, ecs_id_t id) {
    for (int i = 0; i < n; i++) if (arr[i] == id) return 1; return 0;
}
/* conflict iff one's write set intersects the other's read or write set */
static int conflicts(const access_t *x, const access_t *y) {
    for (int i = 0; i < x->nw; i++)
        if (has(y->reads, y->nr, x->writes[i]) || has(y->writes, y->nw, x->writes[i])) return 1;
    for (int i = 0; i < y->nw; i++)
        if (has(x->reads, x->nr, y->writes[i])) return 1;
    return 0;
}

/* ---- scheduler: run a wave of systems concurrently -------------------- */
static ecs_world_t *g_world;
typedef struct { ecs_entity_t sys; int stage; } job_t;
static void *run_job(void *arg) {
    job_t *j = (job_t*)arg;
    ecs_world_t *stage = ecs_get_stage(g_world, j->stage);
    ecs_run(stage, j->sys, 0, NULL);
    return NULL;
}
static void run_wave(ecs_entity_t *systems, int n) {
    ecs_set_stage_count(g_world, n);
    ecs_readonly_begin(g_world, true);
    pthread_t th[MAXC]; job_t jobs[MAXC];
    for (int i = 0; i < n; i++) { jobs[i].sys = systems[i]; jobs[i].stage = i;
        pthread_create(&th[i], NULL, run_job, &jobs[i]); }
    for (int i = 0; i < n; i++) pthread_join(th[i], NULL);
    ecs_readonly_end(g_world); /* serial merge of all stages */
}

int main(int argc, char **argv) {
    int mode = (argc > 1) ? atoi(argv[1]) : 0;
    printf("mode=%d (%s)\n", mode, mode ? "conflict-control" : "disjoint-wave");

    g_world = ecs_init();
    ecs_id(A)=ecs_component(g_world,{.type.size=sizeof(A),.type.alignment=ECS_ALIGNOF(A)});
    ecs_id(B)=ecs_component(g_world,{.type.size=sizeof(B),.type.alignment=ECS_ALIGNOF(B)});
    ecs_id(C)=ecs_component(g_world,{.type.size=sizeof(C),.type.alignment=ECS_ALIGNOF(C)});
    ecs_id(D)=ecs_component(g_world,{.type.size=sizeof(D),.type.alignment=ECS_ALIGNOF(D)});
    ecs_id(E)=ecs_component(g_world,{.type.size=sizeof(E),.type.alignment=ECS_ALIGNOF(E)});
    Tag1=ecs_entity(g_world,{0}); Tag2=ecs_entity(g_world,{0}); Tag3=ecs_entity(g_world,{0});

    for (int i = 0; i < N; i++) {
        ecs_entity_t e = ecs_new(g_world);
        ecs_set_id(g_world,e,ecs_id(A),sizeof(A),&(A){(float)i});
        ecs_set_id(g_world,e,ecs_id(B),sizeof(B),&(B){0});
        ecs_set_id(g_world,e,ecs_id(C),sizeof(C),&(C){(float)i});
        ecs_set_id(g_world,e,ecs_id(D),sizeof(D),&(D){0});
        ecs_set_id(g_world,e,ecs_id(E),sizeof(E),&(E){0});
    }

    /* define systems with explicit term access */
    ecs_entity_t s1 = ecs_system(g_world, { .entity = ecs_entity(g_world,{.name="ReadA_WriteB"}),
        .query.terms = {{ecs_id(A),.inout=EcsIn},{ecs_id(B),.inout=EcsOut}}, .callback=Sys_ReadA_WriteB });
    ecs_entity_t s2 = ecs_system(g_world, { .entity = ecs_entity(g_world,{.name="ReadC_WriteD"}),
        .query.terms = {{ecs_id(C),.inout=EcsIn},{ecs_id(D),.inout=EcsOut}}, .callback=Sys_ReadC_WriteD });
    ecs_entity_t s3 = ecs_system(g_world, { .entity = ecs_entity(g_world,{.name="ReadA_WriteE"}),
        .query.terms = {{ecs_id(A),.inout=EcsIn},{ecs_id(E),.inout=EcsOut}}, .callback=Sys_ReadA_WriteE });
    ecs_entity_t sx = ecs_system(g_world, { .entity = ecs_entity(g_world,{.name="WriteB_conflict"}),
        .query.terms = {{ecs_id(B),.inout=EcsOut}}, .callback=Sys_WriteB_conflict });

    /* 1. extract access sets */
    ecs_entity_t all[] = {s1,s2,s3,sx};
    access_t acc[4];
    printf("\n-- extracted access sets --\n");
    for (int i=0;i<4;i++){ system_access(g_world, all[i], &acc[i]);
        printf("  %-16s reads=%d writes=%d\n", acc[i].name, acc[i].nr, acc[i].nw); }

    /* 2. conflict matrix */
    printf("\n-- conflict matrix (1=cannot co-schedule) --\n      ");
    for (int j=0;j<4;j++) printf("%-3d", j); printf("\n");
    for (int i=0;i<4;i++){ printf("  %-3d ", i);
        for (int j=0;j<4;j++) printf("%-3d", i==j?0:conflicts(&acc[i],&acc[j])); printf("\n"); }

    /* 3. run a wave */
    if (mode == 0) {
        ecs_entity_t wave[] = {s1,s2,s3};
        /* assert the wave is actually conflict-free before scheduling it */
        for (int i=0;i<3;i++) for (int j=i+1;j<3;j++) {
            access_t ai,aj; system_access(g_world,wave[i],&ai); system_access(g_world,wave[j],&aj);
            if (conflicts(&ai,&aj)) { printf("REFUSED: wave has a conflict\n"); return 2; } }
        run_wave(wave, 3);

        /* verify */
        int bad=0,t1=0,t2=0,t3=0;
        ecs_query_t *qa = ecs_query(g_world,{.terms={{ecs_id(A)},{ecs_id(B)},{ecs_id(D)},{ecs_id(E)},{ecs_id(C)}}});
        ecs_iter_t it = ecs_query_iter(g_world, qa);
        while (ecs_query_next(&it)) {
            A *a=ecs_field(&it,A,0); B *b=ecs_field(&it,B,1); D *d=ecs_field(&it,D,2);
            E *e=ecs_field(&it,E,3); C *c=ecs_field(&it,C,4);
            for (int i=0;i<it.count;i++){
                if (b[i].v!=a[i].v+1.0f) bad++;
                if (d[i].v!=c[i].v+1.0f) bad++;
                if (e[i].v!=a[i].v+2.0f) bad++;
                if (ecs_has_id(g_world,it.entities[i],Tag1)) t1++;
                if (ecs_has_id(g_world,it.entities[i],Tag2)) t2++;
                if (ecs_has_id(g_world,it.entities[i],Tag3)) t3++;
            }
        }
        printf("\nverify: bad=%d Tag1=%d Tag2=%d Tag3=%d (expected 0, %d,%d,%d)\n",bad,t1,t2,t3,N,N,N);
        int ok = (bad==0 && t1==N && t2==N && t3==N);
        printf("RESULT: %s\n", ok?"PASS":"FAIL");
        ecs_fini(g_world); return ok?0:1;
    } else {
        /* negative control: deliberately co-schedule two conflicting systems */
        access_t ai,aj; system_access(g_world,s1,&ai); system_access(g_world,sx,&aj);
        printf("\nco-scheduling S1 and SX despite conflicts()=%d (negative control)\n",
            conflicts(&ai,&aj));
        ecs_entity_t wave[] = {s1, sx};
        run_wave(wave, 2);
        printf("RESULT: ran (see TSan for race on B)\n");
        ecs_fini(g_world); return 0;
    }
}
