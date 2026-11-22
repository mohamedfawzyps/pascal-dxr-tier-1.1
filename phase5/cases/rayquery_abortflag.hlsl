// Unreal's Abort() (RayTracingReflectionReorientedParticleCS): a candidate
// passes, a flag is set, the query aborts, and only the flag is read after
// the loop; committed state never is (0.67.0). The any-hit then ends the
// search on that candidate, which commits a hit nothing reads, instead of
// letting traversal run to its end. With --stack every ray meets two
// candidates; whether ANY passes does not depend on their order.
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
    bool found = false;
    while (q.Proceed()) {
        if (q.CandidateTriangleBarycentrics().x > 0.6) {
            found = true;
            q.Abort();
        }
    }
    Result r = (Result)0;
    r.t = found ? 1.0 : 0.0;
    r.hit = 1;
    outBuf[tid.y * width + tid.x] = r;
}
