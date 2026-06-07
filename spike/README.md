# Pipeline-parallelism spikes

Two spikes investigating whether flecs can support a DIY pipeline-parallel
construct. Both rely on the same invariant and confirm it with ThreadSanitizer
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

Conclusion: multi-system task parallelism is feasible on flecs today, given a
scheduler that (a) extracts per-system access from query terms, (b) only
co-schedules non-conflicting systems, (c) runs each on its own stage during a
single readonly window, and (d) merges serially. The term metadata needed for
(a) is already public via `ecs_system_get`.
