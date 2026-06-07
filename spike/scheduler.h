#ifndef SCHEDULER_H
#define SCHEDULER_H

/*
 * A DIY hybrid parallel scheduler for flecs, built entirely on the public API.
 * Combines system/task parallelism (conflict-free systems run side by side)
 * with data parallelism (a system split across threads), per the findings in
 * hybrid-scheduler-design.md.
 *
 *   DependsOn/phase depth (from flecs)    -> stage ordering (who runs after whom)
 *   conflict analysis (from query terms)  -> which systems can share a wave
 *   adaptive width K                      -> how wide each system spreads
 *   thread pool over thread-owned stages  -> drains a shared (system,k,K) queue
 *   one serial merge between waves
 *
 * Ordering reuses flecs' existing relationships: assign a system to a phase
 * (DependsOn a phase entity) for broad "after all previous-stage systems"
 * ordering, and add DependsOn(B, A) for fine-grained edges. Registration order
 * does not matter.
 *
 * Soundness preconditions (see option_d / option_e):
 *   - systems must declare every component they access in place (ecs_field /
 *     ecs_get) in their query terms;
 *   - data-parallel (multi_threaded) systems must not use order_by.
 */

#include "flecs.h"

typedef struct ecs_scheduler_t ecs_scheduler_t;

/* Create a scheduler that runs on `threads` worker threads (>= 1). */
ecs_scheduler_t* scheduler_new(ecs_world_t *world, int threads);

/* Register a system (defines the set; order comes from phases/DependsOn, not
 * from the order of these calls). */
void scheduler_add(ecs_scheduler_t *s, ecs_entity_t system);

/* Compute the schedule: conflict analysis, waves, per-system width. */
void scheduler_build(ecs_scheduler_t *s);

/* Run one frame (all waves, with a serial merge between them). */
void scheduler_run(ecs_scheduler_t *s, ecs_ftime_t delta_time);

/* Print the computed schedule (waves, systems, widths). */
void scheduler_print(const ecs_scheduler_t *s);

void scheduler_free(ecs_scheduler_t *s);

#endif
