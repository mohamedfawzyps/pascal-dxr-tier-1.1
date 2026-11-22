// Spec Tier 1.1 item 6: D3D12_RAYTRACING_PIPELINE_CONFIG1 flags (0.64.0).
//
// "D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES and ..._SKIP_PROCEDURAL_
// PRIMITIVES behave like OR'ing the equivalent RAY_FLAGS into any TraceRay()
// call in a raytracing pipeline, except that these do not show up in a
// RayFlags() call from a shader." (Raytracing.md, D3D12_RAYTRACING_TIER)
//
// A triangle (left) and a procedural box (right) overlapping in the middle,
// the box nearer; every shader writes (what it is) | RayFlags() << 8, so both
// the culling and what RayFlags() reports are compared against WARP.
//
//   pflagtest.exe [warp|hw] <out.bin> [--flags F] [--dxil] [--rayflags R]
//                 [--literal] [--collection] [--grow]
//   pflagtest.exe diff <a.bin> <b.bin> [--spec F]
//
// --literal: TraceRay passes the literal 0, so the shim can mask RayFlags()
// as the spec says; WARP shows the pipeline flags there, so compare with
// `diff --spec F`, which expects WARP's RayFlags() without F.
// --collection: the library, hit groups and config in a COLLECTION, linked
// by a pipeline with the same config (a collection that traces has to carry
// one: WARP and the 1070 both reject it without). With --literal the
// collection alone would mask RayFlags(); --mixmask adds a second library to
// the pipeline whose TraceRay passes flags from a cbuffer, so the pipeline
// cannot, and the shim has to build the collection again unmasked.
// --grow: the pipeline made with raygen and miss, grown by AddToStateObject
// with the hit groups, both carrying the config.
//
// --flags F: a PIPELINE_CONFIG1 subobject with those flags (0x100 skip
// triangles, 0x200 skip procedural); --dxil: the same declared in the library
// as a RaytracingPipelineConfig1 subobject; neither: a PIPELINE_CONFIG.
// --rayflags R: the flags every TraceRay passes (default 0).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;

static void HR(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char b[256];
        std::snprintf(b, sizeof(b), "%s failed, hr=0x%08lX", what, (unsigned long)hr);
        throw std::runtime_error(b);
    }
}

static const UINT W = 128, H = 64;

static const char* kLib = R"(
struct P { uint v; };
struct A { float2 b; };
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> outBuf : register(u0);
cbuffer C : register(b0) { uint rayFlags; };
#if DXILCFG
RaytracingPipelineConfig1 cfg = { 1, DXILCFG };
#endif
[shader("raygeneration")] void RayGen() {
    uint2 i = DispatchRaysIndex().xy;
    RayDesc r;
    r.Origin = float3(-1.0 + 2.0 * (i.x + 0.5) / 128.0, -1.0 + 2.0 * (i.y + 0.5) / 64.0, -1.0);
    r.Direction = float3(0, 0, 1);
    r.TMin = 0.0; r.TMax = 10.0;
    P p; p.v = 0xFFFFFFFF;
#if LITERAL
    TraceRay(scene, 0, 0xFF, 0, 1, 0, r, p);
#else
    TraceRay(scene, rayFlags, 0xFF, 0, 1, 0, r, p);
#endif
    outBuf[i.y * 128 + i.x] = p.v;
}
[shader("miss")] void Miss(inout P p) { p.v = 0 | (RayFlags() << 8); }
[shader("closesthit")] void CHTri(inout P p, in A a) { p.v = 1 | (RayFlags() << 8); }
[shader("closesthit")] void CHProc(inout P p, in A a) { p.v = 2 | (RayFlags() << 8); }
[shader("intersection")] void Isect() { A a; a.b = float2(0, 0); ReportHit(0.5, 0, a); }
)";

