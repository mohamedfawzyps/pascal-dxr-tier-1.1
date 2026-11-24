// RayQuery in an APPLICATION's raygen (0.81.0, plan step 4 b).
//
// A raygen can call TraceRay, so its queries lower exactly as a compute
// shader's do (the rewriter half, 0.80.0: phase5/rewriter/rghost.py,
// proxy/rewriter/rg_host.h). What a compute shader gets from the shim, a
// state object and a shader table of its own, an application's raygen gets
// INSIDE the application's pipeline and tables:
//
//   1. CreateStateObject. Each raygen that uses RayQuery is lowered into a
//      library of its own (the raygen, its own TraceRay calls kept, and the
//      generated shaders dxr11_rq_<raygen>_*), and removed from the
//      application's library. The lowered TraceRay traces the shim's copy of
//      the scene (shimscene) with (query k, queries), and takes its miss
//      index at run time. The raygen's local root signature gets the copy's
//      address and that miss index appended (gidx::ExtendUnits), so its own
//      arguments keep their offsets. The shim's hit groups and miss join the
//      pipeline, shielded from any default local root signature, and given
//      whatever else the raygen is associated with (global root signature,
//      shader config). The payload and attribute limits are raised to the
//      lowering's.
//   2. DispatchRays. The shim's copies of the three tables: the
//      application's records unchanged (copied on the GPU, from wherever they
//      are), the raygen record with the appended arguments, the shim's miss
//      after the application's misses, and after the application's hit
//      records a block of the shim's, one per (instance, geometry, query),
//      which the scene copy's instance contributions point at.
//
// Refused by name, each on the limits list: a collection holding such a
// raygen, or a pipeline linking a collection (plan step 4 c); RayQuery in a
// closest-hit, miss or other DXR shader (4 c); a library with an export
// list or subobjects of its own; a scene picked per ray, or one in the
// raygen's local root signature; a scene not read from its latest build
// yet; a GPU-resident raygen record in a pipeline with several raygens; a
// table of stride 0; GeometryIndex() in the same pipeline.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "geom_index_so.h"

namespace rgq {

struct QueryShape {
    bool anyHit = false, needsBoth = false, trackProc = false;
};

// One raygen of the pipeline that uses RayQuery, lowered.
struct Raygen {
    std::wstring name;             // its export name
    std::wstring prefix;           // dxr11_rq_<name>_
    bool hasAnyHit = false, hasIntersection = false, needsBoth = false;
    bool recordConstants = false, track = false, trackProc = false;
    bool servesTri = true, servesProc = false;
    std::vector<QueryShape> perQuery;   // records per query (0.77.0)
    UINT payloadBytes = 0;
    gidx::Scenes scenes;           // where its lowered TraceRay calls take their scene
    UINT slots = 1;                // scene slots, one structure each
    UINT insert = 0;               // where the appended arguments start in its record
    // Read from the state object once it exists.
    uint8_t idRay[32] = {}, idMiss[32] = {}, idNullTri[32] = {}, idNullProc[32] = {};
    std::vector<std::array<uint8_t, 32>> idHit, idHitProc;
    UINT Queries() const { return perQuery.empty() ? 1u : (UINT)perQuery.size(); }
};

struct Info {
    std::vector<Raygen> raygens;
    UINT raygensInPipeline = 0;    // every raygen, querying or not
};

enum class Outcome { kNothing, kTransformed, kRefused };

// `dev` is the REAL device. On kTransformed create `out->desc` instead, then
// Attach the info to what it made.
Outcome Transform(ID3D12Device* dev, const D3D12_STATE_OBJECT_DESC& in, gidx::Transformed* out,
                  std::shared_ptr<Info>* info, std::string* why);
// Reads the identifiers and keeps the info with the state object.
bool Attach(ID3D12StateObject* so, const std::shared_ptr<Info>& info, std::string* why);
std::shared_ptr<Info> Get(ID3D12StateObject* so);

// Which of info.raygens a dispatch runs: its index, -1 when it runs another
// raygen (the dispatch is the application's own), -2 when that cannot be
// told (*why).
int PickRaygen(const Info& info, const D3D12_DISPATCH_RAYS_DESC& d, std::string* why);

// Records the shim's three tables into `cl` and the DispatchRays on them.
// `sel`: which scene each of the raygen's scene slots traces. `restore` gives
// the application's bindings back after the shim's passes. False with *why,
// nothing dispatched, when it cannot be drawn.
bool Dispatch(ID3D12GraphicsCommandList4* cl, ID3D12Device5* dev, const Info& info, int raygen,
              const D3D12_DISPATCH_RAYS_DESC& d, const gidx::SceneSel& sel, const void* owner,
              const std::function<void()>& restore, std::string* why);

}  // namespace rgq
