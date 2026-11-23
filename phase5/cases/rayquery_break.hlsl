// A `break` out of the Proceed loop (plan step 3 e, 0.72.0). A candidate
// that passes is committed and the loop is left; the rest of the body is
// skipped, and so is every later candidate: on --stack a ray that breaks
// runs the body once, not twice (the atomic in `hit`; `seen` is loop state
// leaving through the break).
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
    uint seen = 0;
    outBuf[idx].hit = 0;
    while (q.Proceed()) {
        seen += 1;
        // A write per candidate: a search the break did not end runs it again.
        // Triangles only, so no box candidate needs this body (the scene has none).
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) InterlockedAdd(outBuf[idx].hit, 1u);
        const float2 b = q.CandidateTriangleBarycentrics();
        // Left without a commit: the other layer is always still to come, so
        // a search this did not end shows in the count whatever the order.
        if (b.x > 0.6) break;
        if (b.x > 0.3) {
            q.CommitNonOpaqueTriangleHit();
            break;
        }
        if (b.y > 0.5) q.CommitNonOpaqueTriangleHit();
    }
    // Status and barycentrics, not t: on --stack both layers have the same
    // barycentrics, so which one came first does not show.
    const uint status = q.CommittedStatus();
    outBuf[idx].t = (float)status;
    outBuf[idx].by = status == COMMITTED_TRIANGLE_HIT ? q.CommittedTriangleBarycentrics().x : -1.0;
    outBuf[idx].bx = (float)seen;
    InterlockedAdd(outBuf[idx].hit, 1u);   // so every ray is compared
}
