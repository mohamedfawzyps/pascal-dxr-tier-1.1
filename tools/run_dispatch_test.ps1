# End to end: a RayQuery COMPUTE shader running on the GTX 1070.
#
# This is the whole project in one test. The application compiles RayQuery,
# creates a compute pipeline and calls Dispatch. None of that can work on Tier
# 1.0 hardware. Through the shim it does:
#
#   CheckFeatureSupport      reports Tier 1.1  (only with DXR_TIER11=1)
#   CreateComputePipelineState  lowers the DXIL, builds a state object and
#                               shader table, returns a stand-in
#   SetPipelineState         recognises the stand-in
#   Dispatch                 becomes DispatchRays, groups times numthreads
#
# WARP running the same shader natively is the oracle, every run.
#
#   .\tools\run_dispatch_test.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

if (-not (Test-Path '.\d3d12.dll')) {
    Write-Host 'd3d12.dll missing; run build_proxy.bat first.'
    Pop-Location; exit 1
}
foreach ($dll in @('dxcompiler.dll', 'dxil.dll')) {
    if (-not (Test-Path ".\$dll")) {
        Write-Host "$dll must sit beside the shim; the rewriter loads it by full path."
        Pop-Location; exit 1
    }
}

# pattern is what raytest builds the scene for; cs picks the shader.
$cases = @(
    @{ name = 'opaque';  pat = 'opaque'; extra = @();
       desc = 'built-in opaque RayQuery, numthreads(8,8,1)' },
    @{ name = 'alpha';   pat = 'alpha';  extra = @();
       desc = 'built-in alpha-tested RayQuery, generated any-hit' },
    @{ name = 'indep';   pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_indep.hlsl');
       desc = 'independent shader, resource read in the Proceed loop, numthreads(16,16,1)' },
    @{ name = 'sm66';    pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_sm66.hlsl');
       desc = 'Shader Model 6.6 binding, which is what Unreal compiles its RayQuery shaders at' },
    @{ name = 'proc';    pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_proc.hlsl', '--proc');
       desc = 'procedural primitives, generated intersection shader' },
    @{ name = 'abort';   pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_abort.hlsl', '--multi');
       desc = 'Abort(), order-independent observable only' },
    @{ name = 'acc';     pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--multi');
       desc = 'the 11 accessors from the Unreal survey, end to end' },
    @{ name = 'ids';     pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_ids.hlsl', '--multi');
       desc = 'CommittedInstanceIndex and CommittedPrimitiveIndex' },
    # The instances carry DIFFERENT hit group contributions, so the shader
    # table has to be sized to the scene rather than to one record. Measured:
    # with a single record this scene loses exactly the hits belonging to the
    # instance contributing 1, 9248 of 18496, silently. So this case fails if
    # the table stops growing.
    @{ name = 'contrib'; pat = 'alpha';  extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--multi', '--contrib');
       desc = 'nonzero InstanceContributionToHitGroupIndex, table sized to the scene' },
    # A triangle-only shader on a scene that ALSO holds procedural geometry.
    # The shim's hit group is triangles-only and the procedural geometry simply
    # reports nothing, which is the same answer Tier 1.1 gives, since the
    # shader never commits a procedural candidate either. Measured, not assumed:
    # the debug layer is silent on this and on the case that IS wrong, so it
    # settles nothing.
    @{ name = 'mixedtri'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--mixed');
       desc = 'triangle-only shader on a scene holding procedural geometry too' },
    # The case the shim could not serve at all until per-index typed records.
    # A shader that COMMITS procedural hits, on a scene that also holds
    # triangles, with the two kinds on different records. Slot 0 gets a
    # TRIANGLES hit group whose any-hit always ignores, so the triangle
    # geometry is traversed with a record of the right type and produces
    # nothing. Measured: with that rejecting record replaced by the real one,
    # this scene gives 15418 hits against WARP's 7396, the 8022 difference
    # being exactly the triangle hits. So this case fails if the typing stops.
    @{ name = 'mixedproc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc.hlsl', '--mixed', '--contrib');
       desc = 'procedural shader on a mixed scene, triangles on their own record' },
    # The last shader-side gap: ONE Proceed loop that commits BOTH kinds. It
    # lowers to an any-hit AND an intersection shader from the same body, plus
    # two closest-hits, because a triangle hit reports committed status 1 and a
    # procedural one 2. Two REAL hit groups, one per record.
    @{ name = 'both'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_both.hlsl', '--mixed', '--contrib');
       desc = 'a query committing BOTH triangle and procedural hits' },
    # Resource arrays through a descriptor table, now that RunRayQuery honours
    # --table and these can run as compute shaders at all. --table binds the
    # real output at slot 2 and decoys at 0, 1 and 3, so a mishandled index
    # writes nowhere visible and the case reports 0 hits.
    @{ name = 'table'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_table.hlsl', '--table');
       desc = 'resource array at a CONSTANT index, through a descriptor table' },
    # The index is a cbuffer load, so the element is reached by a real
    # getelementptr INSTRUCTION rather than a folded constant expression.
    # Measured, with the index poisoned to a constant 0: 0 hits, not 14450.
    @{ name = 'dyn'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dyn.hlsl', '--table');
       desc = 'DYNAMIC descriptor indexing, the index computed at runtime' },
    # The same through NonUniformResourceIndex, which marks the getelementptr
    # with !dx.nonuniform. The value is wave-uniform so the RESULT is the same;
    # what this checks is that the metadata survives the whole path.
    @{ name = 'dynnu'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynnu.hlsl', '--table');
       desc = 'dynamic index through NonUniformResourceIndex' },
    # The SAME shader, created through CreatePipelineState, the pipeline STREAM
    # form, instead of CreateComputePipelineState.
    #
    # This is not a variation on the lowering, it is a different D3D12 entry
    # point, and it is the one a real engine uses. Unreal creates every PSO
    # through the stream form, and until this case existed the shim's
    # substitution covered only the struct form, so every RayQuery shader in a
    # real game reached the driver unexamined. The driver rejected them, and
    # Unreal treats a failed compute PSO as fatal.
    #
    # The stream the harness builds puts the ROOT SIGNATURE FIRST, deliberately,
    # because that is what an engine does and it is exactly what the first
    # version of the walker could not get past.
    @{ name = 'stream'; pat = 'opaque'; extra = @('--stream');
       desc = 'the stream form of CreatePipelineState, which is what Unreal uses' },
    # GeometryIndex, the accessor this project called impossible for longest,
    # and the one a real Unreal 5.8 run refused 108 shaders on.
    #
    # --geom is one bottom-level structure holding FOUR geometries, a quad per
    # quadrant. Every other scene here has one geometry per structure, so the
    # right answer is 0 everywhere and a lowering that dropped the index would
    # pass. Here the candidate values GATE THE COMMIT: geometry + contribution
    # == 3 is skipped, so without --contrib the missing quadrant is geometry 3
    # and with it geometry 1. The gap MOVES, so neither accessor can be a
    # constant and both have to be right.
    @{ name = 'geom'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom');
       desc = 'GeometryIndex, baked into the hit shader copy its record points at' },
    # RayFlags (195), read BOTH inside the Proceed loop and outside it, which
    # take different routes: dx.op.rayFlags in the generated any-hit, where it
    # is legal, and a folded constant in the raygen, where it is not. A
    # lowering using the constant in both places passes a test that reads it in
    # only one.
    #
    # The flags are a UNION of the template argument and the TraceRayInline
    # argument, 0x002 | 0x200 = 0x202, so dropping either half is visible. The
    # loop read gates the commit, so a wrong value there empties the image.
    @{ name = 'flags'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_flags.hlsl');
       desc = 'RayFlags, folded in the raygen and intrinsic in the any-hit' },
    # Values computed BEFORE the Proceed loop and read INSIDE it: a cbuffer
    # threshold and a value derived from the thread id. The ordinary shape of a
    # real alpha test, and what Unreal refused 59 shaders on.
    #
    # Neither is caller state: a cbuffer is bound by the GLOBAL root signature
    # and the ray index is the same ray, so the generated hit shader rebuilds
    # the chain rather than being refused. The two arrive differently, a
    # cbuffer load and a DispatchRaysIndex, so both are exercised.
    # The same shader with the TraceRayInline flags computed at RUNTIME.
    #
    # Unreal does this in 118 shaders. The lowering used to fold the flags into
    # the TraceRay call, so it could only accept a constant; dx.op.traceRay
    # takes RayFlags as an ordinary i32 operand, so it does not have to.
    # Expected answer identical to the static case, 0x202, which is the point:
    # same result, different route. Dropping the operand loses the runtime half
    # and RayFlags() reports 0x002.
    @{ name = 'dynflags'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_dynflags.hlsl');
       desc = 'ray flags computed at runtime, passed through rather than folded' },
    # A loop body with real CONTROL FLOW, so DXC emits a phi inside the loop,
    # and a [branch] hint, so it emits a distinct metadata node.
    #
    # Both were bugs. A phi writes its predecessors as [ %val, %bb12 ] with
    # no label keyword, so the operand scan counted the predecessor BLOCKS as
    # values and refused 118 of Unreal's shaders for reading them. And the
    # metadata scan did not match distinct, so that id stayed invisible and
    # the fresh-id counter handed it out again.
    @{ name = 'phi'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_phi.hlsl');
       desc = 'a phi in the loop body, and a distinct metadata node beside it' },
    # A phi whose predecessor is the ENTRY BLOCK, which DXC gives no label
    # because nothing can branch to it. Normalisation had nothing to rename, so
    # the phi referenced a block that was never defined and the assembler said
    # "use of undefined value '%bb0'".
    #
    # It survived the whole of Phase 5 because EVERY other case here opens with
    # `if (tid.x >= width) return;`, and that guard puts a block between the
    # entry and everything else. One shared habit across every test, hiding one
    # bug.
    @{ name = 'entryphi'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_entryphi.hlsl');
       desc = 'a phi whose predecessor is the entry block, which has no label' },
    @{ name = 'param'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_param.hlsl');
       desc = 'loop body reads a cbuffer threshold and a ray-index value from outside' },
    @{ name = 'geomcontrib'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib');
       desc = 'the same, with a nonzero contribution, so the record index is contribution + geometry' },
    # The same at Shader Model 6.6, which is what Unreal uses, and where the
    # record handle has to be ANNOTATED: a bare createHandleForLib on a local
    # root signature resource is what crashed the Pascal driver.
    @{ name = 'geomsm66'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom_sm66.hlsl', '--geom', '--contrib');
       desc = 'GeometryIndex and contribution at Shader Model 6.6, record handle annotated' },
    # The record constants are BAKED into a copy of the hit shaders per pair,
    # because a hit shader reading a local root signature crashes the Pascal
    # driver (phase5/cases/driver-crash/). `geom` only copies AnyHit and
    # ClosestHit; this copies all four, and needs a triangle AND a procedural
    # hit group per pair. Every commit is gated on the contribution the record
    # reports, so a record pointing at the wrong copy loses its whole instance:
    # measured, with every record forced onto copy 0, 7396 mismatches, which
    # is exactly the procedural hit count.
    @{ name = 'bothgeom'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_bothgeom.hlsl', '--mixed', '--contrib');
       desc = 'both kinds, each commit gated on its baked contribution, four hit shaders copied per pair' },
    # SV_GroupID, SV_GroupThreadID and SV_GroupIndex, which a raygen does not
    # have; rebuilt from DispatchRaysIndex and numthreads. The pixel comes only
    # from the group values and SV_GroupIndex gates the commit in the any-hit.
    # Measured: swapping the axes' group sizes gives 5999 mismatches, and a
    # wrong row stride in the flattening gives 5398. The first version of the
    # gate could not see the row stride at all and passed with it wrong.
    @{ name = 'group'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_group.hlsl');
       desc = 'SV_GroupID, SV_GroupThreadID and SV_GroupIndex, numthreads(16,8,1)' },
    # ExecuteIndirect with a DISPATCH signature, which is how Unreal issues
    # most of its Lumen and MegaLights inline passes. Until 0.39.0 the shim
    # forwarded it and the GPU ran the do-nothing carrier, silently. The
    # arguments are GPU-written, in a COMBINED read state, at byte 36 among
    # decoys of 3 groups, so an ignored dispatch draws nothing and a wrong
    # offset draws a corner.
    @{ name = 'indirect'; pat = 'alpha'; extra = @('--indirect');
       desc = 'indirect compute dispatch, group counts read back from the GPU' },
    @{ name = 'indirectup'; pat = 'opaque'; extra = @('--indirectup');
       desc = 'indirect compute dispatch from an upload buffer, read at record time' },
    # The record pairs are only known at dispatch, so this one REBAKES inside
    # the queue hook, at submit time, on the thread that submits.
    @{ name = 'indirectgeom'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--indirect');
       desc = 'indirect dispatch of a shader whose hit shaders are baked per record pair' },
    # The top-level structure is rebuilt IN PLACE with new contributions after
    # a first dispatch, as an engine does when a level loads. 0.39.0 read each
    # address once and kept a one-record table: 9248 of 18496 hits, and in
    # Escher's open world a GPU hang. Measured separately: with the re-read
    # poisoned, the table padding alone still matches, since this shader bakes
    # no record data and pads with its own record.
    @{ name = 'rebuild'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_acc.hlsl', '--multi', '--contrib', '--rebuild');
       desc = 'top-level structure rebuilt in place with new contributions between dispatches' },
    # A new record layout after the real dispatch, twelve times, in the SAME
    # list before it is submitted, so the real dispatch's table is evicted from
    # the shim's cache while nothing has run. 0.40.1 kept every evicted table
    # until the pipeline died, about 3 GB in Escher's open world; 0.40.2 reuses
    # and frees them, and must never do so while a list that reads one can
    # still run. The gate below turns that check off and must diverge.
    @{ name = 'churn'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '12');
       desc = 'twelve more record layouts recorded after the real dispatch, before submitting' },
    # The same, submitted and waited for after every layout, so evicted tables
    # become idle and the reuse path actually runs.
    @{ name = 'churnflush'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '20', '--churnflush');
       desc = 'twenty more record layouts, each submitted, so shader table buffers are reused' },
    # The scene moves to a new top-level structure with a different layout and
    # the old one is never built again, as Unreal does when its structure
    # outgrows its buffer. 0.40.2 kept the old one live for 64 builds, the two
    # disagreed about records, and the dispatch was refused: 9248 hit/miss
    # mismatches here, two bursts of refusals per Escher session.
    @{ name = 'move'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--move');
       desc = 'top-level structure moved to a new address with a different layout' },
    # The instances in GPU memory, as Unreal writes them, and the scene rebuilt
    # in place with a different layout. The shim reads GPU-written instances
    # a submission late, so at the dispatch it knows only the OLDER build:
    # 0.51.0 drew from that build's table, 9248 hit/miss and 4624 value
    # mismatches, nothing logged. The dispatch now waits for submit and takes
    # the table from exactly the build it traces.
    @{ name = 'stalegpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--gpuinst');
       desc = 'GPU-written instances, scene rebuilt in place with a new layout' },
    # The same, twelve layouts in one list after the real dispatch: each
    # deferred dispatch needs ITS build's instances, not the latest.
    @{ name = 'churngpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '12', '--gpuinst');
       desc = 'GPU-written instances, twelve layouts in one list' },
    @{ name = 'indirectgpuinst'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--indirect', '--gpuinst');
       desc = 'GPU-written instances, rebuilt in place, indirect dispatch' },
    # The rebuild recorded in ANOTHER list, submitted in the same
    # ExecuteCommandLists call just before the dispatch's: until 0.52.2 its
    # read was stamped only after the whole call, and the dispatch was refused.
    @{ name = 'sameecl'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--sameecl', '--gpuinst');
       desc = 'rebuild in another list of the same submission, GPU-written instances' },
    @{ name = 'sameeclind'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--rebuild', '--sameecl', '--gpuinst', '--indirect');
       desc = 'the same, indirect dispatch' },
    # After the dispatch, the bottom-level structure is rebuilt at the SAME
    # address with one geometry instead of four, in a list submitted after
    # the dispatch's. The dispatch traces four. 0.52.2 parsed the instances
    # at submit against the latest structure RECORDED there: a 3-record table
    # where 6 are reached, 9248 hit/miss mismatches, drawn, nothing logged.
    # Unreal reuses addresses as it streams geometry, recording ahead.
    @{ name = 'blasreuse'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--blasreuse', '--gpuinst');
       desc = 'bottom-level address reused by a later-recorded build, GPU-written instances' },
    # Structures made by CopyRaytracingAccelerationStructure, which the shim
    # never sees built. Unreal COMPACTs its bottom-level structures; a CLONE is
    # the same structure too. 0.52.3 drew the cloned bottom-level case with 0
    # hits of 13872, nothing logged, and refused the cloned top-level one.
    @{ name = 'blasclone'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--blasclone');
       desc = 'instance on a CLONE of the bottom-level structure' },
    @{ name = 'blasclonegpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--blasclone', '--gpuinst');
       desc = 'the same, GPU-written instances' },
    @{ name = 'tlasclonegpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--tlasclone', '--gpuinst');
       desc = 'dispatch traces a CLONE of the top-level structure, its read still pending' },
    # A second LIVE top-level structure whose layout conflicts with the real
    # one. Judged over every live structure this is refused; the shim has to
    # resolve the scene the dispatch traces (0.49.0). And the same with the
    # scene taken from the descriptor heap, Unreal's bindless form, the index
    # in the root CBV and the decoy in the neighbouring slots and at t0.
    @{ name = 'decoy'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--decoy');
       desc = 'a second live structure with a conflicting layout, scene resolved through the root SRV' },
    @{ name = 'bindlessrq'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom_bindless_sm66.hlsl', '--geom', '--contrib', '--bindless');
       desc = 'the scene from the descriptor heap, index in the root CBV, conflicting decoy beside it' },
    # A Proceed loop that APPENDS a record per candidate, the MegaLights and
    # Lumen shape. The records land in traversal order, which is undefined, so
    # raytest sorts them into <out>.append and those must be identical too.
    @{ name = 'append'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_append_sm66.hlsl', '--multi', '--append');
       desc = 'a Proceed loop that appends a record per candidate through a UAV counter'; append = $true },
    # Values LOADED before the loop and used inside it, MegaLights' shape: the
    # thread's own output slot (a UAV, prefilled) and a read-only buffer. The
    # UAV value travels in the payload, the SRV is read again in the hit
    # shader. The gate below runs the two sides with different prefill seeds
    # and must diverge, so the pre-read value is shown to drive the image.
    @{ name = 'preload'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_preload_sm66.hlsl', '--prefill', '1');
       desc = 'values loaded before the loop, one carried in the payload, one re-read' },
    # Two RayQuery objects one after the other, the shape of 33 Lumen and
    # Niagara shaders: two traces, one shared any-hit choosing the loop body by
    # a query id in the payload. The bodies commit opposite triangle halves;
    # measured on WARP, swapping them changes 11536 rays, so running the wrong
    # one for either trace cannot match.
    @{ name = 'twoq'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoq_sm66.hlsl', '--multi', '--prefill', '1');
       desc = 'two queries in sequence, one any-hit picking the loop body by query id' },
    # The shim's OWN record layout (0.61.0), for scenes the application's
    # records cannot serve, each refused and drawn as nothing until then. Two
    # instances of a four-geometry structure, the second's contribution one
    # past the first's, so record c + 1 is geometry 1 of one and geometry 0 of
    # the other: 6936 hits on WARP, 0 drawn on 0.60.0.
    @{ name = 'overlap'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--overlap');
       desc = 'two instances disagree about a record: the shim''s own layout' },
    @{ name = 'overlapgpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--overlap', '--gpuinst');
       desc = 'the same, GPU-written instances, drawn at submit' },
    @{ name = 'overlapind'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--overlap', '--indirect');
       desc = 'the same, indirect dispatch' },
    # Triangles and procedural primitives on ONE record, which a record
    # cannot serve for a shader committing procedural hits: in the shim's
    # layout each (instance, geometry) has a record of its own kind.
    @{ name = 'collapse'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc.hlsl', '--mixed');
       desc = 'both kinds on one record, procedural shader' },
    @{ name = 'collapseboth'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_both.hlsl', '--mixed');
       desc = 'both kinds on one record, a shader committing both' },
    # Two live scenes that disagree about a record, BOTH traced: the scene
    # picked per ray from the heap (a keyed slot), slot 3 or 4.
    @{ name = 'heapkey'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom_heapkey_sm66.hlsl', '--geom', '--contrib', '--bindless');
       desc = 'two conflicting scenes, picked per ray from the heap' },
    # DESERIALIZED structures, as Unreal loads offline ones: what they hold is
    # decoded by the driver for tools (0.61.0). 0.52.3 drew the first with 0
    # hits of 13872; 0.53.0 to 0.61.0 refused them by name.
    @{ name = 'deser'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--deserialize');
       desc = 'an instance on a deserialized bottom-level structure' },
    @{ name = 'desergpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--deserialize', '--gpuinst');
       desc = 'the same, GPU-written instances' },
    @{ name = 'deserind'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--deserialize', '--indirect');
       desc = 'the same, indirect dispatch' },
    @{ name = 'tlasdeser'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--tlasdeser');
       desc = 'the dispatch traces a deserialized top-level structure' },
    @{ name = 'tlasdesergpu'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--tlasdeser', '--gpuinst');
       desc = 'the same, GPU-written instances' },
    # Plan step 3 (0.65.0): a switch through the Proceed loop, which the CFG
    # model did not follow, and resource arrays at Shader Model 6.6, where a
    # constant index bound element 0 (0 hits of 14450) and a dynamic one was
    # refused.
    @{ name = 'switch'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_switch.hlsl', '--geom');
       desc = 'a switch inside the Proceed loop and after it' },
    @{ name = 'switchc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_switch.hlsl', '--geom', '--contrib');
       desc = 'the same, contribution 2' },
    @{ name = 'table66'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_table_sm66.hlsl', '--table');
       desc = '6.6 resource array, constant index' },
    @{ name = 'dyn66'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dyn_sm66.hlsl', '--table');
       desc = '6.6 resource array, dynamic index' },
    @{ name = 'dynnu66'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynnu_sm66.hlsl', '--table');
       desc = '6.6 resource array, NonUniformResourceIndex' },
    # Loop state (0.65.0): a count, the nearest t and a value chosen with it,
    # carried across Proceed iterations and out of the loop in the payload.
    @{ name = 'carryout'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_carryout.hlsl', '--geom');
       desc = 'state carried across candidates and out of the loop' },
    @{ name = 'carryoutm'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_carryout.hlsl', '--multi');
       desc = 'the same, two instances' },
    # Committed state read inside the Proceed loop (0.66.0): the any-hit keeps
    # it in the payload, the ray traced forced non-opaque and the original
    # opacity applied by the any-hit from each record's third word. Single
    # candidates per ray (--geom), so what the body sees is comparable; the
    # stacked scene compares only the final hit, the order-free result.
    @{ name = 'cmt'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed.hlsl', '--geom'); desc = 'committed state read in the loop' },
    @{ name = 'cmtop'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed.hlsl', '--geom', '--opaquemix'); desc = 'the same, two quadrants opaque' },
    @{ name = 'cmtopc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed.hlsl', '--geom', '--opaquemix', '--contrib'); desc = 'the same, contribution 2' },
    @{ name = 'cmtif8'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed.hlsl', '--geom', '--opaquemix', '--instflags', '8'); desc = 'the instance forced non-opaque' },
    @{ name = 'cmtif4'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed.hlsl', '--geom', '--opaquemix', '--instflags', '4'); desc = 'the instance forced opaque' },
    @{ name = 'cmtstack'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed_stack.hlsl', '--geom', '--opaquemix', '--stack'); desc = 'two candidates per ray, the final hit' },
    @{ name = 'cmtcullop'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed_cullop.hlsl', '--geom', '--opaquemix'); desc = 'RAY_FLAG_CULL_OPAQUE' },
    @{ name = 'cmtcullnop'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed_cullnop.hlsl', '--geom', '--opaquemix'); desc = 'RAY_FLAG_CULL_NON_OPAQUE' },
    @{ name = 'cmtfnop'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed_forcenop.hlsl', '--geom', '--opaquemix'); desc = 'RAY_FLAG_FORCE_NON_OPAQUE' },
    @{ name = 'cmttwoq'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_committed_twoq.hlsl', '--geom', '--opaquemix'); desc = 'two queries, both reading committed state' },
    @{ name = 'proccmt'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_committed.hlsl', '--proc'); desc = 'procedural: CommittedRayT is RayTCurrent' },
    @{ name = 'proccmtmix'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_committed.hlsl', '--mixed', '--contrib'); desc = 'the same, a mixed scene' },
    # Writes in the loop body and Abort() made exact (0.67.0). One candidate
    # per ray (--geom) for the writes, so each store is comparable; the stacked
    # scenes compare only order-free results.
    @{ name = 'writes'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_writes.hlsl', '--geom'); desc = 'a store and an atomic in the loop body' },
    @{ name = 'writesop'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_writes.hlsl', '--geom', '--opaquemix'); desc = 'the same, two quadrants opaque' },
    @{ name = 'abortflag'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortflag.hlsl', '--geom', '--stack'); desc = 'Abort(), committed state never read: the search ends' },
    @{ name = 'abortcmt'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortcommit.hlsl', '--geom', '--opaquemix'); desc = 'commit then Abort(), and a bare Abort()' },
    @{ name = 'abortcmtc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortcommit.hlsl', '--geom', '--opaquemix', '--contrib'); desc = 'the same, contribution 2' },
    @{ name = 'abortstack'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortstack.hlsl', '--geom', '--opaquemix', '--stack'); desc = 'nothing commits after Abort(), order-free' },
    @{ name = 'abortmix'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortstack.hlsl', '--geom', '--stackmix'); desc = 'the same, an opaque candidate behind a non-opaque one' },
    # Writes in a body that becomes an INTERSECTION shader (0.68.0): one box,
    # two stacked boxes (the atomic counts the body's runs per ray), a mixed
    # scene.
    @{ name = 'procwrites'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_writes.hlsl', '--proc'); desc = 'a store and an atomic in a procedural body' },
    @{ name = 'procwrites2'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_writes.hlsl', '--proc', '--stack'); desc = 'the same, two boxes per ray' },
    @{ name = 'procwritesmix'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_writes.hlsl', '--mixed', '--contrib'); desc = 'the same, a mixed scene' },
    # A procedural query treats triangles too (0.69.0): its body runs for a
    # non-opaque triangle candidate, and an opaque one is committed.
    @{ name = 'proctricand'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_tricand.hlsl', '--mixed', '--contrib'); desc = 'procedural query, body runs for triangle candidates' },
    @{ name = 'procopaquetri'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_proc_status.hlsl', '--mixed', '--contrib'); desc = 'procedural query, an opaque triangle committed' },
    @{ name = 'procstatus'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_status.hlsl', '--mixed', '--contrib'); desc = 'the same, the triangle non-opaque' },
    # A procedural candidate's opacity, computed (0.70.0): an opaque box, a
    # non-opaque one, and the opaque box in a mixed scene.
    @{ name = 'procopaque'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_proc_opacity.hlsl', '--proc'); desc = 'CandidateProceduralPrimitiveNonOpaque on an opaque box' },
    @{ name = 'procnonopaque'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_opacity.hlsl', '--proc'); desc = 'the same, a non-opaque box' },
    @{ name = 'procopaquemix'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_proc_opacity.hlsl', '--mixed', '--contrib'); desc = 'the same, an opaque box and triangle' },
    # Exact Abort() in a procedural query (0.71.0): the body runs once per ray
    # whatever the order, and the committed state survives the abort.
    @{ name = 'procwriteabort'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_writeabort.hlsl', '--proc', '--stack'); desc = 'a write beside Abort() in a procedural body, two boxes' },
    @{ name = 'procabortmix'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_abortmix.hlsl', '--mixed', '--contrib'); desc = 'Abort() on either kind in a procedural query' },
    @{ name = 'procabortcmt'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_abortcmt.hlsl', '--mixed', '--contrib'); desc = 'commit then Abort(), committed state read' },
    @{ name = 'procabortcmtop'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_proc_abortcmt.hlsl', '--mixed', '--contrib'); desc = 'the same, opaque geometry' },
    @{ name = 'procabortcmt1'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_abortcmt.hlsl', '--proc'); desc = 'the same, one box' },
    @{ name = 'procabortorder'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_abort.hlsl', '--proc', '--stack'); desc = 'Abort() on the first candidate: never the near box' },
    # And a triangle query's body runs for procedural candidates (0.69.0).
    @{ name = 'triproccand'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_tri_proccand.hlsl', '--mixed', '--contrib'); desc = 'triangle query, body runs for procedural candidates' },
    # A 128 byte payload removes the 1070's device when dispatched; the
    # lowering pads it to 132 (0.67.0).
    @{ name = 'carry128'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_carry128.hlsl', '--geom', '--opaquemix'); desc = 'a payload that would be 128 bytes' },
    @{ name = 'deserboth'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--tlasdeser', '--deserialize');
       desc = 'a deserialized top-level structure over a deserialized bottom-level one' },
    # A break or return out of the loop body, and loops with several exits
    # (0.72.0, plan step 3 e). Order-free: on --stack both layers have the
    # same barycentrics, and a box test commits only the box it computes.
    @{ name = 'break'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_break.hlsl', '--geom'); desc = 'a break, with and without a commit' },
    @{ name = 'breakstack'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_break.hlsl', '--geom', '--stack'); desc = 'the same, two candidates: a break ends the search' },
    @{ name = 'return'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_return.hlsl', '--geom'); desc = 'a return out of the loop body' },
    @{ name = 'returnstack'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_return.hlsl', '--geom', '--stack'); desc = 'the same, two candidates' },
    @{ name = 'twoexits'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoexits.hlsl', '--geom'); desc = 'a loop left top-tested, two exits with code of their own' },
    @{ name = 'procbreak'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_break.hlsl', '--proc'); desc = 'a break and a return out of a procedural body' },
    @{ name = 'procbreakstack'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_break.hlsl', '--proc', '--stack'); desc = 'the same, two boxes' },
    @{ name = 'procbreakmix'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proc_break.hlsl', '--mixed', '--contrib'); desc = 'the same, a triangle beside the box' },
    # One query traced more than once (0.73.0, plan step 3 f): each search
    # its own TraceRay; a branch choosing the ray, merged by phis.
    @{ name = 'retrace'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_retrace.hlsl', '--geom'); desc = 'one query traced twice' },
    @{ name = 'retracemulti'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_retrace.hlsl', '--multi'); desc = 'the same, two instances' },
    @{ name = 'retracebranch'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_retracebranch.hlsl', '--geom'); desc = 'one query traced on either arm of a branch' },
    @{ name = 'retracebranch1'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_retracebranch.hlsl'); desc = 'the same, one triangle' },
    @{ name = 'retraceloop'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_retraceloop.hlsl', '--geom'); desc = 'one TraceRayInline run in a loop' },
    @{ name = 'retraceloopm'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_retraceloop.hlsl', '--multi'); desc = 'the same, two instances' },
    # Proceed() without a loop (0.74.0, plan step 3 g): the k-th candidate
    # runs the k-th call's body, the last call ends the search; and a query
    # never asked to Proceed() is never searched (0.73.0 drew 14450 hits).
    @{ name = 'proceed1'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proceed1.hlsl', '--geom'); desc = 'Proceed() once, without a loop' },
    @{ name = 'proceed1s'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proceed1.hlsl', '--geom', '--stack'); desc = 'the same, two candidates: the body runs once' },
    @{ name = 'proceed2'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proceed2.hlsl', '--geom'); desc = 'Proceed() twice, one candidate' },
    @{ name = 'proceed2s'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proceed2.hlsl', '--geom', '--stack'); desc = 'the same, two candidates: a body each' },
    @{ name = 'proceedbare'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proceedbare.hlsl', '--geom', '--opaquemix'); desc = 'a bare Proceed(), opaque quadrants committed, the others not' },
    @{ name = 'proceedbares'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_proceedbare.hlsl', '--geom', '--opaquemix', '--stack'); desc = 'the same, two layers' },
    @{ name = 'noproceed'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_noproceed.hlsl'); desc = 'never asked to Proceed(), never searched' },
    # Abort() outside the loop (0.75.0, plan step 3 i): before the search it
    # searches nothing (traced with instance mask 0), after it changes nothing.
    @{ name = 'abortpre'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortpre.hlsl', '--geom'); desc = 'Abort() before the Proceed loop' },
    @{ name = 'abortpres'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortpre.hlsl', '--geom', '--stack'); desc = 'the same, two candidates' },
    @{ name = 'abortpost'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortpost.hlsl', '--geom'); desc = 'Abort() after the Proceed loop' },
    @{ name = 'abortopaque'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_abortopaque.hlsl'); desc = 'Abort() before the one Proceed() of an opaque query' },
    @{ name = 'abortchain'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortchain.hlsl', '--geom'); desc = 'Abort() before a Proceed() without a loop' },
    @{ name = 'abortchains'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_abortchain.hlsl', '--geom', '--stack'); desc = 'the same, two candidates' },
    # Carried values past 16 and of other types (0.76.0, plan step 3 j): the
    # GTX 1070 measured right to an 8192 byte payload.
    @{ name = 'carrymany'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_carrymany.hlsl', '--geom', '--stack'); desc = '77 carried values, a 400 byte payload' },
    @{ name = 'carrytypes'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_carrytypes.hlsl', '--geom'); desc = 'double, i64 and half carried' },
    @{ name = 'carrytypess'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_carrytypes.hlsl', '--geom', '--stack'); desc = 'the same, two candidates' },
    @{ name = 'carrytypesm'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_carrytypes.hlsl', '--multi'); desc = 'the same, two instances' },
    @{ name = 'toploopstate'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_toploopstate.hlsl', '--geom'); desc = 'a top-tested loop whose state leaves it' },
    @{ name = 'toploopstatem'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_toploopstate.hlsl', '--multi'); desc = 'the same, two instances' },
    # Several queries, one needing an intersection shader (0.77.0, plan step
    # 3 k): each query its own hit shaders and records, in the own layout.
    @{ name = 'twoqproc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoqproc.hlsl', '--mixed'); desc = 'two procedural queries with different bodies' },
    @{ name = 'twoqprocc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoqproc.hlsl', '--mixed', '--contrib'); desc = 'the same, the kinds on their own records' },
    @{ name = 'twoqprocp'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoqproc.hlsl', '--proc'); desc = 'the same, boxes only' },
    @{ name = 'twoqmix'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoqmix.hlsl', '--mixed'); desc = 'a triangle query and a procedural one' },
    @{ name = 'twoqmixc'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_twoqmix.hlsl', '--mixed', '--contrib'); desc = 'the same, the kinds on their own records' },
    # Tracked procedural queries in the own layout (0.77.0): until then its
    # state object had no AnyHitProc and failed to build (E_INVALIDARG).
    @{ name = 'ownprocabort'; pat = 'alpha'; envv = 'DXR_TIER11_RQOWN'; extra = @('--cs', 'phase5\cases\rayquery_proc_abortmix.hlsl', '--mixed', '--contrib'); desc = 'Abort() in a procedural query, the own layout forced' },
    @{ name = 'ownprocbreak'; pat = 'alpha'; envv = 'DXR_TIER11_RQOWN'; extra = @('--cs', 'phase5\cases\rayquery_proc_break.hlsl', '--proc', '--stack'); desc = 'a break out of a procedural body, the own layout forced' },
    # A resource array indexed at run time inside the loop (0.78.0, plan step
    # 3 l): --table puts the real output at slot 2, decoys at 0, 1 and 3.
    @{ name = 'dynloop'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynloop.hlsl', '--table'); desc = 'a resource indexed at run time in the loop, the index rebuilt' },
    @{ name = 'dynloop_sm66'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynloop_sm66.hlsl', '--table'); desc = 'a resource indexed at run time in the loop, the index rebuilt, at 6.6' },
    @{ name = 'dynloopc'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynloopc.hlsl', '--table'); desc = 'the same, the index carried in the payload' },
    @{ name = 'dynloopc_sm66'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynloopc_sm66.hlsl', '--table'); desc = 'the same, the index carried in the payload, at 6.6' },
    @{ name = 'dynloopnu'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynloopnu.hlsl', '--table'); desc = 'the same through NonUniformResourceIndex' },
    @{ name = 'dynloopnu_sm66'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynloopnu_sm66.hlsl', '--table'); desc = 'the same through NonUniformResourceIndex, at 6.6' },
    @{ name = 'dynlooph'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynlooph.hlsl', '--table'); desc = 'the handle made before the loop, rebuilt from its carried index' },
    @{ name = 'dynlooph_sm66'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_dynlooph_sm66.hlsl', '--table'); desc = 'the handle made before the loop, rebuilt from its carried index, at 6.6' },
    # A query inside another's loop (0.79.0, plan step 3 l): the outer query
    # stepped, its loop kept in the raygen.
    @{ name = 'nested'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_nested.hlsl', '--geom', '--stack'); desc = 'a query inside another query''s loop' },
    @{ name = 'nestedop'; pat = 'alpha'; extra = @('--cs', 'phase5\cases\rayquery_nestedop.hlsl', '--geom', '--opaquemix', '--stack'); desc = 'the same, opaque geometry committed by traversal' },
    @{ name = 'nestedloop'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_nestedloop.hlsl', '--geom', '--stack'); desc = 'an inner query with a loop of its own' },
    @{ name = 'nestedfront'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_nestedfront.hlsl', '--geom', '--stack'); desc = 'the front layer committed' },
    @{ name = 'nestedmix'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_nestedfront.hlsl', '--mixed', '--contrib'); desc = 'the same on a mixed scene' },
    @{ name = 'nestedstep'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\probe_nested_step.hlsl', '--geom', '--stack'); desc = 'the stepped form written by hand' },
    @{ name = 'retraceinloop'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_retraceinloop.hlsl', '--geom', '--stack'); desc = 'a query traced again inside its own loop, stepped' },
    @{ name = 'looped'; pat = 'opaque'; extra = @('--cs', 'phase5\cases\rayquery_looped.hlsl'); desc = 'a query proceeded once per iteration of an ordinary loop' }
)

$failed = 0
$env:DXR_TIER11 = '1'
foreach ($c in $cases) {
    $n = $c.name
    Write-Host ''
    Write-Host "=== $n : $($c.desc) ==="
    $a = "dp_${n}_a.bin"; $b = "dp_${n}_b.bin"

    # WARP runs RayQuery natively; the 1070 runs it only because of the shim.
    & .\raytest.exe warp rayquery $c.pat $a @($c.extra) | Out-Null
    # A case may set one variable for the hardware run (0.77.0).
    if ($c.envv) { Set-Item "env:$($c.envv)" '1' }
    & .\raytest.exe hw   rayquery $c.pat $b @($c.extra) | Out-Null
    $hwExit = $LASTEXITCODE
    if ($c.envv) { Remove-Item "env:$($c.envv)" }
    if ($hwExit -ne 0) {
        Write-Host '  the hardware run FAILED; the dispatch path did not take over'
        $failed++
        Remove-Item $a, $b -ErrorAction SilentlyContinue
        continue
    }
    $diff = & .\raytest.exe diff $a $b 2>&1
    $diff | Where-Object { $_ -match 'rays|mismatches|max|RESULT' } |
        ForEach-Object { "  $($_.Trim())" }
    $appendOk = $true
    if ($c.append) {
        $appendOk = (Test-Path "$a.append") -and (Test-Path "$b.append") -and
            ((Get-FileHash "$a.append").Hash -eq (Get-FileHash "$b.append").Hash)
        $recs = if (Test-Path "$a.append") { [BitConverter]::ToUInt32([IO.File]::ReadAllBytes("$a.append"), 0) } else { 0 }
        Write-Host "  appended records   : $recs, sorted sets $(if ($appendOk) { 'identical' } else { 'DIFFER' })"
        Remove-Item "$a.append", "$b.append" -ErrorAction SilentlyContinue
    }
    if (($diff -match 'RESULT: MATCH') -and $appendOk) { Write-Host "  $n : PASS" }
    else { Write-Host "  $n : FAIL"; $failed++ }
    Remove-Item $a, $b -ErrorAction SilentlyContinue
}
$env:DXR_TIER11 = ''

# Tier 1.1 is ON by default now: a proxy DLL only sits beside an executable
# because somebody put it there, and that is the opt-in. What has to be proven
# instead is that the OFF switch still works, because it is the only way back
# short of deleting the file, and because it is what a bug report will be asked
# to try first.
#
# A non-zero exit is the EXPECTED outcome in the off cases, so stop 'Stop'
# aborting.
$ErrorActionPreference = 'Continue'

Write-Host ''
Write-Host '=== a shader table still in flight is never reused ==='
# DXR_TIER11_NOHOLD=1 makes the shim treat every evicted table as idle. The
# churn case must then DIVERGE: if it still matched, it could not see a table
# reused too early and its pass above would mean nothing.
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--churn', '12')
& .\raytest.exe warp rayquery alpha dp_hold_a.bin @x | Out-Null
$env:DXR_TIER11_NOHOLD = '1'
& .\raytest.exe hw rayquery alpha dp_hold_b.bin @x | Out-Null
$env:DXR_TIER11_NOHOLD = $null
$env:DXR_TIER11 = ''
$diff = & .\raytest.exe diff dp_hold_a.bin dp_hold_b.bin 2>&1
Remove-Item dp_hold_a.bin, dp_hold_b.bin -ErrorAction SilentlyContinue
if ($diff -match 'RESULT: DIVERGE') {
    Write-Host '  diverges with the check off, so the churn case can see it'
} else {
    Write-Host '  NOT SENSITIVE: churn matched with in-flight tables reused'
    $failed++
}

Write-Host ''
Write-Host '=== the value read before the loop drives the image ==='
# The preload case with prefill seed 1 on WARP and seed 2 on the 1070 must
# DIVERGE: if the image did not depend on the pre-read, a lowering that
# carried or re-read the wrong value would pass the case above.
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_preload_sm66.hlsl', '--prefill')
& .\raytest.exe warp rayquery alpha dp_pre_a.bin @x 1 | Out-Null
& .\raytest.exe hw rayquery alpha dp_pre_b.bin @x 2 | Out-Null
$env:DXR_TIER11 = ''
$diff = & .\raytest.exe diff dp_pre_a.bin dp_pre_b.bin 2>&1
Remove-Item dp_pre_a.bin, dp_pre_b.bin -ErrorAction SilentlyContinue
if ($diff -match 'RESULT: DIVERGE') {
    Write-Host '  diverges with different prefills, so the preload case can see it'
} else {
    Write-Host '  NOT SENSITIVE: preload matched with different prefilled values'
    $failed++
}

Write-Host ''
Write-Host '=== an EMPTY scene draws all misses ==='
# A top-level structure with no instances, as Unreal's is on the first frame
# of a level. WARP crashes tracing one (divide by zero, RayQuery and TraceRay
# alike), so the oracle is the definition: every ray misses. The output is
# prefilled, so a dispatch that is not drawn leaves the pattern's hits
# behind; until 0.52.2 it was refused and left 49154.
foreach ($m in @(@('--gpuinst'), @())) {
    $out = & .\raytest.exe hw rayquery alpha dp_empty.bin --cs phase5\cases\rayquery_geom.hlsl --geom --contrib --empty --prefill 7 @m 2>&1
    Remove-Item dp_empty.bin -ErrorAction SilentlyContinue
    $what = if ($m.Count) { 'GPU-written instances' } else { 'CPU-visible instances' }
    if ($out -match '65536 rays, 0 hits') {
        Write-Host "  $what : drawn, every ray missed"
    } else {
        Write-Host "  $what : NOT DRAWN or wrong: $(($out | Select-String 'rays,').Line)"
        $failed++
    }
}

Write-Host ''
Write-Host '=== an instance with a NULL structure is inactive ==='
# Unreal writes every culled instance with AccelerationStructure 0, which the
# spec calls legal but inactive, discarded at build. 0.53.0 refused the scene
# for it (30 dispatches in Escher). WARP access-violates on one, so the oracle
# is the definition: the scene WITHOUT that instance, drawn on WARP.
$prevTier = $env:DXR_TIER11
$env:DXR_TIER11 = '1'
$g = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib')
& .\raytest.exe warp rayquery alpha dp_null_a.bin @g | Out-Null
foreach ($m in @(@('--gpuinst'), @())) {
    & .\raytest.exe hw rayquery alpha dp_null_b.bin @g --nullinst @m | Out-Null
    $diff = & .\raytest.exe diff dp_null_a.bin dp_null_b.bin 2>&1
    $what = if ($m.Count) { 'GPU-written instances' } else { 'CPU-visible instances' }
    if ($diff -match 'RESULT: MATCH') {
        Write-Host "  $what : MATCH, the inactive instance changed nothing"
    } else {
        Write-Host "  $what : DIVERGE or not drawn"
        $failed++
    }
    Remove-Item dp_null_b.bin -ErrorAction SilentlyContinue
}
Remove-Item dp_null_a.bin -ErrorAction SilentlyContinue
$env:DXR_TIER11 = $prevTier

Write-Host ''
Write-Host '=== tier 1.1 is on by default ==='
$env:DXR_TIER11 = ''
$out = & .\raytest.exe hw rayquery opaque dp_gate.bin 2>&1
Remove-Item dp_gate.bin -ErrorAction SilentlyContinue
if ($out -match 'needs Tier 1\.1') {
    Write-Host '  DEFAULT BROKEN: RayQuery was refused with no configuration at all'
    $failed++
} else {
    Write-Host '  accepted with no configuration at all, as it must'
}

Write-Host ''
Write-Host '=== DXR_TIER11=0 turns it off ==='
$env:DXR_TIER11 = '0'
$out = & .\raytest.exe hw rayquery opaque dp_gate.bin 2>&1
$env:DXR_TIER11 = ''
Remove-Item dp_gate.bin -ErrorAction SilentlyContinue
if ($out -match 'needs Tier 1\.1') {
    Write-Host '  refused with DXR_TIER11=0, as it must'
} else {
    Write-Host '  OFF SWITCH BROKEN: RayQuery was accepted with DXR_TIER11=0'
    $failed++
}

# And through the file, which is the path a user without a terminal takes. The
# env var is cleared above, so this proves the file alone is enough.
Write-Host ''
Write-Host '=== dxr-tier-11.ini turns it off too ==='
'tier11 = 0' | Set-Content dxr-tier-11.ini -Encoding ascii
$out = & .\raytest.exe hw rayquery opaque dp_gate.bin 2>&1
Remove-Item dxr-tier-11.ini, dp_gate.bin -ErrorAction SilentlyContinue
if ($out -match 'needs Tier 1\.1') {
    Write-Host '  refused with tier11 = 0 in dxr-tier-11.ini, as it must'
} else {
    Write-Host '  INI IGNORED: RayQuery was accepted with tier11 = 0 in dxr-tier-11.ini'
    $failed++
}

$ErrorActionPreference = 'Stop'

# The shim's own record layout (0.61.0) carries the cases above that the
# application's records cannot serve, which were refused until then. Each of
# its three parts is shown to carry the result: a poison that touches only the
# own layout must turn a MATCH into a DIVERGE.
#   DXR_TIER11_RQOWN_POISON=1  every record's pair from the first instance
#   DXR_TIER11_RQOWN_POISON=2  every record the shader's own hit group, whatever its kind
#   DXR_TIER11_KEY_POISON=1    every key to the first scene
Write-Host ''
Write-Host '=== the shim''s own record layout carries the result ==='
$env:DXR_TIER11 = '1'
$own = @(
    @{ what = 'record pairs (overlap)'; var = 'DXR_TIER11_RQOWN_POISON'; val = '1';
       x = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib', '--overlap') },
    @{ what = 'record kinds (collapse)'; var = 'DXR_TIER11_RQOWN_POISON'; val = '2';
       x = @('--cs', 'phase5\cases\rayquery_proc.hlsl', '--mixed') },
    @{ what = 'scene keys (heapkey)'; var = 'DXR_TIER11_KEY_POISON'; val = '1';
       x = @('--cs', 'phase5\cases\rayquery_geom_heapkey_sm66.hlsl', '--geom', '--contrib', '--bindless') }
)
foreach ($o in $own) {
    $x = $o.x
    & .\raytest.exe warp rayquery alpha dp_own_a.bin @x | Out-Null
    Set-Item "env:$($o.var)" $o.val
    & .\raytest.exe hw rayquery alpha dp_own_b.bin @x | Out-Null
    Remove-Item "env:$($o.var)"
    $diff = & .\raytest.exe diff dp_own_a.bin dp_own_b.bin 2>&1
    Remove-Item dp_own_a.bin, dp_own_b.bin -ErrorAction SilentlyContinue
    if ($diff -match 'RESULT: DIVERGE') {
        Write-Host "  $($o.what): diverges poisoned, so the case can see it"
    } else {
        Write-Host "  NOT SENSITIVE: $($o.what) matched poisoned"
        $failed++
    }
}
$env:DXR_TIER11 = ''

# Past the local root signature size the GTX 1070 takes, the shim's own
# layouts carry their scene copies in ONE descriptor table, in a reserve at
# the end of the bound heap (0.63.0; until then refused by name).
# DXR_TIER11_LRS_TABLE=1 uses it everywhere: the own-layout cases above must
# still match; DXR_TIER11_RANGE_POISON=1 writes every descriptor of a range
# as its first, so heapkey (two scenes) must diverge. gitest --biglrs pads the
# application's raygen signature to 191 dwords of 192, so there the table is
# the only way, and gitest --twoconflict with the table everywhere, poisoned,
# must diverge too.
Write-Host ''
Write-Host '=== the scene copies in a descriptor table ==='
$env:DXR_TIER11 = '1'
$env:DXR_TIER11_LRS_TABLE = '1'
foreach ($o in $own) {
    $x = $o.x
    & .\raytest.exe warp rayquery alpha dp_tab_a.bin @x | Out-Null
    $env:DXR_TIER11_RQOWN = '1'
    & .\raytest.exe hw rayquery alpha dp_tab_b.bin @x | Out-Null
    $diff = & .\raytest.exe diff dp_tab_a.bin dp_tab_b.bin 2>&1
    $ok = $diff -match 'RESULT: MATCH'
    $sens = $true
    if ($o.var -eq 'DXR_TIER11_KEY_POISON') {
        $env:DXR_TIER11_RANGE_POISON = '1'
        & .\raytest.exe hw rayquery alpha dp_tab_b.bin @x | Out-Null
        Remove-Item env:DXR_TIER11_RANGE_POISON
        $sens = (& .\raytest.exe diff dp_tab_a.bin dp_tab_b.bin 2>&1) -match 'RESULT: DIVERGE'
    }
    Remove-Item env:DXR_TIER11_RQOWN
    Remove-Item dp_tab_a.bin, dp_tab_b.bin -ErrorAction SilentlyContinue
    if ($ok -and $sens) { Write-Host "  $($o.what): matches in the table form$(if ($o.var -eq 'DXR_TIER11_KEY_POISON') { ', diverges with the range poisoned' })" }
    else { Write-Host "  TABLE FORM FAILED: $($o.what) match=$ok sensitive=$sens"; $failed++ }
}
$gi = & .\gitest.exe --twoconflict 2>&1
$ok = $gi -match 'ALL MATCH'
$env:DXR_TIER11_RANGE_POISON = '1'
$gi = & .\gitest.exe --twoconflict 2>&1
Remove-Item env:DXR_TIER11_RANGE_POISON
if ($ok -and ($gi -match 'DIVERGE')) { Write-Host '  gitest --twoconflict: matches in the table form, diverges with the range poisoned' }
else { Write-Host "  TABLE FORM FAILED: gitest --twoconflict match=$ok"; $failed++ }
Remove-Item env:DXR_TIER11_LRS_TABLE
foreach ($l in @('--localscene', '--localscenetable')) {
    $gi = & .\gitest.exe $l --biglrs 2>&1
    if ($gi -match 'ALL MATCH') { Write-Host "  gitest $l --biglrs (191 dwords of the application's): all layouts match" }
    else { Write-Host "  gitest $l --biglrs: NOT ALL MATCH"; $failed++ }
}

# RayQuery in an APPLICATION's raygen (0.81.0, plan step 4 b): gitest's
# raygen runs an inline query beside its own TraceRay, through the shim's
# tables (the application's records, then the shim's block). Every layout,
# both shader models, the scene bound every way one scene can be, the
# argument and table placements. Poisons: 1 the miss index at the
# application's miss, 2 every record of the shim's block the first
# instance's contribution; both must DIVERGE. A scene picked per ray among
# several is refused by name and drawn nowhere, which the log must say.
Write-Host ''
Write-Host '=== RayQuery in an application raygen, closest-hit and miss (gitest --rgquery, --chquery) ==='
foreach ($a in @(@(), @('--sm66'), @('--table'), @('--bindless'), @('--bindlessrc'), @('--dynmiss'),
                 @('--indirect'), @('--indirectgpu'), @('--gpusbt'), @('--gpuinst'), @('--twoscenes'),
                 @('--sm66', '--dynmiss'), @('--sm66', '--indirectgpu'), @('--sm66', '--gpusbt'))) {
    $gi = & .\gitest.exe --rgquery @a 2>&1
    if ($gi -match 'ALL MATCH') { Write-Host "  gitest --rgquery $($a -join ' '): all layouts match" }
    else { Write-Host "  gitest --rgquery $($a -join ' '): NOT ALL MATCH"; $failed++ }
}
foreach ($p in @('1', '2')) {
    $env:DXR_TIER11_RGQ_POISON = $p
    $gi = & .\gitest.exe --rgquery mult 2>&1
    Remove-Item env:DXR_TIER11_RGQ_POISON
    if ($gi -match 'DIVERGE') { Write-Host "  gitest --rgquery, DXR_TIER11_RGQ_POISON=${p}: diverges" }
    else { Write-Host "  POISON NOT SEEN: gitest --rgquery, DXR_TIER11_RGQ_POISON=$p"; $failed++ }
}
# RayQuery in an application's closest-hit and miss (0.82.0, plan step 4 c):
# gitest --chquery, the closest-hit querying from beside its hit, the lower
# half of the rays missing and the miss querying from the ray's origin; with
# --rgquery all three hosted in one pipeline. WARP removes its device on a
# heap-indexed scene traced from a closest-hit or miss, so the heap cases bind
# WARP's scene globally (--warpglobal).
foreach ($a in @(@(), @('--sm66'), @('--table'), @('--bindless', '--warpglobal'), @('--bindlessrc', '--warpglobal'),
                 @('--dynmiss'), @('--indirect'), @('--indirectgpu'), @('--gpusbt'), @('--gpuinst'),
                 @('--twoscenes'), @('--rgquery'), @('--rgquery', '--sm66'), @('--sm66', '--indirectgpu'))) {
    $gi = & .\gitest.exe --chquery @a 2>&1
    if ($gi -match 'ALL MATCH') { Write-Host "  gitest --chquery $($a -join ' '): all layouts match" }
    else { Write-Host "  gitest --chquery $($a -join ' '): NOT ALL MATCH"; $failed++ }
}
$env:DXR_TIER11_RGQ_POISON = '2'
$gi = & .\gitest.exe --chquery mult 2>&1
Remove-Item env:DXR_TIER11_RGQ_POISON
if ($gi -match 'DIVERGE') { Write-Host '  gitest --chquery, DXR_TIER11_RGQ_POISON=2: diverges' }
else { Write-Host '  POISON NOT SEEN: gitest --chquery, DXR_TIER11_RGQ_POISON=2'; $failed++ }
$rlog = Join-Path $PWD 'rgq_refuse.log'
Remove-Item $rlog -ErrorAction SilentlyContinue
$env:DXR_TIER11_LOG = $rlog
& .\gitest.exe --rgquery --scenearray mult 2>&1 | Out-Null
& .\gitest.exe --rgquery --collections mult 2>&1 | Out-Null
Remove-Item env:DXR_TIER11_LOG
$said = Get-Content $rlog -ErrorAction SilentlyContinue
if (($said -match 'NOT DRAWN: a query in an application.s DXR shader whose scene is picked per ray among several') -and
    ($said -match 'REFUSED: RayQuery in a DXR shader of a collection')) {
    Write-Host '  refused by name: a scene picked per ray among several, a RayQuery raygen in a collection'
} else { Write-Host '  REFUSAL NOT LOGGED: --scenearray or --collections with --rgquery'; $failed++ }
Remove-Item $rlog -ErrorAction SilentlyContinue
$env:DXR_TIER11 = ''

# Spec Tier 1.1 item 7: the vertex formats Tier 1.1 adds for bottom-level
# builds (and the Tier 1.0 ones as controls), with the packed strides the
# spec allows. MEASURED native on the GTX 1070 (0.64.0): no emulation, every
# format bit-exact against WARP. Sensitivity: 8-bit positions against float
# ones must DIVERGE, so the case sees the format.
Write-Host ''
Write-Host '=== vertex formats (Tier 1.1 adds seven) ==='
$env:DXR_TIER11 = '1'
$vf = @(@(6,0),@(10,0),@(13,0),@(11,0),@(35,0),@(24,0),@(28,0),@(49,0),@(31,0),@(51,0),@(28,3),@(31,3),@(11,6))
$bad = @()
foreach ($c in $vf) {
    $x = @('--vformat', "$($c[0])"); if ($c[1]) { $x += @('--vstride', "$($c[1])") }
    & .\raytest.exe warp rayquery opaque dp_vf_a.bin @x | Out-Null
    & .\raytest.exe hw rayquery opaque dp_vf_b.bin @x | Out-Null
    if (-not ((& .\raytest.exe diff dp_vf_a.bin dp_vf_b.bin 2>&1) -match 'RESULT: MATCH')) { $bad += "$($c[0])/$($c[1])" }
}
& .\raytest.exe hw rayquery opaque dp_vf_b.bin --vformat 6 | Out-Null
& .\raytest.exe warp rayquery opaque dp_vf_a.bin --vformat 28 | Out-Null
$sens = (& .\raytest.exe diff dp_vf_a.bin dp_vf_b.bin 2>&1) -match 'RESULT: DIVERGE'
Remove-Item dp_vf_a.bin, dp_vf_b.bin -ErrorAction SilentlyContinue
if (-not $bad.Count -and $sens) { Write-Host "  $($vf.Count) format/stride cases match WARP; 8-bit against float diverges" }
else { Write-Host "  VERTEX FORMATS FAILED: $($bad -join ', ') sensitive=$sens"; $failed++ }
$env:DXR_TIER11 = ''

# Loop state (0.65.0): with DXR_TIER11_STATE_POISON=1 no iteration stores
# its state, so carryout must DIVERGE.
Write-Host ''
Write-Host '=== loop state carried in the payload ==='
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_carryout.hlsl', '--geom')
& .\raytest.exe warp rayquery alpha dp_st_a.bin @x | Out-Null
$env:DXR_TIER11_STATE_POISON = '1'
& .\raytest.exe hw rayquery alpha dp_st_b.bin @x | Out-Null
$env:DXR_TIER11_STATE_POISON = $null
if ((& .\raytest.exe diff dp_st_a.bin dp_st_b.bin 2>&1) -match 'RESULT: DIVERGE') { Write-Host '  carryout: diverges with the state poisoned' }
else { Write-Host '  NOT SENSITIVE: carryout matched with the state poisoned'; $failed++ }
foreach ($f in 'dp_st_a.bin', 'dp_st_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
$env:DXR_TIER11 = ''

# Committed state (0.66.0): with DXR_TIER11_OPACITY_POISON=1 every record
# says not opaque, so the opaque quadrants run the body: cmtop must DIVERGE.
Write-Host ''
Write-Host '=== committed state read inside the loop ==='
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_committed.hlsl', '--geom', '--opaquemix')
& .\raytest.exe warp rayquery alpha dp_op_a.bin @x | Out-Null
$env:DXR_TIER11_OPACITY_POISON = '1'
& .\raytest.exe hw rayquery alpha dp_op_b.bin @x | Out-Null
$env:DXR_TIER11_OPACITY_POISON = $null
if ((& .\raytest.exe diff dp_op_a.bin dp_op_b.bin 2>&1) -match 'RESULT: DIVERGE') { Write-Host '  cmtop: diverges with every record said not opaque' }
else { Write-Host '  NOT SENSITIVE: cmtop matched with the opacity poisoned'; $failed++ }
foreach ($f in 'dp_op_a.bin', 'dp_op_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
$env:DXR_TIER11 = ''

# Abort() (0.67.0): with DXR_TIER11_ABORT_POISON=1 an aborting query keeps
# its own flags, so traversal commits an opaque candidate met after the abort,
# as before 0.67.0. abortstack must DIVERGE, on --stackmix: with --stack
# --opaquemix no ray meets both kinds, and the poison matched (measured).
Write-Host ''
Write-Host '=== Abort() exactness ==='
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_abortstack.hlsl', '--geom', '--stackmix')
& .\raytest.exe warp rayquery alpha dp_ab_a.bin @x | Out-Null
$env:DXR_TIER11_ABORT_POISON = '1'
& .\raytest.exe hw rayquery alpha dp_ab_b.bin @x | Out-Null
$env:DXR_TIER11_ABORT_POISON = $null
if ((& .\raytest.exe diff dp_ab_a.bin dp_ab_b.bin 2>&1) -match 'RESULT: DIVERGE') { Write-Host '  abortstack: diverges with traversal committing after the abort' }
else { Write-Host '  NOT SENSITIVE: abortstack matched with the abort poisoned'; $failed++ }
foreach ($f in 'dp_ab_a.bin', 'dp_ab_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
$env:DXR_TIER11 = ''

# Writes in an intersection shader (0.68.0): with DXR_TIER11_WRITE_POISON=1
# the stores of a procedural body are dropped, and procwrites2 must DIVERGE.
# (DXR_TIER11_NODUP_POISON, the geometry flag left off, is NOT a gate: on the
# 1070 it changed nothing in these scenes, measured.)
Write-Host ''
Write-Host '=== writes in an intersection shader ==='
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_proc_writes.hlsl', '--proc', '--stack')
& .\raytest.exe warp rayquery alpha dp_pw_a.bin @x | Out-Null
$env:DXR_TIER11_WRITE_POISON = '1'
& .\raytest.exe hw rayquery alpha dp_pw_b.bin @x | Out-Null
$env:DXR_TIER11_WRITE_POISON = $null
if ((& .\raytest.exe diff dp_pw_a.bin dp_pw_b.bin 2>&1) -match 'RESULT: DIVERGE') { Write-Host '  procwrites2: diverges with the stores dropped' }
else { Write-Host '  NOT SENSITIVE: procwrites2 matched with the stores dropped'; $failed++ }
foreach ($f in 'dp_pw_a.bin', 'dp_pw_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
$env:DXR_TIER11 = ''

# Exact Abort() in a procedural body (0.71.0): with
# DXR_TIER11_PROCABORT_POISON=1 the intersection shader leaves its abort
# unreported, so traversal runs on and procwriteabort must DIVERGE.
Write-Host ''
Write-Host '=== Abort() in a procedural body ==='
$env:DXR_TIER11 = '1'
$x = @('--cs', 'phase5\cases\rayquery_proc_writeabort.hlsl', '--proc', '--stack')
& .\raytest.exe warp rayquery alpha dp_pa_a.bin @x | Out-Null
$env:DXR_TIER11_PROCABORT_POISON = '1'
& .\raytest.exe hw rayquery alpha dp_pa_b.bin @x | Out-Null
$env:DXR_TIER11_PROCABORT_POISON = $null
if ((& .\raytest.exe diff dp_pa_a.bin dp_pa_b.bin 2>&1) -match 'RESULT: DIVERGE') { Write-Host '  procwriteabort: diverges with the abort unreported' }
else { Write-Host '  NOT SENSITIVE: procwriteabort matched with the abort unreported'; $failed++ }
foreach ($f in 'dp_pa_a.bin', 'dp_pa_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
$env:DXR_TIER11 = ''

# A break or return out of the loop body (0.72.0): with
# DXR_TIER11_EXIT_POISON=1 an exit neither ends the search nor says which
# way it left, so breakstack (a second candidate runs the body), return and
# twoexits (the code after the loop that the original skipped) must DIVERGE;
# with the procedural abort poison, procbreakstack must too.
Write-Host ''
Write-Host '=== exits, retraces, chains, aborts outside the loop, carried values, records per query, run-time indices, nested queries ==='
$env:DXR_TIER11 = '1'
$xg = @(
    @{ n = 'breakstack'; v = 'DXR_TIER11_EXIT_POISON'; x = @('--cs', 'phase5\cases\rayquery_break.hlsl', '--geom', '--stack') },
    @{ n = 'return'; v = 'DXR_TIER11_EXIT_POISON'; x = @('--cs', 'phase5\cases\rayquery_return.hlsl', '--geom') },
    @{ n = 'twoexits'; v = 'DXR_TIER11_EXIT_POISON'; x = @('--cs', 'phase5\cases\rayquery_twoexits.hlsl', '--geom') },
    @{ n = 'procbreakstack'; v = 'DXR_TIER11_PROCABORT_POISON'; x = @('--cs', 'phase5\cases\rayquery_proc_break.hlsl', '--proc', '--stack') },
    # One query traced more than once (0.73.0): every search running the
    # first one's loop body, and a branch's ray always the first arm's.
    @{ n = 'retrace'; v = 'DXR_TIER11_RETRACE_POISON'; x = @('--cs', 'phase5\cases\rayquery_retrace.hlsl', '--geom') },
    @{ n = 'retracebranch'; v = 'DXR_TIER11_MERGE_POISON'; x = @('--cs', 'phase5\cases\rayquery_retracebranch.hlsl', '--geom') },
    # Proceed() without a loop (0.74.0): every candidate running the first
    # call's body, nothing ending the search.
    @{ n = 'proceed1s'; v = 'DXR_TIER11_CHAIN_POISON'; x = @('--cs', 'phase5\cases\rayquery_proceed1.hlsl', '--geom', '--stack') },
    @{ n = 'proceed2s'; v = 'DXR_TIER11_CHAIN_POISON'; x = @('--cs', 'phase5\cases\rayquery_proceed2.hlsl', '--geom', '--stack') },
    # Abort() before the search (0.75.0): the flag never read.
    @{ n = 'abortpre'; v = 'DXR_TIER11_PREABORT_POISON'; x = @('--cs', 'phase5\cases\rayquery_abortpre.hlsl', '--geom') },
    @{ n = 'abortopaque'; v = 'DXR_TIER11_PREABORT_POISON'; x = @('--cs', 'phase5\cases\rayquery_abortopaque.hlsl'); p = 'opaque' },
    # Carried values (0.76.0): the high word of an i64 or double dropped, and
    # no iteration storing its state.
    @{ n = 'carrytypes'; v = 'DXR_TIER11_CARRY_POISON'; x = @('--cs', 'phase5\cases\rayquery_carrytypes.hlsl', '--geom') },
    @{ n = 'carrymany'; v = 'DXR_TIER11_STATE_POISON'; x = @('--cs', 'phase5\cases\rayquery_carrymany.hlsl', '--geom', '--stack') },
    # Records per query (0.77.0): every query's boxes served by query 0's
    # intersection shader.
    @{ n = 'twoqproc'; v = 'DXR_TIER11_PERQUERY_POISON'; x = @('--cs', 'phase5\cases\rayquery_twoqproc.hlsl', '--mixed') },
    # A run-time index in the loop (0.78.0): every handle a hit shader
    # rebuilds from one takes element 0, a decoy.
    @{ n = 'dynloopc'; v = 'DXR_TIER11_DYNIDX_POISON'; p = 'opaque'; x = @('--cs', 'phase5\cases\rayquery_dynloopc.hlsl', '--table') },
    @{ n = 'dynlooph_sm66'; v = 'DXR_TIER11_DYNIDX_POISON'; p = 'opaque'; x = @('--cs', 'phase5\cases\rayquery_dynlooph_sm66.hlsl', '--table') },
    # A stepped query (0.79.0): an opaque hit a search found is not committed.
    # --opaquemix needs the alpha pattern: under opaque every geometry is.
    @{ n = 'nestedop'; v = 'DXR_TIER11_NEST_POISON'; x = @('--cs', 'phase5\cases\rayquery_nestedop.hlsl', '--geom', '--opaquemix', '--stack') }
)
foreach ($g in $xg) {
    $x = $g.x
    $pt = if ($g.p) { $g.p } else { 'alpha' }
    & .\raytest.exe warp rayquery $pt dp_xg_a.bin @x | Out-Null
    Set-Item "env:$($g.v)" '1'
    & .\raytest.exe hw rayquery $pt dp_xg_b.bin @x | Out-Null
    Remove-Item "env:$($g.v)"
    if ((& .\raytest.exe diff dp_xg_a.bin dp_xg_b.bin 2>&1) -match 'RESULT: DIVERGE') { Write-Host "  $($g.n): diverges with $($g.v)" }
    else { Write-Host "  NOT SENSITIVE: $($g.n) matched with $($g.v)"; $failed++ }
}
foreach ($f in 'dp_xg_a.bin', 'dp_xg_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
$env:DXR_TIER11 = ''

# Spec Tier 1.1 item 6: PIPELINE_CONFIG1 flags (0.64.0). The GTX 1070
# rejects the subobject (CreateStateObject E_INVALIDARG); the shim ORs the
# flags into every TraceRay (proxy/pipe_flags). tier11/pflagtest.exe against
# WARP: the hit kinds always, RayFlags() too except where the shim masks it
# as the spec says and WARP does not (--spec). The poison must DIVERGE.
Write-Host ''
Write-Host '=== pipeline flags (PIPELINE_CONFIG1) ==='
$pf = @(
    @{ a = @('--flags','0x100'); s = '' }, @{ a = @('--flags','0x200'); s = '' },
    @{ a = @('--flags','0x100','--dxil'); s = '' }, @{ a = @('--flags','0x200','--dxil'); s = '' },
    @{ a = @('--flags','0x100','--literal'); s = '0x100' }, @{ a = @('--flags','0x200','--literal','--dxil'); s = '0x200' },
    @{ a = @('--flags','0x100','--collection'); s = '' }, @{ a = @('--flags','0x200','--collection','--dxil'); s = '' },
    @{ a = @('--flags','0x100','--collection','--literal'); s = '0x100' },
    @{ a = @('--flags','0x100','--collection','--literal','--mixmask'); s = '' },
    @{ a = @('--flags','0x100','--grow'); s = '' }, @{ a = @('--flags','0x200','--grow','--literal'); s = '0x200' }
)
$bad = @()
foreach ($c in $pf) {
    $x = $c.a
    foreach ($f in 'dp_pf_a.bin', 'dp_pf_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
    & .\pflagtest.exe warp dp_pf_a.bin @x | Out-Null
    & .\pflagtest.exe hw dp_pf_b.bin @x | Out-Null
    $d = if ($c.s) { & .\pflagtest.exe diff dp_pf_a.bin dp_pf_b.bin --spec $c.s } else { & .\pflagtest.exe diff dp_pf_a.bin dp_pf_b.bin }
    if (-not ($d -match 'RESULT: MATCH')) { $bad += ($x -join ' ') }
}
$env:DXR_TIER11_PFLAGS_POISON = '1'
& .\pflagtest.exe hw dp_pf_b.bin --flags 0x100 | Out-Null
& .\pflagtest.exe warp dp_pf_a.bin --flags 0x100 | Out-Null
$env:DXR_TIER11_PFLAGS_POISON = $null
$sens = (& .\pflagtest.exe diff dp_pf_a.bin dp_pf_b.bin) -match 'RESULT: DIVERGE'
foreach ($f in 'dp_pf_a.bin', 'dp_pf_b.bin') { if (Test-Path $f) { [IO.File]::Delete((Join-Path $PWD $f)) } }
if (-not $bad.Count -and $sens) { Write-Host "  $($pf.Count) cases match; poisoned, the flags are seen missing" }
else { Write-Host "  PIPELINE FLAGS FAILED: $($bad -join '; ') sensitive=$sens"; $failed++ }

Write-Host '=== the driver''s decode of a DESERIALIZED structure carries the result ==='
# DXR_TIER11_DECODE_POISON=1 makes every decoded bottom-level structure one
# geometry short and every decoded instance's contribution one higher. The
# deserialize cases above, and gitest --deserialize (the GeometryIndex() side,
# 7 layouts; until 0.61.0 refused by name), must then DIVERGE.
$env:DXR_TIER11 = '1'
foreach ($x in @(@('--deserialize'), @('--tlasdeser'))) {
    $args3 = @('--cs', 'phase5\cases\rayquery_geom.hlsl', '--geom', '--contrib') + $x
    & .\raytest.exe warp rayquery alpha dp_dec_a.bin @args3 | Out-Null
    $env:DXR_TIER11_DECODE_POISON = '1'
    & .\raytest.exe hw rayquery alpha dp_dec_b.bin @args3 | Out-Null
    Remove-Item env:DXR_TIER11_DECODE_POISON
    $diff = & .\raytest.exe diff dp_dec_a.bin dp_dec_b.bin 2>&1
    Remove-Item dp_dec_a.bin, dp_dec_b.bin -ErrorAction SilentlyContinue
    if ($diff -match 'RESULT: DIVERGE') { Write-Host "  raytest $x : diverges poisoned, so the case can see it" }
    else { Write-Host "  NOT SENSITIVE: raytest $x matched with the decode poisoned"; $failed++ }
}
$env:DXR_TIER11 = ''
$gi = & .\gitest.exe --deserialize --gpuinst 2>&1
if ($gi -match 'ALL MATCH') { Write-Host '  gitest --deserialize --gpuinst : all layouts match WARP' }
else { Write-Host '  gitest --deserialize --gpuinst : NOT ALL MATCH'; $failed++ }
$env:DXR_TIER11_DECODE_POISON = '1'
$gi = & .\gitest.exe --deserialize --gpuinst 2>&1
Remove-Item env:DXR_TIER11_DECODE_POISON
if ($gi -match 'DIVERGE') { Write-Host '  gitest --deserialize --gpuinst : diverges poisoned' }
else { Write-Host '  NOT SENSITIVE: gitest --deserialize matched with the decode poisoned'; $failed++ }

Write-Host ''
if ($failed -eq 0) {
    Write-Host 'DISPATCH PATH: RayQuery compute shaders run on the 1070 and match WARP.'
} else {
    Write-Host "DISPATCH PATH: $failed check(s) failed."
}
Pop-Location
exit $failed
