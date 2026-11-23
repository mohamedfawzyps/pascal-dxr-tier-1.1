// Abort() in a procedural Proceed body (0.68.0), observed order-free. With
// --proc --stack a ray through the box meets two procedural candidates, the
// NEAR box (t = camZ - 0.5) and the FAR one (t = camZ). The body aborts on the
// FIRST candidate it sees (nothing committed yet: CommittedRayT() is TMax),
// and may commit the near box only once something else has been committed.
// So on any correct implementation the query ends with the far box or with
// nothing, never with the near box, whichever candidate comes first. An
// intersection shader that ignores the Abort() lets traversal run on, and can
// commit the near box after the far one. Written: 1 where the near box ended
// up committed, else 0.
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
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.0 - o.z) / d.z;
            float3 p = o + d * tz;
            const float c = q.CommittedRayT();
            const bool first = c >= tMax;
            const bool farBox = tz > camZ - 0.25;
            if (tz > tMin && tz < c && abs(p.x) < 0.5 && abs(p.y) < 0.5 && (farBox || !first))
                q.CommitProceduralPrimitiveHit(tz);
            if (first) q.Abort();
        }
    }
    Result r = (Result)0;
    const bool nearCommitted = q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT &&
                               q.CommittedRayT() < camZ - 0.25;
    r.t = nearCommitted ? 1.0 : 0.0;
    r.hit = 1;
    outBuf[tid.y * width + tid.x] = r;
}
