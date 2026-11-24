// Compiled at cs_6_6 for its name: the createHandleFromBinding form.
// As rayquery_dynloopc.hlsl, but the slot is also written BEFORE the search, so DXC
// creates the handle there and the loop reuses it: the handle is rebuilt in the
// any-hit from its index, which travels in the payload.
//
// Plan step 3 l (0.78.0): a resource array indexed at run time and used INSIDE
// the Proceed loop, refused until then ("the index is computed in the raygen").
// --table binds the real output at slot 2 and decoys at 0, 1 and 3, so a wrong
// index writes nowhere visible. The scene is opaque, so the ray forces non-opaque. Each candidate's values are written in the loop
// body; nothing is written after it.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RaytracingAccelerationStructure scene : register(t0);
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBufs[4] : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= width || tid.y >= height) return;
    float u = ((tid.x + 0.5) / width)  * 2.0 - 1.0;
    float v = ((tid.y + 0.5) / height) * 2.0 - 1.0;
    RayDesc ray;
    ray.Origin    = float3(u * halfExtent, v * halfExtent, camZ);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = tMin; ray.TMax = tMax;

    uint pick = width / 96;
    uint pix = tid.y * width + tid.x;
    outBufs[pick][pix] = (Result)0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
        float2 b = q.CandidateTriangleBarycentrics();
        if (b.y < 0.5) {
            Result r;
            r.t = q.CandidateTriangleRayT(); r.bx = b.x; r.by = b.y; r.hit = 1;
            outBufs[pick][pix] = r;
            q.CommitNonOpaqueTriangleHit();
        }
    }
}
