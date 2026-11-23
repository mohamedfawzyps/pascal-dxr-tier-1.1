// A procedural query meeting an OPAQUE triangle (0.69.0). RayQuery commits
// it during Proceed(), so CommittedStatus() is COMMITTED_TRIANGLE_HIT there.
// Until 0.69.0 a procedural-only query got an empty triangle hit group with
// no closest-hit, so the commit was never recorded: with raytest's `opaque`
// pattern on --mixed --contrib, 14450 rays came back wrong. Writes the
// committed status and t for every ray. The box's hit plane sits behind
// the triangle and the commit is range-checked, so the result is the
// same in any traversal order.
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
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            // A slab test against the unit box the AABB describes, done in
            // object space exactly as a DXR intersection shader would.
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (-0.25 - o.z) / d.z;   // behind the triangle: no tie, order-free
            float3 p = o + d * tz;
            if (tz > tMin && tz < q.CommittedRayT() &&
                abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
        }
    }

    Result r = (Result)0;
    r.bx = (float)q.CommittedStatus();
    r.t = q.CommittedStatus() != COMMITTED_NOTHING ? q.CommittedRayT() : -1.0;
    r.hit = 1;
    outBuf[tid.y * width + tid.x] = r;
}
