// Refused by name (0.75.0): Abort() between two Proceed() calls without a
// loop. Whether the second call yields depends on code that, lowered, runs
// after the search.
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
    outBuf[idx].hit = 0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    if (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) InterlockedAdd(outBuf[idx].hit, 16u);
    }
    if (tid.x % 3 == 0) q.Abort();
    if (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) InterlockedAdd(outBuf[idx].hit, 256u);
        if (q.CandidateTriangleBarycentrics().x > 0.3) q.CommitNonOpaqueTriangleHit();
    }
    const uint status = q.CommittedStatus();
    outBuf[idx].t = status == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    outBuf[idx].bx = (float)status;
    outBuf[idx].by = 0.0;
    InterlockedAdd(outBuf[idx].hit, 1u);   // so every ray is compared
}
