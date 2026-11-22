// Writes inside the Proceed loop body (plan step 3 h, 0.67.0): a plain store
// and an atomic, which were refused ("the any-hit shader it becomes runs a
// different number of times"). With NO_DUPLICATE_ANYHIT_INVOCATION on every
// geometry the any-hit runs once per candidate RayQuery would yield, so these
// are the same writes. With --geom a ray meets one candidate, so the values
// are comparable with WARP; the atomic counts candidates, which does not
// depend on their order either way.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    const uint idx = tid.y * width + tid.x;
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    outBuf[idx].by = 0.0;
    outBuf[idx].hit = 0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        // A store and an atomic, both in the loop body.
        outBuf[idx].by = (float)q.CandidatePrimitiveIndex() + 100.0 * (q.CandidateTriangleBarycentrics().x > 0.5 ? 1.0 : 0.0);
        uint old;
        InterlockedAdd(outBuf[idx].hit, 1u, old);
        if (q.CandidateTriangleBarycentrics().y > 0.3) q.CommitNonOpaqueTriangleHit();
    }
    outBuf[idx].t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    outBuf[idx].bx = 1.0;
    InterlockedAdd(outBuf[idx].hit, 1u);   // so every ray is compared
}
