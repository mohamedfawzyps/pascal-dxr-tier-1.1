#include "rq_analyze.h"

#include <algorithm>
#include <map>
#include <regex>

namespace rq {
namespace {

const std::map<int, const char*>& KnownTable() {
    static const std::map<int, const char*> t = {
        { kAllocate, "AllocateRayQuery" },
        { kTraceInline, "TraceRayInline" },
        { kProceed, "Proceed" },
        { kAbort, "Abort" },
        { kCommitNonOpaque, "CommitNonOpaqueTriangleHit" },
        { kCommitProcedural, "CommitProceduralPrimitiveHit" },
        { kCandidateProcNonOpaque, "CandidateProceduralPrimitiveNonOpaque" },
        { kCommittedStatus, "CommittedStatus" },
        { kCandidateType, "CandidateType" },
        { kCandidateBary, "CandidateTriangleBarycentrics" },
        { kCommittedBary, "CommittedTriangleBarycentrics" },
        { kCommittedRayT, "CommittedRayT" },
        { kCommittedInstanceIndex, "CommittedInstanceIndex" },
        { kCommittedGeometryIndex, "CommittedGeometryIndex" },
        { kCommittedPrimitiveIndex, "CommittedPrimitiveIndex" },
        { kCandidateWorldToObject, "CandidateWorldToObject" },
        { kCommittedWorldToObject, "CommittedWorldToObject" },
        { kCandidateFrontFace, "CandidateTriangleFrontFace" },
        { kCommittedFrontFace, "CommittedTriangleFrontFace" },
        { kCandidateRayT, "CandidateTriangleRayT" },
        { kCandidateInstanceIndex, "CandidateInstanceIndex" },
        { kCandidateInstanceID, "CandidateInstanceID" },
        { kCandidatePrimitiveIndex, "CandidatePrimitiveIndex" },
        { kCandidateObjectRayOrigin, "CandidateObjectRayOrigin" },
        { kCandidateObjectRayDirection, "CandidateObjectRayDirection" },
        { kCommittedInstanceID, "CommittedInstanceID" },
        { kRayFlags, "RayFlags" },
        { kCandidateGeometryIndex, "CandidateGeometryIndex" },
        { kCandidateInstanceContrib,
          "CandidateInstanceContributionToHitGroupIndex" },
        { kCommittedInstanceContrib,
          "CommittedInstanceContributionToHitGroupIndex" },
    };
    return t;
}

int Imm(const std::string& arg) {
    static const std::regex re(R"(^i32\s+(-?\d+)$)");
    std::smatch m;
    std::string t = arg;
    t.erase(0, t.find_first_not_of(" \t"));
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
    if (std::regex_match(t, m, re)) return std::stoi(m[1].str());
    return 0;
}

}  // namespace

bool IsKnownOpcode(int op) { return KnownTable().count(op) != 0; }

const char* OpcodeName(int op) {
    auto it = KnownTable().find(op);
    return it == KnownTable().end() ? "?" : it->second;
}

// Empty, and that is the interesting part.
//
// CommittedGeometryIndex lived here for most of this project's life, because
// GeometryIndex() in a DXR 1.0 hit shader is ITSELF Tier 1.1: a library using
// it sets shader flag 0x2000000 and CreateStateObject on the GTX 1070 returns
// E_INVALIDARG. That measurement is still true. What changed is that the
// answer no longer has to come from an intrinsic: the shim builds the shader
// table, so it puts the geometry index in the record.
//
// A real Unreal 5.8 run refused 120 shaders on the four record opcodes, more
// than everything else together, which is what made it worth doing.
//
// Kept as a function rather than deleted, because the next opcode with no
// lowering will want somewhere to say so.
std::string NoLoweringReason(int) { return ""; }

bool IsCommittedOp(int op) {
    return op == kCommittedStatus || op == kCommittedBary || op == kCommittedRayT ||
           op == kCommittedInstanceIndex || op == kCommittedGeometryIndex ||
           op == kCommittedPrimitiveIndex || op == kCommittedInstanceID ||
           op == kCommittedFrontFace || op == kCommittedWorldToObject ||
           op == kCommittedInstanceContrib;
}

bool IsCandidateOp(int op) {
    return op == kCandidateType || op == kCandidateBary ||
           op == kCandidateProcNonOpaque ||
           op == kCandidateWorldToObject || op == kCandidateFrontFace ||
           op == kCandidateRayT || op == kCandidateInstanceIndex ||
           op == kCandidateInstanceID || op == kCandidatePrimitiveIndex ||
           op == kCandidateObjectRayOrigin || op == kCandidateObjectRayDirection ||
           op == kCandidateGeometryIndex || op == kCandidateInstanceContrib ||
           op == kRayFlags;
}

std::string FlagNames(int value) {
    static const std::pair<int, const char*> kFlags[] = {
        { 0x001, "FORCE_OPAQUE" }, { 0x002, "FORCE_NON_OPAQUE" },
        { 0x004, "ACCEPT_FIRST_HIT_AND_END_SEARCH" }, { 0x008, "SKIP_CLOSEST_HIT_SHADER" },
        { 0x010, "CULL_BACK_FACING_TRIANGLES" }, { 0x020, "CULL_FRONT_FACING_TRIANGLES" },
        { 0x040, "CULL_OPAQUE" }, { 0x080, "CULL_NON_OPAQUE" },
        { 0x100, "SKIP_TRIANGLES" }, { 0x200, "SKIP_PROCEDURAL_PRIMITIVES" },
    };
    std::string out;
    for (const auto& f : kFlags) {
        if (!(value & f.first)) continue;
        if (!out.empty()) out += "|";
        out += f.second;
    }
    return out.empty() ? "NONE" : out;
}

int Query::DynFlags() const { return trace ? Imm(trace->args[3]) : 0; }
int Query::RayFlags() const { return constFlags | DynFlags(); }

bool Query::StaticFlags() const {
    static const std::regex kImm(R"(^\s*i32\s+(-?\d+)\s*$)");
    return trace && std::regex_match(trace->args[3], kImm);
}

std::pair<std::string, std::string> Query::FlagsOperand() const {
    if (StaticFlags())
        return { "", "i32 " + std::to_string(RayFlags()) };
    const std::string dyn = llm::OperandName(trace->args[3]);
    return { "  %" + pfx + ".flags = or i32 " + dyn + ", " + std::to_string(constFlags),
             "i32 %" + pfx + ".flags" };
}

std::string Query::AsHandle() const {
    return trace ? llm::OperandName(trace->args[2]) : "";
}

std::vector<std::string> Query::RayArgs() const {
    std::vector<std::string> out;
    if (!trace) return out;
    for (size_t i = 4; i < trace->args.size() && i <= 12; ++i)
        out.push_back(trace->args[i]);
    return out;   // mask, ox, oy, oz, tmin, dx, dy, dz, tmax
}

namespace {

std::string RejectModule(const llm::Module& m) {
    std::smatch mm;
    static const std::regex kSm(R"(!dx\.shaderModel\s*=\s*!\{!(\d+)\})");
    if (std::regex_search(m.text, mm, kSm)) {
        const std::regex node("!" + mm[1].str() +
                              R"RX( = !\{!"(\w+)", i32 (\d+), i32 (\d+)\}\s*)RX");
        const int at = llm::FindLine(llm::SplitLines(m.text), node);
        if (at >= 0) {
            std::smatch nm;
            const std::string line = llm::SplitLines(m.text)[at];
            std::regex_match(line, nm, node);
            if (nm[1].str() != "cs")
                return "RayQuery in a \"" + nm[1].str() + "\" shader. DispatchRays only "
                       "launches raygen, so there is no promotion path from any other stage.";
        }
    }
    if (m.text.find("addrspace(3)") != std::string::npos)
        return "shader uses groupshared memory; a raygen shader has no thread group";
    return "";
}

// `others` are the handles of the entry point's other queries, whose ops
// belong to them.
std::string Collect(const llm::Function& fn, Query& q,
                    const std::vector<std::string>& others) {
    for (const auto& b : fn.blocks) {
        for (const auto& i : b.instrs) {
            const int op = i.DxOp();
            if (op < 0) continue;
            const auto uses = i.Uses();
            const bool usesHandle =
                std::find(uses.begin(), uses.end(), q.handle) != uses.end();
            if (!usesHandle) {
                bool theirs = false;
                for (const auto& h : others)
                    if (std::find(uses.begin(), uses.end(), h) != uses.end()) theirs = true;
                if (IsKnownOpcode(op) && op != kAllocate && !theirs)
                    return std::string(OpcodeName(op)) + " at \"" +
                           i.body.substr(0, 60) + "\" does not use the query handle";
                continue;
            }
            if (!IsKnownOpcode(op)) {
                return "unrecognised rayQuery opcode " + std::to_string(op) + " at \"" +
                       i.body.substr(0, 60) + "\". This project has only verified the "
                       "opcodes in the whitelist. Refusing rather than guessing.";
            }
            const std::string no = NoLoweringReason(op);
            if (!no.empty()) return no;

            if (op == kTraceInline) {
                if (q.trace) return "more than one TraceRayInline on one query";
                q.trace = &i; q.traceBlock = &b;
            } else if (op == kProceed) {
                q.proceeds.emplace_back(&b, &i);
            } else if (op == kCommitNonOpaque) {
                q.commits.emplace_back(&b, &i);
            } else if (op == kAbort) {
                q.aborts.emplace_back(&b, &i);
            } else if (op == kCommitProcedural) {
                q.procCommits.emplace_back(&b, &i);
            } else if (IsCandidateOp(op)) {
                if (RecordWord(op) >= 0) q.needsRecordConstants = true;
                q.candidateOps.emplace_back(&b, &i);
            } else if (IsCommittedOp(op)) {
                if (RecordWord(op) >= 0) q.needsRecordConstants = true;
                q.committedOps.emplace_back(&b, &i);
            }
        }
    }
    if (!q.trace) return "query is allocated but never traced";

    // Ray flags computed at runtime are FINE: dx.op.traceRay takes RayFlags
    // as an ordinary i32 operand, not an immediate. Measured, see
    // phase5/cases/reference/lib_dynflags_ref.hlsl. They were refused for one
    // version because Imm() had been silently folding an unknown value to 0
    // and tracing with the wrong flags; passing the value through is the right
    // answer. Unreal computes them at runtime in 118 shaders.
    return "";
}

std::string FindLoop(const llm::Function& fn, Query& q) {
    std::set<std::string> proceedBlocks;
    for (const auto& p : q.proceeds) proceedBlocks.insert(p.first->label);

    std::vector<llm::Loop> found;
    for (const auto& lp : fn.NaturalLoops())
        if (proceedBlocks.count(lp.latch)) found.push_back(lp);

    if (found.size() > 1) return "more than one Proceed loop on one query";
    if (found.size() == 1) { q.hasLoop = true; q.loop = found[0]; return ""; }
    if (q.proceeds.size() > 1)
        return std::to_string(q.proceeds.size()) +
               " Proceed calls but no loop; unrecognised shape";
    return "";
}

// The loop-body blocks a TRIANGLE candidate can reach (0.69.0): see
// reach_as_triangle in rayquery.py, which this mirrors.
std::set<std::string> ReachAs(const llm::Function& fn, const Query& q, int kind) {
    static const std::regex kIcmp(R"(^icmp (eq|ne) i32 (\S+), (\S+?)\s*$)");
    static const std::regex kInt(R"(^-?\d+$)");
    static const std::regex kBr(R"(^br i1 (\S+), label (\S+), label (\S+)$)");
    std::set<std::string> types;
    for (const auto& label : q.loop.body) {
        const llm::Block* b = fn.FindBlock(label);
        if (!b) continue;
        for (const auto& i : b->instrs)
            if (i.DxOp() == kCandidateType && !i.result.empty()) types.insert(i.result);
    }
    std::map<std::string, bool> known;
    for (const auto& label : q.loop.body) {
        const llm::Block* b = fn.FindBlock(label);
        if (!b) continue;
        for (const auto& i : b->instrs) {
            std::smatch m;
            if (i.result.empty() || !std::regex_match(i.body, m, kIcmp)) continue;
            const std::string a = m[2].str(), c = m[3].str();
            long long k;
            if (types.count(a) && std::regex_match(c, kInt)) k = std::stoll(c);
            else if (types.count(c) && std::regex_match(a, kInt)) k = std::stoll(a);
            else continue;
            known[i.result] = (k == kind) == (m[1].str() == "eq");
        }
    }
    std::set<std::string> seen;
    std::vector<std::string> todo{ q.loop.header };
    while (!todo.empty()) {
        const std::string label = todo.back();
        todo.pop_back();
        if (seen.count(label) || !q.loop.body.count(label)) continue;
        seen.insert(label);
        const llm::Block* b = fn.FindBlock(label);
        if (!b) continue;
        const llm::Instr* term = b->Terminator();
        std::smatch m;
        if (term && std::regex_match(term->body, m, kBr) && known.count(m[1].str())) {
            todo.push_back(known[m[1].str()] ? m[2].str() : m[3].str());
        } else {
            for (const auto& s : b->Successors()) todo.push_back(s);
        }
    }
    return seen;
}

// A triangle query whose body writes where a procedural candidate reaches it
// (0.69.0): see _mark_procedural_effects in rayquery.py.
void MarkProceduralEffects(const llm::Module& m, const llm::Function& fn, Query& q) {
    if (!q.hasLoop || !q.procCommits.empty()) return;
    static const std::regex kAttrLine(R"(^attributes\s+#(\d+)\s*=\s*\{(.*)\}\s*$)");
    static const std::regex kDeclLine(R"(^declare\s+.*@([\w.$]+)\s*\(.*\)\s*#(\d+)\s*$)");
    std::map<std::string, std::string> byNum, groups;
    std::smatch mm;
    for (const auto& line : m.lines)
        if (std::regex_match(line, mm, kAttrLine)) byNum[mm[1].str()] = mm[2].str();
    for (const auto& line : m.lines)
        if (std::regex_match(line, mm, kDeclLine)) {
            auto it = byNum.find(mm[2].str());
            groups[mm[1].str()] = it == byNum.end() ? "" : it->second;
        }
    for (const auto& label : ReachAs(fn, q, 1)) {
        const llm::Block* b = fn.FindBlock(label);
        if (!b) continue;
        for (const auto& i : b->instrs) {
            if (i.callee.compare(0, 6, "dx.op.") != 0) continue;
            if (i.callee.compare(0, 15, "dx.op.rayQuery_") == 0 ||
                i.callee == "dx.op.allocateRayQuery")
                continue;
            auto g = groups.find(i.callee);
            if (g != groups.end() && (g->second.find("readnone") != std::string::npos ||
                                      g->second.find("readonly") != std::string::npos))
                continue;
            q.procEffects = true;
            return;
        }
    }
}

std::string RejectFunction(const llm::Function& fn, Query& q) {
    for (const auto& b : fn.blocks) {
        for (const auto& i : b.instrs) {
            const int op = i.DxOp();
            if (op == kBarrier)
                return "shader uses a group barrier; a raygen shader has no thread group";
            std::string c = i.callee;
            std::transform(c.begin(), c.end(), c.begin(), ::tolower);
            if (!c.empty() && c.find(".wave") != std::string::npos)
                return "shader uses wave intrinsics around the query; promotion to "
                       "raygen changes lane occupancy";
        }
    }
    for (const auto& a : q.aborts) {
        if (!q.hasLoop || !q.loop.body.count(a.first->label))
            return "Abort() outside the Proceed loop; traversal can only be "
                   "stopped from the generated any-hit shader";
    }

    if (q.hasLoop) {
        for (const auto& label : q.loop.body) {
            const llm::Block* blk = fn.FindBlock(label);
            if (!blk) continue;
            for (const auto& i : blk->instrs)
                if (IsCommittedOp(i.DxOp())) {
                    q.committedInLoop = true;
                    // An intersection shader has RayTCurrent(), which IS the
                    // committed t, and no payload for the rest.
                    if (!q.procCommits.empty() && i.DxOp() != kCommittedRayT)
                        return "committed state other than CommittedRayT() is read inside a "
                               "Proceed loop that becomes an intersection shader, which has no "
                               "payload to read it from";
                }
        }
        // Committed state as it stands during traversal, 0.66.0: see
        // _reject_function in rayquery.py.
        if (q.committedInLoop && q.procCommits.empty() && !(q.RayFlags() & 1)) {
            q.track = true;
            q.needsRecordConstants = true;
        }
        // Abort(), exactly (0.67.0): see _reject_function in rayquery.py.
        if (!q.aborts.empty() && !q.committedOps.empty() && q.procCommits.empty() &&
            !(q.RayFlags() & 1)) {
            q.track = true;
            q.needsRecordConstants = true;
        }
        // CandidateProceduralPrimitiveNonOpaque() in the intersection shader
        // (0.70.0): see _reject_function in rayquery.py.
        if (!q.procCommits.empty()) {
            for (const auto& c : q.candidateOps)
                if (c.second->DxOp() == kCandidateProcNonOpaque &&
                    q.loop.body.count(c.first->label)) {
                    q.needsRecordConstants = true;
                    q.rayFlagsInLoop = true;
                    break;
                }
        }
        // Its intersection shader has no payload to read committed state
        // from (0.69.0).
        if (q.procEffects && q.committedInLoop)
            return "committed state is read inside a Proceed loop whose body also writes "
                   "for procedural candidates; the intersection shader that serves them "
                   "has no payload to read it from";
        // A procedural query's triangle any-hit sees only what a TRIANGLE
        // candidate reaches (0.69.0): see _reject_function in rayquery.py.
        if (q.NeedsBoth()) {
            const std::set<std::string> tri = ReachAs(fn, q, 0);
            for (const auto& label : tri) {
                const llm::Block* blk = fn.FindBlock(label);
                if (!blk) continue;
                for (const auto& i : blk->instrs) {
                    if (IsCommittedOp(i.DxOp()))
                        return "committed state is read for a triangle candidate in a "
                               "Proceed loop that commits procedural hits; those are made "
                               "in an intersection shader, which cannot record them in "
                               "the payload";
                    if (i.DxOp() == kAbort)
                        return "Abort() is reached by a triangle candidate in a Proceed "
                               "loop that commits procedural hits, which this lowering "
                               "cannot stop exactly yet";
                }
            }
        }
    }
    return "";
}

// Several queries in one entry point, 0.43.0. See _check_queries in
// rayquery.py, which this mirrors message for message.
std::string CheckQueries(const llm::Function& fn, const std::vector<Query>& qs) {
    std::string names;
    for (size_t k = 0; k < qs.size(); ++k) {
        if (k) names += ", ";
        names += qs[k].handle;
    }
    for (const auto& q : qs) {
        if (q.NeedsBoth())
            return std::to_string(qs.size()) + " RayQuery objects in " + fn.name + " (" +
                   names + "), and query " + q.handle + " becomes an intersection "
                   "shader, which cannot read the payload field that says which query "
                   "it serves";
        if (!q.hasLoop) continue;
        for (const auto& label : q.loop.body) {
            const llm::Block* blk = fn.FindBlock(label);
            if (!blk) continue;
            for (const auto& i : blk->instrs) {
                const auto uses = i.Uses();
                for (const auto& o : qs) {
                    if (o.handle == q.handle) continue;
                    if (std::find(uses.begin(), uses.end(), o.handle) != uses.end())
                        return "RayQuery " + o.handle + " is used inside the Proceed "
                               "loop of " + q.handle + "; that loop becomes an any-hit "
                               "shader, which cannot call TraceRay";
                }
            }
        }
    }
    return "";
}

// The pattern, or an error when a shape has no defined lowering.
std::string Classify(Query& q) {
    if (q.NeedsBoth()) {
        q.patternNum = 5;
        q.patternDesc = "both triangle and procedural commits (two hit groups)";
    } else if (q.NeedsIntersection()) {
        q.patternNum = 4;
        q.patternDesc = "procedural primitives (generated intersection shader)";
    } else if (q.NeedsAnyHit()) {
        q.patternNum = 3;
        q.patternDesc = "alpha-tested closest hit (generated any-hit shader)";
    } else if (q.RayFlags() & 0x004) {
        q.patternNum = 2;
        q.patternDesc = "shadow / visibility (miss shader only)";
    } else if (q.RayFlags() & 0x001) {
        q.patternNum = 1;
        q.patternDesc = "opaque closest hit (no any-hit shader)";
    } else {
        return "query has no Proceed loop but is not FORCE_OPAQUE (flags " +
               FlagNames(q.RayFlags()) + "); no lowering is defined for this";
    }
    return "";
}

}  // namespace

AnalyzeResult Analyze(const llm::Module& m) {
    AnalyzeResult r;
    const std::string modErr = RejectModule(m);
    if (!modErr.empty()) { r.error = modErr; return r; }

    std::vector<const llm::Function*> cands;
    for (const auto& f : m.functions)
        if (!f.FindOp(kAllocate).empty()) cands.push_back(&f);
    if (cands.empty()) { r.error = "no dx.op.allocateRayQuery in this module"; return r; }
    if (cands.size() > 1) {
        r.error = "RayQuery used in " + std::to_string(cands.size()) +
                  " functions; only one entry point is supported";
        return r;
    }
    const llm::Function& fn = *cands[0];

    const auto allocs = fn.FindOp(kAllocate);
    std::vector<std::string> handles;
    for (const auto* a : allocs) handles.push_back(a->result);

    std::vector<Query> qs;
    for (size_t k = 0; k < allocs.size(); ++k) {
        Query q;
        q.fn = &fn;
        q.alloc = allocs[k];
        q.handle = allocs[k]->result;
        q.constFlags = Imm(allocs[k]->args[1]);
        if (k) { q.pfx = "rq.q" + std::to_string(k); q.qid = (int)k; }
        std::vector<std::string> others;
        for (const auto& h : handles) if (h != q.handle) others.push_back(h);
        std::string err = Collect(fn, q, others);
        if (!err.empty()) { r.error = err; return r; }
        err = FindLoop(fn, q);
        if (!err.empty()) { r.error = err; return r; }
        MarkProceduralEffects(m, fn, q);
        qs.push_back(q);
    }
    // Before the per-query checks, so a query nested in another's loop is
    // refused for THAT, not for whatever it happens to read there.
    if (qs.size() > 1) {
        const std::string err = CheckQueries(fn, qs);
        if (!err.empty()) { r.error = err; return r; }
    }
    for (auto& q : qs) {
        std::string err = RejectFunction(fn, q);
        if (!err.empty()) { r.error = err; return r; }
        // Classify here rather than on demand, so a shape with no defined
        // lowering is refused by analysis itself.
        err = Classify(q);
        if (!err.empty()) { r.error = err; return r; }
    }
    // One table serves every trace, so record data is needed for all if any
    // query reads it.
    bool rc = false;
    for (const auto& q : qs) rc = rc || q.needsRecordConstants;
    for (auto& q : qs) q.needsRecordConstants = rc;

    r.ok = true;
    r.query = qs[0];
    r.query.others.assign(qs.begin() + 1, qs.end());
    return r;
}

}  // namespace rq

