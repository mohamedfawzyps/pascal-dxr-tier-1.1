// RayQuery in an APPLICATION's raygen, closest-hit or miss (0.81.0 raygen,
// 0.82.0 closest-hit and miss; plan step 4 b, c).
//
// These can call TraceRay, so their queries lower exactly as a compute
// shader's do (the rewriter half, 0.80.0: phase5/rewriter/rghost.py,
// proxy/rewriter/rg_host.h); a closest-hit or miss is given its parameters
// and entry back afterwards (RestoreFunction). What a compute shader gets
// from the shim, a state object and a shader table of its own, these get
// INSIDE the application's pipeline and tables:
//
//   1. CreateStateObject. Each shader that uses RayQuery (a HOSTED shader) is
//      lowered into a library of its own and removed from the application's.
//      Its lowered TraceRay traces the shim's copy of the scene (shimscene),
//      with ray contribution (its first query slot + k) and multiplier (the
//      pipeline's query slots), and takes its miss index at run time. The
//      local root signature of its UNITS (a raygen or miss: itself; a
//      closest-hit: each hit group importing it, with that group's shaders)
//      gets the copy's address and that miss index appended
//      (gidx::ExtendUnits), so the application's own arguments keep their
//      offsets. The shim's hit groups and misses join the pipeline, shielded
//      from any default local root signature, and given whatever else the
//      shader is associated with by name. Payload and attribute limits are
//      raised to the lowering's; a closest-hit or miss raises the recursion
//      depth by 1, since its query traces one level deeper.
//   2. DispatchRays. The shim's copies of the three tables, made on the GPU
//      (proxy/rg_table.hlsl) from wherever the application's are: every
//      record copied, the appended arguments written into each record of a
//      unit; the shim's misses after the application's; after the
//      application's hit records the shim's block, one record per (instance,
//      geometry, query slot), which the scene copy's instance contributions
//      point at.
//
// Refused by name, each on the limits list: a collection holding such a
// shader, or a pipeline linking a collection (plan step 4 d); RayQuery in an
// any-hit, intersection or callable shader (plan step 7); a library with an
// export list or subobjects of its own; a scene picked per ray among several,
// or one in a local root signature; a scene not read from its latest build
// yet; a table of stride 0; more than 15 query slots in a pipeline;
// GeometryIndex() in the same pipeline.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "geom_index_so.h"

namespace rgq {

struct QueryShape {
    bool anyHit = false, needsBoth = false, trackProc = false;
};

// Records that carry a hosted shader's appended arguments: those of a raygen
// or miss export, or of a hit group.
struct Unit {
    std::wstring name;
    UINT insert = 0;               // where the arguments start in its records
    uint8_t id[32] = {};           // read from the state object once it exists
};

// One shader of the pipeline that uses RayQuery, lowered.
struct Hosted {
    std::wstring name;             // its export name
    std::wstring prefix;           // dxr11_rq_<name>_
    UINT kind = 7;                 // DXIL shader kind: 7 raygen, 10 closest-hit, 11 miss
    bool hasAnyHit = false, hasIntersection = false, needsBoth = false;
    bool recordConstants = false, track = false, trackProc = false;
    bool servesTri = true, servesProc = false;
    std::vector<QueryShape> perQuery;   // records per query (0.77.0)
    UINT payloadBytes = 0;
    gidx::Scenes scenes;           // where its lowered TraceRay calls take their scene
    UINT slots = 1;                // its scene slots, one structure each
    UINT queryBase = 0;            // its first query slot in the shim's block
    std::vector<Unit> units;
    uint8_t idMiss[32] = {}, idNullTri[32] = {}, idNullProc[32] = {};
    std::vector<std::array<uint8_t, 32>> idHit, idHitProc;
    UINT Queries() const { return perQuery.empty() ? 1u : (UINT)perQuery.size(); }
};

struct Info {
    std::vector<Hosted> hosted;
    UINT queries = 0;              // query slots in all
    UINT srvs = 1;                 // scene copies every unit's records carry
};

enum class Outcome { kNothing, kTransformed, kRefused };

// `dev` is the REAL device. On kTransformed create `out->desc` instead, then
// Attach the info to what it made.
Outcome Transform(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in, gidx::Transformed* out,
                  std::shared_ptr<Info>* info, std::string* why);
// Reads the identifiers and keeps the info with the state object.
bool Attach(ID3D12StateObject* so, const std::shared_ptr<Info>& info, std::string* why);
std::shared_ptr<Info> Get(ID3D12StateObject* so);

// Whether a dispatch reaches no hosted shader, and so can be forwarded as it
// is: none is a closest-hit or miss, and its raygen record, read where the
// CPU can, is another raygen's.
bool Untouched(const Info& info, const D3D12_DISPATCH_RAYS_DESC& d);

// Records the shim's three tables into `cl` and the DispatchRays on them.
// `sels`, per hosted shader: which scene each of its scene slots traces.
// `restore` gives the application's bindings back after the shim's passes.
// False with *why, nothing dispatched, when it cannot be drawn.
bool Dispatch(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, const Info& info,
              const D3D12_DISPATCH_RAYS_DESC& d, const std::vector<gidx::SceneSel>& sels,
              const void* owner, const std::function<void()>& restore, std::string* why);

}  // namespace rgq
