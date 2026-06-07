# Pipeline-parallelism spikes

Five spikes investigating whether flecs can support a DIY pipeline-parallel
construct. They rely on the same invariant and confirm it with ThreadSanitizer
(decisive signal) plus a negative control (proves the harness has teeth).

The invariant: freeze the world once (`ecs_readonly_begin`), run concurrent
work that only reads frozen storage or writes **disjoint** memory, defer all
structural changes to per-thread stages, then a single serial merge
(`ecs_readonly_end`). There are no flecs-provided locks on storage; safety is
the caller's responsibility.

| Spike | Question | Result |
|-------|----------|--------|
| `option_a.c` | Can two phases run concurrently on one world? | Yes — disjoint writes, no real races |
| `option_b.c` | Can *different systems* be scheduled concurrently (task parallelism)? | Yes — gated by query-term conflict analysis |
| `option_c.c` | Can data + system parallelism nest in one thread pool? | Yes — TSan-clean; concurrent same-query iteration is safe (except `order_by`) |
| `option_d.c` | Is the `order_by` hazard ours or flecs'? | flecs' — it asserts `ECS_UNSUPPORTED`; the constraint is pre-existing, not introduced by our scheduler |
| `option_e.c` | Is the term-based conflict analysis sound? | Only if systems declare all in-place access — undeclared `ecs_get` races; deferred `ecs_set` is safe |

---

# Option A

Investigates whether two phases can run **concurrently** on the same world,
on different OS threads, with no flecs-provided locking, given the invariant:

- storage frozen once (`ecs_readonly_begin`),
- disjoint write sets across the concurrent phases,
- structural changes deferred to per-thread stages,
- a single serial merge (`ecs_readonly_end`).

`option_a.c` runs two phases on two threads sharing one frozen world:

- **mode 0 (disjoint):** thread A writes `Position`, thread B writes `Velocity`.
- **mode 1 (conflict, negative control):** both threads write `Position`.

## Build & run

```sh
cp ../distr/flecs.c ../distr/flecs.h .

# debug build (assertions / violation detectors on)
gcc -g -O1 -I. option_a.c flecs.c -o option_a -lpthread -lm
./option_a 0   # disjoint  -> PASS
./option_a 1   # conflict  -> functionally "passes" (assertions don't catch value races)

# ThreadSanitizer (decisive signal)
gcc -g -O1 -fsanitize=thread -DN=20000 -I. option_a.c flecs.c -o option_a_tsan -lpthread -lm
TSAN_OPTIONS="halt_on_error=0" ./option_a_tsan 0   # only benign *_count globals
TSAN_OPTIONS="halt_on_error=0" ./option_a_tsan 1   # + 1 REAL race on the Position heap block
```

## Findings

- **Disjoint phases: no real data races.** All TSan reports are on global diagnostic
  counters (`ecs_os_api_malloc_count`, `ecs_block_allocator_alloc_count`, ...) bumped
  via a non-atomic `ecs_os_linc`. They only feed the stats addon; racing them produces
  fuzzy metrics, never a logic/memory-safety problem.
- **Conflict mode adds a genuine race** on the actual `Position` component storage
  (`heap block of size 262144`), proving the harness is sensitive.
- Reads from frozen storage are lock-free; per-stage command buffers are truly
  thread-local; the serial merge applies both buffers correctly.

To get a 100% clean TSan run, install a custom OS API with atomic increments
(`ecs_os_set_api` with atomic `ainc_`/`lainc_`).

---

# Option B: multi-system scheduling with conflict analysis

`option_b.c` builds a tiny DIY scheduler that runs **different systems**
concurrently (true task parallelism), where safety comes from read/write
conflict analysis derived from each system's query terms.

What it demonstrates:

1. **Access extraction** (`system_access`): walk `ecs_system_get(w,s)->query->terms`
   and resolve each term's `inout` to a read/write set, mirroring flecs'
   own `flecs_pipeline_check_term` (owned `EcsInOutDefault` => InOut;
   `EcsIn` => read; `EcsOut`/`EcsInOut` => write).
2. **Conflict matrix** (`conflicts`): two systems conflict iff one writes a
   component the other reads or writes (read/read is fine).
3. **Wave scheduler** (`run_wave`): freeze once, run each system on its own
   stage/thread via `ecs_run(stage, sys, ...)`, merge once.

Systems:

- `ReadA_WriteB` `[in A][out B]`, `ReadC_WriteD` `[in C][out D]`,
  `ReadA_WriteE` `[in A][out E]` — pairwise conflict-free (share read of A).
- `WriteB_conflict` `[out B]` — conflicts with `ReadA_WriteB` on B.

```sh
gcc -g -O1 -I. option_b.c flecs.c -o option_b -lpthread -lm
./option_b 0   # runs the conflict-free wave {S1,S2,S3} in parallel -> PASS

gcc -g -O1 -fsanitize=thread -DN=20000 -I. option_b.c flecs.c -o option_b_tsan -lpthread -lm
TSAN_OPTIONS="halt_on_error=0" ./option_b_tsan 0   # disjoint wave: only benign *_count globals
TSAN_OPTIONS="halt_on_error=0" ./option_b_tsan 1   # co-schedules S1+SX: REAL race on B storage
```

