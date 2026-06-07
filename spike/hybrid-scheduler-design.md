# Hybrid Parallel Scheduler — Design (initial findings)

**Status:** exploratory design, backed by validated spikes (`option_a.c`, `option_b.c`)
**Scope:** a DIY scheduler on top of flecs that runs *different systems concurrently*
(system/task parallelism) **and** *splits individual systems across threads*
(data parallelism), in the same frame.

---

## 1. Problem

flecs' built-in pipeline parallelizes **within** a system only: for each system,
all worker threads process disjoint slices of that system's entities, in lockstep,
with a sync point between systems. It never runs two *different* systems on
different cores at the same time. So a frame full of small or unevenly-sized
systems leaves cores idle, and every system pays a barrier even when it could
have overlapped with its neighbours.

We want a scheduler that can do both kinds of parallelism at once:

- **System parallelism** — independent systems run side by side on different cores.
- **Data parallelism** — a single large system is split across multiple cores.
- **Hybrid** — both in the same frame, with threads shared across systems.

---

## 2. What the investigation established

These are the load-bearing facts the design rests on. All were verified against
the flecs source and/or by ThreadSanitizer spikes (see §9).

1. **flecs has no locks on world storage.** Concurrency safety is not lock-based.
   It comes from an *invariant*: freeze the world (readonly mode), let threads
   only read frozen storage or write to their own per-thread command buffer, then
   apply all buffers in a single serial merge. There is no mutex to remove or add;
   there is an invariant to uphold.

2. **Two phases can run concurrently on one world** (Option A, TSan-clean) as long
   as their writes are disjoint. Reads of frozen storage are lock-free; per-stage
   command buffers are genuinely thread-local; the serial merge applies them
   correctly.

3. **Different systems can be scheduled concurrently** (Option B, TSan-clean),
   with safety determined entirely by **read/write conflict analysis derived from
   each system's query terms** — the same information flecs' own pipeline builder
   uses (`flecs_pipeline_check_term`). Co-scheduling a conflicting pair produces a
   real race exactly where the analysis predicts.

4. **Data parallelism and system parallelism nest.** A system's entity-slicing
   uses *call-local* `(index, count)` parameters, and the deferral target is
   whichever stage you pass in — the two are decoupled. So a thread can run "slice
   k of K of system B" while writing to *its own* stage buffer.

---

## 3. Core concepts

### Stage = single-writer command buffer
A flecs *stage* is a thread-local buffer for deferred (structural) mutations plus
thread-local allocators. The one hard rule: **at most one thread touches a given
stage at a time.** This is what makes deferral lock-free. The scheduler binds one
stage to each pool thread, 1:1, for the lifetime of the pool.

### Readonly window + serial merge
A frame (or a wave) runs inside `ecs_readonly_begin` / `ecs_readonly_end`. While
open, storage is frozen: systems read real component data directly and defer all
structural changes (add/remove/delete/create) to their stage. `ecs_readonly_end`
merges every stage's buffer into the world on one thread. The merge is inherently
serial — it is the synchronization point, not a lock.

### Conflict analysis
For each system, resolve its query terms into a **read set** and **write set** of
component ids (mirroring `flecs_pipeline_check_term`):
- owned `$this` default term → read+write; shared/`up` term → read-only
- `In` → read; `Out`/`InOut` → write; `InOutNone`/`Filter` → no access
- `Not` + `Out` → a write (intent to add a component)
- pair / wildcard ids compared with `ecs_id_match` (a `(Rel,*)` write conflicts
  with any `(Rel,X)`)

Two systems **conflict** iff one writes a component the other reads or writes.
Read/read never conflicts. This is conservative at the component-id level (it can
flag systems that touch the same component on disjoint entities), matching flecs'
own conservatism.

### Wave
A **wave** is a set of systems that (a) are pairwise conflict-free and (b) respect
ordering dependencies. All systems in a wave may run concurrently. Between waves
there is one serial merge so that wave *k*'s writes are visible to wave *k+1*.

