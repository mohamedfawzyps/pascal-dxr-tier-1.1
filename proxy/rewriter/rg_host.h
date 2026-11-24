#pragma once

#include <string>

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

}  // namespace rq
