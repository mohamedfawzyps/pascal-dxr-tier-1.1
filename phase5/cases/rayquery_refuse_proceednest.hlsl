// Refused by name (0.74.0): a Proceed() inside the body of the one before
// it, without a loop; the first body's code after the inner call would run
// for the second candidate.
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
    if (q.Proceed()) {
        n = q.CandidateTriangleBarycentrics().x > 0.5 ? 1 : 2;
        if (q.Proceed()) {
            q.CommitNonOpaqueTriangleHit();
        }
    }
    Result r = (Result)0;
    r.t = (float)q.CommittedStatus();
    r.hit = n;
    outBuf[idx] = r;
}
