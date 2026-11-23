// A bare Proceed(), its result unused, the query not forced opaque (plan step
// 3 g, 0.74.0). Opaque candidates are committed by traversal; at the first
// non-opaque one Proceed() returns true and the shader never resumes, so it
// is not committed. With --geom --opaquemix two quadrants are opaque.
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
    q.Proceed();
    const uint status = q.CommittedStatus();
    Result r = (Result)0;
    r.t = status == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    r.hit = 1 + status;
    outBuf[idx] = r;
}
