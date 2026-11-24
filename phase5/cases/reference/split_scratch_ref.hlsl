// What the split's scratch buffer compiles to (0.84.0): a RWByteAddressBuffer
// at u0 in the shim's register space, one dword loaded and one stored, at 6.5
// and 6.6. The split pass writes exactly these shapes.
RWByteAddressBuffer scratch : register(u0, space2147418112);
[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    uint v = scratch.Load(gid.x * 4);
    scratch.Store(gi * 4, v + 1);
}
