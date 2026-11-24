// A query traced and proceeded once per iteration of an ordinary loop
// (0.79.0). Its one Proceed() sits in the latch of that loop, which the
// analysis took for the query's Proceed loop until then. Three rays per
// pixel, the hits counted; FORCE_OPAQUE, so each Proceed() yields nothing.
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
    Result r = (Result)0;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    [loop] for (uint k = 0; k < 3; ++k) {
        RayDesc ray;
        ray.Origin    = float3(u * halfExtent + 0.1 * k, v * halfExtent, camZ);
        ray.Direction = float3(0, 0, -1);
        ray.TMin = tMin; ray.TMax = tMax;
        q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            r.hit += 1; r.t += q.CommittedRayT(); r.bx += q.CommittedTriangleBarycentrics().x;
        }
    }
    outBuf[tid.y * width + tid.x] = r;
}
