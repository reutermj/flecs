# Pipeline-parallelism spike: Option A

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
