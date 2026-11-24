// Refused by name (0.73.0): the query traced again inside its own Proceed
// loop, which becomes an any-hit shader and cannot call TraceRay. Since
// 0.79.0 the query is stepped (nest.py), and refused because a procedural
// candidate would reach the retrace (the stepped search sees triangles only).
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
    uint n = 0;
    while (q.Proceed()) {
        if (++n > 1) break;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    }
    Result r = (Result)0;
    r.t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    r.hit = 1;
    outBuf[idx] = r;
}
