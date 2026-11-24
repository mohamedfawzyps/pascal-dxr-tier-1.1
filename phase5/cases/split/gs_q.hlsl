// Plan step 5 probe, part Q of gs_orig.hlsl split by hand: the query alone,
// an ordinary RayQuery compute shader with no groupshared memory, barrier or
// wave op, so the shim lowers it as it lowers any. Reads the jitter part A
// left, writes the hit and its distance for part B.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<float4> scratch : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint slot = tid.y * ((width + 7) & ~7u) + tid.x;
    const float j = scratch[slot].x;
    const float2 ndc = float2(((tid.x + 0.5) / width) * 2.0 - 1.0, ((tid.y + 0.5) / height) * 2.0 - 1.0);
    RayDesc ray;
    ray.Origin = float3(ndc * halfExtent + float2(j, 0.0), camZ);
    ray.Direction = float3(0.0, 0.0, -1.0);
    ray.TMin = tMin;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    scratch[slot] = float4(j, hit ? q.CommittedRayT() : -1.0, hit ? 1.0 : 0.0, 0.0);
}
