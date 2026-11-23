// Refused by name (0.72.0): the candidate read AFTER the Proceed loop. A
// break leaves with a candidate still current, but running out of candidates
// does not, and DXC puts the read where both ways meet, so code that did not
// leave through the break reaches it too. Neither the raygen nor the any-hit
// can answer it there.
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
        if (q.CandidateTriangleBarycentrics().x > 0.5) break;
    }
    Result r = (Result)0;
    r.t = q.CandidateTriangleRayT();
    r.hit = 1;
    outBuf[idx] = r;
}
