// Abort() with committed state read after it (0.67.0). Traversal carried on
// after the abort and an OPAQUE candidate met later was committed by
// traversal itself, which RayQuery never does. Now the query is traced
// forced non-opaque and the aborted any-hit rejects every later candidate.
// With --geom --opaquemix a ray meets one candidate, so the result is
// order-independent; the opaque quadrants are committed without the body.
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
    while (q.Proceed()) {
        const float2 b = q.CandidateTriangleBarycentrics();
        if (b.x > 0.5) {
            q.CommitNonOpaqueTriangleHit();   // commit, then abort
            q.Abort();
        } else if (b.y > 0.5) {
            q.Abort();                        // a bare abort
        }
    }
    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t = q.CommittedRayT();
        r.bx = q.CommittedTriangleBarycentrics().x;
        r.by = (float)q.CommittedGeometryIndex();
    } else {
        r.t = -1.0;
    }
    r.hit = 1;
    outBuf[tid.y * width + tid.x] = r;
}
