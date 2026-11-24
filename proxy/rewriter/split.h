#pragma once

#include <string>
#include <vector>

namespace rq {

// A RayQuery compute shader beside groupshared memory, group barriers or wave
// ops, split at its query (0.84.0, plan step 5). See split.py in
// phase5/rewriter, which this mirrors byte for byte: A runs up to the query
// and stores what the rest needs in a scratch buffer (u0, space
// kGeomIndexSpace), Q is the query alone (an ordinary RayQuery compute
// shader, lowered as any), B runs from where the query's control flow
// rejoins. Runs on normalised text.
struct SplitResult {
    bool ok = false;
    std::string error;
    std::string a, q, b;
    unsigned K = 0;          // dwords a thread in the scratch buffer, the flag included
    unsigned G = 0;          // dwords a group, for the groupshared arrays saved
    unsigned threads[3] = { 1, 1, 1 };
    unsigned stored = 0;
    std::vector<std::string> saved;
};

SplitResult SplitAtQuery(const std::string& text);

}  // namespace rq
