// Writes in a procedural Proceed loop body (plan step 3 h, 0.68.0): a store
// and an atomic in the body that becomes the INTERSECTION shader. Refused
// until 0.68.0 because the spec let an intersection shader run more than once
// per primitive; with D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_
// INVOCATION, which the proxy sets on every geometry, it "must not
// redundantly invoke the intersection shader for the same ray and procedural
// primitive instance" (Raytracing.md v1.49). The atomic counts how many times
// the body runs per ray, which is what a redundant invocation would change.
// With --proc a ray meets one AABB; with --proc --stack two, both giving the
// same stored value, so it is order-free either way.
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

    outBuf[idx].by = -1.0;
    outBuf[idx].hit = 0;
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.0 - o.z) / d.z;
            float3 p = o + d * tz;
            // A store and an atomic, both in the body.
            outBuf[idx].by = abs(p.x) + abs(p.y);
            uint old;
            InterlockedAdd(outBuf[idx].hit, 1u, old);
            // Against the committed t, as the spec requires of a procedural
            // commit ("The system does not do this work for procedural
            // primitives"), so --stack's two boxes are order-free too.
            if (tz > tMin && tz < q.CommittedRayT() && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
        }
    }
    outBuf[idx].t = q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT
                        ? q.CommittedRayT() : -1.0;
    outBuf[idx].bx = 1.0;
    InterlockedAdd(outBuf[idx].hit, 1u);   // so every ray is compared
}
