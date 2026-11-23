// More than 16 carried values (plan step 3 j, 0.76.0): 38 integer sums as
// loop state that leaves the loop, 77 payload values, a 400 byte payload.
// Each sum is a different nonlinear function of the candidate, so DXC cannot
// merge them; sums do not depend on the order candidates arrive in. Refused
// until 0.76.0 (16 values); the GTX 1070 measured right to 4096 bytes.
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
    uint a0 = 0;
    uint a1 = 0;
    uint a2 = 0;
    uint a3 = 0;
    uint a4 = 0;
    uint a5 = 0;
    uint a6 = 0;
    uint a7 = 0;
    uint a8 = 0;
    uint a9 = 0;
    uint a10 = 0;
    uint a11 = 0;
    uint a12 = 0;
    uint a13 = 0;
    uint a14 = 0;
    uint a15 = 0;
    uint a16 = 0;
    uint a17 = 0;
    uint a18 = 0;
    uint a19 = 0;
    uint a20 = 0;
    uint a21 = 0;
    uint a22 = 0;
    uint a23 = 0;
    uint a24 = 0;
    uint a25 = 0;
    uint a26 = 0;
    uint a27 = 0;
    uint a28 = 0;
    uint a29 = 0;
    uint a30 = 0;
    uint a31 = 0;
    uint a32 = 0;
    uint a33 = 0;
    uint a34 = 0;
    uint a35 = 0;
    uint a36 = 0;
    uint a37 = 0;
    while (q.Proceed()) {
        const uint p = q.CandidatePrimitiveIndex() + q.CandidateGeometryIndex() * 7 + 3;
        a0 += ((p * 3u) ^ (p >> 0)) + 0u;
        a1 += ((p * 5u) ^ (p >> 1)) + 1u;
        a2 += ((p * 7u) ^ (p >> 2)) + 2u;
        a3 += ((p * 9u) ^ (p >> 3)) + 3u;
        a4 += ((p * 11u) ^ (p >> 4)) + 4u;
        a5 += ((p * 13u) ^ (p >> 0)) + 5u;
        a6 += ((p * 15u) ^ (p >> 1)) + 6u;
        a7 += ((p * 17u) ^ (p >> 2)) + 7u;
        a8 += ((p * 19u) ^ (p >> 3)) + 8u;
        a9 += ((p * 21u) ^ (p >> 4)) + 9u;
        a10 += ((p * 23u) ^ (p >> 0)) + 10u;
        a11 += ((p * 25u) ^ (p >> 1)) + 11u;
        a12 += ((p * 27u) ^ (p >> 2)) + 12u;
        a13 += ((p * 29u) ^ (p >> 3)) + 13u;
        a14 += ((p * 31u) ^ (p >> 4)) + 14u;
        a15 += ((p * 33u) ^ (p >> 0)) + 15u;
        a16 += ((p * 35u) ^ (p >> 1)) + 16u;
        a17 += ((p * 37u) ^ (p >> 2)) + 17u;
        a18 += ((p * 39u) ^ (p >> 3)) + 18u;
        a19 += ((p * 41u) ^ (p >> 4)) + 19u;
        a20 += ((p * 43u) ^ (p >> 0)) + 20u;
        a21 += ((p * 45u) ^ (p >> 1)) + 21u;
        a22 += ((p * 47u) ^ (p >> 2)) + 22u;
        a23 += ((p * 49u) ^ (p >> 3)) + 23u;
        a24 += ((p * 51u) ^ (p >> 4)) + 24u;
        a25 += ((p * 53u) ^ (p >> 0)) + 25u;
        a26 += ((p * 55u) ^ (p >> 1)) + 26u;
        a27 += ((p * 57u) ^ (p >> 2)) + 27u;
        a28 += ((p * 59u) ^ (p >> 3)) + 28u;
        a29 += ((p * 61u) ^ (p >> 4)) + 29u;
        a30 += ((p * 63u) ^ (p >> 0)) + 30u;
        a31 += ((p * 65u) ^ (p >> 1)) + 31u;
        a32 += ((p * 67u) ^ (p >> 2)) + 32u;
        a33 += ((p * 69u) ^ (p >> 3)) + 33u;
        a34 += ((p * 71u) ^ (p >> 4)) + 34u;
        a35 += ((p * 73u) ^ (p >> 0)) + 35u;
        a36 += ((p * 75u) ^ (p >> 1)) + 36u;
        a37 += ((p * 77u) ^ (p >> 2)) + 37u;
        if (q.CandidateTriangleBarycentrics().x > 0.3) q.CommitNonOpaqueTriangleHit();
    }
    const uint s = a0 * 1u + a1 * 3u + a2 * 5u + a3 * 7u + a4 * 9u + a5 * 11u + a6 * 13u + a7 * 15u + a8 * 17u + a9 * 19u + a10 * 21u + a11 * 23u + a12 * 25u + a13 * 27u + a14 * 29u + a15 * 31u + a16 * 33u + a17 * 35u + a18 * 37u + a19 * 39u + a20 * 41u + a21 * 43u + a22 * 45u + a23 * 47u + a24 * 49u + a25 * 51u + a26 * 53u + a27 * 55u + a28 * 57u + a29 * 59u + a30 * 61u + a31 * 63u + a32 * 65u + a33 * 67u + a34 * 69u + a35 * 71u + a36 * 73u + a37 * 75u;
    Result r = (Result)0;
    r.t = (float)(s & 0xFFFFu);
    r.bx = (float)((s >> 16) & 0xFFFFu);
    r.hit = q.CommittedStatus() + 1;
    outBuf[idx] = r;
}
