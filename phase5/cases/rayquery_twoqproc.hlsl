// Two queries, both serving procedural boxes, with DIFFERENT bodies (0.77.0).
// Until 0.77.0 refused: several queries shared one hit group and chose their
// loop body by a query id in the payload, and an intersection shader has no
// payload to read it from. Now each query has hit shaders and records of its
// own. Run it on --mixed: a box and a triangle.
//
// Each body commits a box at a different plane, both nearer than the
// triangle at z = 0, so the answer does not depend on traversal order; a
// query served by the other's intersection shader reports the other's t.
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

    RayQuery<RAY_FLAG_NONE> q0;
    q0.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q0.Proceed()) {
        if (q0.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q0.CandidateObjectRayOrigin();
            float3 d = q0.CandidateObjectRayDirection();
            float tz = (0.5 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < tMax && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q0.CommitProceduralPrimitiveHit(tz);
        } else {
            float2 b = q0.CandidateTriangleBarycentrics();
            if (b.x + b.y < 0.75)
                q0.CommitNonOpaqueTriangleHit();
        }
    }
    const float t0 = q0.CommittedRayT();
    const uint s0 = q0.CommittedStatus();

    RayQuery<RAY_FLAG_NONE> q1;
    q1.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q1.Proceed()) {
        if (q1.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q1.CandidateObjectRayOrigin();
            float3 d = q1.CandidateObjectRayDirection();
            float tz = (0.25 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < tMax && p.x > -0.25 && abs(p.y) < 0.5)
                q1.CommitProceduralPrimitiveHit(tz);
        } else {
            float2 b = q1.CandidateTriangleBarycentrics();
            if (b.x > 0.25)
                q1.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    r.t = s0 ? t0 : 0.0;
    r.bx = q1.CommittedStatus() ? q1.CommittedRayT() : 0.0;
    r.by = (float)(s0 + 4 * q1.CommittedStatus());
    r.hit = (s0 | q1.CommittedStatus()) ? 1 : 0;
    outBuf[tid.y * width + tid.x] = r;
}
