// Commit, then Abort(), in a procedural body, with the committed state read
// after the loop (0.71.0). The abort is reported as a hit of a reserved kind
// that ends the search, so the hit that WAS committed is the one recorded in
// the payload by the procedural any-hit, and both closest-hits leave the
// payload alone after an abort. The box's hit plane sits in FRONT of the
// mixed scene's triangle, so the box is the answer in any order: met first,
// it is committed and the search ends; met after the triangle, it is nearer
// and replaces it. Opaque or not (raytest's `opaque` pattern).
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
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            float3 o = q.CandidateObjectRayOrigin();
            float3 d = q.CandidateObjectRayDirection();
            float tz = (0.25 - o.z) / d.z;
            float3 p = o + d * tz;
            if (tz > tMin && tz < q.CommittedRayT() && abs(p.x) < 0.5 && abs(p.y) < 0.5)
                q.CommitProceduralPrimitiveHit(tz);
            q.Abort();
        }
    }
    Result r = (Result)0;
    r.bx = (float)q.CommittedStatus();
    r.t = q.CommittedStatus() != COMMITTED_NOTHING ? q.CommittedRayT() : -1.0;
    r.by = q.CommittedStatus() != COMMITTED_NOTHING ? (float)q.CommittedInstanceIndex() : -1.0;
    r.hit = 1;
    outBuf[idx] = r;
}
