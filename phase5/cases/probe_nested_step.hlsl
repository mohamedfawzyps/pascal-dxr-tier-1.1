// PROBE for 0.79.0, not a case: rayquery_nested.hlsl with the outer query
// STEPPED, as the lowering would do it. Each Proceed() is one search that
// keeps the nearest candidate past the last one, by (t, instance, geometry,
// primitive), and commits nothing; the body then runs where a trace can be
// made. A legal RayQuery order (front to back), so it must give the same.
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

    float lt = -1.0; uint li = 0, lg = 0, lp = 0;
    bool committed = false; float ct = tMax; float2 cb = 0; uint ci = 0;
    [loop] for (uint guard = 0; guard < 64; ++guard) {
        RayDesc sr = ray; sr.TMax = ct;
        RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> s;
        s.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, sr);
        bool found = false; float bt = 0; uint bi = 0, bg = 0, bp = 0; float2 bb = 0;
        while (s.Proceed()) {
            float t = s.CandidateTriangleRayT();
            uint i = s.CandidateInstanceIndex(), g = s.CandidateGeometryIndex(), p = s.CandidatePrimitiveIndex();
            bool past = t > lt || (t == lt && (i > li || (i == li && (g > lg || (g == lg && p > lp)))));
            bool nearer = !found || t < bt || (t == bt && (i < bi || (i == bi && (g < bg || (g == bg && p < bp)))));
            if (past && nearer) {
                found = true; bt = t; bi = i; bg = g; bp = p; bb = s.CandidateTriangleBarycentrics();
            }
        }
        if (!found) break;
        lt = bt; li = bi; lg = bg; lp = bp;
        // The body, now in the raygen.
        RayDesc back;
        back.Origin = ray.Origin + ray.Direction * bt;
        back.Direction = float3(0, 0, 1);
        back.TMin = 0.001; back.TMax = 100.0;
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q2;
        q2.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, back);
        q2.Proceed();
        if (q2.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            committed = true; ct = bt; cb = bb; ci = bi;
        }
    }
    Result r = (Result)0;
    if (committed) { r.t = ct; r.bx = cb.x; r.by = cb.y + ci; r.hit = 1; }
    outBuf[tid.y * width + tid.x] = r;
}
