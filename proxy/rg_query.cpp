// RayQuery in an APPLICATION's raygen, closest-hit or miss (0.81.0, 0.82.0).
// See rg_query.h.
#include "rg_query.h"

#include <d3d12.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <regex>
#include <set>

#include "as_tracker.h"
#include "dxil_scan.h"
#include "gpu_hold.h"
#include "proxy_log.h"
#include "res_tracker.h"
#include "rg_table_cs.h"
#include "shim_scene.h"
#include "rewriter/dxc_host.h"
#include "rewriter/geom_index.h"
#include "rewriter/ll_model.h"
#include "rewriter/nvapi_fold.h"
#include "rewriter/rg_host.h"
#include "rewriter/rq_analyze.h"
#include "rewriter/rq_lower.h"

using Microsoft::WRL::ComPtr;

namespace rgq {
namespace {

const UINT kId = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;        // 32
const UINT kRecAlign = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;   // 32
const UINT kTableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;  // 64
const UINT kAttrBytes = 8;
const UINT kRaygen = 7, kClosestHit = 10, kMiss = 11;
// The constants appended after the scene copies: the miss index, the
// shader's first query slot, the pipeline's query slots (0.83.0).
const UINT kConstants = 3;
// {5f1e0a63-2b7d-4c94-9e38-71a0c4d2b816}
const GUID kInfoGuid = { 0x5f1e0a63, 0x2b7d, 0x4c94, { 0x9e, 0x38, 0x71, 0xa0, 0xc4, 0xd2, 0xb8, 0x16 } };

UINT AlignUp(UINT v, UINT a) { return (v + a - 1) / a * a; }
std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& s) {
    std::string o;
    for (wchar_t c : s) o.push_back((char)c);
    return o;
}

class InfoHolder : public IUnknown {
public:
    explicit InfoHolder(std::shared_ptr<Info> i) : m_info(std::move(i)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** p) override {
        if (riid == __uuidof(IUnknown)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = InterlockedDecrement(&m_refs);
        if (!r) delete this;
        return r;
    }
    const std::shared_ptr<Info>& Get() const { return m_info; }
private:
    LONG m_refs = 1;
    std::shared_ptr<Info> m_info;
};

// The functions of a disassembled library that use RayQuery, by export name.
std::set<std::string> QueryFunctions(const std::string& text) {
    static const std::regex kDefine(R"RX(^define [^@]*@("([^"]+)"|([\w.$-]+))\()RX");
    std::set<std::string> out;
    std::string fn;
    std::smatch m;
    for (const auto& l : llm::SplitLines(text)) {
        if (std::regex_search(l, m, kDefine)) {
            fn = rq::ExportName(m[2].matched ? m[2].str() : m[3].str());
            continue;
        }
        if (l == "}") { fn.clear(); continue; }
        if (!fn.empty() && (l.find("@dx.op.rayQuery_") != std::string::npos ||
                            l.find("@dx.op.allocateRayQuery") != std::string::npos))
            out.insert(fn);
    }
    return out;
}

// The functions a lowered library defines, by export name.
std::vector<std::wstring> Defined(const std::string& text, const std::string& prefix) {
    static const std::regex kDefine(R"RX(^define [^@]*@("([^"]+)"|([\w.$-]+))\()RX");
    std::vector<std::wstring> out;
    std::smatch m;
    for (const auto& l : llm::SplitLines(text))
        if (std::regex_search(l, m, kDefine)) {
            const std::string n = rq::ExportName(m[2].matched ? m[2].str() : m[3].str());
            if (n.compare(0, prefix.size(), prefix) == 0) out.push_back(Wide(n));
        }
    return out;
}

// The lowered text with every TraceRay but the lowering's own removed: what
// the scene scan reads, so the shader's own calls are not scene slots.
std::string OwnCallsOnly(const std::string& text) {
    std::string out;
    for (const auto& l : llm::SplitLines(text)) {
        if (l.find("call void @dx.op.traceRay.") != std::string::npos &&
            l.find("call void @dx.op.traceRay.struct.Payload(") == std::string::npos)
            continue;
        out += l;
        out += '\n';
    }
    return out;
}

// One shader, lowered and given its own signature back: its text and shape.
struct Lowered {
    Hosted h;
    std::string text;
    std::vector<unsigned> slotOf;
    std::string inner;   // dxr11_rq_<its own name>_: its generated shaders' prefix in the library
};

// `name` is the function's own name, `as` the one it is exported by (0.83.0:
// an export list may rename it); the shim's names follow the exported one, so
// two libraries holding the same function do not clash.
bool LowerShader(const std::string& norm, const std::string& name, const std::wstring& as, uint32_t kind,
                 Lowered* out, std::string* why) {
    std::string cs, text;
    if (!rq::RaygenAsCs(norm, name, &cs, why)) return false;
    if (!rq::FoldNvapi(cs, &text, why)) return false;
    llm::Module m(text);
    auto a = rq::Analyze(m);
    if (!a.ok) { *why = a.error; return false; }
    auto l = rq::Lower(m, a.query, rq::RaygenExports(name));
    if (!l.ok) { *why = l.error; return false; }
    if (!rq::RestoreFunction(l.text, cs, norm, name, &out->text, why)) return false;
    Hosted& r = out->h;
    r.name = as;
    r.prefix = L"dxr11_rq_" + as + L"_";
    out->inner = "dxr11_rq_" + name + "_";
    r.kind = kind;
    r.hasAnyHit = a.query.AnyNeedsAnyHit();
    r.hasIntersection = a.query.NeedsIntersection();
    r.needsBoth = a.query.NeedsBoth();
    r.recordConstants = a.query.needsRecordConstants;
    r.track = a.query.track;
    r.trackProc = a.query.trackProc;
    for (const auto& o : a.query.others) r.track = r.track || o.track;
    r.servesProc = r.hasIntersection || r.needsBoth;
    r.servesTri = r.needsBoth || !r.hasIntersection;
    if (a.query.perQuery) {
        auto shape = [](const rq::Query& q) {
            QueryShape s;
            s.anyHit = q.hasLoop;
            s.needsBoth = q.NeedsBoth();
            s.trackProc = q.trackProc;
            return s;
        };
        r.perQuery.push_back(shape(a.query));
        for (const auto& o : a.query.others) r.perQuery.push_back(shape(o));
    }
    r.payloadBytes = (UINT)l.payloadBytes;

    // Its own calls' scenes, one slot each. A slot picked by key (an array
    // element, the heap) holds one scene here: a dispatch whose keys reach
    // several is refused (Dispatch).
    const std::string own = OwnCallsOnly(out->text);
    gidx::ScanScenes(own, &r.scenes);
    if (r.scenes.unknown) {
        *why = "a lowered TraceRay whose scene could not be traced back to a register or the heap";
        return false;
    }
    const auto slots = gidx::SceneSlots(r.scenes);
    std::vector<gidx::Scenes> calls;
    gidx::ScanCallScenes(own, &calls);
    for (const auto& c : calls) {
        const int j = gidx::SlotOfCall(slots, c);
        if (j < 0) { *why = "a lowered TraceRay whose scene is not one of the shader's scene slots"; return false; }
        out->slotOf.push_back((unsigned)j);
    }
    r.slots = (UINT)slots.size();
    return true;
}

// A library without the shaders moved out of it: its bytes, or *empty.
bool StripLibrary(std::string norm, const std::vector<std::string>& names, const void* orig, size_t size,
                  std::vector<uint8_t>* out, bool* empty, std::string* why) {
    *empty = false;
    for (const auto& n : names) {
        std::string next;
        bool e = false;
        if (!rq::StripFunction(norm, n, &next, &e, why)) return false;
        if (e) { *empty = true; return true; }
        norm = next;
    }
    std::string err;
    if (gidx::ReassembleLibrary(norm, orig, size, false, out, &err)) return true;
    // The module flags the shaders left need are the validator's to say.
    std::string fixed;
    if (rq::FixFlags(norm, err, &fixed) && gidx::ReassembleLibrary(fixed, orig, size, false, out, &err))
        return true;
    *why = "the application's library without its RayQuery shaders did not assemble: " + err;
    return false;
}

bool LocalRs(ID3D12Device* dev, bool pairSrv, ComPtr<ID3D12RootSignature>* out) {
    D3D12_ROOT_PARAMETER rp{};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp.Descriptor.ShaderRegister = 0;
    rp.Descriptor.RegisterSpace = 1;
    D3D12_ROOT_SIGNATURE_DESC d{};
    d.NumParameters = pairSrv ? 1u : 0u;
    d.pParameters = pairSrv ? &rp : nullptr;
    d.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) return false;
    return SUCCEEDED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                              IID_PPV_ARGS(out->GetAddressOf())));
}

// The record copy pass (rg_table.hlsl).
struct TablePipe {
    ID3D12Device* device = nullptr;
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
};
std::mutex g_pipeLock;
TablePipe g_pipe;

bool CopyWithArgs(ID3D12GraphicsCommandList4* cl, ID3D12Device* dev, D3D12_GPU_VIRTUAL_ADDRESS src,
                  UINT records, UINT srcStride, D3D12_GPU_VIRTUAL_ADDRESS dst, UINT dstStride,
                  D3D12_GPU_VIRTUAL_ADDRESS meta, UINT groups, std::string* why) {
    if (!records) return true;
    std::lock_guard<std::mutex> g(g_pipeLock);
    if (g_pipe.device != dev || !g_pipe.pso) {
        TablePipe p;
        p.device = dev;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        if (FAILED(dev->CreateRootSignature(0, g_rgTableCS, sizeof(g_rgTableCS), IID_PPV_ARGS(&p.rs)))) {
            *why = "could not create the record copy root signature";
            return false;
        }
        pd.pRootSignature = p.rs.Get();
        pd.CS.pShaderBytecode = g_rgTableCS;
        pd.CS.BytecodeLength = sizeof(g_rgTableCS);
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&p.pso)))) {
            *why = "could not create the record copy pipeline";
            return false;
        }
        g_pipe = p;
    }
    cl->SetComputeRootSignature(g_pipe.rs.Get());
    cl->SetPipelineState(g_pipe.pso.Get());
    const UINT c[4] = { records, srcStride, dstStride, groups };
    cl->SetComputeRoot32BitConstants(0, 4, c, 0);
    cl->SetComputeRootShaderResourceView(1, src);
    cl->SetComputeRootShaderResourceView(2, meta);
    cl->SetComputeRootUnorderedAccessView(3, dst);
    cl->Dispatch((records + 63) / 64, 1, 1);
    return true;
}

