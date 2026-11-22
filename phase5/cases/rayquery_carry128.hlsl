// A payload of exactly 128 bytes (0.67.0). rayquery_carryout's state plus one
// more loop-carried counter: 92 fixed bytes, 4 header phis, 4 values leaving
// the loop and the iteration flag. On the GTX 1070 a 128 byte payload builds
// and REMOVES THE DEVICE when dispatched (measured; 120 and 132 to 156 run),
// so the lowering pads it to 132. This case fails if the pad goes.
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
    uint count = 0;
    float nearest = 1e30;
    uint pick = 0;
    uint extra0 = 0;
    while (q.Proceed()) {
        const float t = q.CandidateTriangleRayT();
        count += 1;
        extra0 += 3;
        if (t < nearest) {
            nearest = t;
            pick = q.CandidatePrimitiveIndex() + 7;
        }
        if (q.CandidateTriangleBarycentrics().x > 0.25) q.CommitNonOpaqueTriangleHit();
    }

    // Every ray that saw a candidate counts as a "hit" for the comparison, so
    // the carried values are compared on all of them, not only where one was
    // committed; the committed status rides in by.
    Result r = (Result)0;
    const bool committed = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    r.t = nearest < 1e29 ? nearest : -1.0;
    r.bx = (float)count;
    r.by = (float)pick + (committed ? 1000.0 : 0.0);
    r.t += (float)(extra0) * 0.0001;
    r.hit = (count > 0 || committed) ? 1 : 0;
    outBuf[tid.y * width + tid.x] = r;
}
