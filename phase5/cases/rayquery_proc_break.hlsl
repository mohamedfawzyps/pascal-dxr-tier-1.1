// A break and a return out of a procedural Proceed body (plan step 3 e,
// 0.72.0). The body counts its runs (16 each) and, on a box it meets, either
// commits it and breaks, or returns; elsewhere it goes on. Each way out ends
// the search, so on --proc --stack a ray that leaves runs the body once, not
// twice, whichever box comes first; one that does not runs it twice. Only
// order-free results are written: the count, the committed status, and
// whether the code after the loop ran. An intersection shader can neither
// end the search nor write the payload, so this was refused on 0.71.0.
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
    outBuf[idx].by = 0.0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            InterlockedAdd(outBuf[idx].hit, 16u);
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.0 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < q.CommittedRayT() && abs(p.x) < 0.5 && abs(p.y) < 0.5) {
                if (p.x > 0.0) {
                    q.CommitProceduralPrimitiveHit(tz);
                    break;
                }
                if (p.y > 0.0) {
                    outBuf[idx].by = 5.0;
                    return;
                }
            }
        }
    }
    outBuf[idx].t = (float)q.CommittedStatus();
    outBuf[idx].bx = 1.0;
    InterlockedAdd(outBuf[idx].hit, 1u);   // so every ray is compared
}
