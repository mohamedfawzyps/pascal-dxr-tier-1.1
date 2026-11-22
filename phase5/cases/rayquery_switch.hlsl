// A `switch` inside the Proceed loop and after it (plan step 3, 0.65.0).
//
// The rewriter's CFG model followed only `br`. A switch terminator spans
// several lines in the disassembly, so a block ending in one had no
// successors: the blocks it reaches looked unreachable, dominators and the
// Proceed loop were computed without them. 55 of 125 dumped Unreal shaders
// contain a switch; none had lost a block yet, which was luck.
//
// [forcecase] makes DXC emit a real switch instead of an if chain. The loop's
// switch decides per geometry (needs --geom) how a candidate is judged, with
// a case that commits only past a barycentric threshold, so each arm is
// visible in the image; the one after the loop picks what is written from
// the committed geometry.
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
    while (q.Proceed()) {
        const float2 b = q.CandidateTriangleBarycentrics();
        [forcecase] switch (q.CandidateGeometryIndex()) {
        case 0:
            q.CommitNonOpaqueTriangleHit();
            break;
        case 1:
            if (b.x > 0.3) q.CommitNonOpaqueTriangleHit();
            break;
        case 2:
            break;
        default:
            if (b.y < 0.5) q.CommitNonOpaqueTriangleHit();
            break;
        }
    }

    Result r = (Result)0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        [forcecase] switch (q.CommittedGeometryIndex()) {
        case 0: r.t = q.CommittedRayT(); break;
        case 1: r.t = q.CommittedRayT() + 10.0; break;
        default: r.t = q.CommittedRayT() + 20.0; break;
        }
        r.bx = q.CommittedTriangleBarycentrics().x;
        r.by = q.CommittedTriangleBarycentrics().y;
        r.hit = 1;
    }
    outBuf[tid.y * width + tid.x] = r;
}
