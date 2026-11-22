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
  exits.
- f. One query traced twice (`TraceRayInline` again on the same object).
- g. `Proceed()` called without a loop, more than once.
- h. General writes in the loop body (only counter appends lower today),
  and appends together with `Abort()` or an intersection shader. RayQuery's
  candidate order is itself implementation-defined, so the question is the
  count, which `NO_DUPLICATE_ANYHIT_INVOCATION` already fixes. DONE 0.67.0
  for any-hit bodies, with `Abort()` made exact (it ends the search, or with
  committed state read, rejects every later candidate, opaque ones too).
  LEFT: a write in a body that becomes an intersection shader, which the
  spec lets run more than once per primitive. Found doing it: 124 and 128
  byte payloads break the 1070's driver; the lowering pads both to 132.
- i. `Abort()` outside the loop.
- j. More than 16 carried values, and carried values of other types.
- k. Several queries where one needs an intersection shader: give each
  query its own hit group offset, so no query id has to travel in a payload.
- l. A query used inside another query's loop (nested), and a dynamically
  indexed resource inside the loop.

### 4. Spec item 3, RayQuery in raygen, closest-hit and miss (2 to 3 versions)

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
- a build older than the last 8 kept.

## Size

About 13 to 16 versions, most of it in steps 5 and 6. Steps 1 and 2 are
small and close two of the seven spec items.
