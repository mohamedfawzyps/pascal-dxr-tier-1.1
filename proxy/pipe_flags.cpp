// See the header.
#include "pipe_flags.h"

#include "geom_index_so.h"
#include "proxy_log.h"
#include "state_object_cache.h"
#include "rewriter/dxc_host.h"
#include "rewriter/ll_model.h"

#include <windows.h>

#include <memory>
#include <regex>

using Microsoft::WRL::ComPtr;

namespace pflags {
namespace {

// {3B7E9A12-5C4D-4E8F-A1B2-6D0C9E7F4A21}
const GUID kCollGuid = { 0x3b7e9a12, 0x5c4d, 0x4e8f, { 0xa1, 0xb2, 0x6d, 0x0c, 0x9e, 0x7f, 0x4a, 0x21 } };
// {8C2F4D61-0A7B-4B3E-9E15-7F6A2D1C8B03}
const GUID kKeepGuid = { 0x8c2f4d61, 0x0a7b, 0x4b3e, { 0x9e, 0x15, 0x7f, 0x6a, 0x2d, 0x1c, 0x8b, 0x03 } };

// A collection's copy of the application's description, and how it was built.
struct CollInfo {
    StateObjectStore store;
    Mode applied;
    Mode declared;   // its own CONFIG1, if it has one
};

template <class T>
class Holder : public IUnknown {
public:
    explicit Holder(std::unique_ptr<T> v) : m_v(std::move(v)) {}
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
    T* Get() { return m_v.get(); }
private:
    LONG m_refs = 1;
    std::unique_ptr<T> m_v;
};

CollInfo* GetColl(ID3D12StateObject* so) {
    if (!so) return nullptr;
    IUnknown* u = nullptr;
    UINT size = sizeof(u);
    if (FAILED(so->GetPrivateData(kCollGuid, &size, &u)) || !u) return nullptr;
    auto* h = static_cast<Holder<CollInfo>*>(u);
    CollInfo* c = h->Get();
    u->Release();   // the object keeps its own reference
    return c;
}

// The CONFIG1 a description asks for itself, API or library.
bool Declared(const D3D12_STATE_OBJECT_DESC& d, Mode* m, std::string* why) {
    bool any = false;
    auto take = [&](UINT flags) {
        if (any && m->flags != flags) {
            *why = "two pipeline configs with different flags";
            return false;
        }
        any = true;
        m->set = true;
        m->flags = flags;
        return true;
    };
    for (UINT i = 0; i < d.NumSubobjects; ++i) {
        const auto& s = d.pSubobjects[i];
        if (!s.pDesc) continue;
        if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1) {
            if (!take((UINT)static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(s.pDesc)->Flags)) return false;
        } else if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
            const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s.pDesc);
            UINT depth = 0, flags = 0;
            if (gidx::LibraryConfig1(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength, &depth,
                                     &flags) &&
                !take(flags))
                return false;
        } else if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            const auto* c = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc);
            CollInfo* ci = GetColl(c->pExistingCollection);
            if (ci && ci->declared.set && !take(ci->declared.flags)) return false;
        }
    }
    return true;
}

const std::regex& TraceRe() {
    static const std::regex r(R"RX((@dx\.op\.traceRay\.[^(]*\(i32 157, %dx\.types\.Handle [^,]+, i32 )([^,]+)(,))RX");
    return r;
}
const std::regex& RayFlagsRe() {
    static const std::regex r(R"RX(^(\s*)(%[\w.]+) = (call i32 @dx\.op\.rayFlags\.i32\(i32 144\).*)$)RX");
    return r;
}

// Every TraceRay's ray flags operand in `text`: all literals without `bits`?
void ScanTraces(const std::string& text, UINT bits, bool* literalClean, bool* traces, bool* reads) {
    for (const auto& l : llm::SplitLines(text)) {
        if (l.find("@dx.op.rayFlags.") != std::string::npos && l.find(" = call") != std::string::npos)
            *reads = true;
        std::smatch m;
        if (!std::regex_search(l, m, TraceRe())) continue;
        *traces = true;
        const std::string tok = m[2].str();
        if (!std::regex_match(tok, std::regex(R"(-?\d+)"))) { *literalClean = false; continue; }
        if ((UINT)std::stoll(tok) & bits) *literalClean = false;
    }
}