namespace rq {

bool NumThreads(const llm::Module& m, int out[3]) {
    // !dx.entryPoints -> the entry record -> its properties node -> tag 4.
    std::smatch mm;
    if (!std::regex_search(m.text, mm,
                           std::regex(R"(!dx\.entryPoints\s*=\s*!\{!(\d+)\})")))
        return false;

    auto nodeBody = [&](const std::string& id, std::string* body) {
        const std::regex re("!" + id + R"( = !\{(.*)\}\s*)");
        for (const auto& line : llm::SplitLines(m.text)) {
            std::smatch lm;
            if (std::regex_match(line, lm, re)) { *body = lm[1].str(); return true; }
        }
        return false;
    };

    std::string entry;
    if (!nodeBody(mm[1].str(), &entry)) return false;
    auto fields = llm::SplitArgs(entry);
    if (fields.size() < 5) return false;

    std::string props;
    if (fields[4].empty() || fields[4][0] != '!') return false;
    if (!nodeBody(fields[4].substr(1), &props)) return false;

    // Properties are tag/value pairs; tag 4 names the numthreads node.
    auto pf = llm::SplitArgs(props);
    for (size_t i = 0; i + 1 < pf.size(); i += 2) {
        if (pf[i] != "i32 4") continue;
        std::string nt;
        if (pf[i + 1].empty() || pf[i + 1][0] != '!') return false;
        if (!nodeBody(pf[i + 1].substr(1), &nt)) return false;
        auto v = llm::SplitArgs(nt);
        if (v.size() != 3) return false;
        for (int k = 0; k < 3; ++k) {
            std::smatch vm;
            if (!std::regex_match(v[k], vm, std::regex(R"(i32\s+(\d+))"))) return false;
            out[k] = std::stoi(vm[1].str());
        }
        return true;
    }
    return false;
}

}  // namespace rq
