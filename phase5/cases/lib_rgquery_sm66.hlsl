// RayQuery in an APPLICATION's raygen (plan step 4, 0.80.0): the raygen of a
// DXR 1.0 library traces a ray of its own with TraceRay, through the
// application's hit group and miss, and runs an inline query beside it. The
// query is alpha-tested (a Proceed loop reading a resource and a cbuffer), so
// its loop becomes a generated any-hit.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float> alphaCut : register(t1);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);

struct Pay { float t; uint hit; };

[shader("raygeneration")] void RayGen() {
    uint2 p = DispatchRaysIndex().xy;
    float u = ((p.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((p.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    Pay pay; pay.t = 0; pay.hit = 0;
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, pay);

    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateTriangleBarycentrics().y < alphaCut[0])
            q.CommitNonOpaqueTriangleHit();
    }
    Result r = (Result)0;
    r.t = pay.t;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        float2 b = q.CommittedTriangleBarycentrics();
        r.bx = q.CommittedRayT(); r.by = b.y;
    }
    r.hit = pay.hit + 2 * (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1 : 0);
    outBuf[p.y * width + p.x] = r;
}

[shader("closesthit")] void ClosestHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.t = RayTCurrent(); p.hit = 1;
}

[shader("miss")] void Miss(inout Pay p) { p.hit = 0; }
