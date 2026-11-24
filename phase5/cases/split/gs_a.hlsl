// Plan step 5 probe, part A of gs_orig.hlsl split by hand: everything before
// the query, ending by writing what the rest needs (the jitter) to the
// scratch buffer at u1, one slot per thread of the whole grid.
cbuffer CB : register(b0) {
    uint width; uint height; float halfExtent; float camZ;
    float tMin; float tMax; float2 _pad;
};
RWStructuredBuffer<float4> scratch : register(u1);

groupshared float gsJ[64];

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gsJ[gi] = (float)((tid.x * 7 + tid.y * 13) % 17) * 0.015625;   // exact on every device
    GroupMemoryBarrierWithGroupSync();
    const float j = gsJ[63 - gi];
    const uint slot = tid.y * ((width + 7) & ~7u) + tid.x;
    scratch[slot] = float4(j, 0.0, 0.0, 0.0);
}
