# Finishing Tier 1.1: the plan

Written 2026-09-26, after 0.63.0. Replaces the order in CLAUDE.md for Tier 1.1.

## Why this exists

Tier 1.1 was worked from the project brief and from refusals the test
harness hit, not from the spec's definition. That drifted: item a of the
brief (`GeometryIndex()`) took 20 versions of edge cases Escher never hits,
while one spec item was never tracked at all. From here, work follows the
spec's list only.

## Definition of done

The spec (`DirectX-Specs/d3d/Raytracing.md`, `D3D12_RAYTRACING_TIER_1_1`)
defines Tier 1.1 as seven additions. Tier 1.1 is finished when each one
works on the GTX 1070 and matches WARP in a test that has been shown able
to fail. Anything that cannot be done has to be shown impossible in DXR 1.0
terms, not assumed.

| # | Spec item | Status after 0.63.0 |
|---|---|---|
| 1 | `DispatchRays` through `ExecuteIndirect` | Done |
| 2 | `AddToStateObject` | Done |
| 3 | RayQuery in any shader stage | Compute only; steps 3 to 7 |
| 4 | `GeometryIndex()` | Done |
| 5 | `RAY_FLAG_SKIP_TRIANGLES`, `SKIP_PROCEDURAL_PRIMITIVES` | Native on the 1070 (Phase 4) |
| 6 | `D3D12_RAYTRACING_PIPELINE_CONFIG1` flags | Done, 0.64.0 (the 1070 rejects it; `proxy/pipe_flags`) |
| 7 | Extra vertex formats for builds | Done, 0.64.0 (measured native) |

## Rules against drift

- Work only on the next step below. Do not go back into a finished item.
- A new edge case found in a finished item goes on the limits list at the
  end, and waits.
- Each step opens with a probe: one cheap measurement that decides the
  design. Never a design first.
- Tests per step: the targeted case, and a poison showing it can fail. The
  full gitest matrix only when a step touches the variant; the three-form
  matrix only at close-out.
- Each report says which spec item moved, and what is left of it.

## Steps

### 1. Spec item 7: vertex formats (1 version) - DONE 0.64.0, native

Tier 1.1 adds `R16G16B16A16_UNORM`, `R16G16_UNORM`, `R10G10B10A2_UNORM`,
`R8G8B8A8_UNORM`, `R8G8_UNORM`, `R8G8B8A8_SNORM` and `R8G8_SNORM` as vertex
position formats for bottom-level builds.

- Probe: build one BLAS per format on the 1070 and on WARP, trace, compare.
- If the 1070 accepts them and matches: nothing to build; add the test.
- If not: at `BuildRaytracingAccelerationStructure` and the prebuild query,
  convert the positions on the GPU to `R32G32B32_FLOAT` (a compute pass, by
  address, honouring stride and index buffers), into a shim buffer kept
  alive for updates, and build from that. D3D's UNORM/SNORM conversion rules
  are exact in float32; WARP is the oracle.

### 2. Spec item 6: `PIPELINE_CONFIG1` skip flags (1 version) - DONE 0.64.0

Note for step 4: RayQuery lowered inside a DXR shader becomes a TraceRay,
which must NOT get the pipeline flags (they apply to TraceRay, not to
RayQuery): lower after `pipe_flags`, or leave the generated calls alone.

The spec: the pipeline flags behave like OR'ing the ray flags into every
`TraceRay`, except they do not show up in `RayFlags()`.

- Probe: does the 1070 carry an undefined ray flag bit through to
  `RayFlags()` in a hit shader? That decides how `RayFlags()` is corrected.