thread_local const Force* t_force = nullptr;

// The shader and pipeline configs a description declares at its scope.
struct Configs {
    bool shader = false, pipeline = false;
    UINT payload = 0, attr = 0, depth = 0;
};
Configs ConfigsOf(const D3D12_STATE_OBJECT_DESC& d) {
    Configs c;
    for (UINT i = 0; i < d.NumSubobjects; ++i) {
        const auto& so = d.pSubobjects[i];
        if (!so.pDesc) continue;
        if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG) {
            const auto* x = static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(so.pDesc);
            c.shader = true;
            c.payload = x->MaxPayloadSizeInBytes;
            c.attr = x->MaxAttributeSizeInBytes;
        } else if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG) {
            c.pipeline = true;
            c.depth = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(so.pDesc)->MaxTraceRecursionDepth;
        }
    }
    return c;
}

}  // namespace

void SetForce(const Force* f) { t_force = f; }

Outcome Transform(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in, gidx::Transformed* out,
                  std::shared_ptr<Info>* infoOut, std::string* why, const pflags::CreateFn& create) {
    const UINT n = in.NumSubobjects;
    const D3D12_STATE_SUBOBJECT* s = in.pSubobjects;
    const Force* force = t_force;
    std::vector<UINT> queryLibs;
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s[i].pDesc) continue;
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        if (Dxr11ContainerUsesRayQuery(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength))
            queryLibs.push_back(i);
    }
    // Collections this links whose shaders the shim lowered (0.83.0): their
    // hosted shaders are the pipeline's too.
    std::vector<std::shared_ptr<Info>> linked;
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION || !s[i].pDesc) continue;
        const auto* ec = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s[i].pDesc);
        std::shared_ptr<Info> ci = Get(ec->pExistingCollection);
        if (!ci || ci->hosted.empty()) continue;
        if (ec->NumExports) {
            *why = "a collection with RayQuery shaders linked through an export list";
            return Outcome::kRefused;
        }
        linked.push_back(ci);
    }
    const bool collection = in.Type == D3D12_STATE_OBJECT_TYPE_COLLECTION;
    if (queryLibs.empty() && linked.empty()) {
        if (!force) return Outcome::kNothing;
        // A collection with nothing to lower, built again for a pipeline
        // hosting RayQuery shaders: only its configs change.
        std::vector<UINT> none;
        if (!gidx::ExtendUnits(dev, in, 1, kConstants, {}, {}, out, &none, why)) return Outcome::kRefused;
        for (auto& so : out->subs) {
            if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG && so.pDesc) {
                D3D12_RAYTRACING_SHADER_CONFIG c = *static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(so.pDesc);
                c.MaxPayloadSizeInBytes = (std::max)(c.MaxPayloadSizeInBytes, force->payload);
                c.MaxAttributeSizeInBytes = (std::max)(c.MaxAttributeSizeInBytes, force->attr);
                out->raw.emplace_back(sizeof(c));
                std::memcpy(out->raw.back().data(), &c, sizeof(c));
                so.pDesc = out->raw.back().data();
            } else if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG && so.pDesc) {
                D3D12_RAYTRACING_PIPELINE_CONFIG c = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(so.pDesc);
                c.MaxTraceRecursionDepth = force->depth;
                out->raw.emplace_back(sizeof(c));
                std::memcpy(out->raw.back().data(), &c, sizeof(c));
                so.pDesc = out->raw.back().data();
            }
        }
        out->desc.Type = in.Type;
        out->desc.NumSubobjects = (UINT)out->subs.size();
        out->desc.pSubobjects = out->subs.data();
        out->summary = "a collection built again with the configs of a pipeline hosting RayQuery shaders";
        infoOut->reset();
        return Outcome::kTransformed;
    }

    auto info = std::make_shared<Info>();
    info->collection = collection;
    // Each library with RayQuery: its exported shaders lowered, every shader
    // with RayQuery removed from it (one it does not export would still make
    // the driver reject the library), the rest kept under its export list
    // less the shaders removed.
    std::deque<Lowered> lowered;
    std::deque<std::vector<uint8_t>> stripped;
    std::map<UINT, const std::vector<uint8_t>*> code;
    std::map<UINT, std::vector<D3D12_EXPORT_DESC>> exportsLeft;   // libraries with a list
    std::set<UINT> removed;                                       // libraries with nothing left
    for (UINT i : queryLibs) {
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        const void* bytes = ld->DXILLibrary.pShaderBytecode;
        const size_t size = ld->DXILLibrary.BytecodeLength;
        if (gidx::LibraryHasSubobjects(bytes, size)) {
            *why = "RayQuery in a DXR shader of a library that declares subobjects of its own";
            return Outcome::kRefused;
        }
        std::string text, err;
        if (!dxch::Disassemble(bytes, size, &text, &err)) {
            *why = "a RayQuery library could not be disassembled: " + err;
            return Outcome::kRefused;
        }
        const std::string norm = llm::Normalize(text);
        std::vector<std::pair<std::string, uint32_t>> fns;
        gidx::LibraryFunctions(bytes, size, &fns);
        // The name a function is exported by: its own, a rename, or none.
        auto exportedAs = [&](const std::string& fn, std::wstring* as) {
            if (!ld->NumExports) { *as = Wide(fn); return true; }
            for (UINT e = 0; e < ld->NumExports; ++e) {
                const auto& x = ld->pExports[e];
                if ((x.ExportToRename ? x.ExportToRename : x.Name) == Wide(fn)) { *as = x.Name; return true; }
            }
            return false;
        };
        std::vector<std::string> names;
        for (const auto& q : QueryFunctions(norm)) {
            names.push_back(q);
            std::wstring as;
            if (!exportedAs(q, &as)) continue;   // removed, nothing to lower
            uint32_t kind = 0;
            for (const auto& f : fns) if (f.first == q) kind = f.second;
            if (kind != kRaygen && kind != kClosestHit && kind != kMiss) {
                *why = "RayQuery in " + q + ", an any-hit, intersection or callable shader (plan step 7)";
                return Outcome::kRefused;
            }
            lowered.emplace_back();
            std::string w;
            if (!LowerShader(norm, q, as, kind, &lowered.back(), &w)) {
                *why = "RayQuery in " + q + ": " + w;
                return Outcome::kRefused;
            }
        }
        if (names.empty()) {
            *why = "a library flagged as using RayQuery with no function that does";
            return Outcome::kRefused;
        }
        stripped.emplace_back();
        bool empty = false;
        if (!StripLibrary(norm, names, bytes, size, &stripped.back(), &empty, why)) return Outcome::kRefused;
        if (ld->NumExports) {
            std::vector<D3D12_EXPORT_DESC> left;
            for (UINT e = 0; e < ld->NumExports; ++e) {
                const auto& x = ld->pExports[e];
                const std::wstring fn = x.ExportToRename ? x.ExportToRename : x.Name;
                bool gone = false;
                for (const auto& q : names) gone = gone || Wide(q) == fn;
                if (!gone) left.push_back(x);
            }
            if (left.empty()) empty = true;
            else exportsLeft[i] = left;
        }
        if (empty) removed.insert(i);
        else code[i] = &stripped.back();
    }

    // Every hosted shader: this description's, then the linked collections'.
    for (const auto& L : lowered) info->hosted.push_back(L.h);
    const size_t own = info->hosted.size();
    for (const auto& ci : linked)
        for (const auto& h : ci->hosted) info->hosted.push_back(h);
    // Query slots: each shader's after the one before, assigned where the
    // pipeline is made; the shaders read theirs at run time. A scene copy's
    // structure serves 15 ray contributions.
    UINT total = 0;
    for (auto& h : info->hosted) {
        h.queryBase = total;
        total += h.Queries();
    }
    if (!collection && total > shimscene::PairsPerStructure()) {
        *why = std::to_string(total) + " RayQuery query slots in one pipeline, more than the " +
               std::to_string(shimscene::PairsPerStructure()) + " a scene copy holds";
        return Outcome::kRefused;
    }
    info->queries = total;
    // This description's units carry `srvs` scene copies: its shaders' most.
    UINT srvs = 1;
    for (size_t k = 0; k < own; ++k) srvs = (std::max)(srvs, info->hosted[k].slots);
    for (size_t k = 0; k < own; ++k) info->hosted[k].srvs = srvs;
    std::deque<std::vector<uint8_t>> libs;
    for (auto& L : lowered) {
        std::string pointed, err;
        if (!rq::PointRaygenQueries(llm::Normalize(L.text), L.h.Queries(), L.h.recordConstants ? 1u : 0u,
                                    L.slotOf, L.h.slots, std::vector<unsigned>(L.h.slots, 1u), &pointed, why))
            return Outcome::kRefused;
        libs.emplace_back();
        if (!dxch::AssembleAndSign(pointed, &libs.back(), &err)) {
            *why = "the lowered library of " + Narrow(L.h.name) + " did not assemble: " + err;
            return Outcome::kRefused;
        }
    }

    // Explicit associations of a library's subobject to a hosted shader:
    // what it names cannot be told here.
    std::set<std::wstring> hostedNames;
    for (size_t k = 0; k < own; ++k) hostedNames.insert(info->hosted[k].name);
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION || !s[i].pDesc) continue;
        const auto* a = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
        for (UINT e = 0; e < a->NumExports; ++e)
            if (hostedNames.count(a->pExports[e])) {
                *why = "a RayQuery shader associated by name with a library's subobject";
                return Outcome::kRefused;
            }
    }

    // The units: a raygen or miss itself, a closest-hit each hit group
    // importing it with that group's shaders. Their local root signatures
    // extended by one root SRV per scene slot (the shim's copy), then the
    // three constants (miss index, first query slot, query slots).
    const auto groups = gidx::HitGroupUnits(in);
    std::vector<std::vector<std::wstring>> units;
    std::vector<std::pair<size_t, size_t>> unitOf;   // (hosted, unit)
    for (size_t k = 0; k < own; ++k) {
        Hosted& h = info->hosted[k];
        if (h.kind != kClosestHit) {
            h.units.push_back({ h.name });
            units.push_back({ h.name });
            unitOf.push_back({ k, 0 });
            continue;
        }
        for (const auto& g : groups)
            if (std::find(g.begin() + 1, g.end(), h.name) != g.end()) {
                h.units.push_back({ g[0] });
                units.push_back(g);
                unitOf.push_back({ k, h.units.size() - 1 });
            }
        if (h.units.empty() && collection) {
            *why = "a closest-hit with RayQuery in a collection that holds no hit group importing it";
            return Outcome::kRefused;
        }
    }
    std::vector<UINT> offsets;
    if (!gidx::ExtendUnits(dev, in, srvs, kConstants, units, code, out, &offsets, why)) return Outcome::kRefused;
    for (size_t u = 0; u < units.size(); ++u)
        info->hosted[unitOf[u].first].units[unitOf[u].second].insert = offsets[u];

    // Libraries keep their order through ExtendUnits: what is left of each
    // gets its export list less the shaders removed, and one with nothing
    // left goes.
    {
        UINT ordinal = 0;
        std::vector<UINT> libAt;   // input index of each library, in order
        for (UINT i = 0; i < n; ++i)
            if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) libAt.push_back(i);
        std::vector<size_t> drop;
        for (size_t k = 0; k < out->subs.size(); ++k) {
            if (out->subs[k].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) continue;
            const UINT i = ordinal < libAt.size() ? libAt[ordinal] : ~0u;
            ++ordinal;
            if (removed.count(i)) { drop.push_back(k); continue; }
            auto el = exportsLeft.find(i);
            if (el == exportsLeft.end()) continue;
            out->exportDescs.push_back(el->second);
            D3D12_DXIL_LIBRARY_DESC ld = *static_cast<const D3D12_DXIL_LIBRARY_DESC*>(out->subs[k].pDesc);
            ld.NumExports = (UINT)out->exportDescs.back().size();
            ld.pExports = out->exportDescs.back().data();
            out->libs.push_back(ld);
            out->subs[k].pDesc = &out->libs.back();
        }
        if (!drop.empty()) {
            // Associations point at subobjects by address: as indices while
            // the array loses the libraries, then back.
            for (auto& a : out->assoc)
                a.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>(
                    (uintptr_t)(a.pSubobjectToAssociate - out->subs.data()));
            for (auto it = drop.rbegin(); it != drop.rend(); ++it) {
                out->subs.erase(out->subs.begin() + (ptrdiff_t)*it);
                for (auto& a : out->assoc) {
                    const size_t t = (size_t)(uintptr_t)a.pSubobjectToAssociate;
                    if (t > *it)
                        a.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>((uintptr_t)(t - 1));
                }
            }
            for (auto& a : out->assoc)
                a.pSubobjectToAssociate = &out->subs[(size_t)(uintptr_t)a.pSubobjectToAssociate];
        }
    }

    // The association pointers into out->subs, as indices, while it grows.
    for (auto& a : out->assoc)
        a.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>(
            (uintptr_t)(a.pSubobjectToAssociate - out->subs.data()));
    const size_t before = out->subs.size();

    // Payload and attribute limits raised to the lowering's. The recursion
    // depth: at least 1, and 1 more when a closest-hit or miss queries, its
    // query tracing one level below it.
    UINT payload = 0;
    bool deeper = false;
    for (const auto& h : info->hosted) {
        payload = (std::max)(payload, h.payloadBytes);
        deeper = deeper || h.kind != kRaygen;
    }
    // Built again for a pipeline (0.83.0): its configs exactly.
    if (force) payload = (std::max)(payload, force->payload);
    bool config = false;
    for (size_t k = 0; k < before; ++k) {
        auto& so = out->subs[k];
        if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG && so.pDesc) {
            D3D12_RAYTRACING_SHADER_CONFIG c = *static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(so.pDesc);
            c.MaxPayloadSizeInBytes = (std::max)(c.MaxPayloadSizeInBytes, payload);
            c.MaxAttributeSizeInBytes = (std::max)((std::max)(c.MaxAttributeSizeInBytes, kAttrBytes),
                                                   force ? force->attr : 0u);
            info->hasConfig = true;
            info->payload = c.MaxPayloadSizeInBytes;
            info->attr = c.MaxAttributeSizeInBytes;
            out->raw.emplace_back(sizeof(c));
            std::memcpy(out->raw.back().data(), &c, sizeof(c));
            so.pDesc = out->raw.back().data();
            config = true;
        } else if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG && so.pDesc) {
            D3D12_RAYTRACING_PIPELINE_CONFIG c = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(so.pDesc);
            const UINT want = force ? force->depth : (std::max)(c.MaxTraceRecursionDepth + (deeper ? 1u : 0u), 1u);
            info->depth = want;
            if (want > D3D12_RAYTRACING_MAX_DECLARABLE_TRACE_RECURSION_DEPTH) {
                *why = "a closest-hit or miss with RayQuery in a pipeline already at the deepest recursion "
                       "DXR allows";
                return Outcome::kRefused;
            }
            if (want == c.MaxTraceRecursionDepth) continue;
            c.MaxTraceRecursionDepth = want;
            out->raw.emplace_back(sizeof(c));
            std::memcpy(out->raw.back().data(), &c, sizeof(c));
            so.pDesc = out->raw.back().data();
        }
    }
    if (!config) {
        *why = "a RayQuery shader in a pipeline or collection whose shader config is not at its scope";
        return Outcome::kRefused;
    }

    // A pipeline and the collections it links agree on the configs (D3D12
    // refuses the pipeline otherwise): each linked collection built with
    // other ones is built again from its kept description with these.
    if (!collection) {
        for (size_t k = 0; k < before; ++k) {
            if (out->subs[k].Type != D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION || !out->subs[k].pDesc) continue;
            const auto* ec = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(out->subs[k].pDesc);
            D3D12_STATE_OBJECT_DESC od{};
            pflags::Mode mode;
            if (!pflags::CollectionOriginal(ec->pExistingCollection, &od, &mode)) {
                *why = "a linked collection the shim did not see being created";
                return Outcome::kRefused;
            }
            Configs has;
            if (std::shared_ptr<Info> ci = Get(ec->pExistingCollection)) {
                has.shader = ci->hasConfig;
                has.payload = ci->payload;
                has.attr = ci->attr;
                has.depth = ci->depth;
                has.pipeline = true;
            } else {
                has = ConfigsOf(od);
            }
            const bool payloadOk = !has.shader || (has.payload == info->payload && has.attr == info->attr);
            const bool depthOk = !has.pipeline || has.depth == info->depth;
            if (payloadOk && depthOk) continue;
            Force f;
            f.payload = info->payload;
            f.attr = info->attr;
            f.depth = info->depth;
            Microsoft::WRL::ComPtr<ID3D12StateObject> rebuilt;
            SetForce(&f);
            const HRESULT hr = create ? create(od, mode, &rebuilt) : E_FAIL;
            SetForce(nullptr);
            if (FAILED(hr) || !rebuilt) {
                char b[160];
                std::snprintf(b, sizeof(b), "a linked collection could not be built again with the pipeline's "
                              "configs (hr=0x%08lx)", (unsigned long)hr);
                *why = b;
                return Outcome::kRefused;
            }
            D3D12_EXISTING_COLLECTION_DESC nc = *ec;
            nc.pExistingCollection = rebuilt.Get();
            out->colls.push_back(nc);
            out->subs[k].pDesc = &out->colls.back();
            info->keep.push_back(rebuilt);
        }
    }

    auto name = [&](const std::wstring& w) {
        out->names.push_back(w);
        return out->names.back().c_str();
    };
    // The lowered libraries, each through an export list: the shader under
    // the name it is exported by, its generated shaders under the shim's
    // names for that one.
    std::vector<std::vector<std::wstring>> shadersOf;
    {
        size_t k = 0;
        for (const auto& lib : libs) {
            const Lowered& L = lowered[k];
            const Hosted& r = info->hosted[k++];
            std::string text, err;
            if (!dxch::Disassemble(lib.data(), lib.size(), &text, &err)) {
                *why = "a lowered library could not be read back";
                return Outcome::kRefused;
            }
            std::vector<D3D12_EXPORT_DESC> ex;
            std::vector<std::wstring> outer;
            for (const auto& w : Defined(text, L.inner)) {
                outer.push_back(r.prefix + w.substr(L.inner.size()));
                const bool same = outer.back() == w;
                ex.push_back({ name(outer.back()), same ? nullptr : name(w), D3D12_EXPORT_FLAG_NONE });
            }
            const std::wstring self = Wide(L.inner.substr(9, L.inner.size() - 10));   // its own name
            ex.push_back({ name(r.name), self == r.name ? nullptr : name(self), D3D12_EXPORT_FLAG_NONE });
            shadersOf.push_back(outer);
            out->code.push_back(lib);   // kept with the description
            out->exportDescs.push_back(ex);
            D3D12_DXIL_LIBRARY_DESC ld{};
            ld.DXILLibrary.pShaderBytecode = out->code.back().data();
            ld.DXILLibrary.BytecodeLength = out->code.back().size();
            ld.NumExports = (UINT)out->exportDescs.back().size();
            ld.pExports = out->exportDescs.back().data();
            out->libs.push_back(ld);
            out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &out->libs.back() });
        }
    }

    ComPtr<ID3D12RootSignature> pairRs, emptyRs;
    auto sig = [&](bool pairSrv) -> ID3D12RootSignature* {
        ComPtr<ID3D12RootSignature>& r = pairSrv ? pairRs : emptyRs;
        if (!r && !LocalRs(dev, pairSrv, &r)) return nullptr;
        return r.Get();
    };
    auto assoc = [&](size_t target, const std::vector<LPCWSTR>& list) {
        out->exportLists.push_back(list);
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION a{};
        a.NumExports = (UINT)out->exportLists.back().size();
        a.pExports = out->exportLists.back().data();
        a.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>((uintptr_t)target);
        out->assoc.push_back(a);
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION, &out->assoc.back() });
    };
    std::map<ID3D12RootSignature*, size_t> sigAt;
    auto localSig = [&](ID3D12RootSignature* rs) {
        auto it = sigAt.find(rs);
        if (it != sigAt.end()) return it->second;
        out->sigs.push_back(rs);
        out->lrs.push_back({ rs });
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &out->lrs.back() });
        return sigAt[rs] = out->subs.size() - 1;
    };
    auto addGroup = [&](const D3D12_HIT_GROUP_DESC& hg) {
        out->raw.emplace_back(sizeof(hg));
        std::memcpy(out->raw.back().data(), &hg, sizeof(hg));
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, out->raw.back().data() });
    };

    for (size_t k = 0; k < own; ++k) {
        Hosted& r = info->hosted[k];
        // Its generated shaders, under the shim's names.
        const std::vector<std::wstring>& shaders = shadersOf[k];
        auto has = [&](const std::wstring& w) {
            return std::find(shaders.begin(), shaders.end(), w) != shaders.end();
        };
        struct Set { bool anyHit, both, isect, trackProc; };
        std::vector<Set> sets;
        if (r.perQuery.empty()) sets.push_back({ r.hasAnyHit, r.needsBoth, r.hasIntersection, r.trackProc });
        else for (const auto& q : r.perQuery) sets.push_back({ q.anyHit, q.needsBoth, q.needsBoth, q.trackProc });
        auto nm = [&](const wchar_t* base, size_t q) {
            return r.prefix + base + (q ? L"_q" + std::to_wstring(q) : std::wstring());
        };
        std::vector<LPCWSTR> hgNames;
        for (size_t q = 0; q < sets.size(); ++q) {
            const Set& st = sets[q];
            D3D12_HIT_GROUP_DESC hg{};
            hg.HitGroupExport = name(nm(L"HitGroup", q));
            hg.ClosestHitShaderImport = name(nm(L"ClosestHit", q));
            if (st.both) {
                hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
                if (has(nm(L"AnyHit", q))) hg.AnyHitShaderImport = name(nm(L"AnyHit", q));
                D3D12_HIT_GROUP_DESC hp{};
                hp.HitGroupExport = name(nm(L"HitGroupProc", q));
                hp.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
                hp.IntersectionShaderImport = name(nm(L"Isect", q));
                hp.ClosestHitShaderImport = name(nm(L"ClosestHitProc", q));
                if (st.trackProc) hp.AnyHitShaderImport = name(nm(L"AnyHitProc", q));
                addGroup(hp);
                hgNames.push_back(hp.HitGroupExport);
            } else if (st.isect) {
                hg.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
                hg.IntersectionShaderImport = name(nm(L"Isect", q));
            } else {
                hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
                if (st.anyHit) hg.AnyHitShaderImport = name(nm(L"AnyHit", q));
            }
            addGroup(hg);
            hgNames.push_back(hg.HitGroupExport);
        }
        D3D12_HIT_GROUP_DESC nt{};
        nt.HitGroupExport = name(r.prefix + L"HitGroupNullTri");
        nt.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        nt.AnyHitShaderImport = name(r.prefix + L"AnyHitNull");
        D3D12_HIT_GROUP_DESC np{};
        np.HitGroupExport = name(r.prefix + L"HitGroupNullProc");
        np.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        np.IntersectionShaderImport = name(r.prefix + L"IsectNull");
        for (const auto* h : { &nt, &np }) {
            addGroup(*h);
            hgNames.push_back(h->HitGroupExport);
        }

        // Local root signatures, explicit, so no default of the
        // application's reaches the shim's shaders: the hit groups and their
        // shaders a root SRV to their record's pair, or nothing; the miss
        // nothing.
        std::vector<LPCWSTR> hitExports(hgNames), missExports;
        for (const auto& w : shaders) {
            if (w == r.prefix + L"Miss") missExports.push_back(name(w));
            else hitExports.push_back(name(w));
        }
        ID3D12RootSignature* hitRs = sig(r.recordConstants);
        ID3D12RootSignature* missRs = sig(false);
        if (!hitRs || !missRs) {
            *why = "could not create the local root signatures of the shim's hit groups";
            return Outcome::kRefused;
        }
        if (hitRs == missRs) {
            std::vector<LPCWSTR> all(hitExports);
            all.insert(all.end(), missExports.begin(), missExports.end());
            assoc(localSig(hitRs), all);
        } else {
            assoc(localSig(hitRs), hitExports);
            assoc(localSig(missRs), missExports);
        }
        // Everything else the shader is associated with by name (a global
        // root signature, a shader config), the shim's shaders are too.
        std::vector<LPCWSTR> ours(hitExports);
        ours.insert(ours.end(), missExports.begin(), missExports.end());
        for (size_t i = 0; i < before; ++i) {
            if (out->subs[i].Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION) continue;
            const auto* a = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(out->subs[i].pDesc);
            const size_t t = (size_t)(uintptr_t)a->pSubobjectToAssociate;
            if (t >= before || out->subs[t].Type == D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE) continue;
            bool names = false;
            for (UINT e = 0; e < a->NumExports; ++e) names = names || r.name == a->pExports[e];
            if (names) assoc(t, ours);
        }
    }
    for (auto& a : out->assoc)
        a.pSubobjectToAssociate = &out->subs[(size_t)(uintptr_t)a.pSubobjectToAssociate];
    out->desc.Type = in.Type;
    out->desc.NumSubobjects = (UINT)out->subs.size();
    out->desc.pSubobjects = out->subs.data();
    char b[200];
    std::snprintf(b, sizeof(b), "%zu RayQuery shader(s) lowered into the application's %s (%zu from linked "
                  "collections), %u query slot(s), %zu subobjects%s", info->hosted.size(),
                  collection ? "collection" : "pipeline", info->hosted.size() - own, total, out->subs.size(),
                  deeper ? ", recursion depth raised by 1" : "");
    out->summary = b;
    *infoOut = info;
    return Outcome::kTransformed;
}

