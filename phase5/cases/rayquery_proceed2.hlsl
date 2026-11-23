// Proceed() twice, one after the other, each with a body of its own (plan
// step 3 g, 0.74.0). The first body only looks (and a value it reads is used
// after), the second commits; neither commit can cull the other candidate,
// so on --stack both bodies run once whatever the order.
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
    float first = -1.0;
    if (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) InterlockedAdd(outBuf[idx].hit, 16u);
        first = q.CandidateTriangleBarycentrics().x;
    }
    if (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) InterlockedAdd(outBuf[idx].hit, 256u);
        if (q.CandidateTriangleBarycentrics().y > 0.3) q.CommitNonOpaqueTriangleHit();
    }
    const uint status = q.CommittedStatus();
    outBuf[idx].t = (float)status;
    outBuf[idx].bx = first;
    outBuf[idx].by = 0.0;
    InterlockedAdd(outBuf[idx].hit, 1u);
}
