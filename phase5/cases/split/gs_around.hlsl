// Plan step 5 b (0.85.0): a path AROUND the query that joins it after, the
// commonest shape there is: `if (c) { query }` and on. A thread for which c
// is false goes straight from part A to where the query's control flow
// rejoins, so it must skip part Q and still run part B, with the values the
// phis there take on its edge (t, hit and bx below are -2, false and 0 for
// it). Groupshared memory and a barrier on both sides, c divergent within a
// group. Compiled at cs_6_5, and at cs_6_6 as gs_around_sm66.hlsl.
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
    gsJ[gi] = (float)((tid.x * 7 + tid.y * 13) % 17) * 0.015625;
    GroupMemoryBarrierWithGroupSync();
    const float j = gsJ[63 - gi];

    float t = -2.0;
    float bx = 0.0;
    bool hit = false;
    if (((tid.x >> 2) + tid.y) % 3 != 0) {
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
        hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        t = hit ? q.CommittedRayT() : -1.0;
        bx = hit ? q.CommittedTriangleBarycentrics().x : 0.0;
    }

    gsT[gi] = t;
    GroupMemoryBarrierWithGroupSync();
    Result r;
    r.t = t;
    r.bx = gsT[gi ^ 1];
    r.by = j + bx;
    r.hit = hit ? 1u : 0u;
    if (tid.x < width && tid.y < height) outBuf[tid.y * width + tid.x] = r;
}