## Findings

- The computed conflict matrix flags exactly one pair (S1 x SX on B); the
  three-system wave is conflict-free.
- **Disjoint wave: no real data races** under TSan (all reports are the same
  benign global metric counters as Option A). Three distinct systems ran
  concurrently and correctly; all deferred structural adds merged.
- **Negative control:** co-scheduling the flagged conflicting pair produces a
  genuine race on the `B` component storage
  (`SUMMARY: data race in Sys_WriteB_conflict`) — proving the conflict
  analysis is precisely what provides safety.

### Edge cases covered (expanded)

`option_b.c` now exercises the term forms a naive analysis gets wrong, with
`system_access()` mirroring flecs' `flecs_pipeline_check_term`:

| Term form | Resolves to | Verified by |
|-----------|-------------|-------------|
| owned `$this` default | InOut (read+write) | base case |
| shared / `up`-traversed default | In (read only) | `UpReadConfig_WriteF` |
| `EcsIn` / `EcsOut` / `EcsInOut` | read / write / both | all systems |
| `EcsInOutNone` (`[none]`) | no access (no conflict) | `NoneB` vs B writer |
| `oper==Not && EcsOut` | write (component add) | `AddMarker` |
| pair `(Rel,Tgt)` | id matched via `ecs_id_match` | `WritePairApples`/`ReadPairOranges` |
| wildcard pair `(Rel,*)` | overlaps any `(Rel,X)` | `WritePairWild` |
| bare `EcsWildcard` write | write barrier (conflicts all) | `write_all` flag |

The spike self-checks its conflict matrix against the expected pairs and runs
four modes:

```sh
gcc -g -O1 -I. option_b.c flecs.c -o option_b -lpthread -lm
./option_b 0   # conflict-free wave of 7 distinct systems -> PASS + "matrix matches expected"

gcc -g -O1 -fsanitize=thread -DN=20000 -I. option_b.c flecs.c -o option_b_tsan -lpthread -lm
TSAN_OPTIONS="halt_on_error=0" ./option_b_tsan 0   # wave: only benign counters, no Sys_ races
TSAN_OPTIONS="halt_on_error=0" ./option_b_tsan 1   # plain B write/write           -> race in Sys_WriteB2
TSAN_OPTIONS="halt_on_error=0" ./option_b_tsan 2   # wildcard pair vs concrete     -> race in Sys_WritePairWild
TSAN_OPTIONS="halt_on_error=0" ./option_b_tsan 3   # shared up-read vs Config write -> race in Sys_WriteConfig
```

Per-mode TSan classification (by exact `#0` frame):

| Mode | benign counter races | real component races (`Sys_*`) |
|------|----------------------|--------------------------------|
| 0 conflict-free wave | 101 | **0** |
| 1 plain write/write   | 2 | 1 (`Sys_WriteB2`, B storage) |
| 2 wildcard overlap    | 1 | 1 (`Sys_WritePairWild`, Likes pair) |
| 3 shared-read vs write| 1 | 1 (`Sys_WriteConfig`, Config) |

> Classification note: TSan does not print struct field names, so a "heap
> block" race is not automatically a component-storage race. Classify by the
> `#0` frame: every non-`Sys_*` race here is a non-atomic `ecs_os_linc` into a
> diagnostic counter — the allocator counts (`ecs_os_api_*_count`,
> `ecs_block_allocator_alloc_count`) **and** `world->info.queries_ran_total`
> (flecs.c:84713), bumped at the end of every query iteration. A single
> atomic-OS-API fix (`ecs_os_set_api` with atomic `lainc_`) silences all of
> them.

Conclusion: multi-system task parallelism is feasible on flecs today, given a
scheduler that (a) extracts per-system access from query terms — including
pairs, wildcards, shared/up terms and `Not`+`Out` adds, (b) only co-schedules
non-conflicting systems, (c) runs each on its own stage during a single
readonly window, and (d) merges serially. The term metadata needed for (a) is
already public via `ecs_system_get`, and `flecs_pipeline_check_term` is the
reference for resolving it.

---

# Option C: data + system parallelism in one pool

`option_c.c` validates the hybrid model at runtime: **W threads, one stage each
(1:1)**, draining a single shared atomic work queue of `(system, k, K)` stripes
drawn from several `multi_threaded`, conflict-free systems. A thread runs each
task via `ecs_run_worker(my_stage, sys, k, K)` — the stage it defers to (its
thread id) is **decoupled** from the slice index `k`, which is what lets any
thread pick up any system's stripe while keeping a private, race-free buffer.

