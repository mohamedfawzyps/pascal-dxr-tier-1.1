// REFUSAL CASE (0.69.0): committed state read where a TRIANGLE candidate
// reaches it, in a query that commits procedural hits. The payload cannot hold
// procedural commits, which an intersection shader makes.
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
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && q.CommittedRayT() < tMax)
            InterlockedAdd(outBuf[tid.y * width + tid.x].hit, 1u);
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            // A slab test against the unit box the AABB describes, done in
            // object space exactly as a DXR intersection shader would.
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.0 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < tMax &&
                abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT) {
        r.t = q.CommittedRayT();
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
