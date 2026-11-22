// State carried ACROSS Proceed iterations and OUT of the loop (plan step 3 d,
// 0.65.0): a count of candidates, the nearest candidate t seen, and a value
// picked from it, all used after the loop. Refused until 0.65.0 ("value
// defined in the Proceed loop is used after it"). The any-hit shader is one
// iteration; the spec keeps an any-hit's payload writes even when it ignores
// its hit ("Any modifications made to the ray payload up to this point in the
// any hit shader are preserved"), so the state travels in the payload.
//
// Needs --geom (four geometries) so the values differ per ray. The COUNT of
// candidates depends only on which triangles the ray crosses, not on order,
// so it is comparable with WARP; so are the minimum t and the value chosen
// with it.
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
    while (q.Proceed()) {
        const float t = q.CandidateTriangleRayT();
        count += 1;
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
    r.hit = (count > 0 || committed) ? 1 : 0;
    outBuf[tid.y * width + tid.x] = r;
}
