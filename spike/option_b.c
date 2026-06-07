/*
 * Option B spike (expanded): DIY multi-system scheduler + conflict analysis,
 * now covering the term edge cases that a naive analysis would get wrong.
 *
 * Access extraction faithfully mirrors flecs_pipeline_check_term:
 *   - owned $this default term            => InOut (read+write)
 *   - shared / up-traversed default term  => In   (read only)
 *   - EcsIn => read; EcsOut/EcsInOut => write; EcsInOutNone/Filter => no access
 *   - oper == Not && EcsOut               => intent to ADD a component (write)
 *   - bare EcsWildcard write              => write barrier (conflicts w/ everything)
 *   - pair / wildcard-pair ids            => overlap via ecs_id_match (both ways)
 *
 * Conflict(X,Y): one writes an id the other reads or writes (read/read is fine),
 * with wildcard/pair overlap handled by ecs_id_match.
 *
 * Modes:
 *   0  conflict-free wave of 7 distinct systems (owned, pair, up-read, Not+Out) -> clean
 *   1  plain write/write on B                 -> real race
 *   2  wildcard pair write vs concrete pair   -> real race (wildcard detection)
 *   3  shared up-read of Config vs Config write -> real race (shared-read path)
 *
 * Usage: ./option_b [0|1|2|3]
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
typedef struct { float v; } E;
typedef struct { float v; } F;
typedef struct { float v; } Config;
typedef struct { float v; } Likes; /* relationship carrying data */

static ecs_entity_t ecs_id(A), ecs_id(B), ecs_id(C), ecs_id(E), ecs_id(F),
                    ecs_id(Config), ecs_id(Likes);
static ecs_entity_t Apples, Oranges, Marker, Tag1, Tag2, Parent;

/* ---- system callbacks -------------------------------------------------- */
static void Sys_ReadA_WriteB(ecs_iter_t *it) {
    A *a = ecs_field(it, A, 0); B *b = ecs_field(it, B, 1);
    for (int i=0;i<it->count;i++){ b[i].v=a[i].v+1.0f; ecs_add_id(it->world,it->entities[i],Tag1); }
}
static void Sys_ReadA_WriteE(ecs_iter_t *it) {
    A *a = ecs_field(it, A, 0); E *e = ecs_field(it, E, 1);
    for (int i=0;i<it->count;i++){ e[i].v=a[i].v+2.0f; ecs_add_id(it->world,it->entities[i],Tag2); }
}
static void Sys_WriteB2(ecs_iter_t *it) { /* conflicts with Sys_ReadA_WriteB on B */
    B *b = ecs_field(it, B, 0);
    for (int i=0;i<it->count;i++) b[i].v=99.0f;
}
static void Sys_WritePairApples(ecs_iter_t *it) {
    Likes *l = ecs_field(it, Likes, 0);
    for (int i=0;i<it->count;i++) l[i].v=7.0f;
}
static void Sys_ReadPairOranges(ecs_iter_t *it) {
    Likes *l = ecs_field(it, Likes, 0); volatile float s=0;
    for (int i=0;i<it->count;i++) s+=l[i].v; (void)s;
}
static void Sys_WritePairWild(ecs_iter_t *it) { /* (Likes,*) overlaps Apples & Oranges */
    Likes *l = ecs_field(it, Likes, 0);
    for (int i=0;i<it->count;i++) l[i].v=8.0f;
}
static void Sys_UpReadConfig_WriteF(ecs_iter_t *it) {
    Config *c = ecs_field(it, Config, 0); /* shared: single value from Parent */
    F *f = ecs_field(it, F, 1);
    for (int i=0;i<it->count;i++) f[i].v=c->v+1.0f;
}
static void Sys_WriteConfig(ecs_iter_t *it) {
    Config *c = ecs_field(it, Config, 0);
    for (int i=0;i<it->count;i++) c[i].v=42.0f;
}
static void Sys_AddMarker(ecs_iter_t *it) { /* Not+Out: deferred structural add */
    for (int i=0;i<it->count;i++) ecs_add_id(it->world, it->entities[i], Marker);
}
static void Sys_NoneB(ecs_iter_t *it) { /* matches B via [none] but must not conflict */
    A *a = ecs_field(it, A, 0); volatile float s=0;
    for (int i=0;i<it->count;i++) s+=a[i].v; (void)s;
}

