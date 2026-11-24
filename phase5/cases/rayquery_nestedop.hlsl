// rayquery_nested.hlsl without FORCE_NON_OPAQUE (0.79.0): run on --geom
// --opaquemix --stack, geometries 0 and 2 opaque, so in those quadrants
// traversal commits the front layer itself, without the body, and the back
// one is never yielded; in 1 and 3 the body commits the back layer.
//
// A query traced INSIDE another query's Proceed loop (plan step 3 l, 0.79.0).
// Refused until then: the outer loop became an any-hit shader, which cannot
// call TraceRay.
//
// Run on --geom --stack: every ray meets the same quadrant in two layers. For
// each candidate the inner query looks back toward the camera from the
// candidate's point; the candidate is committed only when something is in the
// way, so the BACK layer is committed. Order-free: the front candidate is
// never committed and the back one always is, whichever comes first.
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

    RayQuery<RAY_FLAG_NONE> q1;
    q1.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q1.Proceed()) {
        if (q1.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        RayDesc back;
        back.Origin = ray.Origin + ray.Direction * q1.CandidateTriangleRayT();
        back.Direction = float3(0, 0, 1);
        back.TMin = 0.001; back.TMax = 100.0;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q2;
        q2.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, back);
        q2.Proceed();
        if (q2.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            q1.CommitNonOpaqueTriangleHit();
    }
    Result r = (Result)0;
    if (q1.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        r.t = q1.CommittedRayT();
        float2 b = q1.CommittedTriangleBarycentrics();
        r.bx = b.x; r.by = b.y + q1.CommittedInstanceIndex(); r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
