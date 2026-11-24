// A query traced again inside its own Proceed loop, its body triangle-only
// (0.79.0): stepped (nest.py), the retrace starting a new search the next
// Proceed() steps through. At the first candidate the query is traced again
// along a ray moved across, so what is committed comes from the second ray.
// Run on --geom --stack: order-free, the nearest candidate passing the test is
// committed.
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
    q.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, ray);
    uint n = 0;
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        if (n == 0) {
            n = 1;
            RayDesc moved = ray;
            moved.Origin.x += 0.25 * halfExtent;
            q.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, moved);
            continue;
        }
        if (q.CandidateTriangleBarycentrics().y < 0.5)
            q.CommitNonOpaqueTriangleHit();
    }
    Result r = (Result)0;
    r.t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    r.hit = 1;
    outBuf[idx] = r;
}