static bool g_literal = false;
static bool g_mixMask = false;
// --mixmask: a second raygen, never dispatched, tracing with flags that are
// not a literal.
static const char* kLib2 = R"(
struct P2 { uint v; };
RaytracingAccelerationStructure scene : register(t0);
cbuffer C : register(b0) { uint rayFlags; };
[shader("raygeneration")] void RayGen2() {
    RayDesc r; r.Origin = float3(0, 0, -1); r.Direction = float3(0, 0, 1); r.TMin = 0; r.TMax = 10;
    P2 p; p.v = 0;
    TraceRay(scene, rayFlags, 0xFF, 0, 1, 0, r, p);
}
)";
static const char* g_src = kLib;
static ComPtr<IDxcBlob> Compile(UINT dxilFlags) {
    static HMODULE m = LoadLibraryW(L"dxcompiler.dll");
    if (!m) throw std::runtime_error("dxcompiler.dll not found");
    auto create = (DxcCreateInstanceProc)GetProcAddress(m, "DxcCreateInstance");
    ComPtr<IDxcCompiler3> c;
    HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&c)), "create IDxcCompiler3");
    const std::wstring d = L"DXILCFG=" + std::to_wstring(dxilFlags);
    const wchar_t* args[] = { L"-T", L"lib_6_5", L"-D", d.c_str(), L"-D",
                              g_literal ? L"LITERAL=1" : L"LITERAL=0" };
    DxcBuffer buf{ g_src, std::strlen(g_src), DXC_CP_UTF8 };
    ComPtr<IDxcResult> res;
    HR(c->Compile(&buf, args, 6, nullptr, IID_PPV_ARGS(&res)), "Compile");
    HRESULT st = E_FAIL; res->GetStatus(&st);
    if (FAILED(st)) {
        ComPtr<IDxcBlobUtf8> e;
        res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&e), nullptr);
        throw std::runtime_error(std::string("DXC: ") + (e ? e->GetStringPointer() : "?"));
    }
    ComPtr<IDxcBlob> obj;
    HR(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr), "DXC object");
    return obj;
}

