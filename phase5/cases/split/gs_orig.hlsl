// Plan step 5 probe (0.84.0): a RayQuery compute shader beside groupshared
// memory, group barriers and a wave op, which a raygen cannot host (no thread
// group; waves laid out differently). The shim's route is to split it at the
// query: gs_a.hlsl up to the query, gs_q.hlsl the query alone (an ordinary
// RayQuery compute shader the shim already lowers), gs_b.hlsl from the query
// on, values passed through a scratch buffer. This is the original: WARP runs
// it natively as the ground truth.
//
// Before the query: a jitter per thread, exchanged through groupshared memory
// across a barrier with the mirror thread of the group. After it: the hit
// distance shared through groupshared memory across a second barrier, and a
// wave op on a group-uniform value (which lanes form a wave differs between
// WARP and NVIDIA even natively, so only a layout-free result is comparable).
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

groupshared float gsJ[64];
groupshared float gsT[64];

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gsJ[gi] = (float)((tid.x * 7 + tid.y * 13) % 17) * 0.015625;   // exact on every device
    GroupMemoryBarrierWithGroupSync();
    const float j = gsJ[63 - gi];

    const float2 ndc = float2(((tid.x + 0.5) / width) * 2.0 - 1.0, ((tid.y + 0.5) / height) * 2.0 - 1.0);
    RayDesc ray;
    ray.Origin = float3(ndc * halfExtent + float2(j, 0.0), camZ);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    const float t = hit ? q.CommittedRayT() : -1.0;

    gsT[gi] = t;
    GroupMemoryBarrierWithGroupSync();
    Result r;
    r.t = t;
    r.bx = gsT[gi ^ 1];
    r.by = WaveReadLaneFirst(gsT[0]) + j;
    r.hit = hit ? 1u : 0u;
    if (tid.x < width && tid.y < height) outBuf[tid.y * width + tid.x] = r;
}
