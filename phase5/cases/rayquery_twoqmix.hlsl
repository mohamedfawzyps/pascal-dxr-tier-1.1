// Two queries, only the second serving procedural boxes (0.77.0): the first
// is an alpha-tested triangle query, whose records for a box are the
// procedural record that reports nothing (a box reaches its body and does
// nothing there), and the second commits boxes and
// triangles. Run it on --mixed.
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
        if (q0.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            q0.CandidateTriangleBarycentrics().y < 0.5)
            q0.CommitNonOpaqueTriangleHit();
    }
    const uint s0 = q0.CommittedStatus();
    const float t0 = s0 ? q0.CommittedRayT() : 0.0;

    RayQuery<RAY_FLAG_NONE> q1;
    q1.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q1.Proceed()) {
        if (q1.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q1.CandidateObjectRayOrigin();
            float3 d = q1.CandidateObjectRayDirection();
            float tz = (0.25 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < tMax && abs(p.x) < 0.5 && p.y < 0.25)
                q1.CommitProceduralPrimitiveHit(tz);
        } else {
            q1.CommitNonOpaqueTriangleHit();
        }
    }

    Result r = (Result)0;
    r.t = t0;
    r.bx = q1.CommittedStatus() ? q1.CommittedRayT() : 0.0;
    r.by = (float)(s0 + 4 * q1.CommittedStatus());
    r.hit = (s0 | q1.CommittedStatus()) ? 1 : 0;
    outBuf[tid.y * width + tid.x] = r;
}
