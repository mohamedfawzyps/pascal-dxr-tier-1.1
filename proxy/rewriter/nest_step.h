#pragma once

#include <string>

namespace rq {

// A query used inside another query's Proceed loop (0.79.0): the outer query
// is STEPPED, its loop kept in the raygen and each Proceed() a search by a
// fresh query that keeps the nearest candidate past the last one. See nest.py,
// which this mirrors byte for byte. Runs on normalised text after the NVAPI
// fold, before analysis. Returns false with *why set when the outer body does
// something for a procedural candidate (the search sees triangles only).
bool StepNested(const std::string& in, std::string* out, std::string* why);

}  // namespace rq