bool Attach(ID3D12StateObject* so, const std::shared_ptr<Info>& info, std::string* why) {
    ComPtr<ID3D12StateObjectProperties> props;
    if (FAILED(so->QueryInterface(IID_PPV_ARGS(&props)))) { *why = "no state object properties"; return false; }
    if (info->collection) {
        auto* h = new InfoHolder(info);
        so->SetPrivateDataInterface(kInfoGuid, h);
        h->Release();
        return true;
    }
    auto id = [&](const std::wstring& w, uint8_t* dst) {
        const void* p = props->GetShaderIdentifier(w.c_str());
        if (!p) return false;
        std::memcpy(dst, p, kId);
        return true;
    };
    for (auto& r : info->hosted) {
        for (auto& u : r.units)
            if (!id(u.name, u.id)) { *why = "an identifier of a RayQuery shader's records is missing"; return false; }
        if (!id(r.prefix + L"Miss", r.idMiss) || !id(r.prefix + L"HitGroupNullTri", r.idNullTri) ||
            !id(r.prefix + L"HitGroupNullProc", r.idNullProc)) {
            *why = "an identifier of the shim's shaders is missing";
            return false;
        }
        const size_t sets = r.perQuery.empty() ? 1 : r.perQuery.size();
        r.idHit.assign(sets, {});
        r.idHitProc.assign(sets, {});
        for (size_t q = 0; q < sets; ++q) {
            const std::wstring sfx = q ? L"_q" + std::to_wstring(q) : std::wstring();
            if (!id(r.prefix + L"HitGroup" + sfx, r.idHit[q].data())) {
                *why = "an identifier of the shim's hit groups is missing";
                return false;
            }
            id(r.prefix + L"HitGroupProc" + sfx, r.idHitProc[q].data());
        }
    }
    auto* h = new InfoHolder(info);
    so->SetPrivateDataInterface(kInfoGuid, h);
    h->Release();
    return true;
}

