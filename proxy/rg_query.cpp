// RayQuery in an APPLICATION's raygen (0.81.0). See rg_query.h.
#include "rg_query.h"

#include <d3d12.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <regex>
#include <set>

#include "as_tracker.h"
#include "dxil_scan.h"
#include "gpu_hold.h"
#include "proxy_log.h"
#include "res_tracker.h"
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
const UINT kRaygenKind = 7;
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
// the scene scan reads, so the application's own call is not a scene slot.
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

// One raygen, lowered: the library to add, and its shape.
bool LowerRaygen(const std::string& norm, const std::string& name, Raygen* r,
                 std::vector<uint8_t>* lib, std::string* why) {
    std::string cs, text;
    if (!rq::RaygenAsCs(norm, name, &cs, why)) return false;
    if (!rq::FoldNvapi(cs, &text, why)) return false;
    llm::Module m(text);
    auto a = rq::Analyze(m);
    if (!a.ok) { *why = a.error; return false; }
    auto l = rq::Lower(m, a.query, rq::RaygenExports(name));
    if (!l.ok) { *why = l.error; return false; }
    r->name = Wide(name);
    r->prefix = Wide("dxr11_rq_" + name + "_");
    r->hasAnyHit = a.query.AnyNeedsAnyHit();
    r->hasIntersection = a.query.NeedsIntersection();
    r->needsBoth = a.query.NeedsBoth();
    r->recordConstants = a.query.needsRecordConstants;
    r->track = a.query.track;
    r->trackProc = a.query.trackProc;
    for (const auto& o : a.query.others) r->track = r->track || o.track;
    r->servesProc = r->hasIntersection || r->needsBoth;
    r->servesTri = r->needsBoth || !r->hasIntersection;
    if (a.query.perQuery) {
        auto shape = [](const rq::Query& q) {
            QueryShape s;
            s.anyHit = q.hasLoop;
            s.needsBoth = q.NeedsBoth();
            s.trackProc = q.trackProc;
            return s;
        };
        r->perQuery.push_back(shape(a.query));
        for (const auto& o : a.query.others) r->perQuery.push_back(shape(o));
    }
    r->payloadBytes = (UINT)l.payloadBytes;

    // Its own calls' scenes, one slot each.
    const std::string own = OwnCallsOnly(l.text);
    gidx::ScanScenes(own, &r->scenes);
    if (r->scenes.unknown) {
        *why = "a lowered TraceRay whose scene could not be traced back to a register or the heap";
        return false;
    }
    // A slot picked by key (an array element, the heap) holds one scene
    // here: a dispatch whose keys reach several is refused (Dispatch).
    const auto slots = gidx::SceneSlots(r->scenes);
    std::vector<gidx::Scenes> calls;
    gidx::ScanCallScenes(own, &calls);
    std::vector<unsigned> slotOf;
    for (const auto& c : calls) {
        const int j = gidx::SlotOfCall(slots, c);
        if (j < 0) { *why = "a lowered TraceRay whose scene is not one of the shader's scene slots"; return false; }
        slotOf.push_back((unsigned)j);
    }
    r->slots = (UINT)slots.size();
    std::string pointed;
    if (!rq::PointRaygenQueries(llm::Normalize(l.text), r->Queries(), r->recordConstants ? 1u : 0u, slotOf,
                                r->slots, std::vector<unsigned>(r->slots, 1u), &pointed, why))
        return false;
    std::string err;
    if (!dxch::AssembleAndSign(pointed, lib, &err)) {
        *why = "the lowered raygen library did not assemble: " + err;
        return false;
    }
    return true;
}

// A library without the raygens moved out of it: its bytes, or *empty.
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
    *why = "the application's library without its RayQuery raygen did not assemble: " + err;
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

}  // namespace

