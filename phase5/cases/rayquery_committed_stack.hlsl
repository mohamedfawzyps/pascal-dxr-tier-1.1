// rayquery_committed.hlsl, one variant.
#define ORDERFREE 1
// With --stack: two candidates per ray, so only the order-free result, the
// final nearest hit, is compared (0.66.0).
// Committed state read INSIDE the Proceed loop (plan step 3 c, 0.66.0).
//
// Refused until 0.66.0: the any-hit is a separate invocation and cannot see
// the committed hit. Now it keeps it in the payload, and because an OPAQUE
// candidate is committed without reaching an any-hit, the ray is traced
// forced non-opaque and the any-hit applies the original opacity itself.
//
// What WARP can check. The order candidates arrive in is implementation-
// defined, and what a loop body sees as committed depends on it, so only
// order-independent results are compared:
// - with --geom (four quadrants, one triangle each) a ray meets at most ONE
//   candidate, so what it sees committed is exactly the start: nothing, and
//   t = TMax. With --opaquemix two quadrants are opaque: committed without
//   the body ever running, which the carried `seen` shows.
// - the commit rule is "nearer than what is committed", whose final result
//   is the nearest passing candidate in any order.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

#ifndef QFLAGS
#define QFLAGS RAY_FLAG_NONE
#endif

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    RayQuery<QFLAGS> q;
    q.TraceRayInline(scene, QFLAGS, 0xFF, ray);
    uint seen = 0;          // what the body saw committed, packed
    float seenT = -1.0;     // the committed t it saw
    while (q.Proceed()) {
        const float t = q.CandidateTriangleRayT();
        seen = seen * 4 + q.CommittedStatus() + 1;
        seenT = q.CommittedRayT();
        if (q.CandidateTriangleBarycentrics().x > 0.2 && t < q.CommittedRayT())
            q.CommitNonOpaqueTriangleHit();
    }

    Result r = (Result)0;
    const bool committed = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    r.t = committed ? q.CommittedRayT() : -1.0;
#ifdef ORDERFREE
    r.bx = 0.0;
    r.by = committed ? 1000.0 * (q.CommittedGeometryIndex() + 1) : 0.0;
#else
    r.bx = seenT;
    r.by = (float)seen + (committed ? 1000.0 * (q.CommittedGeometryIndex() + 1) : 0.0);
#endif
    r.hit = 1;
    outBuf[tid.y * width + tid.x] = r;
}
