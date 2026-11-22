// Two queries one after the other, both reading committed state inside their
// Proceed loops (0.66.0): the shared any-hit dispatches to each query's own
// opacity block. Needs --geom (single candidates) and --opaquemix.
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
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    uint seen = 0;
    while (q.Proceed()) {
        seen = seen * 4 + q.CommittedStatus() + 1;
        if (q.CandidateTriangleBarycentrics().x > 0.2 && q.CandidateTriangleRayT() < q.CommittedRayT())
            q.CommitNonOpaqueTriangleHit();
    }
    const float t1 = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;

    // The same ray mirrored in x, so it meets another quadrant.
    ray.Origin.x = -ray.Origin.x;
    RayQuery<RAY_FLAG_NONE> q2;
    q2.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    uint seen2 = 0;
    while (q2.Proceed()) {
        seen2 = seen2 * 4 + q2.CommittedStatus() + 1;
        if (q2.CandidateTriangleBarycentrics().y > 0.2 && q2.CandidateTriangleRayT() < q2.CommittedRayT())
            q2.CommitNonOpaqueTriangleHit();
    }

    Result r = (Result)0;
    r.t = t1;
    r.bx = q2.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q2.CommittedRayT() : -1.0;
    r.by = (float)(seen * 64 + seen2);
    r.hit = 1;
    outBuf[tid.y * width + tid.x] = r;
}
