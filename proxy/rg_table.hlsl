// Copies an application's shader records into the shim's table for a
// pipeline whose raygen, closest-hit or miss uses RayQuery (0.82.0, see
// proxy/rg_query.h), writing the arguments the shim appended to those
// shaders' local root signatures (the scene copy's address, the miss index)
// into every record of theirs.
//
// One thread per record. The record is copied by ADDRESS through a root SRV,
// at the shim's stride, the tail zeroed, as geom_table.hlsl does. If its
// shader identifier is one of `groups`, that group's dwords go at its offset.
//
//   meta, dwords:  per group k, at 11 * k: 8 identifier dwords, the offset in
//                  the record (bytes), how many dwords, and where in meta
//                  they start.
//
// Built with: dxc -T cs_6_0 -E main -Vn g_rgTableCS -Fh proxy\rg_table_cs.h proxy\rg_table.hlsl
#define RS "RootConstants(num32BitConstants=4, b0), SRV(t0), SRV(t1), UAV(u0)"

cbuffer P : register(b0) { uint records; uint srcStride; uint dstStride; uint groups; };
ByteAddressBuffer src : register(t0);
ByteAddressBuffer meta : register(t1);
RWByteAddressBuffer dst : register(u0);

[RootSignature(RS)]
[numthreads(64, 1, 1)]
void main(uint r : SV_DispatchThreadID) {
    if (r >= records) return;
    const uint s = r * srcStride, d = r * dstStride;
    for (uint b = 0; b < dstStride; b += 4)
        dst.Store(d + b, b < srcStride ? src.Load(s + b) : 0u);
    for (uint k = 0; k < groups; ++k) {
        bool same = true;
        for (uint w = 0; w < 8; ++w)
            same = same && src.Load(s + w * 4) == meta.Load((k * 11 + w) * 4);
        if (same) {
            const uint off = meta.Load((k * 11 + 8) * 4), n = meta.Load((k * 11 + 9) * 4),
                       first = meta.Load((k * 11 + 10) * 4);
            for (uint i = 0; i < n; ++i)
                dst.Store(d + off + 4 * i, meta.Load((first + i) * 4));
            break;
        }
    }
}