// The flags OR'd into every TraceRay, and RayFlags() masked with `mask`.
std::string Rewrite(const std::string& normalized, UINT flags, bool mask) {
    std::vector<std::string> out;
    int n = 0;
    for (const auto& l : llm::SplitLines(normalized)) {
        std::smatch m;
        if (flags && std::regex_search(l, m, TraceRe())) {
            const std::string tok = m[2].str();
            std::string rep;
            if (std::regex_match(tok, std::regex(R"(-?\d+)"))) {
                rep = std::to_string((int32_t)((UINT)std::stoll(tok) | flags));
            } else {
                rep = "%pflags." + std::to_string(n++);
                const size_t ind = l.find_first_not_of(' ');
                out.push_back(std::string(ind == std::string::npos ? 2 : ind, ' ') + rep + " = or i32 " + tok + ", " +
                              std::to_string((int32_t)flags));
            }
            out.push_back(m.prefix().str() + m[1].str() + rep + m[3].str() + m.suffix().str());
            continue;
        }
        if (flags && mask && std::regex_match(l, m, RayFlagsRe())) {
            out.push_back(m[1].str() + m[2].str() + ".pfraw = " + m[3].str());
            out.push_back(m[1].str() + m[2].str() + " = and i32 " + m[2].str() + ".pfraw, " +
                          std::to_string((int32_t)~flags));
            continue;
        }
        out.push_back(l);
    }
    return llm::JoinLines(out);
}

bool Disasm(const D3D12_DXIL_LIBRARY_DESC* ld, std::string* text, std::string* why) {
    std::string err;
    if (!dxch::Disassemble(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength, text, &err)) {
        *why = "a library could not be disassembled: " + err;
        return false;
    }
    *text = llm::Normalize(*text);
    return true;
}

}  // namespace

Outcome Apply(const D3D12_STATE_OBJECT_DESC& in, const Mode* forced, const CreateFn& create, Rewritten* out,
              std::string* why) {
    Mode mode;
    if (!Declared(in, &mode, why)) return Outcome::kRefused;
    if (forced) mode = *forced;
    if (!mode.set) return Outcome::kNothing;

    // Every library the pipeline runs, its own and its collections', to see
    // whether masking RayFlags() is exact.
    const UINT n = in.NumSubobjects;
    std::vector<std::string> texts(n);
    std::vector<int> needs(n, 0);   // 1: rewrite this library, 2: build this collection again
    bool clean = true;
    for (UINT i = 0; i < n; ++i) {
        const auto& s = in.pSubobjects[i];
        if (!s.pDesc) continue;
        if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
            const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s.pDesc);
            if (!Disasm(ld, &texts[i], why)) return Outcome::kRefused;
            bool traces = false, reads = false;
            ScanTraces(texts[i], mode.flags, &clean, &traces, &reads);
            UINT d = 0, f = 0;
            const bool own1 = gidx::LibraryConfig1(ld->DXILLibrary.pShaderBytecode, ld->DXILLibrary.BytecodeLength,
                                                   &d, &f);
            if ((mode.flags && (traces || reads)) || own1) needs[i] = 1;
        } else if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
            const auto* c = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc);
            CollInfo* ci = GetColl(c->pExistingCollection);
            if (!ci) {
                *why = "a collection the shim did not see created, so it cannot be built again with the "
                       "pipeline's flags";
                return Outcome::kRefused;
            }
            const D3D12_STATE_OBJECT_DESC cd = ci->store.Desc(D3D12_STATE_OBJECT_TYPE_COLLECTION);
            for (UINT k = 0; k < cd.NumSubobjects; ++k) {
                if (cd.pSubobjects[k].Type != D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY || !cd.pSubobjects[k].pDesc)
                    continue;
                std::string t;
                if (!Disasm(static_cast<const D3D12_DXIL_LIBRARY_DESC*>(cd.pSubobjects[k].pDesc), &t, why))
                    return Outcome::kRefused;
                bool traces = false, reads = false;
                ScanTraces(t, mode.flags, &clean, &traces, &reads);
            }
            needs[i] = 2;   // decided against the final mode below
        }
    }
    if (!forced) mode.mask = clean;
    // DXR_TIER11_PFLAGS_POISON=1: the config is still stripped, but no
    // TraceRay gets the flags; the sensitivity check for tier11/pflagtest.
    static const bool poison = GetEnvironmentVariableA("DXR_TIER11_PFLAGS_POISON", nullptr, 0) != 0;
    const UINT applied = poison ? 0u : mode.flags;

    // The description again: libraries rewritten, CONFIG1 made CONFIG,
    // collections built again where they were built another way. One entry
    // per original, so association pointers keep their index.
    out->mode = mode;
    out->subs.assign(in.pSubobjects, in.pSubobjects + n);
    UINT rewritten = 0, rebuilt = 0;
    for (UINT i = 0; i < n; ++i) {
        auto& s = out->subs[i];
        if (!s.pDesc) continue;
        if (s.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1) {
            out->cfgs.push_back({ static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(s.pDesc)->MaxTraceRecursionDepth });
            s.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
            s.pDesc = &out->cfgs.back();
        } else if (needs[i] == 1) {
            const auto* ld = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(s.pDesc);
            out->code.emplace_back();
            std::string err;
            if (!gidx::ReassembleLibrary(Rewrite(texts[i], applied, mode.mask), ld->DXILLibrary.pShaderBytecode,
                                         ld->DXILLibrary.BytecodeLength, true, &out->code.back(), &err)) {
                *why = "a library with the pipeline flags applied did not assemble: " + err;
                return Outcome::kRefused;
            }
            D3D12_DXIL_LIBRARY_DESC nl = *ld;
            nl.DXILLibrary = { out->code.back().data(), out->code.back().size() };
            out->libs.push_back(nl);
            s.pDesc = &out->libs.back();
            ++rewritten;
        } else if (needs[i] == 2) {
            const auto* c = static_cast<const D3D12_EXISTING_COLLECTION_DESC*>(s.pDesc);
            CollInfo* ci = GetColl(c->pExistingCollection);
            if (ci->applied == mode) continue;
            ComPtr<ID3D12StateObject> nc;
            const HRESULT hr = create(ci->store.Desc(D3D12_STATE_OBJECT_TYPE_COLLECTION), mode, &nc);
            if (FAILED(hr) || !nc) {
                char b[128];
                std::snprintf(b, sizeof(b), "a collection built again with the pipeline's flags failed, hr=0x%08lX",
                              (unsigned long)hr);
                *why = b;
                return Outcome::kRefused;
            }
            out->keep.push_back(nc);
            D3D12_EXISTING_COLLECTION_DESC ec = *c;
            ec.pExistingCollection = nc.Get();
            out->colls.push_back(ec);
            s.pDesc = &out->colls.back();
            ++rebuilt;
        }
    }
    // An association points into the array it is in: a copy, pointing at the
    // same index of ours. `subs` does not move from here on.
    for (auto& s : out->subs) {
        if (s.Type != D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION || !s.pDesc) continue;
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION a = *static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(s.pDesc);
        const ptrdiff_t k = a.pSubobjectToAssociate - in.pSubobjects;
        if (k < 0 || (UINT)k >= n) continue;
        a.pSubobjectToAssociate = &out->subs[(size_t)k];
        out->assoc.push_back(a);
        s.pDesc = &out->assoc.back();
    }
    out->desc = in;
    out->desc.pSubobjects = out->subs.data();
    char b[256];
    std::snprintf(b, sizeof(b), "pipeline flags 0x%X applied: %u librar%s rewritten, %u collection(s) built again, "
                  "RayFlags() %s", mode.flags, rewritten, rewritten == 1 ? "y" : "ies", rebuilt,
                  !mode.flags ? "unchanged" : mode.mask ? "masked, as the spec says"
                  : "NOT masked (a TraceRay passes flags that are not literals without them), as WARP shows them");
    out->summary = b;
    return Outcome::kTransformed;
}

