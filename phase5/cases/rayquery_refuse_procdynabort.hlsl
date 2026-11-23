// REFUSAL CASE (0.71.0): Abort() in a procedural query whose ray flags are
// computed at run time. The exact lowering traces it forced non-opaque and
// the intersection shader then takes the query's own opacity and cull flags
// as constants, which it cannot do here.
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

    const uint flags = (width & 1u) ? RAY_FLAG_CULL_OPAQUE : RAY_FLAG_NONE;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, flags, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.0 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < q.CommittedRayT() && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
            q.Abort();
        }
    }
    Result r = (Result)0;
    r.t = q.CommittedStatus() != COMMITTED_NOTHING ? q.CommittedRayT() : -1.0;
    r.hit = 1;
    outBuf[idx] = r;
}