static ComPtr<ID3D12Resource> Buffer(ID3D12Device* dev, UINT64 size, D3D12_HEAP_TYPE heap,
                                     D3D12_RESOURCE_STATES state, bool uav = false) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)),
       "CreateCommittedResource");
    return r;
}
static ComPtr<ID3D12Resource> Upload(ID3D12Device* dev, const void* p, size_t n) {
    auto b = Buffer(dev, n, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* m = nullptr; HR(b->Map(0, nullptr, &m), "Map"); std::memcpy(m, p, n); b->Unmap(0, nullptr);
    return b;
}

struct Gpu {
    ComPtr<ID3D12Device5> dev;
    ComPtr<ID3D12CommandQueue> q;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList4> cl;
    ComPtr<ID3D12Fence> f;
    UINT64 fv = 0;
    std::vector<ComPtr<ID3D12Resource>> keep;
    void flush() {
        HR(cl->Close(), "Close");
        ID3D12CommandList* l[] = { cl.Get() };
        q->ExecuteCommandLists(1, l);
        HR(q->Signal(f.Get(), ++fv), "Signal");
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(fv, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
        HR(dev->GetDeviceRemovedReason(), "device (removed)");
        HR(al->Reset(), "allocator Reset");
        HR(cl->Reset(al.Get(), nullptr), "list Reset");
    }
    ComPtr<ID3D12Resource> Build(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& in) {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        dev->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
        auto scratch = Buffer(dev.Get(), info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        auto as = Buffer(dev.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                         D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs = in;
        d.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        d.DestAccelerationStructureData = as->GetGPUVirtualAddress();
        cl->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl->ResourceBarrier(1, &b);
        keep.push_back(scratch);
        return as;
    }
};

static int Run(bool warp, const char* outPath, UINT flags, bool dxil, UINT rayFlags, int shape) {
    ComPtr<IDXGIFactory6> fac;
    HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)), "factory");
    ComPtr<IDXGIAdapter1> ad;
    if (warp) HR(fac->EnumWarpAdapter(IID_PPV_ARGS(&ad)), "warp");
    else HR(fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)), "hw");
    Gpu g;
    HR(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.q)), "queue");
    HR(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.al)), "alloc");
    HR(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.al.Get(), nullptr, IID_PPV_ARGS(&g.cl)),
       "list");
    HR(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.f)), "fence");

    // A triangle from x -0.9 to 0.4, a box from x -0.4 to 0.9: both in the
    // middle, the box nearer (it reports t 0.5, the triangle is at t 1).
    const float tri[9] = { -0.9f, -0.9f, 0, 0.4f, -0.9f, 0, -0.25f, 0.9f, 0 };
    auto vb = Upload(g.dev.Get(), tri, sizeof(tri));
    D3D12_RAYTRACING_GEOMETRY_DESC gt{};
    gt.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    gt.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    gt.Triangles.VertexBuffer = { vb->GetGPUVirtualAddress(), 12 };
    gt.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    gt.Triangles.VertexCount = 3;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
    bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bi.NumDescs = 1; bi.pGeometryDescs = &gt;
    auto blasTri = g.Build(bi);
    const D3D12_RAYTRACING_AABB box{ -0.4f, -0.7f, -0.2f, 0.9f, 0.7f, 0.2f };
    auto ab = Upload(g.dev.Get(), &box, sizeof(box));
    D3D12_RAYTRACING_GEOMETRY_DESC gp{};
    gp.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    gp.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    gp.AABBs.AABBCount = 1;
    gp.AABBs.AABBs = { ab->GetGPUVirtualAddress(), sizeof(box) };
    bi.pGeometryDescs = &gp;
    auto blasProc = g.Build(bi);
    D3D12_RAYTRACING_INSTANCE_DESC inst[2]{};
    for (int k = 0; k < 2; ++k) {
        inst[k].Transform[0][0] = inst[k].Transform[1][1] = inst[k].Transform[2][2] = 1;
        inst[k].InstanceMask = 0xFF;
        inst[k].InstanceContributionToHitGroupIndex = k;
        inst[k].AccelerationStructure = (k ? blasProc : blasTri)->GetGPUVirtualAddress();
    }
    auto ib = Upload(g.dev.Get(), inst, sizeof(inst));
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    ti.NumDescs = 2; ti.InstanceDescs = ib->GetGPUVirtualAddress();
    auto tlas = g.Build(ti);
    g.flush();

    // Global root signature: b0 constant, t0 scene, u0 output.
    D3D12_ROOT_PARAMETER rp[3]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 1;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd{ 3, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> rb, re;
    HR(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rb, &re), "serialize RS");
    ComPtr<ID3D12RootSignature> rs;
    HR(g.dev->CreateRootSignature(0, rb->GetBufferPointer(), rb->GetBufferSize(), IID_PPV_ARGS(&rs)), "RS");

    auto lib = Compile(dxil ? flags : 0);
    D3D12_DXIL_LIBRARY_DESC ld{};
    ld.DXILLibrary = { lib->GetBufferPointer(), lib->GetBufferSize() };
    D3D12_HIT_GROUP_DESC hgt{ L"HGTri", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"CHTri", nullptr };
    D3D12_HIT_GROUP_DESC hgp{ L"HGProc", D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE, nullptr, L"CHProc", L"Isect" };
    D3D12_RAYTRACING_SHADER_CONFIG sc{ 4, 8 };
    D3D12_RAYTRACING_PIPELINE_CONFIG pc{ 1 };
    D3D12_RAYTRACING_PIPELINE_CONFIG1 pc1{ 1, (D3D12_RAYTRACING_PIPELINE_FLAGS)flags };
    ID3D12RootSignature* rsp = rs.Get();
    std::vector<D3D12_STATE_SUBOBJECT> sub = {
        { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ld },
        { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgt },
        { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgp },
        { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
        { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &rsp },
    };
    if (!dxil) {
        if (flags) sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &pc1 });
        else sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pc });
    }
    D3D12_STATE_OBJECT_DESC sd{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)sub.size(), sub.data() };
    ComPtr<ID3D12StateObject> so;
    D3D12_STATE_SUBOBJECT cfgSub = sub.back();   // the pipeline config, when not in the DXIL
    if (shape == 1) {
        // A collection of everything, config included, then a pipeline of
        // it and the same config.
        D3D12_STATE_OBJECT_DESC cd{ D3D12_STATE_OBJECT_TYPE_COLLECTION, (UINT)sub.size(), sub.data() };
        ComPtr<ID3D12StateObject> coll;
        HR(g.dev->CreateStateObject(&cd, IID_PPV_ARGS(&coll)), "CreateStateObject(collection)");
        D3D12_EXISTING_COLLECTION_DESC ec{ coll.Get(), 0, nullptr };
        std::vector<D3D12_STATE_SUBOBJECT> ps = { { D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &ec } };
        if (!dxil) ps.push_back(cfgSub);
        ComPtr<IDxcBlob> lib2;
        D3D12_DXIL_LIBRARY_DESC ld2{};
        if (g_mixMask) {
            g_src = kLib2;
            lib2 = Compile(0);
            g_src = kLib;
            ld2.DXILLibrary = { lib2->GetBufferPointer(), lib2->GetBufferSize() };
            ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &ld2 });
            ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc });
            ps.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &rsp });
        }
        D3D12_STATE_OBJECT_DESC pd{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)ps.size(), ps.data() };
        HR(g.dev->CreateStateObject(&pd, IID_PPV_ARGS(&so)), "CreateStateObject(pipeline)");
    } else if (shape == 2) {
        // Raygen and miss first, growable; the hit groups added.
        D3D12_EXPORT_DESC first[2] = { { L"RayGen", nullptr, D3D12_EXPORT_FLAG_NONE },
                                       { L"Miss", nullptr, D3D12_EXPORT_FLAG_NONE } };
        D3D12_EXPORT_DESC later[3] = { { L"CHTri", nullptr, D3D12_EXPORT_FLAG_NONE },
                                       { L"CHProc", nullptr, D3D12_EXPORT_FLAG_NONE },
                                       { L"Isect", nullptr, D3D12_EXPORT_FLAG_NONE } };
        D3D12_DXIL_LIBRARY_DESC l1 = ld, l2 = ld;
        l1.NumExports = 2; l1.pExports = first;
        l2.NumExports = 3; l2.pExports = later;
        D3D12_STATE_OBJECT_CONFIG grow{ D3D12_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS };
        std::vector<D3D12_STATE_SUBOBJECT> a = {
            { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &l1 },
            { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
            { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &rsp },
            { D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &grow },
        };
        if (!dxil) a.push_back(cfgSub);
        D3D12_STATE_OBJECT_DESC ad{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)a.size(), a.data() };
        ComPtr<ID3D12StateObject> base;
        HR(g.dev->CreateStateObject(&ad, IID_PPV_ARGS(&base)), "CreateStateObject(base)");
        std::vector<D3D12_STATE_SUBOBJECT> b = {
            { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &l2 },
            { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgt },
            { D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hgp },
            { D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &sc },
            { D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &rsp },
            { D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &grow },
        };
        if (!dxil) b.push_back(cfgSub);
        D3D12_STATE_OBJECT_DESC bd{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)b.size(), b.data() };
        ComPtr<ID3D12Device7> d7;
        HR(g.dev.As(&d7), "ID3D12Device7");
        HR(d7->AddToStateObject(&bd, base.Get(), IID_PPV_ARGS(&so)), "AddToStateObject");
    } else {
        HR(g.dev->CreateStateObject(&sd, IID_PPV_ARGS(&so)), "CreateStateObject");
    }
    ComPtr<ID3D12StateObjectProperties> props;
    HR(so.As(&props), "properties");

    // Table: raygen at 0, miss at 64, hit groups at 128 and 192.
    std::vector<uint8_t> t(256, 0);
    const wchar_t* ids[4] = { L"RayGen", L"Miss", L"HGTri", L"HGProc" };
    for (int k = 0; k < 4; ++k) {
        void* id = props->GetShaderIdentifier(ids[k]);
        if (!id) throw std::runtime_error("no identifier");
        std::memcpy(t.data() + 64 * k, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    }
    auto table = Upload(g.dev.Get(), t.data(), t.size());
    auto out = Buffer(g.dev.Get(), W * H * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    const D3D12_GPU_VIRTUAL_ADDRESS base = table->GetGPUVirtualAddress();
    D3D12_DISPATCH_RAYS_DESC dr{};
    dr.RayGenerationShaderRecord = { base, 32 };
    dr.MissShaderTable = { base + 64, 32, 32 };
    dr.HitGroupTable = { base + 128, 128, 64 };
    dr.Width = W; dr.Height = H; dr.Depth = 1;
    g.cl->SetComputeRootSignature(rs.Get());
    g.cl->SetComputeRoot32BitConstants(0, 1, &rayFlags, 0);
    g.cl->SetComputeRootShaderResourceView(1, tlas->GetGPUVirtualAddress());
    g.cl->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    g.cl->SetPipelineState1(so.Get());
    g.cl->DispatchRays(&dr);
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = out.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g.cl->ResourceBarrier(1, &b);
    auto rbk = Buffer(g.dev.Get(), W * H * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    g.cl->CopyBufferRegion(rbk.Get(), 0, out.Get(), 0, W * H * 4);
    g.flush();
    std::vector<uint32_t> v(W * H);
    void* m = nullptr; HR(rbk->Map(0, nullptr, &m), "Map out");
    std::memcpy(v.data(), m, v.size() * 4); rbk->Unmap(0, nullptr);
    UINT n[3] = {};
    for (uint32_t x : v) if ((x & 0xFF) < 3) ++n[x & 0xFF];
    std::printf("%s: miss %u, triangle %u, procedural %u; RayFlags() seen 0x%X\n", warp ? "WARP" : "hw",
                n[0], n[1], n[2], v[W * H / 2 + W / 2] >> 8);
    FILE* fo = nullptr;
    if (fopen_s(&fo, outPath, "wb") || !fo) throw std::runtime_error("cannot write output");
    fwrite(v.data(), 4, v.size(), fo);
    fclose(fo);
    return 0;
}

static int Diff(const char* a, const char* b, UINT spec) {
    auto read = [](const char* p) {
        std::vector<uint32_t> v(W * H);
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") || !f || fread(v.data(), 4, v.size(), f) != v.size()) v.clear();
        if (f) fclose(f);
        return v;
    };
    auto va = read(a), vb = read(b);
    if (va.empty() || vb.empty()) { std::printf("RESULT: DIVERGE (missing output)\n"); return 1; }
    UINT kind = 0, flags = 0;
    for (size_t i = 0; i < va.size(); ++i) {
        if ((va[i] & 0xFF) != (vb[i] & 0xFF)) ++kind;
        else if ((va[i] & ~(spec << 8)) != vb[i]) ++flags;
    }
    std::printf("hit kind mismatches %u, RayFlags() mismatches %u\nRESULT: %s\n", kind, flags,
                kind || flags ? "DIVERGE" : "MATCH");
    return kind || flags ? 1 : 0;
}

int main(int argc, char** argv) {
    try {
        if (argc >= 4 && !std::strcmp(argv[1], "diff"))
            return Diff(argv[2], argv[3], argc >= 6 && !std::strcmp(argv[4], "--spec")
                                              ? (UINT)std::strtoul(argv[5], nullptr, 0) : 0u);
        if (argc < 3) { std::printf("pflagtest.exe [warp|hw] out.bin [--flags F] [--dxil] [--rayflags R]\n"); return 2; }
        UINT flags = 0, rayFlags = 0;
        bool dxil = false;
        int shape = 0;
        for (int i = 3; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--literal")) g_literal = true;
            if (!std::strcmp(argv[i], "--collection")) shape = 1;
            if (!std::strcmp(argv[i], "--grow")) shape = 2;
            if (!std::strcmp(argv[i], "--mixmask")) g_mixMask = true;
            if (!std::strcmp(argv[i], "--flags") && i + 1 < argc) flags = std::strtoul(argv[++i], nullptr, 0);
            else if (!std::strcmp(argv[i], "--rayflags") && i + 1 < argc) rayFlags = std::strtoul(argv[++i], nullptr, 0);
            else if (!std::strcmp(argv[i], "--dxil")) dxil = true;
        }
        return Run(!std::strcmp(argv[1], "warp"), argv[2], flags, dxil, rayFlags, shape);
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
