// Spec Tier 1.1 item 6: D3D12_RAYTRACING_PIPELINE_CONFIG1 flags (0.64.0).
//
// The spec: D3D12_RAYTRACING_PIPELINE_FLAG_SKIP_TRIANGLES and
// ..._SKIP_PROCEDURAL_PRIMITIVES "behave like OR'ing the equivalent RAY_FLAGS
// above into any TraceRay() call in a raytracing pipeline, except that these
// do not show up in a RayFlags() call from a shader."
//
// Measured (tier11/pflagtest.cpp): the GTX 1070 REJECTS a pipeline carrying a
// PIPELINE_CONFIG1, as a subobject or declared in a library
// (CreateStateObject E_INVALIDARG), so until 0.64.0 an application using one
// got no pipeline at all. The ray flags themselves work natively.
//
// So the shim does what the spec says the flags mean: every TraceRay in the
// pipeline's libraries, and in the collections it links, gets the flags OR'd
// into its ray flags; a CONFIG1 becomes a CONFIG of the same depth; and where
// the shaders read RayFlags(), the flags are masked back off when that is
// exact: when every TraceRay of the pipeline passes flags that are literals
// without those bits (then no ray can carry them but through the pipeline).
// Otherwise RayFlags() shows them, as WARP does (measured: WARP shows them,
// against the spec), and the log says so.
//
// A collection is compiled before the pipeline that links it, so the shim
// keeps a copy of every collection's description (NoteCollection) and builds
// the collection again with the pipeline's flags when they differ from the
// ones it was built with.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace pflags {

// What was applied to a state object: the flags, and whether RayFlags() is
// masked. `set` false: nothing (no CONFIG1 anywhere).
struct Mode {
    bool set = false;
    UINT flags = 0;
    bool mask = false;
    bool operator==(const Mode& o) const { return set == o.set && flags == o.flags && mask == o.mask; }
    bool operator!=(const Mode& o) const { return !(*this == o); }
};

// The description handed on instead of the application's; owns everything
// it points at.
struct Rewritten {
    D3D12_STATE_OBJECT_DESC desc{};
    Mode mode;
    std::vector<D3D12_STATE_SUBOBJECT> subs;
    std::deque<std::vector<uint8_t>> code;
    std::deque<D3D12_DXIL_LIBRARY_DESC> libs;
    std::deque<D3D12_RAYTRACING_PIPELINE_CONFIG> cfgs;
    std::deque<D3D12_EXISTING_COLLECTION_DESC> colls;
    std::deque<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> assoc;
    std::vector<Microsoft::WRL::ComPtr<ID3D12StateObject>> keep;   // collections built again
    std::string summary;
};

enum class Outcome { kNothing, kTransformed, kRefused };

// Creates a collection from `desc` with `mode` forced: the device's own path,
// so everything else the shim does to a collection happens to it too.
using CreateFn = std::function<HRESULT(const D3D12_STATE_OBJECT_DESC& desc, const Mode& mode,
                                       Microsoft::WRL::ComPtr<ID3D12StateObject>* out)>;

// `forced`: a mode imposed from outside (a collection built again for a
// pipeline); null, the one the description itself asks for.
Outcome Apply(const D3D12_STATE_OBJECT_DESC& in, const Mode* forced, const CreateFn& create, Rewritten* out,
              std::string* why);

// After a collection is created: `original` is the application's description
// (copied and kept on the object), `applied` the mode it was built with.
void NoteCollection(ID3D12StateObject* so, const D3D12_STATE_OBJECT_DESC& original, const Mode& applied);

// A collection's kept copy of the application's description and the mode it
// was built with: for building it again (0.83.0, rg_query: with the shader
// and pipeline configs a pipeline hosting RayQuery shaders imposes). The
// description lives as long as the collection. False when none was kept.
bool CollectionOriginal(ID3D12StateObject* so, D3D12_STATE_OBJECT_DESC* desc, Mode* applied);

// Keeps collections built again alive as long as the pipeline using them.
void KeepWith(ID3D12StateObject* so, const Rewritten& r);

}  // namespace pflags
