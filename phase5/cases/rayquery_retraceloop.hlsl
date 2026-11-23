// One TraceRayInline run several times (plan step 3 f, 0.73.0): a loop
// traces the same query from three origins and adds up the hits.
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
    RayQuery<RAY_FLAG_NONE> q;
    uint hits = 0;
    float tsum = 0.0;
    [loop] for (uint k = 0; k < 3; ++k) {
        RayDesc r2 = ray;
        r2.Origin.x += 0.2 * k;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, r2);
        while (q.Proceed()) {
            if (q.CandidateTriangleBarycentrics().x > 0.3) q.CommitNonOpaqueTriangleHit();
        }
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            hits += 1;
            tsum += q.CommittedRayT();
        }
    }
    Result r = (Result)0;
    r.t = tsum;
    r.bx = (float)hits;
    r.hit = 1;
    outBuf[idx] = r;
}
