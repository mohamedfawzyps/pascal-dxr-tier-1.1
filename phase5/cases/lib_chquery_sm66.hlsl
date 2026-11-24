// lib_chquery.hlsl at 6.6 (0.82.0): the same closest-hit and miss queries. As there:
// The raygen traces a ray of its own; the closest-hit it reaches runs an
// alpha-tested query from the hit point, and the miss one from a point beside
// the ray, both writing what the query committed into the payload. A
// closest-hit and a miss can call TraceRay, so their queries lower as a
// raygen's do, one recursion level deeper.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
StructuredBuffer<float> alphaCut : register(t1);
RWStructuredBuffer<uint4> outBuf : register(u0);

struct Pay { float t; uint hit; float qt; uint qhit; };

uint Query(float3 origin, inout float t) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateTriangleBarycentrics().y < alphaCut[0])
            q.CommitNonOpaqueTriangleHit();
    }
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0;
    t = q.CommittedRayT();
    return 1 + q.CommittedPrimitiveIndex();
}

[shader("raygeneration")] void RayGen() {
    uint2 p = DispatchRaysIndex().xy;
    float u = ((p.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((p.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;
    Pay pay; pay.t = 0; pay.hit = 0; pay.qt = 0; pay.qhit = 0;
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, pay);
    outBuf[p.y * width + p.x] = uint4(asuint(pay.t), pay.hit, asuint(pay.qt), pay.qhit);
}

[shader("closesthit")] void ClosestHit(inout Pay p, BuiltInTriangleIntersectionAttributes a) {
    p.t = RayTCurrent();
    p.hit = 1;
    float t = 0;
    float3 o = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    p.qhit = Query(float3(o.x + 0.25, o.y, o.z + 0.5), t);
    p.qt = t;
}

[shader("miss")] void Miss(inout Pay p) {
    p.hit = 0;
    float t = 0;
    p.qhit = 100 + Query(WorldRayOrigin() * 0.5, t);
    p.qt = t;
}
