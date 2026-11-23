// CandidateProceduralPrimitiveNonOpaque() (0.70.0). Folded to true until
// 0.70.0, on the premise that only non-opaque procedural primitives reach a
// Proceed loop; every one does, and on an opaque box (raytest's `opaque`
// pattern) 7396 rays read it non-opaque. Now computed from the ray's flags and
// the record's word 2. The atomic adds 256 for an opaque candidate, 16 for a
// non-opaque one; the box's hit plane sits behind the mixed scene's triangle
// and the commit is range-checked, so the result is order-free.
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

    outBuf[tid.y * width + tid.x].hit = 0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            InterlockedAdd(outBuf[tid.y * width + tid.x].hit, q.CandidateProceduralPrimitiveNonOpaque() ? 16u : 256u);
            // A slab test against the unit box the AABB describes, done in
            // object space exactly as a DXR intersection shader would.
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (-0.25 - o.z) / d.z;   // behind the triangle: no tie, order-free
            float3 p = o + d * tz;
            if (tz > tMin && tz < q.CommittedRayT() &&
                abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
        }
    }

    outBuf[tid.y * width + tid.x].t = q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT ? q.CommittedRayT() : -1.0;
    InterlockedAdd(outBuf[tid.y * width + tid.x].hit, 1u);
}