Outcome Transform(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in, gidx::Transformed* out,
                  std::shared_ptr<Info>* infoOut, std::string* why) {
    const UINT n = in.NumSubobjects;
    const D3D12_STATE_SUBOBJECT* s = in.pSubobjects;
    std::vector<UINT> queryLibs;
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s[i].pDesc) continue;
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        if (Dxr11ContainerUsesRayQuery(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength))
            queryLibs.push_back(i);
    }
    if (queryLibs.empty()) return Outcome::kNothing;
    if (in.Type != D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE) {
        *why = "RayQuery in a DXR shader of a collection (plan step 4 c)";
        return Outcome::kRefused;
    }
    for (UINT i = 0; i < n; ++i)
        if (s[i].Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            *why = "RayQuery in a raygen of a pipeline that links a collection (plan step 4 c)";
            return Outcome::kRefused;
        }

    auto info = std::make_shared<Info>();
    // Every raygen of the pipeline, to know whether a dispatch can only run ours.
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !s[i].pDesc) continue;
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        std::vector<std::pair<std::string, uint32_t>> fns;
        gidx::LibraryFunctions(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength, &fns);
        for (const auto& f : fns) {
            if (f.second != kRaygenKind) continue;
            if (!ld->NumExports) { ++info->raygensInPipeline; continue; }
            for (UINT e = 0; e < ld->NumExports; ++e) {
                const auto& x = ld->pExports[e];
                if ((x.ExportToRename ? x.ExportToRename : x.Name) == Wide(f.first)) ++info->raygensInPipeline;
            }
        }
    }

    // Each library with RayQuery: its raygens lowered, the rest kept.
    std::deque<std::vector<uint8_t>> lowered, stripped;
    std::map<UINT, const std::vector<uint8_t>*> code;
    std::vector<const std::vector<uint8_t>*> added;   // lowered libraries to add
    std::vector<std::string> loweredText;
    for (UINT i : queryLibs) {
        const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s[i].pDesc);
        const void* bytes = ld->DXILLibrary.pShaderBytecode;
        const size_t size = ld->DXILLibrary.BytecodeLength;
        if (ld->NumExports) {
            *why = "RayQuery in a raygen of a library included through an export list";
            return Outcome::kRefused;
        }
        if (gidx::LibraryHasSubobjects(bytes, size)) {
            *why = "RayQuery in a raygen of a library that declares subobjects of its own";
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
        std::vector<std::string> raygens;
        for (const auto& q : QueryFunctions(norm)) {
            uint32_t kind = 0;
            for (const auto& f : fns) if (f.first == q) kind = f.second;
            if (kind != kRaygenKind) {
                *why = "RayQuery in " + q + ", a DXR shader other than a raygen (plan step 4 c)";
                return Outcome::kRefused;
            }
            raygens.push_back(q);
        }
        if (raygens.empty()) {
            *why = "a library flagged as using RayQuery with no function that does";
            return Outcome::kRefused;
        }
        for (const auto& q : raygens) {
            Raygen r;
            lowered.emplace_back();
            std::string w;
            if (!LowerRaygen(norm, q, &r, &lowered.back(), &w)) {
                *why = "RayQuery in raygen " + q + ": " + w;
                return Outcome::kRefused;
            }
            info->raygens.push_back(std::move(r));
            added.push_back(&lowered.back());
        }
        stripped.emplace_back();
        bool empty = false;
        if (!StripLibrary(norm, raygens, bytes, size, &stripped.back(), &empty, why)) return Outcome::kRefused;
        // A library left with no shader is replaced by one lowered library.
        if (empty) {
            code[i] = added.back();
            added.pop_back();
        } else {
            code[i] = &stripped.back();
        }
    }

    // Explicit associations of a library's subobject to the raygen: what it
    // names cannot be told here.
    std::set<std::wstring> rgNames;
    for (const auto& r : info->raygens) rgNames.insert(r.name);
    for (UINT i = 0; i < n; ++i) {
        if (s[i].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION || !s[i].pDesc) continue;
        const auto* a = static_cast<const D3D12_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s[i].pDesc);
        for (UINT e = 0; e < a->NumExports; ++e)
            if (rgNames.count(a->pExports[e])) {
                *why = "a RayQuery raygen associated by name with a library's subobject";
                return Outcome::kRefused;
            }
    }

    // The raygens' local root signatures, extended: one root SRV per scene
    // slot (the shim's copy), then the miss index. One unit per raygen.
    UINT srvs = 0;
    for (const auto& r : info->raygens) srvs = (std::max)(srvs, r.slots);
    std::vector<std::vector<std::wstring>> units;
    for (const auto& r : info->raygens) units.push_back({ r.name });
    std::vector<UINT> offsets;
    if (!gidx::ExtendUnits(dev, in, srvs, 1, units, code, out, &offsets, why)) return Outcome::kRefused;
    for (size_t k = 0; k < info->raygens.size(); ++k) info->raygens[k].insert = offsets[k];
    for (auto& r : info->raygens) r.slots = srvs;   // every raygen's record carries `srvs`

    // The association pointers into out->subs, as indices, while it grows.
    for (auto& a : out->assoc)
        a.pSubobjectToAssociate = reinterpret_cast<const D3D12_STATE_SUBOBJECT*>(
            (uintptr_t)(a.pSubobjectToAssociate - out->subs.data()));
    const size_t before = out->subs.size();

    // Payload and attribute limits raised to the lowering's; a depth of 0 to 1.
    UINT payload = 0;
    for (const auto& r : info->raygens) payload = (std::max)(payload, r.payloadBytes);
    bool config = false;
    for (size_t k = 0; k < before; ++k) {
        auto& so = out->subs[k];
        if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG && so.pDesc) {
            D3D12_RAYTRACING_SHADER_CONFIG c = *static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(so.pDesc);
            c.MaxPayloadSizeInBytes = (std::max)(c.MaxPayloadSizeInBytes, payload);
            c.MaxAttributeSizeInBytes = (std::max)(c.MaxAttributeSizeInBytes, kAttrBytes);
            out->raw.emplace_back(sizeof(c));
            std::memcpy(out->raw.back().data(), &c, sizeof(c));
            so.pDesc = out->raw.back().data();
            config = true;
        } else if (so.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG && so.pDesc) {
            D3D12_RAYTRACING_PIPELINE_CONFIG c = *static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(so.pDesc);
            if (c.MaxTraceRecursionDepth) continue;
            c.MaxTraceRecursionDepth = 1;
            out->raw.emplace_back(sizeof(c));
            std::memcpy(out->raw.back().data(), &c, sizeof(c));
            so.pDesc = out->raw.back().data();
        }
    }
    if (!config) {
        *why = "a RayQuery raygen in a pipeline whose shader config is not at state object scope";
        return Outcome::kRefused;
    }

    // The lowered libraries not already in place of the application's.
    for (const auto* b : added) {
        out->code.push_back(*b);   // kept with the description
        D3D12_DXIL_LIBRARY_DESC ld{};
        ld.DXILLibrary.pShaderBytecode = out->code.back().data();
        ld.DXILLibrary.BytecodeLength = out->code.back().size();
        out->libs.push_back(ld);
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &out->libs.back() });
    }

    ComPtr<ID3D12RootSignature> pairRs, emptyRs;
    auto sig = [&](bool pairSrv) -> ID3D12RootSignature* {
        ComPtr<ID3D12RootSignature>& r = pairSrv ? pairRs : emptyRs;
        if (!r && !LocalRs(dev, pairSrv, &r)) return nullptr;
        return r.Get();
    };
    auto name = [&](const std::wstring& w) {
        out->names.push_back(w);
        return out->names.back().c_str();
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
    auto localSig = [&](ID3D12RootSignature* rs) {
        out->sigs.push_back(rs);
        out->lrs.push_back({ rs });
        out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE, &out->lrs.back() });
        return out->subs.size() - 1;
    };

    for (size_t k = 0; k < info->raygens.size(); ++k) {
        Raygen& r = info->raygens[k];
        // Its generated shaders, as the lowered library defines them.
        const std::vector<uint8_t>* lib = nullptr;
        {
            size_t at = 0;
            for (auto& l : lowered) { if (at++ == k) { lib = &l; break; } }
        }
        std::string text, err;
        if (!lib || !dxch::Disassemble(lib->data(), lib->size(), &text, &err)) {
            *why = "the lowered raygen library could not be read back";
            return Outcome::kRefused;
        }
        const std::vector<std::wstring> shaders = Defined(text, Narrow(r.prefix));
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
        std::vector<LPCWSTR> groups;
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
                out->raw.emplace_back(sizeof(hp));
                std::memcpy(out->raw.back().data(), &hp, sizeof(hp));
                out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, out->raw.back().data() });
                groups.push_back(hp.HitGroupExport);
            } else if (st.isect) {
                hg.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
                hg.IntersectionShaderImport = name(nm(L"Isect", q));
            } else {
                hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
                if (st.anyHit) hg.AnyHitShaderImport = name(nm(L"AnyHit", q));
            }
            out->raw.emplace_back(sizeof(hg));
            std::memcpy(out->raw.back().data(), &hg, sizeof(hg));
            out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, out->raw.back().data() });
            groups.push_back(hg.HitGroupExport);
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
            out->raw.emplace_back(sizeof(*h));
            std::memcpy(out->raw.back().data(), h, sizeof(*h));
            out->subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, out->raw.back().data() });
            groups.push_back(h->HitGroupExport);
        }

        // Local root signatures, explicit, so no default of the
        // application's reaches the shim's shaders: the hit groups and their
        // shaders a root SRV to their record's pair, or nothing; the miss
        // nothing.
        std::vector<LPCWSTR> hitExports(groups), missExports;
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
        // Everything else the raygen is associated with by name (a global
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
    char b[160];
    std::snprintf(b, sizeof(b), "%zu RayQuery raygen(s) lowered into the application's pipeline, "
                  "%zu subobjects", info->raygens.size(), out->subs.size());
    out->summary = b;
    *infoOut = info;
    return Outcome::kTransformed;
}