std::shared_ptr<Info> Get(ID3D12StateObject* so) {
    if (!so) return nullptr;
    IUnknown* u = nullptr;
    UINT size = sizeof(u);
    if (FAILED(so->GetPrivateData(kInfoGuid, &size, &u)) || !u) return nullptr;
    std::shared_ptr<Info> i = static_cast<InfoHolder*>(u)->Get();
    u->Release();
    return i;
}

bool Untouched(const Info& info, const D3D12_DISPATCH_RAYS_DESC& d) {
    for (const auto& h : info.hosted)
        if (h.kind != kRaygen) return false;
    const restrack::Found f = restrack::Find(d.RayGenerationShaderRecord.StartAddress);
    if (!f.resource || (f.heap != D3D12_HEAP_TYPE_UPLOAD && f.heap != D3D12_HEAP_TYPE_READBACK) ||
        f.offset + kId > f.resource->GetDesc().Width)
        return false;
    uint8_t* p = nullptr;
    D3D12_RANGE rr{ (SIZE_T)f.offset, (SIZE_T)(f.offset + kId) };
    if (FAILED(f.resource->Map(0, &rr, reinterpret_cast<void**>(&p))) || !p) return false;
    uint8_t id[32];
    std::memcpy(id, p + f.offset, kId);
    D3D12_RANGE none{ 0, 0 };
    f.resource->Unmap(0, &none);
    for (const auto& h : info.hosted)
        for (const auto& u : h.units)
            if (!std::memcmp(id, u.id, kId)) return false;
    return true;
}