This is the first spike to stress **concurrent iteration of the same query** (a
system's own data-parallel workers), i.e. the change-detection path.

```sh
gcc -g -O1 -I. option_c.c flecs.c -o option_c -lpthread -lm
./option_c 0   # hybrid wave {Move, Decay, Spin} -> PASS

gcc -g -O1 -fsanitize=thread -DN=40000 -I. option_c.c flecs.c -o option_c_tsan -lpthread -lm
TSAN_OPTIONS="halt_on_error=0" ./option_c_tsan 0   # only benign counters + idempotent change-detection write
TSAN_OPTIONS="halt_on_error=0" ./option_c_tsan 1   # Move + Move2 both write Pos -> race in Sys_Move
```

## Findings

- **Data + system parallelism nest cleanly.** Three systems, each split into W
  stripes, all stripes in one queue, drained by W threads on thread-owned stages:
  TSan-clean (no component-storage races), correct results, one merge.
- **Concurrent same-query iteration is safe** for normal cached queries. The only
  shared writes are `q->eval_count` and `world->info.queries_ran_total` (benign
  non-atomic stat counters) and `cache->prev_match_count = cache->match_count` —
  a benign idempotent race, because `match_count` is constant during the frozen
  readonly window. No shared cursor, no match-list mutation, no data corruption.
- **Caveat:** `order_by` queries sort the cache at iter-init
  (`flecs_query_cache_sort_tables`) — a real shared mutation, **unsafe** under
  concurrent iteration. Exclude sorted queries from data-parallel scheduling (or
  sort once before the wave).
- **Negative control** (`Move` + `Move2` both writing Pos) races in `Sys_Move` as
  the conflict analysis predicts.

---

# Option D: is the order_by hazard flecs' own?

`option_d.c` answers whether the `order_by` concurrency hazard found in Option C
is specific to our DIY pool or inherent to flecs. It runs a `multi_threaded`
`order_by` system in **flecs' own** native pipeline (`ecs_set_threads` +
`ecs_progress`), mutating the sort key each frame to force re-sorting.

```sh
gcc -g -O1 -I. option_d.c flecs.c -o option_d -lpthread -lm
./option_d 4 40        # debug build -> ABORTS:
                       # "cannot sort query in multithreaded mode" (ECS_UNSUPPORTED)

# guard compiled out (NDEBUG) under TSan -> flecs' own sort path races
gcc -g -O1 -DNDEBUG -fsanitize=thread -DN=8000 -I. option_d.c flecs.c -o option_d_ndebug_tsan -lpthread -lm
TSAN_OPTIONS="halt_on_error=0" ./option_d_ndebug_tsan 4 20
```

## Findings

- flecs **explicitly guards** `order_by` + multithreaded: an assert
  `ECS_UNSUPPORTED` ("cannot sort query in multithreaded mode") in
  `flecs_query_cache_build_sorted_table_range` (flecs.c) fires the moment a sort
  is needed during multithreaded iteration. flecs' native pipeline aborts on it.
- With the guard removed (`NDEBUG`), flecs' own pipeline races in its sort path
  (`build_sorted_table_range`, `sort_tables`) and on the component data itself —
  proving the assert is the only thing preventing the race.
- **Conclusion:** the `order_by` restriction is a pre-existing flecs constraint,
  identical for flecs' built-in multithreading and for our scheduler. We inherit
  it; we do not introduce it.

---

# Option E: soundness precondition of the conflict analysis

`option_e.c` probes the assumption the whole conflict-analysis model rests on:
the analysis only sees a system's **declared** query terms, but a callback can
touch other components via the direct API.

- `ecs_get(world, e, T)` — in-place read of frozen storage, **not** deferred.
- `ecs_set/add/remove`   — deferred to the stage, applied at the serial merge.

So an *undeclared* `ecs_get` is invisible to the analysis and can race; an
undeclared `ecs_set` is safe (deferred).

```sh
gcc -g -O1 -I. option_e.c flecs.c -o option_e -lpthread -lm
./option_e 0   # B reads Position via ecs_get (undeclared) -> conflict=0 (false negative)
./option_e 1   # B declares [in Position] -> conflict=1 (analysis prevents co-scheduling)
./option_e 2   # B does ecs_set(Other) undeclared (deferred) -> conflict=0 and genuinely safe

gcc -g -O1 -fsanitize=thread -DN=20000 -I. option_e.c flecs.c -o option_e_tsan -lpthread -lm
TSAN_OPTIONS="halt_on_error=0" ./option_e_tsan 0   # REAL race in Sys_ReadVel (ecs_get vs ecs_field write)
TSAN_OPTIONS="halt_on_error=0" ./option_e_tsan 2   # clean (deferred write never races)
```

## Findings

- **The analysis is only sound if systems declare every in-place access.** Mode 0
  is judged `conflict=0` yet TSan reports a real race (`Sys_ReadVel`, the
  undeclared `ecs_get(Position)` vs the in-place write). Declaring `[in Position]`
  (mode 1) makes the analysis flag the conflict.
- **Deferred ops are exempt.** Mode 2's undeclared `ecs_set` is TSan-clean — it is
  buffered to the stage and never races with in-place access.
- **Precondition:** a system must declare (in its terms) every component it reads
  via `ecs_get` or accesses via `ecs_field`. This is the same contract flecs' own
  pipeline assumes; it is inherited, not introduced.
