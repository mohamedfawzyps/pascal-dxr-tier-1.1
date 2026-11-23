// Carried values of types other than i32, float and i1 (plan step 3 j,
// 0.76.0): a double, a 64-bit integer and a min16float read from a UAV
// before the loop and used in its body, and a double and a 64-bit integer
// as loop state that leaves the loop. Integer and exact-float arithmetic
// only, so the sums do not depend on the order candidates arrive in.
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

    // Read from the output buffer the harness clears, so the values are
    // caller state the any-hit cannot read again (a UAV), not constants.
    const uint seed = outBuf[idx].hit + idx;
    const double dbase = (double)seed * 0.5 + 1.0;
    const uint64_t lbase = ((uint64_t)seed << 33) | 5;
    const min16float hbase = (min16float)(seed & 7);
    outBuf[idx].hit = 0;

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    double dsum = 0.0;
    uint64_t lsum = 0;
    while (q.Proceed()) {
        const uint p = q.CandidatePrimitiveIndex() + 1;
        dsum += dbase * (double)p;
        lsum += lbase + (uint64_t)p;
        if ((float)hbase + q.CandidateTriangleBarycentrics().x > 0.3) q.CommitNonOpaqueTriangleHit();
    }
    Result r = (Result)0;
    r.t = (float)(dsum - dbase * (double)(uint)(dsum / dbase));
    r.bx = (float)(uint)(lsum >> 33);
    r.by = (float)(uint)(lsum & 0xFFFF);
    r.hit = q.CommittedStatus() + 1;
    outBuf[idx] = r;
}
