// Plan step 5 probe, part B of gs_orig.hlsl split by hand: everything after
// the query, from what parts A and Q left in the scratch buffer.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
struct Result { float t; float bx; float by; uint hit; };
RWStructuredBuffer<Result> outBuf : register(u0);
RWStructuredBuffer<float4> scratch : register(u1);

groupshared float gsT[64];

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    const uint slot = tid.y * ((width + 7) & ~7u) + tid.x;
    const float4 s = scratch[slot];
    const float j = s.x, t = s.y;
    const bool hit = s.z != 0.0;
    gsT[gi] = t;
    GroupMemoryBarrierWithGroupSync();
    Result r;
    r.t = t;
    r.bx = gsT[gi ^ 1];
    r.by = WaveReadLaneFirst(gsT[0]) + j;
    r.hit = hit ? 1u : 0u;
    if (tid.x < width && tid.y < height) outBuf[tid.y * width + tid.x] = r;
}
