/*
 * Demo + verification for the hybrid scheduler.
 *
 * System graph (registered in this order):
 *   Gravity : [in Mass][inout Vel]      vel += mass
 *   Move    : [in Vel][out Pos]         pos += vel
 *   AI      : [in Pos][out AIState]     ai  = pos
 *   Render  : [in Pos][out RenderData]  rd  = 2*pos
 *   Decay   : [inout Health]            health -= 1
 *   Spin    : [inout Energy]            energy += 1
 *
 * Dependencies force: Gravity -> Move -> {AI, Render} (data hazards on Vel/Pos),
 * while Decay/Spin are independent. The scheduler should discover this.
 *
 * All ops are integer-valued so results are exact and verifiable analytically.
 *
 * Usage: ./scheduler_demo [threads] [frames]
 */
#include "scheduler.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef N
#define N 100000
#endif

typedef struct { float v; } Mass;
typedef struct { float v; } Vel;
typedef struct { float v; } Pos;
typedef struct { float v; } AIState;
typedef struct { float v; } RenderData;
typedef struct { float v; } Health;
typedef struct { float v; } Energy;
typedef struct { float v; } Sound;

static ecs_entity_t ecs_id(Mass), ecs_id(Vel), ecs_id(Pos), ecs_id(AIState),
                    ecs_id(RenderData), ecs_id(Health), ecs_id(Energy), ecs_id(Sound);

static void Gravity(ecs_iter_t *it){ Mass *m=ecs_field(it,Mass,0); Vel *v=ecs_field(it,Vel,1);
    for(int i=0;i<it->count;i++) v[i].v += m[i].v; }
static void Move(ecs_iter_t *it){ Vel *v=ecs_field(it,Vel,0); Pos *p=ecs_field(it,Pos,1);
    for(int i=0;i<it->count;i++) p[i].v += v[i].v; }
static void AI(ecs_iter_t *it){ Pos *p=ecs_field(it,Pos,0); AIState *a=ecs_field(it,AIState,1);
    for(int i=0;i<it->count;i++) a[i].v = p[i].v; }
static void Render(ecs_iter_t *it){ Pos *p=ecs_field(it,Pos,0); RenderData *r=ecs_field(it,RenderData,1);
    for(int i=0;i<it->count;i++) r[i].v = 2.0f*p[i].v; }
static void Decay(ecs_iter_t *it){ Health *h=ecs_field(it,Health,0);
    for(int i=0;i<it->count;i++) h[i].v -= 1.0f; }
static void Spin(ecs_iter_t *it){ Energy *e=ecs_field(it,Energy,0);
    for(int i=0;i<it->count;i++) e[i].v += 1.0f; }
/* Independent of every other system (own component). Only a phase can order it
 * after the OnUpdate systems -- there is no data conflict to do so. */
static void Audio(ecs_iter_t *it){ Sound *s=ecs_field(it,Sound,0);
    for(int i=0;i<it->count;i++) s[i].v += 1.0f; }