### Task
The unit of scheduling is a **`(system, k, K)`** task: "run slice `k` of `K` of
this system." A pool thread executes it on its own stage:
```
ecs_run_worker(ecs_get_stage(world, my_thread_id), system, k, K, dt, NULL);
//             └─ stage: which buffer writes defer to ─┘     └ which entities ┘
```
`K` is the system's data-parallel width; `K = 1` means "run the whole system on
one thread."

---

## 4. The hybrid model in one sentence

> **Conflict analysis decides which systems share a wave; per-system `K` decides
> how wide each system spreads across cores; a shared work queue over thread-owned
> stages balances the rest.**

- *System parallelism* = multiple systems' tasks coexisting in the wave's queue.
- *Data parallelism* = a single system contributing `K > 1` tasks.
- A wave with many systems supplies plenty of tasks without finely subdividing any
  one system, so threads stay busy and load-balance by pulling the next available
  task — which may belong to any system in the wave (they're all conflict-free, so
  any thread can run any task with no extra synchronization).

Adaptive `K` per system:
- tiny system → `K = 1` (run whole, in the slack around big systems)
- large system → `K = W` (spread across the pool)
- a single dominant system that would tail-block its wave → `K > W` (finer, opt-in)

---

## 5. Execution model (per frame)

```
build_schedule(pipeline):
    systems        = ordered list of active systems in the pipeline
    conflict_graph = edges between systems whose access sets conflict
    waves          = greedy list-schedule:
                       walk systems in order; add to the current wave if it
                       conflicts with nothing already in it AND its ordering
                       deps are satisfied; otherwise close the wave and start
                       a new one.

run_frame():
    for wave in waves:
        ecs_readonly_begin(world, multi_threaded=true)
        tasks = []
        for sys in wave:
            K = choose_width(sys)            # 1 for tiny, up to W for large
            for k in 0..K-1: tasks.push((sys, k, K))
        run_pool(tasks)                      # W threads drain the queue,
                                             # each on its own stage
        ecs_readonly_end(world)              # serial merge of all stages
```

`run_pool` is a standard work queue: W worker threads, each owning one stage,
each pulling the next task (atomic counter or lock-free deque) and running it via
`ecs_run_worker` on its stage until the queue is empty.

---

## 6. Building blocks

**Execution layer — 100% native flecs public API** (proven in the spikes):
- `ecs_set_stage_count(world, W)` — allocate W per-thread command buffers
- `ecs_readonly_begin` / `ecs_readonly_end` — freeze window + serial merge
- `ecs_get_stage(world, i)` — get a thread's stage
- `ecs_run_worker(stage, system, k, K, dt, param)` — run a system slice on a stage
- `ecs_system_get(world, sys)->query->terms` — term metadata for conflict analysis

**Scheduling layer — custom (this is the project):**
- conflict analysis (small; built on flecs term metadata)
- wave packing (greedy list-schedule over the conflict graph, order-preserving)
- per-system width selection (`K`)
- the work queue + thread pool coordination

The custom code is scheduling *policy* on top of flecs' execution substrate. It
does not reimplement iteration, staging, deferral, or merging.

---

## 7. Safety model / invariants

The scheduler is correct as long as it upholds the same invariant flecs relies on:

1. **One thread per stage at a time.** Pool threads own stages 1:1; a thread runs
   many tasks sequentially on its stage, never two threads on one stage.
2. **All structural mutations are deferred** to the running thread's stage (never
   direct world mutation off the main thread). Enforced by the readonly window.
3. **Concurrently-running systems are conflict-free** (the wave invariant). Within
   a single system, workers process disjoint entity slices, so their direct
   component-value writes never overlap.
4. **One serial merge per wave**, on one thread, after all tasks complete.

flecs' debug build provides violation detectors (`table->_->lock`, readonly
assertions) and `ecs_access_begin` access counters that fire on many illegal
patterns; ThreadSanitizer is the decisive check for value-write races.

---

## 8. Constraints & caveats

- **`multi_threaded` flag required for data parallelism.** If a system is not
  flagged `multi_threaded`, `ecs_worker_iter` slicing is skipped and every thread
  would run the full query → races. Non-`multi_threaded` systems can still be
  system-parallel at `K = 1` (one whole task), just not split.