void NoteCollection(ID3D12StateObject* so, const D3D12_STATE_OBJECT_DESC& original, const Mode& applied) {
    if (!so) return;
    auto ci = std::make_unique<CollInfo>();
    std::string why;
    if (!ci->store.Append(original, false, why)) {
        ProxyLog("[dxr-tier-11-proxy-log] a collection could not be kept for pipeline flags (%s)\n", why.c_str());
        return;
    }
    ci->store.StripAdditionsFlag();
    ci->applied = applied;
    std::string w;
    Declared(original, &ci->declared, &w);
    auto* h = new Holder<CollInfo>(std::move(ci));
    so->SetPrivateDataInterface(kCollGuid, h);
    h->Release();
}

bool CollectionOriginal(ID3D12StateObject* so, D3D12_STATE_OBJECT_DESC* desc, Mode* applied) {
    CollInfo* c = GetColl(so);
    if (!c) return false;
    *desc = c->store.Desc(D3D12_STATE_OBJECT_TYPE_COLLECTION);
    *applied = c->applied;
    return true;
}

void KeepWith(ID3D12StateObject* so, const Rewritten& r) {
    if (!so || r.keep.empty()) return;
    auto v = std::make_unique<std::vector<ComPtr<ID3D12StateObject>>>(r.keep);
    auto* h = new Holder<std::vector<ComPtr<ID3D12StateObject>>>(std::move(v));
    so->SetPrivateDataInterface(kKeepGuid, h);
    h->Release();
}

}  // namespace pflags
