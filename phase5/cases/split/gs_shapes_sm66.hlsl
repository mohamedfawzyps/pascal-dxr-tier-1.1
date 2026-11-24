// Plan step 5 c (0.86.0): shapes crossing the split beyond scalars and
// groupshared i32/float arrays. A groupshared double array and a groupshared
// uint64_t array written before the query and read after it (two dwords an
// element in the group area); a local float array written before the query
// and read, at dynamic indices, for the ray, inside the query region and
// after it; a local uint64_t array written before the query and inside its
// region (part Q copies it out again), read after it. DXC keeps a local
// array indexed dynamically as an alloca, which a later
// part has to re-create and fill. Compiled at cs_6_5, and at cs_6_6 as
// gs_shapes_sm66.hlsl.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

groupshared double gsD[64];
groupshared uint64_t gsL[64];

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    float la[8];
    uint64_t lw[4];
    for (uint i = 0; i < 8; ++i) la[i] = (float)((tid.x + i * 5) % 9) * 0.0078125;
    for (uint i = 0; i < 4; ++i) lw[i] = ((uint64_t)(tid.y + i) << 33) | (tid.x * 3 + i);
    gsD[gi] = (double)(tid.x % 7) * 0.25;
    gsL[gi] = ((uint64_t)gi << 40) + tid.y;
    GroupMemoryBarrierWithGroupSync();
    const uint k = (tid.x ^ tid.y) & 7;

    const float2 ndc = float2(((tid.x + 0.5) / width) * 2.0 - 1.0, ((tid.y + 0.5) / height) * 2.0 - 1.0);
    RayDesc ray;
    ray.Origin = float3(ndc * halfExtent + float2(la[k], 0.0), camZ);
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
    float t = -1.0;
    if (hit) {
        // Inside the query region (a query op in this block): the local
        // arrays reach part Q, which writes lw back for part B.
        t = q.CommittedRayT() + la[(k + 2) & 7];
        lw[k & 3] += 1;
    }

    GroupMemoryBarrierWithGroupSync();
    Result r;
    r.t = t + la[(k + 3) & 7];
    r.bx = (float)gsD[gi ^ 5];
    const uint64_t w = gsL[gi ^ 9] + lw[gi & 3] + lw[k & 3];   // gi & 3 is k & 3 for some threads: read from the array
    r.by = (float)(uint)(w >> 32) + (float)((uint)w & 0xFFFF) * 0.25;   // one step past the diff tolerance
    r.hit = hit ? 1u : 0u;
    if (tid.x < width && tid.y < height) outBuf[tid.y * width + tid.x] = r;
}
