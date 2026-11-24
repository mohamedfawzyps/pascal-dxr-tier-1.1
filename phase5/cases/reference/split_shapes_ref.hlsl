// What DXC makes of groupshared variables and local arrays of every shape a
// split might carry across the query (0.86.0, plan step 5 c). Compiled with
// -T cs_6_5 -enable-16bit-types; the .ll is read, not run.
RWStructuredBuffer<float4> o : register(u0);
cbuffer CB : register(b0) { uint n; uint m; };

struct S { float a; uint b; float2 c; };
groupshared float4 g4[64];
groupshared uint2 g2[8][8];
groupshared S gs[16];
groupshared half gh[64];
groupshared double gd[8];
groupshared bool gb[64];
groupshared uint gscalar;

[numthreads(8, 8, 1)]
void main(uint gi : SV_GroupIndex) {
    float la[8];
    uint2 lb[4];
    S ls[2];
    for (uint i = 0; i < 8; ++i) la[i] = i * n;
    for (uint i = 0; i < 4; ++i) lb[i] = uint2(i, n);
    ls[0].a = n; ls[1].a = m; ls[n & 1].b = gi; ls[0].c = float2(1, 2); ls[1].c = float2(3, 4);
    g4[gi] = float4(gi, 0, 0, 1);
    g2[gi & 7][gi >> 3] = uint2(gi, n);
    gs[gi & 15].a = gi; gs[gi & 15].b = n; gs[gi & 15].c = float2(gi, n);
    gh[gi] = (half)gi;
    gd[gi & 7] = gi;
    gb[gi] = (gi & 1) != 0;
    if (gi == 0) gscalar = n;
    GroupMemoryBarrierWithGroupSync();
    float4 r = g4[63 - gi] + float4(g2[m & 7][n & 7], gs[m & 15].a + gs[m & 15].b, gs[n & 15].c.y);
    r.x += gh[m & 63] + (float)gd[n & 7] + (gb[m & 63] ? 1 : 0) + gscalar;
    r.y += la[m & 7] + lb[n & 3].y + ls[m & 1].a + ls[n & 1].b + ls[m & 1].c.x;
    o[gi] = r;
}