bool Dispatch(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, const Info& info,
              const D3D12_DISPATCH_RAYS_DESC& d, const std::vector<gidx::SceneSel>& sels,
              const void* owner, const std::function<void()>& restore, std::string* why) {
    const size_t H = info.hosted.size();
    if (sels.size() != H) { *why = "no scene selection for every RayQuery shader"; return false; }
    // Each shader's slots, as indices into the scenes the dispatch traces.
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> scenes;
    std::vector<std::vector<UINT>> sceneOf(H);
    for (size_t h = 0; h < H; ++h) {
        const Hosted& hs = info.hosted[h];
        const gidx::SceneSel& sel = sels[h];
        const std::string who = " (in " + Narrow(hs.name) + ")";
        if (!sel.valid) {
            *why = (sel.why.empty() ? "which scene a query traces is not known" : sel.why) + who;
            return false;
        }
        const UINT S = (UINT)gidx::SceneSlots(hs.scenes).size();
        if (!S || sel.need.size() != S || sel.fixed.size() != sel.Subs() || sel.scenes.empty()) {
            *why = "the scene selection does not match the query's scene slots" + who;
            return false;
        }
        for (UINT j = 0; j < S; ++j)
            if (sel.need[j] > 1) {
                *why = "a query in an application's DXR shader whose scene is picked per ray among several "
                       "scenes (an array at a dynamic element, or the heap at a computed index)" + who;
                return false;
            }
        for (UINT j = 0; j < S; ++j) {
            const int fx = sel.fixed[sel.Sub(j)];
            if (fx < 0) {
                *why = "a query in an application's DXR shader whose scene is in a local root signature" + who;
                return false;
            }
            const D3D12_GPU_VIRTUAL_ADDRESS a = sel.scenes[(size_t)fx];
            auto it = std::find(scenes.begin(), scenes.end(), a);
            sceneOf[h].push_back((UINT)(it - scenes.begin()));
            if (it == scenes.end()) scenes.push_back(a);
        }
    }
    const auto& missT = d.MissShaderTable;
    const auto& hitT = d.HitGroupTable;
    if ((!missT.StrideInBytes && missT.SizeInBytes > kId) || (!hitT.StrideInBytes && hitT.SizeInBytes)) {
        *why = "a miss or hit group table of stride 0 beside a RayQuery shader";
        return false;
    }

    // Each scene as its latest build: its instances, and their rows.
    const UINT D = (UINT)scenes.size();
    std::vector<std::vector<astrack::TlasInfo::Row>> rows(D);
    for (UINT x = 0; x < D; ++x) {
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
        const UINT64 b = astrack::LatestBuild(scenes[x]);
        if (b && astrack::InstancesAt(scenes[x], b, &descs, &rows[x])) continue;
        if (!astrack::Current({ scenes[x] }) || !astrack::Rows(scenes[x], &rows[x])) {
            *why = "a scene a query traces is not read from its latest build yet";
            return false;
        }
    }

    // After the application's hit records the shim's block: each scene's
    // copy reaches it from base on, Q query slots per (instance, geometry).
    const UINT appRg = (UINT)(d.RayGenerationShaderRecord.SizeInBytes & ~3ull);
    const UINT appMissStride = (UINT)missT.StrideInBytes;
    const UINT appMiss = appMissStride ? (UINT)(missT.SizeInBytes / appMissStride) : (missT.SizeInBytes ? 1u : 0u);
    const UINT appMissSrc = appMissStride ? appMissStride : (UINT)(missT.SizeInBytes & ~3ull);
    const UINT appHitStride = (UINT)hitT.StrideInBytes;
    const UINT appHit = appHitStride ? (UINT)(hitT.SizeInBytes / appHitStride) : 0u;
    const UINT Q = info.queries;
    shimscene::Activate(Q);
    std::vector<shimscene::Copy> cps(D);
    std::vector<UINT> base(D + 1, appHit);
    for (UINT x = 0; x < D; ++x) {
        std::string cw;
        if (rows[x].empty()) {
            cps[x].tlas = { scenes[x] };
            cps[x].kc = 1;
            cps[x].gmax = 1;
            cps[x].base = base[x];
            base[x + 1] = base[x];
            continue;
        }
        if (!shimscene::Ensure(cl, dev, scenes[x], Q, base[x], owner, &cps[x], &cw)) {
            restore();
            *why = "no copy of a scene a query traces: " + cw;
            return false;
        }
        for (const void* r : cps[x].tlasRes) gpuhold::Use(r, owner);
        gpuhold::Use(cps[x].contribRes, owner);
        const UINT64 end = (UINT64)base[x] + (UINT64)cps[x].count * cps[x].kc * cps[x].gmax;
        if (end > 0xFFFFFFull) {
            restore();
            *why = "the hit group table would need more than 2^24 records";
            return false;
        }
        base[x + 1] = (UINT)end;
    }
    bool track = false, rc = false;
    for (const auto& h : info.hosted) { track = track || h.track; rc = rc || h.recordConstants; }
    for (UINT x = 0; x < D; ++x) {
        if (rows[x].size() != cps[x].count && !rows[x].empty()) {
            restore();
            *why = "the instances of a scene are not read as its copy has them";
            return false;
        }
        for (const auto& r : rows[x]) {
            if (!r.active) continue;
            std::string w;
            if (!r.geometries) w = "an instance on a bottom-level structure whose geometry the shim does not know";
            else if ((r.reach & astrack::kReachTriangles) && (r.reach & astrack::kReachProcedural))
                w = "a bottom-level structure holding both triangles and procedural primitives";
            else if (r.geometries > cps[x].gmax) w = "an instance with more geometries than its scene copy has room for";
            else if (track && !r.opacityKnown)
                w = "a query reads committed state inside its loop, and an instance's structure is deserialized";
            if (!w.empty()) { restore(); *why = w; return false; }
        }
    }

    // Sizes and places: the records the units' arguments need room in, the
    // shim's table (GPU memory), and the upload buffer holding what the shim
    // writes itself: the copy pass's groups and arguments, the shim's misses,
    // its block and its pairs.
    auto argDwords = [](const Hosted& h) { return 2 * h.srvs + kConstants; };
    UINT needRg = appRg, needMiss = (std::max)(appMissSrc, kId), needHit = (std::max)(appHitStride, kId + (rc ? 8u : 0u));
    UINT G = 0, argsAll = 0;
    for (const auto& h : info.hosted) {
        argsAll += argDwords(h);
        for (const auto& u : h.units) {
            ++G;
            UINT& need = h.kind == kRaygen ? needRg : h.kind == kMiss ? needMiss : needHit;
            need = (std::max)(need, u.insert + 4 * argDwords(h));
        }
    }
    const UINT rgBytes = AlignUp(needRg, kRecAlign);
    const UINT missStride = AlignUp(needMiss, kRecAlign);
    const UINT hitStride = AlignUp(needHit, kRecAlign);
    const UINT records = base[D];
    const UINT block = records - appHit;
    const UINT missOff = AlignUp(rgBytes, kTableAlign);
    const UINT misses = appMiss + (UINT)H;
    const UINT hitOff = AlignUp(missOff + missStride * misses, kTableAlign);
    const UINT64 total = (UINT64)hitOff + (UINT64)hitStride * (std::max)(records, 1u);
    const UINT metaDwords = 11 * G + argsAll;
    const UINT u1 = AlignUp(4 * metaDwords, kTableAlign);
    const UINT u2 = AlignUp(u1 + missStride * (UINT)H, kTableAlign);
    const UINT u3 = u2 + hitStride * block;
    const UINT64 upBytes = (UINT64)u3 + 12ull * block + 4;

    auto table = gidx::AcquireBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, total, owner);
    auto up = gidx::AcquireBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, upBytes, owner);
    if (!table || !up) {
        restore();
        *why = "could not create the shim's tables";
        return false;
    }
    uint8_t* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    if (FAILED(up->Map(0, &none, reinterpret_cast<void**>(&p))) || !p) {
        restore();
        *why = "could not map the shim's records";
        return false;
    }
    std::memset(p, 0, (size_t)upBytes);
    const D3D12_GPU_VIRTUAL_ADDRESS uva = up->GetGPUVirtualAddress();
    // DXR_TIER11_RGQ_POISON=1 points every miss index at the application's
    // first miss: the sensitivity check of the run-time miss index. =2 gives
    // every record of the shim's block the first instance's contribution;
    // =3 tells every shader the pipeline has one query slot (0.83.0).
    static const int poison = [] {
        char b[4] = {};
        if (!GetEnvironmentVariableA("DXR_TIER11_RGQ_POISON", b, sizeof(b))) return 0;
        return b[0] == '3' ? 3 : b[0] == '2' ? 2 : 1;
    }();
    uint32_t* meta = reinterpret_cast<uint32_t*>(p);
    {
        UINT g = 0, first = 11 * G;
        for (size_t h = 0; h < H; ++h) {
            const Hosted& hs = info.hosted[h];
            // Its arguments: each slot's scene copy, then the index of its
            // miss (after the application's), its first query slot, and the
            // pipeline's query slots.
            for (UINT j = 0; j < hs.srvs; ++j) {
                const UINT x = sceneOf[h][j < hs.slots ? j : 0];
                const D3D12_GPU_VIRTUAL_ADDRESS a = cps[x].tlas[0];
                std::memcpy(meta + first + 2 * j, &a, 8);
            }
            meta[first + 2 * hs.srvs] = poison == 1 ? 0u : appMiss + (UINT)h;
            meta[first + 2 * hs.srvs + 1] = hs.queryBase;
            meta[first + 2 * hs.srvs + 2] = poison == 3 ? 1u : Q;
            for (const auto& u : hs.units) {
                std::memcpy(meta + 11 * g, u.id, kId);
                meta[11 * g + 8] = u.insert;
                meta[11 * g + 9] = argDwords(hs);
                meta[11 * g + 10] = first;
                ++g;
            }
            first += argDwords(hs);
            std::memcpy(p + u1 + missStride * (UINT)h, hs.idMiss, kId);
        }
    }
    // One record per (instance, geometry, query slot), of the kind its
    // structure holds; nothing reaches the rest, which get a rejecting
    // triangle record.
    auto idFor = [&](uint8_t reach, UINT s) -> const void* {
        const Hosted* hp = &info.hosted[0];
        for (const auto& h : info.hosted)
            if (s >= h.queryBase && s < h.queryBase + h.Queries()) hp = &h;
        const Hosted& rg = *hp;
        const UINT q = s - rg.queryBase;
        if (!rg.perQuery.empty()) {
            if (reach & astrack::kReachProcedural) return rg.idHitProc[q].data();
            if (reach & astrack::kReachTriangles) return rg.idHit[q].data();
            return rg.idNullTri;
        }
        if (reach & astrack::kReachProcedural)
            return rg.servesProc ? (rg.servesTri ? rg.idHitProc[0].data() : rg.idHit[0].data()) : rg.idNullProc;
        if (reach & astrack::kReachTriangles) return rg.servesTri ? rg.idHit[0].data() : rg.idNullTri;
        return rg.idNullTri;
    };
    for (UINT r = 0; r < block; ++r) std::memcpy(p + u2 + r * hitStride, info.hosted[0].idNullTri, kId);
    for (UINT x = 0; x < D; ++x) {
        const UINT gmax = cps[x].gmax, per = cps[x].kc * gmax;
        for (UINT i = 0; i < (UINT)rows[x].size(); ++i) {
            const auto& row = rows[x][i];
            if (!row.active) continue;
            for (UINT g = 0; g < row.geometries; ++g)
                for (UINT s = 0; s < Q; ++s) {
                    const UINT r = base[x] + i * per + g * Q + s - appHit;
                    std::memcpy(p + u2 + r * hitStride, idFor(row.reach, s), kId);
                    if (!rc) continue;
                    const uint32_t pair[3] = { g, poison == 2 ? rows[x][0].contribution : row.contribution,
                                               g < row.opaque.size() ? (uint32_t)row.opaque[g] : 0u };
                    std::memcpy(p + u3 + r * 12u, pair, sizeof(pair));
                }
        }
    }
    if (rc)
        for (UINT r = 0; r < block; ++r) {
            const D3D12_GPU_VIRTUAL_ADDRESS pa = uva + u3 + (UINT64)r * 12u;
            std::memcpy(p + u2 + r * hitStride + kId, &pa, 8);
        }
    up->Unmap(0, nullptr);

    // The table, assembled on the GPU: the application's records from
    // wherever they are, the units' arguments written into theirs, the
    // shim's from the upload buffer. Disjoint ranges, so the passes need no
    // barrier between them.
    const D3D12_GPU_VIRTUAL_ADDRESS tva = table->GetGPUVirtualAddress();
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = table.Get();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &bar);
    std::string cw;
    const bool ok =
        CopyWithArgs(cl, dev, d.RayGenerationShaderRecord.StartAddress, 1, appRg, tva, rgBytes, uva, G, &cw) &&
        CopyWithArgs(cl, dev, missT.StartAddress, appMiss, appMissSrc, tva + missOff, missStride, uva, G, &cw) &&
        gidx::CopyRecords(cl, dev, uva + u1, (UINT)H, missStride, tva + missOff + (UINT64)appMiss * missStride,
                          missStride, &cw) &&
        CopyWithArgs(cl, dev, hitT.StartAddress, appHit, appHitStride, tva + hitOff, hitStride, uva, G, &cw) &&
        gidx::CopyRecords(cl, dev, uva + u2, block, hitStride, tva + hitOff + (UINT64)appHit * hitStride,
                          hitStride, &cw);
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &bar);
    restore();
    if (!ok) {
        *why = "could not copy the shader tables: " + cw;
        return false;
    }
    D3D12_DISPATCH_RAYS_DESC mine = d;
    mine.RayGenerationShaderRecord = { tva, rgBytes };
    mine.MissShaderTable = { tva + missOff, (UINT64)missStride * misses, missStride };
    mine.HitGroupTable = { tva + hitOff, (UINT64)hitStride * (std::max)(records, 1u), hitStride };
    cl->DispatchRays(&mine);
    static LONG first = 0;
    if (InterlockedCompareExchange(&first, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] RayQuery in DXR shaders: first dispatch, %u application hit records "
                 "and %u of the shim's, %u application miss records and %zu of the shim's\n",
                 appHit, block, appMiss, H);
    return true;
}

}  // namespace rgq
