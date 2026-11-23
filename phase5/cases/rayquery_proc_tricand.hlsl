// A procedural query's body runs for TRIANGLE candidates too (0.69.0).
// RayQuery yields every non-opaque candidate to the body, whatever it commits,
// so the atomic before the CandidateType() test counts triangles as well.
// Until 0.69.0 a procedural-only query got an empty triangle hit group, and on
// --mixed --contrib 14450 triangle candidates went uncounted, silently.
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

    outBuf[idx].by = -1.0;
    outBuf[idx].hit = 0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        InterlockedAdd(outBuf[idx].hit, 16u);   // every candidate, either kind
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (-0.25 - o.z) / d.z;   // behind the triangle: no tie, order-free
            float3 p = o + d * tz;
            // A store and an atomic, both in the body.
            outBuf[idx].by = abs(p.x) + abs(p.y);
            uint old;
            InterlockedAdd(outBuf[idx].hit, 1u, old);
            // Against the committed t, as the spec requires of a procedural
            // commit ("The system does not do this work for procedural
            // primitives"), so --stack's two boxes are order-free too.
            if (tz > tMin && tz < q.CommittedRayT() && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
        }
    }
    outBuf[idx].t = q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT
                        ? q.CommittedRayT() : -1.0;
    outBuf[idx].bx = 1.0;
    InterlockedAdd(outBuf[idx].hit, 1u);   // so every ray is compared
}
