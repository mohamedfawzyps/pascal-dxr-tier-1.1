// Plan step 5 (0.84.0): groupshared memory that lives ACROSS the query. gsJ
// is written before the query and read after it, so the split has to save
// it in the scratch buffer's group area and give it back to part B. The
// query is alpha-tested (a Proceed loop, so part Q becomes a raygen with a
// generated any-hit), and threads past the image return BEFORE the query
// (so B must tell them apart: the alive flag), yet still take part in B's
// barrier through the groupshared copy-in. Compiled at cs_6_5, and at
// cs_6_6 as gs_save_sm66.hlsl.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

groupshared float gsJ[64];
groupshared uint gsN[64];

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gsJ[gi] = (float)((tid.x * 7 + tid.y * 13) % 17) * 0.015625;
    gsN[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    const float j = gsJ[63 - gi];
    if (tid.x >= width - 8) return;

    const float2 ndc = float2(((tid.x + 0.5) / width) * 2.0 - 1.0, ((tid.y + 0.5) / height) * 2.0 - 1.0);
    RayDesc ray;
    ray.Origin = float3(ndc * halfExtent + float2(j, 0.0), camZ);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        const float2 b = q.CandidateTriangleBarycentrics();
        if (b.x + b.y > 0.25) q.CommitNonOpaqueTriangleHit();
    }
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    const float t = hit ? q.CommittedRayT() : -1.0;
    const float bx = hit ? q.CommittedTriangleBarycentrics().x : 0.0;

    InterlockedAdd(gsN[gi ^ 8], hit ? 1u : 0u);
    GroupMemoryBarrierWithGroupSync();   // whole groups return above, so this is uniform
    Result r;
    r.t = t + gsJ[gi ^ 1];
    r.bx = bx;
    r.by = j;
    r.hit = (hit ? 1u : 0u) + 2u * gsN[gi];
    if (tid.y < height) outBuf[tid.y * width + tid.x] = r;
}
