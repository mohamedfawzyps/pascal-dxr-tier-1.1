// A `return` out of the Proceed loop (plan step 3 e, 0.72.0). A candidate
// that passes writes its own result and ends the shader; the code after the
// loop does not run for that ray.
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
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        const float2 b = q.CandidateTriangleBarycentrics();
        if (b.x > 0.6) {
            Result e = (Result)0;
            e.t = 2.0;
            e.by = b.y;
            e.hit = 3;
            outBuf[idx] = e;
            return;
        }
        if (b.y > 0.2) q.CommitNonOpaqueTriangleHit();
    }
    Result r = (Result)0;
    r.t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    r.hit = 1;
    outBuf[idx] = r;
}
