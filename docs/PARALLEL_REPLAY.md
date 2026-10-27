# Parallel replay (NX_PARALLEL_REPLAY)

Goal: the S20 FE's island at a steady 30 game frames a second (60 shown) in daylight, cooler, with room for 1080 rows.

## Why

Measured on the S20 FE (Snapdragon 865), island at noon, 900 rows, 2026-10-27:

- The replay (NVN command lists translated to Vulkan, `gpu_submit`) takes 31-34 ms a frame, on the CPU the whole
  time (`replay on the CPU`, nvn_vk.cpp), for ~4,900 draws: ~6.6 us a draw.
- It is bound by memory latency, not by instructions: ~46% of cycles stalled on the backend; PGO + ThinLTO (code
  layout), a small ring for `NX_VK_DEFER` (Vulkan calls on another thread), and a memo in front of the uniform
  uploads (83% hits) each changed nothing. Each draw touches ~50 cold cache lines (objects, views, sets, guest
  memory), at DRAM latency.
- Work that waits on memory scales with cores: several threads keep several misses in flight.

## Where the draws are (one frame, island, noon)

Passes in order (render target, draws):

| | draws |
|---|---|
| 386x386 + depth (one pass, split in 300-draw parts) | 1,451 |
| ~22 small passes | ~22 |
| shadow map, depth only 1536x1536 | 1,034 |
| main 2000x900 + depth, first run | 1,004 |
| small 1000x450 passes | ~84 |
| main 2000x900 + depth, second run | 1,109 |
| post chain, UI (~40 small passes) | ~190 |

94% of the draws are in four runs of ~1,000-1,450 draws. Between them, and inside the main runs, a few commands
have effects in order: `UpdateUniformBuffer` (writes guest memory; ~60 a frame, 10 in the main target),
`ReportCounter` (timestamps into guest memory), `DispatchCompute` (3 in the main target: written ranges, write-backs),
copies.

## Design

1. **Flatten.** The frame's lists (with the lists they call) become one vector of `{const Cmd*, const CmdList*}`.
2. **Segments.** The replay thread (A) walks the vector. A *segment* is a run of commands with one set of render
   targets and no ordered command inside (dispatch, copy, `UpdateUniformBuffer` that overlaps what the batch reads).
   Small runs stay on A.
3. **State snapshot.** At a segment's start A copies the NVN state (`g_state`) and the targets, hands the range to a
   worker, and walks the range applying only state commands (cheap) to reach the state after it. `ReportCounter`
   runs on A during that walk.
4. **Workers** (2-3, on the big cores) replay their range into their own primary command buffer with thread-local
   draw state: `g_state`, the pass (`g_rendering`, `g_pass`...), bound state, descriptor scratch and set cache,
   their own descriptor pool and dedup, a chunk of the upload ring. Shared caches (images, views, samplers,
   pipelines, layouts) take a lock on their slow paths.
5. **Order.** The command buffers are queued in the order of their ranges; A's own parts in between. Images stay in
   `GENERAL` (already the case), and every pass ends with a full memory barrier (already the case), so a worker's
   commands do not depend on another's layout tracking.
6. **Ordered effects.** A dispatch, a copy, or an `UpdateUniformBuffer` that overlaps a range bound earlier in the
   batch: A waits for the batch's workers, then runs it in place. Texture uploads found by workers go into a
   command buffer queued before the batch.

## Steps

- P0: flatten, segment, log what would run where (no change in behaviour). Measure segments a frame.
- P1: workers, run one at a time (A waits for each): thread safety without concurrency.
- P2: concurrency; measure; then default on for Android when the island, menus, interiors and a 30-minute walk are
  clean.

`NX_PARALLEL_REPLAY=0` keeps the single-thread replay.

## State (2026-10-27)

Working (`NX_PARALLEL_REPLAY=1`, off by default): correct picture on the island, no bails in steady play (the
replay thread makes the targets' images during its walk), ~4,700 of ~4,900 draws a frame in parallel. Not yet faster:
~30 ms against ~33 ms for the single-thread replay, because chunks cost much more in total (45-63 ms of CPU against
~28 ms): each thread keeps its own descriptor-set cache, upload copies (6.4 -> 9 MB a frame) and handle slots, so the
work they share on one thread is redone per thread, and per-draw cost rose ~50% even on the replay thread.

Per-thread state is `PerThread<T>` (nvn_per_thread.h): a slot per replaying thread chosen by the thread pointer
register. `thread_local` in the app's library cost the single-thread replay 15% (TLSDESC calls), and Android refuses
the initial-exec model there.

Next: caches shared across the threads (sets, copies) behind per-thread fronts; then measure again.