- Build: rewrite every `TraceRay` in the pipeline's libraries to OR the
  flag; where a library reads `RayFlags()`, correct what it sees (by the
  probe's answer). Strip `CONFIG1` to `CONFIG` for the driver.
- Test: a mixed scene with the pipeline flag set, and a shader reading
  `RayFlags()`, against WARP.

### 3. Spec item 3, compute gaps (3 to 4 versions)

Every refusal left in the rewriter for a compute shader, listed from the
code (`rq_analyze.cpp`, `rq_lower.cpp`) on 2026-09-26, not from memory:

- a. The CFG model follows `switch` edges (55 of 125 Unreal shaders have
  one). Refused as "Proceed calls but no loop" when the loop runs through
  one. DONE 0.65.0 (`rayquery_switch.hlsl`).
- b. A 6.6 resource array at a dynamic index, and FOUND WHILE DOING IT, a
  6.6 array at a CONSTANT index bound element 0, silently. DONE 0.65.0.
- c. Committed state read inside the Proceed loop (comparing a candidate's
  t with the committed one is a common alpha idiom). Track the committed
  hit in the payload from the any-hit; opaque commits need care. DONE
  0.66.0: traced forced non-opaque, the any-hit applying opacity from a
  third record word (`rayquery_committed*.hlsl`).
- d. A value defined in the loop and used after it: carry it OUT through
  the payload, as values already travel in. DONE 0.65.0, with loop-carried
  state (header phis) too (`rayquery_carryout.hlsl`).
- e. A `break` or `return` out of the loop body, and a loop with several
  exits. DONE 0.72.0: an exit ends the search as `Abort()` does and stores
  its id; the raygen branches on it after `TraceRay`. Top-tested loops (the
  `Proceed()` in the header) are recognised; a procedural body reports an
  exit as a hit of kind 127 - k. Found doing it, fixed: traversal began at
  `TraceRayInline`, before code between it and the first `Proceed()`. Left of
  it on the limits list: a candidate read after the loop where both ways out
  meet (refused by name), and loop state or carried values in a body that is
  also an intersection shader (refused, as before).
- f. One query traced twice (`TraceRayInline` again on the same object).
  DONE 0.73.0: a search per trace (reaching definitions), lowered as
  several queries are; traces a branch chooses between are one search,
  their ray merged by phis where it starts. Left of it on the limits list: a
  different acceleration structure on each arm, and such a search into a
  top-tested loop.
- g. `Proceed()` called without a loop, more than once. DONE 0.74.0: the
  calls are a virtual loop, the any-hit counting candidates and ending the
  search after the last call's body. Found doing it, fixed: a query never
  asked to `Proceed()` was traced as if it had searched (14450 rays). Left
  of it on the limits list: a call nested in the body of the one before, a
  write between two calls that a later body may read, a body with more than
  one way out, and a chain in a query serving procedural primitives.
- h. General writes in the loop body (only counter appends lower today),
  and appends together with `Abort()` or an intersection shader. RayQuery's
  candidate order is itself implementation-defined, so the question is the
  count, which `NO_DUPLICATE_ANYHIT_INVOCATION` already fixes. DONE 0.67.0
  for any-hit bodies, with `Abort()` made exact (it ends the search, or with
  committed state read, rejects every later candidate, opaque ones too).
  Found doing it: 124 and 128 byte payloads break the 1070's driver; the
  lowering pads both to 132. A write in a body that becomes an intersection
  shader: DONE 0.68.0 (the spec, read again: with the no-duplicate flag the
  proxy sets, no redundant intersection invocations). LEFT: `Abort()` in a
  procedural body, where traversal continues (legal where a commit uses the
  committed t only as "nearer than it", not in general:
  `rayquery_proc_abort.hlsl`), and a write beside it, refused by name.
  Found starting it, fixed in 0.69.0: a query was lowered as if the scene
  held only the kind of geometry it commits (procedural queries lost
  triangle candidates and opaque triangle commits, triangle queries lost box
  candidates). LEFT also: `Abort()` reached by a candidate of the kind a
  query does not commit (refused by name in a procedural query). Found next,
  fixed in 0.70.0: `CandidateProceduralPrimitiveNonOpaque()` was folded to
  true. Exact `Abort()` here: DONE 0.71.0 (tracked procedural: forced
  non-opaque trace, a procedural any-hit recording commits, the abort
  reported as a hit of kind 127 that ends the search). STEP 3 h DONE; left
  of it on the limits list: a procedural query with `Abort()` and ray flags
  computed at run time (refused by name).
- i. `Abort()` outside the loop. DONE 0.75.0: before the search it makes
  the search empty (instance mask 0 when the raygen's flag is set), after it
  changes nothing. Left of it on the limits list: an `Abort()` between two
  `Proceed()` calls without a loop.
- j. More than 16 carried values, and carried values of other types. DONE
  0.76.0: the cap is 2024 values (a payload of at most 8192 bytes, the
  largest the 1070 was measured right at); `half`, `i16`, `i64` and `double`
  travel in 32-bit fields. Found doing it, fixed: a top-tested loop whose
  state leaves it was not lowered (a GEP defined twice).
- k. Several queries where one needs an intersection shader: give each
  query its own hit group offset, so no query id has to travel in a payload.
  DONE 0.77.0: each query its own hit shaders, its TraceRay passing its
  query id as the ray contribution, drawn in the shim's own record layout.
  Found doing it, fixed: the own layout of a tracked procedural query never
  built, and own-layout records assumed one contribution per instance. Left
  of it on the limits list: more than 15 such queries, and such a shader
  whose scene is not resolved.
- l. A query used inside another query's loop (nested), and a dynamically
  indexed resource inside the loop. The resource: DONE 0.78.0 (the index
  rebuilt or carried like any value, the handle rebuilt from it; a handle
  made in the loop body turned into its library form, which at 6.6 had
  failed at the assembler). The nested query: DONE 0.79.0, the outer query
  STEPPED by a pre-pass: its loop kept in the raygen, each `Proceed()` a
  search by a fresh query for the nearest non-opaque candidate past the last
  (front to back, a legal RayQuery order), a closer opaque hit committed
  without the body. Covers a query retraced inside its own loop too. Found
  doing it, fixed: a loop was taken for a query's Proceed loop because its
  latch held a `Proceed()`, without its branch testing it. STEP 3 DONE; left
  of it on the limits list: a stepped body that acts for a procedural
  candidate.

### 4. Spec item 3, RayQuery in raygen, closest-hit and miss (2 to 3 versions)

- a. The rewriter half for a raygen: DONE 0.80.0. Probe: a run-time
  TraceRay miss index works on WARP and the 1070. The raygen is presented to
  the compute lowering as a compute shader, and the application's library
  stripped of it.
- b. The proxy half: DONE 0.81.0 (`proxy/rg_query`). The state object (the
  two libraries, the shim's hit groups and miss, the raygen's local root
  signature extended with the scene copy and the miss index), and at
  DispatchRays the shim's tables, copied on the GPU: the application's
  records unchanged, then the shim's miss and block, reached through a scene
  copy whose contributions point at it. `gitest --rgquery`, two poisons.
  Found doing it, fixed: a resource record type holding a comma.
- c. Closest-hit and miss: DONE 0.82.0. Hosted like a raygen and given
  their own signature back; recursion depth grows by 1; hosted shaders share
  one shim block, their arguments written into every record of their units
  by a GPU pass (`proxy/rg_table.hlsl`). `gitest --chquery`.
- d. Collections and export lists: DONE 0.83.0. A shader's query slot and
  the pipeline's slot count are read at run time, so a collection is lowered
  before its pipeline exists; a linked collection built with other configs
  is built again with the pipeline's (D3D12 refuses a mismatch). `gitest
  --collections`, `--grow`, `--rgsplit`. STEP 4 DONE.

These can call `TraceRay`, so the lowering is the compute one, in place.
The generated hit groups are added to the application's state object; the
shader tables are the shim's copies (the variant machinery already builds
them); the query traces a scene copy whose contributions point at the
shim's block of records; recursion depth grows by 1 where a closest-hit or
miss queries.

### 5. Spec item 3, compute beside groupshared, barriers, wave ops (3 to 4 versions)

Split the shader at the query: compute pass A up to the query (writes the
ray and live values, and groupshared per group), `DispatchRays` for the
query, compute pass B from the query on (reloads them). Groups and waves
keep their layout in A and B. Probe first: split one real Unreal-shaped
shader by hand, check against WARP.

- Probe DONE (2026-11-24): `phase5/cases/split/gs_orig.hlsl` (a groupshared
  jitter exchanged across a barrier before the query, the hit distance
  shared through groupshared across a barrier after it, a wave op) against
  its hand split `gs_a` / `gs_q` / `gs_b`, run in turn by `raytest --split`
  with a scratch buffer at u1: 14500 of 65536 rays hit on both, 0
  mismatches; the chain without A diverges. The query part is an ordinary
  RayQuery compute shader, which the shim lowers as it is, so the split
  needs no new lowering. (Found writing it: `frac(sin(x) * 43758)` is not a
  usable test input, the WARP and NVIDIA `sin` differ enough to move rays.)
- Design it decides: the rewriter splits the compute shader into A, Q and
  B, three compute shaders. A runs the original up to the query and stores
  every value live past it, an alive flag per thread, and any groupshared
  array a later read can see (copied out per group after a barrier); Q is
  the query region alone, reading its inputs and writing its results; B
  restores groupshared (copy in, barrier), skips threads not alive, and
  runs from the query's exit. The proxy turns the application's Dispatch
  into A, Q (lowered, through `DispatchRays`) and B with UAV barriers
  between, the scratch bound through the application's root signature
  extended by one root UAV (the bindings replayed onto it). First version:
  a query outside any loop, its region single-entry and single-exit.
- a. First version DONE 0.84.0. `split.py` / `split.cpp`, byte-identical:
  the region runs from the first query op to the nearest block that
  post-dominates every query op. Handles, cbuffer reads, thread and group
  ids and groupshared addresses are rebuilt in a later part; every other
  scalar is stored (one dword, two for i64 and double). A groupshared array
  of i32 or float that A writes and B reads is saved per group. An early
  return before the query joins A's barrier and leaves the alive flag 0.
  Each part drops the resource records it makes no handle for (lowered
  into a library, an unused one is an unused global the validator
  refuses). The proxy (`rq_pipeline`, `DispatchSplit`) writes the group
  counts into the scratch buffer's header (`WriteBufferImmediate`), then
  runs A, the lowered Q and B, with UAV barriers between; a Q deferred to
  submit (its scene not read yet) puts B in the segment after it. Seven
  dispatch cases (6.5 and 6.6; opaque and alpha-tested; groupshared saved;
  deferred; mixed; four geometries) match WARP; two poisons diverge.
- b. An indirect dispatch of a split pipeline: DONE 0.85.0, A, Q and B all
  issued at submit once the group counts are read back. A path around the
  query that joins it at P (`if (c) { query }`): DONE 0.85.0, through a
  block of A storing alive 2 and P's phi values for that edge.
- c. Next: a query inside a loop, a path around the query joining past P,
  a local array or a groupshared variable of another shape crossing the
  split, a barrier or wave op inside the query region (the Proceed body).

### 6. Spec item 3, pixel, vertex, mesh, amplification (4 or more versions)

Same split, across a draw: a first pass of the draw writes each
invocation's ray and live values (colour and depth writes and UAVs off), a
`DispatchRays` runs the queries, the real draw reads the results. The hard
parts: a key per invocation that is the same in both passes, overdraw,
helper lanes. Probe first on one pixel shader; decide per stage.

### 7. Spec item 3, RayQuery in any-hit, intersection, callable

These cannot call `TraceRay` in DXR 1.0, and no lowering is known. A
research step with a gate: find a route that is exact, or write down why
none exists.

### 8. Close-out

Three-form matrix, dispatch suite, an Escher run. Then the limits list:

- an application local root signature already at 192 dwords (global root
  signature route);
- a descriptor table needed while a 1,000,000-descriptor heap is bound;
- a scene not resolved through the root signature;
- a build older than the last 8 kept;
- a procedural query with `Abort()` and ray flags computed at run time
  (0.71.0);
- a candidate read after the Proceed loop where both ways out meet (0.72.0);
- loop state, or values carried out, in a body that is also an
  intersection shader, which has no payload (0.69.0; a per-ray scratch UAV
  would be the route);
- a query traced on either arm of a branch against a different acceleration
  structure on each (a phi of resource handles), or into a top-tested loop
  (0.73.0);
- `Proceed()` without a loop: a call nested in the body of the one before, a
  write between two calls that a later body may read, a body with more than
  one way out, a chain in a query serving procedural primitives (0.74.0);
- an `Abort()` between two `Proceed()` calls without a loop (0.75.0);
- several queries where one needs an intersection shader: more than 15 of
  them (one scene copy holds 15 ray contributions), or a scene not resolved
  through the root signature (the application's record layout has no room
  for records per query) (0.77.0);
- a query with another query inside its loop (or retraced inside it) whose
  body does something for a procedural candidate: the stepped search records
  triangles only, recording a box needing loop state in an intersection
  shader (0.79.0);
- a query in an application's raygen, closest-hit or miss (0.81.0,
  0.82.0): whose scene is picked per ray among several (a key table's
  capacity would have to be fixed when the application creates its state
  object), or is in a local root signature, or is not read from its latest
  build at the dispatch (not deferred to submit); a miss or hit table of
  stride 0; a library with its own subobjects, or a shader associated by
  name with a library's subobject; a shader config not at state object
  scope; GeometryIndex() in the same pipeline; more than 15 query slots in
  one pipeline; a closest-hit or miss query in a pipeline already at
  recursion depth 31; (0.83.0) a collection with RayQuery shaders linked
  through an export list, a closest-hit with RayQuery in a collection
  holding no hit group that imports it, GeometryIndex() in a DXR shader that
  uses RayQuery;
- a split pipeline (0.84.0) whose scratch buffer would pass 4 GB, or whose
  root signature is at 63 or 64 dwords (no room for the scratch buffer's
  root UAV).

## Size

About 13 to 16 versions, most of it in steps 5 and 6. Steps 1 and 2 are
small and close two of the seven spec items.
