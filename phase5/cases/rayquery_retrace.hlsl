// One query traced twice (plan step 3 f, 0.73.0): TraceRayInline again on
// the same object starts a new search, which forgets the first one's
// committed hit. The second ray is shifted, so the two answers differ.
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
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateTriangleBarycentrics().x > 0.3) q.CommitNonOpaqueTriangleHit();
    }
    const float t1 = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    ray.Origin.x += 0.25;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateTriangleBarycentrics().y > 0.3) q.CommitNonOpaqueTriangleHit();
    }
    Result r = (Result)0;
    r.t = t1;
    r.bx = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedTriangleBarycentrics().x : -1.0;
    r.hit = 1;
    outBuf[idx] = r;
}
