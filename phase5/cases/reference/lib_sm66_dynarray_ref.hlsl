// What a resource ARRAY indexed at run time looks like in a Shader Model 6.6
// library (plan step 3, 0.65.0): the shape the rewriter's 6.6 path must emit
// for rayquery_dynarray at cs_6_6. Compile with
//   dxc -T lib_6_6 -Fc lib_sm66_dynarray_ref.ll lib_sm66_dynarray_ref.hlsl
// and, for the compute side the rewriter starts from,
//   dxc -T cs_6_6 -E CS -Fc cs_sm66_dynarray_ref.ll lib_sm66_dynarray_ref.hlsl
RWStructuredBuffer<float> outBufs[4] : register(u0);
cbuffer CB : register(b0) { uint width; };

struct P { float v; };

[shader("raygeneration")]
void RayGen() {
    uint i = width / 128;
    outBufs[i][DispatchRaysIndex().x] = 1.0;
    outBufs[NonUniformResourceIndex(DispatchRaysIndex().y & 3)][DispatchRaysIndex().x] = 2.0;
}

[numthreads(8, 8, 1)]
void CS(uint3 t : SV_DispatchThreadID) {
    uint i = width / 128;
    outBufs[i][t.x] = 1.0;
    outBufs[NonUniformResourceIndex(t.y & 3)][t.x] = 2.0;
}