int main(int argc, char **argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 8;
    int frames  = (argc > 2) ? atoi(argv[2]) : 10;

    ecs_world_t *world = ecs_init();
    #define C(T) ecs_id(T)=ecs_component(world,{.type.size=sizeof(T),.type.alignment=ECS_ALIGNOF(T)})
    C(Mass);C(Vel);C(Pos);C(AIState);C(RenderData);C(Health);C(Energy);C(Sound);

    for (int i = 0; i < N; i++) {
        ecs_entity_t e = ecs_new(world);
        ecs_set_id(world,e,ecs_id(Mass),sizeof(Mass),&(Mass){2});
        ecs_set_id(world,e,ecs_id(Vel),sizeof(Vel),&(Vel){0});
        ecs_set_id(world,e,ecs_id(Pos),sizeof(Pos),&(Pos){0});
        ecs_set_id(world,e,ecs_id(AIState),sizeof(AIState),&(AIState){0});
        ecs_set_id(world,e,ecs_id(RenderData),sizeof(RenderData),&(RenderData){0});
        ecs_set_id(world,e,ecs_id(Health),sizeof(Health),&(Health){1000});
        ecs_set_id(world,e,ecs_id(Energy),sizeof(Energy),&(Energy){0});
        ecs_set_id(world,e,ecs_id(Sound),sizeof(Sound),&(Sound){0});
    }

    /* Phases: Gravity/Move/Decay/Spin in OnUpdate, AI/Render/Audio in PostUpdate.
     * Ordering now comes from these phases + a fine-grained DependsOn, NOT from
     * registration order. */
    #define ONUPD  .add = ecs_ids(ecs_dependson(EcsOnUpdate))
    #define POSTUPD .add = ecs_ids(ecs_dependson(EcsPostUpdate))
    ecs_entity_t g = ecs_system(world, {.entity=ecs_entity(world,{.name="Gravity", ONUPD}), .multi_threaded=true,
        .query.terms={{ecs_id(Mass),.inout=EcsIn},{ecs_id(Vel),.inout=EcsInOut}}, .callback=Gravity});
    ecs_entity_t m = ecs_system(world, {.entity=ecs_entity(world,{.name="Move", ONUPD}), .multi_threaded=true,
        .query.terms={{ecs_id(Vel),.inout=EcsIn},{ecs_id(Pos),.inout=EcsOut}}, .callback=Move});
    ecs_entity_t a = ecs_system(world, {.entity=ecs_entity(world,{.name="AI", POSTUPD}), .multi_threaded=true,
        .query.terms={{ecs_id(Pos),.inout=EcsIn},{ecs_id(AIState),.inout=EcsOut}}, .callback=AI});
    ecs_entity_t r = ecs_system(world, {.entity=ecs_entity(world,{.name="Render", POSTUPD}), .multi_threaded=true,
        .query.terms={{ecs_id(Pos),.inout=EcsIn},{ecs_id(RenderData),.inout=EcsOut}}, .callback=Render});
    ecs_entity_t d = ecs_system(world, {.entity=ecs_entity(world,{.name="Decay", ONUPD}), .multi_threaded=true,
        .query.terms={{ecs_id(Health),.inout=EcsInOut}}, .callback=Decay});
    ecs_entity_t s = ecs_system(world, {.entity=ecs_entity(world,{.name="Spin", ONUPD}), .multi_threaded=true,
        .query.terms={{ecs_id(Energy),.inout=EcsInOut}}, .callback=Spin});
    ecs_entity_t au = ecs_system(world, {.entity=ecs_entity(world,{.name="Audio", POSTUPD}), .multi_threaded=true,
        .query.terms={{ecs_id(Sound),.inout=EcsInOut}}, .callback=Audio});

    /* fine-grained edge: Move runs after Gravity (also implied by the Vel
     * conflict, but declared here to exercise system->system DependsOn) */
    ecs_add_pair(world, m, EcsDependsOn, g);

    ecs_scheduler_t *sched = scheduler_new(world, threads);
    /* registration order deliberately scrambled to prove it no longer matters */
    scheduler_add(sched, au); scheduler_add(sched, r); scheduler_add(sched, s);
    scheduler_add(sched, m); scheduler_add(sched, d); scheduler_add(sched, a);
    scheduler_add(sched, g);
    scheduler_build(sched);
    scheduler_print(sched);

    for (int f = 0; f < frames; f++) scheduler_run(sched, 1.0f);

    /* analytic ground truth after F frames (per-frame: vel+=2, pos+=vel):
     *   vel = 2F ; pos = F(F+1) ; ai = pos ; rd = 2*pos ;
     *   health = 1000 - F ; energy = F                               */
    float F = (float)frames;
    float exp_vel = 2*F, exp_pos = F*(F+1), exp_ai = exp_pos, exp_rd = 2*exp_pos,
          exp_health = 1000 - F, exp_energy = F, exp_sound = F;

    int bad = 0;
    ecs_query_t *q = ecs_query(world,{.terms={
        {ecs_id(Vel)},{ecs_id(Pos)},{ecs_id(AIState)},{ecs_id(RenderData)},
        {ecs_id(Health)},{ecs_id(Energy)},{ecs_id(Sound)}}});
    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        Vel *v=ecs_field(&it,Vel,0); Pos *p=ecs_field(&it,Pos,1); AIState *ai=ecs_field(&it,AIState,2);
        RenderData *rd=ecs_field(&it,RenderData,3); Health *h=ecs_field(&it,Health,4);
        Energy *en=ecs_field(&it,Energy,5); Sound *so=ecs_field(&it,Sound,6);
        for (int i=0;i<it.count;i++){
            if (v[i].v!=exp_vel||p[i].v!=exp_pos||ai[i].v!=exp_ai||rd[i].v!=exp_rd||
                h[i].v!=exp_health||en[i].v!=exp_energy||so[i].v!=exp_sound) bad++;
        }
    }
    printf("\nframes=%d N=%d  expected: vel=%.0f pos=%.0f ai=%.0f rd=%.0f health=%.0f energy=%.0f sound=%.0f\n",
        frames, N, exp_vel, exp_pos, exp_ai, exp_rd, exp_health, exp_energy, exp_sound);
    printf("mismatched entities: %d\n", bad);
    printf("RESULT: %s\n", bad==0 ? "PASS" : "FAIL");

    scheduler_free(sched);
    ecs_fini(world);
    return bad==0 ? 0 : 1;
}