- **Immediate (non-staged) systems** must run outside the readonly window (they
  mutate the world directly). They become their own single-threaded wave, like
  flecs' `immediate` ops.
- **The merge is serial and unavoidable.** Heavy structural churn concentrates
  cost in the merge; this bounds achievable speedup (Amdahl). Same as flecs today.
- **Conservative conflicts.** Component-id-level analysis can serialize systems
  that actually touch disjoint entities. Acceptable to start; could be refined
  later (e.g. archetype-level analysis) if it limits parallelism in practice.
- **Concurrent iteration of the *same* query** (a system's own data-parallel
  workers) — **verified TSan-clean** by `option_c.c`. The only shared writes on
  this path are two non-atomic stats counters (`q->eval_count`,
  `world->info.queries_ran_total`) and one change-detection snapshot
  (`cache->prev_match_count = cache->match_count`, cache.c iter-init). The latter
  is a benign idempotent race: during the readonly window `match_count` is
  constant (cache mutations happen at merge), so every worker writes the same
  stable value and nothing reads it until after the merge. No shared cursor or
  match-list mutation; no data corruption.
  - **Exception — `order_by` / sorted queries.** Their iter-init *sorts the cache*
    (`flecs_query_cache_sort_tables`), a real shared mutation that is **not** safe
    under concurrent iteration. Data-parallel systems must avoid `order_by` (or
    the sort must be performed once before the wave). `group_by` should be
    re-checked similarly before relying on it.
- **Table-advance overhead** of fine-grained chunking is understood but treated as
  an implementation detail; not a design driver. Default to `K ∈ {1..W}` and only
  revisit if profiling demands it.

---

## 9. Validation status

| Finding | Spike | Result |
|---|---|---|
| No storage locks; safety = readonly + defer + merge | source review | confirmed |
| Two phases concurrent, disjoint writes | `option_a.c` | TSan-clean; conflict control races as predicted |
| Multi-system scheduling via term conflict analysis | `option_b.c` | TSan-clean wave of 7 systems; 3 negative controls race exactly where the matrix predicts |
| Edge cases: pairs, wildcards, `up`/shared reads, `Not`+`Out`, `InOutNone` | `option_b.c` | access extraction + conflict matrix self-check pass |
| Data + system parallelism nest (stage ≠ slice) | source review (`system.c:75-103`) | confirmed |
| Hybrid pool: multiple `multi_threaded` systems split into stripes, drained from one shared queue over thread-owned stages | `option_c.c` | TSan-clean; concurrent same-query iteration safe (see §8); conflict control races on Pos as predicted |

---

## 10. Open questions / next steps

1. ~~**Hybrid spike (`option_c`).**~~ **Done** — `option_c.c`: W threads, one
   stage each, draining a shared queue of `(system, k, K)` stripes across three
   `multi_threaded` systems in one readonly window. TSan-clean; conflict control
   races as predicted. Confirms data + system parallelism nest at runtime.
2. ~~**Change-detection under concurrency.**~~ **Characterized** (§8): safe for
   normal cached queries (only benign stats counters + an idempotent
   `prev_match_count` write); **unsafe for `order_by`/sorted queries**, which must
   be excluded or sorted once before the wave.
3. **Wave packing algorithm.** Greedy list-schedule first; evaluate whether
   ordering constraints + conflict graph leave enough parallelism on real
   pipelines, and whether a smarter packing helps.
4. **Width selection policy.** How to choose `K` per system (static heuristic by
   matched-entity count vs. measured cost feedback).
5. **Benchmark vs. built-in pipeline.** Compare frame time against `ecs_progress`
   + `ecs_set_threads` on representative system sets (many-small-systems and
   few-large-systems workloads) to confirm the hybrid actually wins.
6. **Atomic metric counters.** Provide an OS API with atomic `lainc_` so the
   benign diagnostic-counter races (allocator counts, `queries_ran_total`) drop
   out of TSan runs, leaving only real findings.