/* ---- conflict analysis (mirrors flecs_pipeline_check_term) -------------- */
#define MAXC 16
typedef struct {
    ecs_id_t reads[MAXC], writes[MAXC]; int nr, nw;
    int write_all;            /* bare wildcard write => conflicts with everything */
    const char *name;
} access_t;

static void add_id(ecs_id_t *arr, int *n, ecs_id_t id) {
    for (int i=0;i<*n;i++) if (arr[i]==id) return; arr[(*n)++]=id;
}

static void system_access(ecs_world_t *w, ecs_entity_t sys, access_t *o) {
    const ecs_query_t *q = ecs_system_get(w, sys)->query;
    o->nr=o->nw=0; o->write_all=0; o->name=ecs_get_name(w, sys);
    for (int t=0;t<q->term_count;t++){
        const ecs_term_t *term=&q->terms[t];
        int16_t inout=term->inout, oper=term->oper;
        if (inout==EcsInOutFilter || inout==EcsInOutNone) continue;
        int from_any  = ecs_term_match_0((ecs_term_t*)term);
        int from_this = ecs_term_match_this((ecs_term_t*)term);
        int is_shared = !from_any && (!from_this || !(term->src.id & EcsSelf));
        if (inout==EcsInOutDefault){
            if (from_any) continue;          /* id passed to system, not r/w */
            inout = is_shared ? EcsIn : EcsInOut;
        }
        int writes = (inout==EcsOut || inout==EcsInOut) ||
                     (oper==EcsNot && inout==EcsOut); /* Not+Out = add component */
        int reads  = (inout==EcsIn || inout==EcsInOut);
        if (writes){
            if (term->id==EcsWildcard) o->write_all=1;
            else add_id(o->writes,&o->nw,term->id);
        }
        if (reads) add_id(o->reads,&o->nr,term->id);
    }
}

static int overlap(ecs_id_t a, ecs_id_t b){
    if (a==EcsWildcard || b==EcsWildcard || a==EcsAny || b==EcsAny) return 1;
    return ecs_id_match(a,b) || ecs_id_match(b,a);
}
static int hits(const ecs_id_t *arr,int n,ecs_id_t id){
    for (int i=0;i<n;i++) if (overlap(arr[i],id)) return 1; return 0;
}
static int conflicts(const access_t *x,const access_t *y){
    if (x->write_all || y->write_all) return (x->nr||x->nw||y->nr||y->nw);
    for (int i=0;i<x->nw;i++) if (hits(y->reads,y->nr,x->writes[i])||hits(y->writes,y->nw,x->writes[i])) return 1;
    for (int i=0;i<y->nw;i++) if (hits(x->reads,x->nr,y->writes[i])) return 1;
    return 0;
}

/* ---- scheduler --------------------------------------------------------- */
static ecs_world_t *g_world;
typedef struct { ecs_entity_t sys; int stage; } job_t;
static void *run_job(void *arg){ job_t *j=arg;
    ecs_run(ecs_get_stage(g_world,j->stage), j->sys, 0, NULL); return NULL; }
static void run_wave(ecs_entity_t *sys,int n){
    ecs_set_stage_count(g_world,n);
    ecs_readonly_begin(g_world,true);
    pthread_t th[MAXC]; job_t jb[MAXC];
    for (int i=0;i<n;i++){ jb[i].sys=sys[i]; jb[i].stage=i; pthread_create(&th[i],NULL,run_job,&jb[i]); }
    for (int i=0;i<n;i++) pthread_join(th[i],NULL);
    ecs_readonly_end(g_world);
}

