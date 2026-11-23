// Abort() in a procedural body, exact (0.71.0), observed through a write.
// The body counts its runs, commits the box it meets, and aborts. RayQuery
// stops at the first candidate, whichever it is, so the body runs ONCE per
// ray on any implementation. With --proc --stack a ray through the box meets
// two; until 0.71.0 the intersection shader could not stop traversal, so a
// write beside an Abort() there was refused.
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

    outBuf[idx].hit = 0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            InterlockedAdd(outBuf[idx].hit, 16u);
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.0 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < q.CommittedRayT() && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
            q.Abort();
        }
    }
    outBuf[idx].t = 0.0;
    outBuf[idx].bx = 1.0;
    outBuf[idx].by = 0.0;
    InterlockedAdd(outBuf[idx].hit, 1u);   // so every ray is compared
}
