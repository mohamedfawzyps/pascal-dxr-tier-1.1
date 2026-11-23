// Two ways out of the Proceed loop with different code after each (plan step
// 3 e, 0.72.0): leaving through a break runs one tail, running out of
// candidates the other; a value from the iteration that broke is used after.
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
    bool broke = false;
    float at = 0.0;
    while (q.Proceed()) {
        const float t = q.CandidateTriangleRayT();
        if (q.CandidateTriangleBarycentrics().x > 0.5) {
            at = t;
            broke = true;
            break;
        }
    }
    Result r = (Result)0;
    if (broke) {
        r.t = at;
        r.bx = 7.0;
    } else {
        r.t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
        r.bx = 3.0;
    }
    r.hit = 1;
    outBuf[idx] = r;
}
