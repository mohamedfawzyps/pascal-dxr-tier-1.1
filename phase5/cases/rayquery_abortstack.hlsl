// Nothing commits after Abort() (0.67.0), observed without depending on the
// order candidates arrive in, which is implementation-defined. The body saves
// the committed status and t at the moment it aborts; after the loop they
// must be unchanged, on any correct implementation, whichever candidate came
// first. With --stackmix a ray meets a non-opaque candidate in front and an
// opaque one behind it (--stack --opaquemix does NOT: both layers share one
// structure, so one opacity per quadrant); before 0.67.0 traversal committed
// the opaque one after the abort. Only the violation flag is written.
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
    uint aborted = 0;
    uint atStatus = 0;
    float atT = 0.0;
    while (q.Proceed()) {
        if (q.CandidateTriangleBarycentrics().x > 0.2) {
            atStatus = q.CommittedStatus();
            atT = atStatus == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : 0.0;
            aborted = 1;
            q.Abort();
        }
    }
    const uint status = q.CommittedStatus();
    const float t = status == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : 0.0;
    Result r = (Result)0;
    r.t = (aborted != 0 && (status != atStatus || t != atT)) ? 1.0 : 0.0;
    r.hit = 1;
    outBuf[tid.y * width + tid.x] = r;
}
