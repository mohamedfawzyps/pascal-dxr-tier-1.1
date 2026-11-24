#pragma once

#include <string>
#include <vector>

#include "rq_lower.h"

namespace rq {

// RayQuery in an APPLICATION's raygen (0.80.0). See rghost.py, which this
// mirrors byte for byte. All on normalised text.

// The library presented as a compute shader holding only `raygen`, for the
// compute lowering; false with *why when the library has no such raygen.
bool RaygenAsCs(const std::string& in, const std::string& raygen, std::string* out,
                std::string* why);
// The library without `raygen`; *empty when it was the library's only shader.
bool StripFunction(const std::string& in, const std::string& raygen, std::string* out,
                   bool* empty, std::string* why);
// The lowering's export names for a raygen's queries.
Exports RaygenExports(const std::string& raygen);
// The module flags the validator computed, from what it said; false when it
// said nothing of them.
bool FixFlags(const std::string& in, const std::string& said, std::string* out);

// The lowered raygen library made ready for the application's pipeline
// (0.81.0): the lowered TraceRay's miss index read at run time (the constant
// at b0, space kGeomIndexSpace, in the raygen's local root signature), and
// the lowered calls pointed at the shim's copy of the scene with (query k,
// queries); the application's own TraceRay left alone. point_queries in
// rghost.py. `mult`: the lowered calls' multiplier, 1 with record data.
std::string ShimMiss(const std::string& in);
bool PointRaygenQueries(const std::string& in, unsigned queries, unsigned mult,
                        const std::vector<unsigned>& slotOf, unsigned slots,
                        const std::vector<unsigned>& caps, std::string* out, std::string* why);

}  // namespace rq