int main(int argc,char**argv){
    int mode=(argc>1)?atoi(argv[1]):0;
    g_world=ecs_init();
    #define COMP(T) ecs_id(T)=ecs_component(g_world,{.type.size=sizeof(T),.type.alignment=ECS_ALIGNOF(T)})
    COMP(A);COMP(B);COMP(C);COMP(E);COMP(F);COMP(Config);COMP(Likes);
    Apples=ecs_entity(g_world,{.name="Apples"}); Oranges=ecs_entity(g_world,{.name="Oranges"});
    Marker=ecs_entity(g_world,{.name="Marker"}); Tag1=ecs_entity(g_world,{0}); Tag2=ecs_entity(g_world,{0});

    Parent=ecs_new(g_world); ecs_set_id(g_world,Parent,ecs_id(Config),sizeof(Config),&(Config){1.0f});
    for (int i=0;i<N;i++){
        ecs_entity_t e=ecs_new(g_world);
        ecs_set_id(g_world,e,ecs_id(A),sizeof(A),&(A){(float)i});
        ecs_set_id(g_world,e,ecs_id(B),sizeof(B),&(B){0});
        ecs_set_id(g_world,e,ecs_id(C),sizeof(C),&(C){0});
        ecs_set_id(g_world,e,ecs_id(E),sizeof(E),&(E){0});
        ecs_set_id(g_world,e,ecs_id(F),sizeof(F),&(F){0});
        ecs_set_id(g_world,e,ecs_pair(ecs_id(Likes),Apples),sizeof(Likes),&(Likes){0});
        ecs_set_id(g_world,e,ecs_pair(ecs_id(Likes),Oranges),sizeof(Likes),&(Likes){0});
        ecs_add_pair(g_world,e,EcsChildOf,Parent);
    }

    /* systems ----------------------------------------------------------- */
    ecs_entity_t S1=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="ReadA_WriteB"}),
        .query.terms={{ecs_id(A),.inout=EcsIn},{ecs_id(B),.inout=EcsOut}},.callback=Sys_ReadA_WriteB});
    ecs_entity_t S2=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="ReadA_WriteE"}),
        .query.terms={{ecs_id(A),.inout=EcsIn},{ecs_id(E),.inout=EcsOut}},.callback=Sys_ReadA_WriteE});
    ecs_entity_t SB2=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="WriteB2"}),
        .query.terms={{ecs_id(B),.inout=EcsOut}},.callback=Sys_WriteB2});
    ecs_entity_t SpW=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="WritePairApples"}),
        .query.terms={{ecs_pair(ecs_id(Likes),Apples),.inout=EcsOut}},.callback=Sys_WritePairApples});
    ecs_entity_t SpR=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="ReadPairOranges"}),
        .query.terms={{ecs_pair(ecs_id(Likes),Oranges),.inout=EcsIn}},.callback=Sys_ReadPairOranges});
    ecs_entity_t SwW=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="WritePairWild"}),
        .query.terms={{ecs_pair(ecs_id(Likes),EcsWildcard),.inout=EcsOut}},.callback=Sys_WritePairWild});
    ecs_entity_t SuR=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="UpReadConfig_WriteF"}),
        .query.terms={{ecs_id(Config),.inout=EcsIn,.src.id=EcsUp,.trav=EcsChildOf},
                      {ecs_id(F),.inout=EcsOut}},.callback=Sys_UpReadConfig_WriteF});
    ecs_entity_t ScW=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="WriteConfig"}),
        .query.terms={{ecs_id(Config),.inout=EcsOut}},.callback=Sys_WriteConfig});
    ecs_entity_t Sam=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="AddMarker"}),
        .query.terms={{ecs_id(A),.inout=EcsIn},{Marker,.inout=EcsOut,.oper=EcsNot}},.callback=Sys_AddMarker});
    ecs_entity_t Srm=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="ReadMarker"}),
        .query.terms={{Marker,.inout=EcsIn}},.callback=Sys_NoneB});
    ecs_entity_t Snb=ecs_system(g_world,{.entity=ecs_entity(g_world,{.name="NoneB"}),
        .query.terms={{ecs_id(A),.inout=EcsIn},{ecs_id(B),.inout=EcsInOutNone}},.callback=Sys_NoneB});

    ecs_entity_t all[]={S1,S2,SB2,SpW,SpR,SwW,SuR,ScW,Sam,Srm,Snb};
    const int NS=sizeof(all)/sizeof(all[0]);
    access_t acc[16];
    printf("-- extracted access sets --\n");
    for (int i=0;i<NS;i++){ system_access(g_world,all[i],&acc[i]);
        printf("  %2d %-20s reads=%d writes=%d%s\n",i,acc[i].name,acc[i].nr,acc[i].nw,
            acc[i].write_all?" [write_all]":""); }

    printf("\n-- conflict matrix --\n     ");
    for (int j=0;j<NS;j++) printf("%2d ",j); printf("\n");
    for (int i=0;i<NS;i++){ printf("  %2d ",i);
        for (int j=0;j<NS;j++) printf("%2d ", i==j?0:conflicts(&acc[i],&acc[j])); printf("\n"); }

    /* self-check matrix against expected conflicting pairs (by index) */
    int exp[][2]={{2,0},{3,5},{4,5},{6,7},{8,9}}; /* SB2-S1, SpW-SwW, SpR-SwW, SuR-ScW, Sam-Srm */
    int ne=sizeof(exp)/sizeof(exp[0]), mism=0;
    for (int i=0;i<NS;i++) for (int j=0;j<NS;j++){ if(i==j) continue;
        int e=0; for(int k=0;k<ne;k++) if((exp[k][0]==i&&exp[k][1]==j)||(exp[k][0]==j&&exp[k][1]==i)) e=1;
        if (conflicts(&acc[i],&acc[j])!=e){ printf("  MISMATCH (%d,%d): got %d want %d\n",
            i,j,conflicts(&acc[i],&acc[j]),e); mism++; } }
    printf("analysis self-check: %s\n", mism?"FAIL":"matrix matches expected");

    if (mode==0){
        ecs_entity_t wave[]={S1,S2,SpW,SpR,SuR,Sam,Snb}; int n=7;
        for (int i=0;i<n;i++) for (int j=i+1;j<n;j++){ access_t x,y;
            system_access(g_world,wave[i],&x); system_access(g_world,wave[j],&y);
            if (conflicts(&x,&y)){ printf("REFUSED: wave conflict %s/%s\n",x.name,y.name); return 2; } }
        run_wave(wave,n);
        int bad=0,t1=0,t2=0,mk=0;
        ecs_query_t *q=ecs_query(g_world,{.terms={{ecs_id(A)},{ecs_id(B)},{ecs_id(E)},{ecs_id(F)}}});
        ecs_iter_t it=ecs_query_iter(g_world,q);
        while (ecs_query_next(&it)){
            A*a=ecs_field(&it,A,0);B*b=ecs_field(&it,B,1);E*e=ecs_field(&it,E,2);F*f=ecs_field(&it,F,3);
            for (int i=0;i<it.count;i++){
                if (b[i].v!=a[i].v+1.0f) bad++;
                if (e[i].v!=a[i].v+2.0f) bad++;
                if (f[i].v!=2.0f) bad++;            /* Config(1)+1 */
                if (ecs_has_id(g_world,it.entities[i],Tag1)) t1++;
                if (ecs_has_id(g_world,it.entities[i],Tag2)) t2++;
                if (ecs_has_id(g_world,it.entities[i],Marker)) mk++;
            }
        }
        printf("\nverify: bad=%d Tag1=%d Tag2=%d Marker=%d (expect 0,%d,%d,%d)\n",bad,t1,t2,mk,N,N,N);
        int ok=(!bad && t1==N && t2==N && mk==N && !mism);
        printf("RESULT: %s\n", ok?"PASS":"FAIL");
        ecs_fini(g_world); return ok?0:1;
    }
    ecs_entity_t w1[]={S1,SB2}, w2[]={SpW,SwW}, w3[]={SuR,ScW};
    ecs_entity_t *w = mode==1?w1 : mode==2?w2 : w3;
    const char *desc = mode==1?"plain B write/write" : mode==2?"wildcard pair vs concrete" : "shared up-read vs Config write";
    printf("\nnegative control mode %d (%s): co-scheduling despite conflict\n", mode, desc);
    run_wave(w,2);
    printf("RESULT: ran (see TSan for the real race)\n");
    ecs_fini(g_world); return 0;
}