bool Attach(ID3D12StateObject* so, const std::shared_ptr<Info>& info, std::string* why) {
    ComPtr<ID3D12StateObjectProperties> props;
    if (FAILED(so->QueryInterface(IID_PPV_ARGS(&props)))) { *why = "no state object properties"; return false; }
    auto id = [&](const std::wstring& w, uint8_t* dst) {
        const void* p = props->GetShaderIdentifier(w.c_str());
        if (!p) return false;
        std::memcpy(dst, p, kId);
        return true;
    };
    for (auto& r : info->raygens) {
        if (!id(r.name, r.idRay) || !id(r.prefix + L"Miss", r.idMiss) ||
            !id(r.prefix + L"HitGroupNullTri", r.idNullTri) || !id(r.prefix + L"HitGroupNullProc", r.idNullProc)) {
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

int PickRaygen(const Info& info, const D3D12_DISPATCH_RAYS_DESC& d, std::string* why) {
    // The record's identifier, when the CPU can read it.
    const restrack::Found f = restrack::Find(d.RayGenerationShaderRecord.StartAddress);
    if (f.resource && (f.heap == D3D12_HEAP_TYPE_UPLOAD || f.heap == D3D12_HEAP_TYPE_READBACK) &&
        f.offset + kId <= f.resource->GetDesc().Width) {
        uint8_t* p = nullptr;
        D3D12_RANGE rr{ (SIZE_T)f.offset, (SIZE_T)(f.offset + kId) };
        if (SUCCEEDED(f.resource->Map(0, &rr, reinterpret_cast<void**>(&p))) && p) {
            uint8_t id[32];
            std::memcpy(id, p + f.offset, kId);
            D3D12_RANGE none{ 0, 0 };
            f.resource->Unmap(0, &none);
            for (size_t k = 0; k < info.raygens.size(); ++k)
                if (!std::memcmp(id, info.raygens[k].idRay, kId)) return (int)k;
            return -1;
        }
    }
    // In GPU memory: ours when it is the pipeline's only raygen.
    if (info.raygensInPipeline == 1 && info.raygens.size() == 1) return 0;
    *why = "the raygen record is in GPU memory and the pipeline has several raygens, so which one "
           "the dispatch runs cannot be told";
    return -2;
}

bool Dispatch(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, const Info& info, int k,
              const D3D12_DISPATCH_RAYS_DESC& d, const gidx::SceneSel& sel, const void* owner,
              const std::function<void()>& restore, std::string* why) {
    const Raygen& rg = info.raygens[(size_t)k];
    if (!sel.valid) {
        *why = sel.why.empty() ? "which scene the raygen's query traces is not known" : sel.why;
        return false;
    }
    const UINT S = (UINT)gidx::SceneSlots(rg.scenes).size();
    if (!S || sel.need.size() != S || sel.fixed.size() != sel.Subs() || sel.scenes.empty()) {
        *why = "the scene selection does not match the query's scene slots";
        return false;
    }
    for (UINT j = 0; j < S; ++j)
        if (sel.need[j] > 1) {
            *why = "a query in an application's raygen whose scene is picked per ray among several "
                   "scenes (an array at a dynamic element, or the heap at a computed index)";
            return false;
        }
    for (int fx : sel.fixed)
        if (fx < 0) {
            *why = "a query in an application's raygen whose scene is in the raygen's local root "
                   "signature";
            return false;
        }
    const auto& missT = d.MissShaderTable;
    const auto& hitT = d.HitGroupTable;
    if ((!missT.StrideInBytes && missT.SizeInBytes > kId) || (!hitT.StrideInBytes && hitT.SizeInBytes)) {
        *why = "a miss or hit group table of stride 0 beside a RayQuery raygen";
        return false;
    }

    // Each scene as its latest build: its instances, and their rows.
    const UINT D = (UINT)sel.scenes.size();
    std::vector<std::vector<astrack::TlasInfo::Row>> rows(D);
    for (UINT x = 0; x < D; ++x) {
        const D3D12_GPU_VIRTUAL_ADDRESS a = sel.scenes[x];
        std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descs;
        const UINT64 b = astrack::LatestBuild(a);
        if (b && astrack::InstancesAt(a, b, &descs, &rows[x])) continue;
        if (!astrack::Current({ a }) || !astrack::Rows(a, &rows[x])) {
            *why = "the scene the raygen's query traces is not read from its latest build yet";
            return false;
        }
    }

    // Its records: (application miss records, then the shim's), and after
    // the application's hit records the shim's block, which each scene's
    // copy reaches from `base` on.
    const UINT appRg = (UINT)(d.RayGenerationShaderRecord.SizeInBytes & ~3ull);
    const UINT appMissStride = (UINT)missT.StrideInBytes;
    const UINT appMiss = appMissStride ? (UINT)(missT.SizeInBytes / appMissStride) : (missT.SizeInBytes ? 1u : 0u);
    const UINT appMissSrc = appMissStride ? appMissStride : (UINT)(missT.SizeInBytes & ~3ull);
    const UINT appHitStride = (UINT)hitT.StrideInBytes;
    const UINT appHit = appHitStride ? (UINT)(hitT.SizeInBytes / appHitStride) : 0u;
    const UINT Q = rg.Queries();
    shimscene::Activate(Q);
    std::vector<shimscene::Copy> cps(D);
    std::vector<UINT> base(D + 1, appHit);
    for (UINT x = 0; x < D; ++x) {
        std::string cw;
        if (rows[x].empty()) {
            cps[x].tlas = { sel.scenes[x] };
            cps[x].kc = 1;
            cps[x].gmax = 1;
            cps[x].base = base[x];
            base[x + 1] = base[x];
            continue;
        }
        if (!shimscene::Ensure(cl, dev, sel.scenes[x], Q, base[x], owner, &cps[x], &cw)) {
            restore();
            *why = "no copy of the scene the raygen's query traces: " + cw;
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
    for (UINT x = 0; x < D; ++x) {
        if (rows[x].size() != cps[x].count && !rows[x].empty()) {
            restore();
            *why = "the instances of the scene are not read as its copy has them";
            return false;
        }
        for (const auto& r : rows[x]) {
            if (!r.active) continue;
            std::string w;
            if (!r.geometries) w = "an instance on a bottom-level structure whose geometry the shim does not know";
            else if ((r.reach & astrack::kReachTriangles) && (r.reach & astrack::kReachProcedural))
                w = "a bottom-level structure holding both triangles and procedural primitives";
            else if (r.geometries > cps[x].gmax) w = "an instance with more geometries than its scene copy has room for";
            else if (rg.track && !r.opacityKnown)
                w = "the query reads committed state inside its loop, and an instance's structure is deserialized";
            if (!w.empty()) { restore(); *why = w; return false; }
        }
    }

    // Sizes and places, in the shim's table (GPU memory) and in the upload
    // buffer holding what the shim writes itself.
    const UINT rgSrvs = rg.slots;
    const UINT appendBytes = 8 * rgSrvs + 4;
    const UINT rgBytes = AlignUp(rg.insert + appendBytes, kRecAlign);
    const UINT missStride = AlignUp((std::max)(appMissSrc, kId), kRecAlign);
    const UINT hitStride = AlignUp((std::max)(appHitStride, kId + (rg.recordConstants ? 8u : 0u)), kRecAlign);
    const UINT records = base[D];
    const UINT block = records - appHit;
    const UINT missOff = AlignUp(rgBytes, kTableAlign);
    const UINT hitOff = AlignUp(missOff + missStride * (appMiss + 1), kTableAlign);
    const UINT64 total = (UINT64)hitOff + (UINT64)hitStride * (std::max)(records, 1u);
    const UINT u1 = AlignUp(appendBytes, kTableAlign);
    const UINT u2 = AlignUp(u1 + missStride, kTableAlign);
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
    // The appended arguments: each slot's scene copy, then the miss index.
    // DXR_TIER11_RGQ_POISON=1 points the miss index at the application's
    // first miss: the sensitivity check of the run-time miss index. =2 gives
    // every record of the shim's block the first instance's contribution.
    static const int poison = [] {
        char b[4] = {};
        if (!GetEnvironmentVariableA("DXR_TIER11_RGQ_POISON", b, sizeof(b))) return 0;
        return b[0] == '2' ? 2 : 1;
    }();
    for (UINT j = 0; j < rgSrvs; ++j) {
        const UINT su = j < S ? sel.Sub(j) : 0u;
        const D3D12_GPU_VIRTUAL_ADDRESS a = cps[(size_t)sel.fixed[su]].tlas[0];
        std::memcpy(p + 8 * j, &a, 8);
    }
    const uint32_t missIndex = poison == 1 ? 0u : appMiss;
    std::memcpy(p + 8 * rgSrvs, &missIndex, 4);
    std::memcpy(p + u1, rg.idMiss, kId);
    // One record per (instance, geometry, query), of the kind its structure
    // holds; nothing reaches the rest, which get the rejecting triangle one.
    auto idFor = [&](uint8_t reach, UINT q) -> const void* {
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
    for (UINT r = 0; r < block; ++r) std::memcpy(p + u2 + r * hitStride, rg.idNullTri, kId);
    for (UINT x = 0; x < D; ++x) {
        const UINT gmax = cps[x].gmax, per = cps[x].kc * gmax;
        for (UINT i = 0; i < (UINT)rows[x].size(); ++i) {
            const auto& row = rows[x][i];
            if (!row.active) continue;
            for (UINT g = 0; g < row.geometries; ++g)
                for (UINT q = 0; q < Q; ++q) {
                    const UINT r = base[x] + i * per + g * Q + q - appHit;
                    std::memcpy(p + u2 + r * hitStride, idFor(row.reach, q), kId);
                    if (!rg.recordConstants) continue;
                    const uint32_t pair[3] = { g, poison == 2 ? rows[x][0].contribution : row.contribution,
                                               g < row.opaque.size() ? (uint32_t)row.opaque[g] : 0u };
                    std::memcpy(p + u3 + r * 12u, pair, sizeof(pair));
                }
        }
    }
    if (rg.recordConstants)
        for (UINT r = 0; r < block; ++r) {
            const D3D12_GPU_VIRTUAL_ADDRESS pa = uva + u3 + (UINT64)r * 12u;
            std::memcpy(p + u2 + r * hitStride + kId, &pa, 8);
        }
    up->Unmap(0, nullptr);

    // The table, assembled on the GPU: the application's records from
    // wherever they are, the shim's from the upload buffer. Disjoint ranges,
    // so the passes need no barrier between them.
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
        gidx::CopyRecords(cl, dev, d.RayGenerationShaderRecord.StartAddress, 1, (std::min)(appRg, rg.insert),
                          tva, rg.insert, &cw) &&
        gidx::CopyRecords(cl, dev, uva, 1, appendBytes, tva + rg.insert, appendBytes, &cw) &&
        gidx::CopyRecords(cl, dev, missT.StartAddress, appMiss, appMissSrc, tva + missOff, missStride, &cw) &&
        gidx::CopyRecords(cl, dev, uva + u1, 1, missStride, tva + missOff + (UINT64)appMiss * missStride,
                          missStride, &cw) &&
        gidx::CopyRecords(cl, dev, hitT.StartAddress, appHit, appHitStride, tva + hitOff, hitStride, &cw) &&
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
    mine.RayGenerationShaderRecord = { tva, (UINT64)rg.insert + appendBytes };
    mine.MissShaderTable = { tva + missOff, (UINT64)missStride * (appMiss + 1), missStride };
    mine.HitGroupTable = { tva + hitOff, (UINT64)hitStride * (std::max)(records, 1u), hitStride };
    cl->DispatchRays(&mine);
    static LONG first = 0;
    if (InterlockedCompareExchange(&first, 1, 0) == 0)
        ProxyLog("[dxr-tier-11-proxy-log] RayQuery raygen: first dispatch, %u application hit records "
                 "and %u of the shim's, %u application miss records and the shim's\n",
                 appHit, block, appMiss);
    return true;
}

}  // namespace rgq
