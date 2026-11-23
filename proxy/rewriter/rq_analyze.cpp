#include "rq_analyze.h"

#include <algorithm>
#include <cstdlib>
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

int Query::DynFlags() const { return trace ? Imm(Args()[3]) : 0; }
int Query::RayFlags() const { return constFlags | DynFlags(); }

bool Query::StaticFlags() const {
    static const std::regex kImm(R"(^\s*i32\s+(-?\d+)\s*$)");
    return trace && std::regex_match(Args()[3], kImm);
}

std::pair<std::string, std::string> Query::FlagsOperand() const {
    if (StaticFlags())
        return { "", "i32 " + std::to_string(RayFlags()) };
    const std::string dyn = llm::OperandName(Args()[3]);
    return { "  %" + pfx + ".flags = or i32 " + dyn + ", " + std::to_string(constFlags),
             "i32 %" + pfx + ".flags" };
}

std::string Query::AsHandle() const {
    return trace ? llm::OperandName(Args()[2]) : "";
}

std::vector<std::string> Query::RayArgs() const {
    std::vector<std::string> out;
    if (!trace) return out;
    const auto& a = Args();
    for (size_t i = 4; i < a.size() && i <= 12; ++i)
        out.push_back(a[i]);
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
            // Traced more than once (0.73.0): another search.
            if (q.segmented && q.owner.at(&i) != q.search) continue;
            if (!IsKnownOpcode(op)) {
                return "unrecognised rayQuery opcode " + std::to_string(op) + " at \"" +
                       i.body.substr(0, 60) + "\". This project has only verified the "
                       "opcodes in the whitelist. Refusing rather than guessing.";
            }
            const std::string no = NoLoweringReason(op);
            if (!no.empty()) return no;

            if (op == kTraceInline) {
                if (q.trace && !q.segmented) return "more than one TraceRayInline on one query";
                if (!q.trace) { q.trace = &i; q.traceBlock = &b; }
                q.traces.emplace_back(&b, &i);
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

// The loop's exits other than the latch's (0.72.0): see _absorb_exits in
// rayquery.py, which this mirrors.
static std::string AbsorbExits(const llm::Function& fn, Query& q) {
    std::set<std::string>& body = q.loop.body;
    std::map<std::string, int> order;
    for (size_t k = 0; k < fn.blocks.size(); ++k) order[fn.blocks[k].label] = (int)k;
    auto byOrder = [&](const std::set<std::string>& in) {
        std::vector<std::string> v(in.begin(), in.end());
        std::sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) {
            return order[a] < order[b];
        });
        return v;
    };
    std::map<std::string, std::vector<std::string>> succ;
    for (const auto& b : fn.blocks) succ[b.label] = b.Successors();
    const auto preds = fn.Preds();
    std::set<std::string> need;
    for (const auto* list : { &q.commits, &q.procCommits })
        for (const auto& c : *list) need.insert(c.first->label);
    for (const auto& c : q.candidateOps)
        if (c.second->DxOp() != kRayFlags) need.insert(c.first->label);
    std::set<std::string> reach;
    std::vector<std::string> todo;
    for (const auto& l : body)
        if (l != q.loop.latch)
            for (const auto& s : succ[l])
                if (!body.count(s)) todo.push_back(s);
    while (!todo.empty()) {
        const std::string l = todo.back();
        todo.pop_back();
        if (reach.count(l) || body.count(l)) continue;
        reach.insert(l);
        for (const auto& s : succ[l]) todo.push_back(s);
    }
    std::set<std::string> want;
    for (const auto& l : need)
        if (!body.count(l) && reach.count(l)) want.insert(l);
    if (!want.empty()) {
        std::set<std::string> back;
        todo.assign(want.begin(), want.end());
        while (!todo.empty()) {
            const std::string l = todo.back();
            todo.pop_back();
            if (back.count(l) || body.count(l) || !reach.count(l)) continue;
            back.insert(l);
            auto it = preds.find(l);
            if (it != preds.end())
                for (const auto& p : it->second) todo.push_back(p);
        }
        for (const auto& l : byOrder(back)) {
            auto it = preds.find(l);
            if (it != preds.end())
                for (const auto& p : it->second)
                    if (p == q.loop.latch || (!body.count(p) && !back.count(p)))
                        return "the candidate is read or committed in " + l +
                               ", after the Proceed loop, which code that did not leave "
                               "the loop also reaches";
            for (const auto& c : q.committedOps)
                if (c.first->label == l)
                    return "committed state is read in " + l + ", on the way out of the "
                           "Proceed loop after the candidate is used there";
        }
        body.insert(back.begin(), back.end());
    }
    // An Abort() DXC sank into the way out joins too (0.75.0), where only the
    // way out reaches it: see _absorb_exits in rayquery.py.
    {
        std::set<std::string> abs;
        for (const auto& a : q.aborts)
            if (!body.count(a.first->label)) abs.insert(a.first->label);
        for (const auto& ab : byOrder(abs)) {
            if (!reach.count(ab)) continue;
            std::set<std::string> back;
            std::vector<std::string> todo2{ ab };
            while (!todo2.empty()) {
                const std::string l = todo2.back();
                todo2.pop_back();
                if (back.count(l) || body.count(l) || !reach.count(l)) continue;
                back.insert(l);
                auto it = preds.find(l);
                if (it != preds.end())
                    for (const auto& p : it->second) todo2.push_back(p);
            }
            bool ok = true;
            for (const auto& l : back) {
                auto it = preds.find(l);
                if (it == preds.end()) continue;
                for (const auto& p : it->second)
                    if (p == q.loop.latch || (!body.count(p) && !back.count(p))) ok = false;
            }
            if (ok) body.insert(back.begin(), back.end());
        }
    }
    q.exits.clear();
    for (const auto& l : byOrder(body)) {
        if (l == q.loop.latch) continue;
        for (const auto& s : succ[l])
            if (!body.count(s)) q.exits.emplace_back(l, s);
    }
    return "";
}

// Proceed() without a loop, once or more (0.74.0): see _find_chain in
// rayquery.py, which this mirrors message for message.
static std::string FindChain(const llm::Function& fn, Query& q) {
    std::map<std::string, int> order;
    for (size_t k = 0; k < fn.blocks.size(); ++k) order[fn.blocks[k].label] = (int)k;
    auto byOrder = [&](const std::set<std::string>& in) {
        std::vector<std::string> v(in.begin(), in.end());
        std::sort(v.begin(), v.end(), [&](const std::string& a, const std::string& b) {
            return order[a] < order[b];
        });
        return v;
    };
    const auto dom = fn.Dominators();
    const auto preds = fn.Preds();
    std::map<std::string, std::vector<std::string>> succ;
    for (const auto& b : fn.blocks) succ[b.label] = b.Successors();
    auto calls = q.proceeds;
    std::sort(calls.begin(), calls.end(), [](const auto& a, const auto& b) {
        return a.second->index < b.second->index;
    });
    auto before = [&](const std::pair<const llm::Block*, const llm::Instr*>& a,
                      const std::pair<const llm::Block*, const llm::Instr*>& b) {
        if (a.first->label == b.first->label) return a.second->index < b.second->index;
        auto d = dom.find(b.first->label);
        return d != dom.end() && d->second.count(a.first->label) != 0;
    };
    for (size_t k = 0; k + 1 < calls.size(); ++k)
        if (!before(calls[k], calls[k + 1]))
            return "Proceed() calls without a loop that are not one after another on every "
                   "path";
    static const std::regex kBr(R"(^br i1 (\S+), label (\S+), label (\S+)$)");
    std::vector<Query::ChainCall> chain;
    for (size_t k = 0; k < calls.size(); ++k) {
        Query::ChainCall c;
        c.block = calls[k].first;
        c.instr = calls[k].second;
        const llm::Instr* term = c.block->Terminator();
        std::smatch m;
        if (term && std::regex_match(term->body, m, kBr) && m[1].str() == c.instr->result &&
            m[2].str() != m[3].str()) {
            auto pit = preds.find(m[2].str());
            if (pit != preds.end() && pit->second == std::vector<std::string>{ c.block->label })
                c.entry = m[2].str();
        }
        if (!c.entry.empty()) {
            for (const auto& b : fn.blocks) {
                auto d = dom.find(b.label);
                if (d != dom.end() && d->second.count(c.entry)) c.region.insert(b.label);
            }
            if (k + 1 < calls.size() && c.region.count(calls[k + 1].first->label))
                return "a Proceed() inside the body of the Proceed() before it, without a loop";
        }
        std::set<std::string> targets;
        for (const auto& l : byOrder(c.region)) {
            for (const auto& s2 : succ[l])
                if (!c.region.count(s2)) { c.exits.emplace_back(l, s2); targets.insert(s2); }
            const llm::Instr* t = fn.FindBlock(l)->Terminator();
            if (t) {
                std::string tb = t->body;
                while (!tb.empty() && (tb.back() == ' ' || tb.back() == '\t')) tb.pop_back();
                if (tb == "ret void") c.rets.push_back(l);
            }
        }
        if (targets.size() > 1 || (!targets.empty() && !c.rets.empty()))
            return "the body of a Proceed() without a loop has more than one way out";
        if (!targets.empty()) c.exit = *targets.begin();
        chain.push_back(c);
    }
    for (size_t k = 0; k + 1 < chain.size(); ++k) {
        const auto& c = chain[k];
        const std::string nb = chain[k + 1].block->label;
        std::vector<std::string> todo;
        if (c.entry.empty()) {
            if (c.block->label == nb) continue;
            todo = succ[c.block->label];
        } else if (c.exit.empty()) {
            continue;
        } else {
            todo.push_back(c.exit);
        }
        std::set<std::string> seen;
        while (!todo.empty()) {
            const std::string l = todo.back();
            todo.pop_back();
            if (l == nb || seen.count(l)) continue;
            seen.insert(l);
            const llm::Instr* t = fn.FindBlock(l)->Terminator();
            if (t) {
                std::string tb = t->body;
                while (!tb.empty() && (tb.back() == ' ' || tb.back() == '\t')) tb.pop_back();
                if (tb == "ret void")
                    return "a Proceed() without a loop that is not always followed by the next one";
            }
            for (const auto& s2 : succ[l]) todo.push_back(s2);
        }
    }
    std::set<std::string> body;
    for (size_t k = 0; k < chain.size(); ++k)
        for (const auto& l : chain[k].region) { body.insert(l); q.chainRegion[l] = (int)k; }
    for (const auto* list : { &q.commits, &q.procCommits, &q.candidateOps })
        for (const auto& c : *list) {
            if (c.second->DxOp() == kRayFlags) continue;
            if (!body.count(c.first->label))
                return "the candidate is used at \"" + c.second->body.substr(0, 60) +
                       "\", where no Proceed() result guards it";
        }
    q.chain = chain;
    q.hasLoop = true;
    q.loop.header = "%rq.pd" + std::to_string(q.qid);
    q.loop.latch = "%rq.pn" + std::to_string(q.qid);
    q.loop.body = body;
    return "";
}

// Code between two Proceed() calls runs once in the raygen, after the
// search (0.74.0): see _check_chain_between in rayquery.py.
static std::string CheckChainBetween(const llm::Module& m, const llm::Function& fn,
                                     const Query& q) {
    if (q.chain.size() < 2) return "";
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
    auto touches = [&](const llm::Instr& i, bool write) {
        if (i.callee.compare(0, 6, "dx.op.") != 0) return false;
        if (i.callee.compare(0, 15, "dx.op.rayQuery_") == 0 || i.callee == "dx.op.allocateRayQuery")
            return false;
        auto g = groups.find(i.callee);
        const std::string gt = g == groups.end() ? "" : g->second;
        if (write)
            return gt.find("readnone") == std::string::npos && gt.find("readonly") == std::string::npos;
        return gt.find("readnone") == std::string::npos;
    };
    bool later = false;
    for (size_t k = 1; k < q.chain.size(); ++k)
        for (const auto& l : q.chain[k].region)
            for (const auto& i : fn.FindBlock(l)->instrs)
                if (touches(i, false)) later = true;
    if (!later) return "";
    const auto dom = fn.Dominators();
    const auto& first = q.chain[0];
    const std::string lastb = q.chain.back().block->label;
    std::map<std::string, std::vector<std::string>> succ;
    for (const auto& b : fn.blocks) succ[b.label] = b.Successors();
    auto reaches = [&](const std::string& a, const std::string& target) {
        std::set<std::string> seen;
        std::vector<std::string> todo{ a };
        while (!todo.empty()) {
            const std::string l = todo.back();
            todo.pop_back();
            if (l == target) return true;
            if (seen.count(l)) continue;
            seen.insert(l);
            for (const auto& s2 : succ[l]) todo.push_back(s2);
        }
        return false;
    };
    for (const auto& b : fn.blocks) {
        auto d = dom.find(b.label);
        if (q.loop.body.count(b.label) || d == dom.end() || !d->second.count(first.block->label))
            continue;
        if (!reaches(b.label, lastb)) continue;
        for (const auto& i : b.instrs) {
            if (b.label == first.block->label && i.index <= first.instr->index) continue;
            if (b.label == lastb && i.index >= q.chain.back().instr->index) continue;
            if (touches(i, true))
                return "code between two Proceed() calls writes memory (" + i.callee +
                       ") that a later call's body may read; the body runs during the search, "
                       "before it";
        }
    }
    return "";
}

// Abort() outside the Proceed loop (0.75.0): see _place_aborts in
// rayquery.py, which this mirrors.
static std::string PlaceAborts(const llm::Function& fn, Query& q) {
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> outer, inner;
    for (const auto& a : q.aborts)
        (q.hasLoop && q.loop.body.count(a.first->label) ? inner : outer).push_back(a);
    q.aborts = inner;
    if (outer.empty()) return "";
    const llm::Block* sb = nullptr;
    const llm::Instr* si = nullptr;
    if (q.proceeds.empty()) {
    } else if (!q.chain.empty()) {
        sb = q.chain[0].block; si = q.chain[0].instr;
    } else if (q.hasLoop && q.top) {
        sb = q.traceBlock; si = q.traceBlock->Terminator();
    } else if (q.hasLoop) {
        for (const auto& p : q.proceeds)
            if (p.first->label != q.loop.latch) { sb = p.first; si = p.second; break; }
    } else {
        sb = q.proceeds[0].first; si = q.proceeds[0].second;
    }
    std::vector<const llm::Instr*> traces;
    if (q.traces.empty()) traces.push_back(q.trace);
    else for (const auto& t : q.traces) traces.push_back(t.second);
    std::map<std::string, std::vector<std::string>> succ;
    for (const auto& b : fn.blocks) succ[b.label] = b.Successors();
    auto inBlock = [](const llm::Block* b, const llm::Instr* t) {
        for (const auto& i : b->instrs) if (&i == t) return true;
        return false;
    };
    auto reaches = [&](const llm::Block* ab, const llm::Instr* ai,
                       const llm::Block* rb, const llm::Instr* ri) {
        if (ab->label == rb->label && ai->index < ri->index) {
            for (const auto* t : traces)
                if (inBlock(ab, t) && ai->index < t->index && t->index < ri->index) return false;
            return true;
        }
        for (const auto* t : traces)
            if (inBlock(ab, t) && t->index > ai->index) return false;
        std::set<std::string> seen;
        std::vector<std::string> todo = succ[ab->label];
        while (!todo.empty()) {
            const std::string l = todo.back();
            todo.pop_back();
            if (seen.count(l)) continue;
            seen.insert(l);
            const llm::Block* blk = fn.FindBlock(l);
            std::vector<const llm::Instr*> ts;
            for (const auto* t : traces) if (inBlock(blk, t)) ts.push_back(t);
            if (l == rb->label) {
                bool killed = false;
                for (const auto* t : ts) if (t->index < ri->index) killed = true;
                if (!killed) return true;
                continue;
            }
            if (!ts.empty()) continue;
            for (const auto& s2 : succ[l]) todo.push_back(s2);
        }
        return false;
    };
    for (const auto& a : outer) {
        if (sb && reaches(a.first, a.second, sb, si)) {
            q.preAborts.push_back(a);
            continue;
        }
        bool between = false;
        for (size_t k = 1; k < q.chain.size(); ++k)
            if (reaches(a.first, a.second, q.chain[k].block, q.chain[k].instr)) between = true;
        if (between)
            return "Abort() between two Proceed() calls without a loop; whether the next call "
                   "yields depends on code that runs after the search in the lowering";
        q.postAborts.push_back(a);
    }
    return "";
}

static std::string FindLoopShape(const llm::Function& fn, Query& q);

std::string FindLoop(const llm::Function& fn, Query& q) {
    const std::string err = FindLoopShape(fn, q);
    if (!err.empty()) return err;
    return PlaceAborts(fn, q);
}

static std::string FindLoopShape(const llm::Function& fn, Query& q) {
    std::set<std::string> proceedBlocks;
    for (const auto& p : q.proceeds) proceedBlocks.insert(p.first->label);

    std::vector<llm::Loop> found;
    for (const auto& lp : fn.NaturalLoops())
        if (proceedBlocks.count(lp.latch)) found.push_back(lp);

    if (found.size() > 1) return "more than one Proceed loop on one query";
    if (found.empty()) {
        // Top-tested (0.72.0): see _find_loop in rayquery.py.
        static const std::regex kBr(R"(^br i1 (\S+), label (\S+), label (\S+)$)");
        const auto preds = fn.Preds();
        for (const auto& lp : fn.NaturalLoops()) {
            if (!proceedBlocks.count(lp.header)) continue;
            const llm::Block* hdr = fn.FindBlock(lp.header);
            const llm::Instr* pr = nullptr;
            for (const auto& p : q.proceeds)
                if (p.first->label == lp.header) { pr = p.second; break; }
            const llm::Instr* term = hdr->Terminator();
            std::smatch m;
            if (!term || !std::regex_match(term->body, m, kBr) || m[1].str() != pr->result ||
                !lp.body.count(m[2].str()) || lp.body.count(m[3].str()))
                return "a Proceed loop tested at its top has an unexpected header shape";
            for (const auto& i : hdr->instrs)
                if (!(i.isPhi || &i == pr || &i == term))
                    return "the Proceed loop header computes " +
                           (i.result.empty() ? i.body.substr(0, 40) : i.result) +
                           " itself; the any-hit shader has no header to compute it in";
            const std::string first = m[2].str();
            auto pit = preds.find(first);
            if (pit == preds.end() || pit->second != std::vector<std::string>{ lp.header })
                return "a Proceed loop tested at its top has an unexpected body entry";
            bool merged = false;
            for (auto& f : found)
                if (f.latch == lp.header) {
                    f.body.insert(lp.body.begin(), lp.body.end());
                    merged = true;
                }
            if (merged) continue;
            llm::Loop t;
            t.header = first;
            t.latch = lp.header;
            t.body = lp.body;
            found.push_back(t);
            q.top = true;
        }
        if (found.size() > 1) return "more than one Proceed loop on one query";
    }
    if (found.size() == 1) {
        q.hasLoop = true;
        q.loop = found[0];
        return AbsorbExits(fn, q);
    }
    // No loop. One bare Proceed is the FORCE_OPAQUE shape.
    if (q.proceeds.size() == 1 && (q.RayFlags() & 1)) return "";
    if (!q.proceeds.empty()) return FindChain(fn, q);
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
    std::vector<std::string> todo;
    if (!q.chain.empty()) {
        for (const auto& c : q.chain)
            if (!c.entry.empty()) todo.push_back(c.entry);
    } else {
        todo.push_back(q.loop.header);
    }
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

    if (q.hasLoop) {
        for (const auto& label : q.loop.body) {
            const llm::Block* blk = fn.FindBlock(label);
            if (!blk) continue;
            for (const auto& i : blk->instrs)
                if (IsCommittedOp(i.DxOp())) q.committedInLoop = true;
        }
        // An intersection shader has RayTCurrent(), which IS the committed t,
        // and no payload for the rest; only what a procedural candidate
        // reaches matters (0.71.0, the whole body before).
        if (!q.procCommits.empty()) {
            for (const auto& label : ReachAs(fn, q, 1)) {
                const llm::Block* blk = fn.FindBlock(label);
                if (!blk) continue;
                for (const auto& i : blk->instrs)
                    if (IsCommittedOp(i.DxOp()) && i.DxOp() != kCommittedRayT)
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
        // A break or return ends the search the same way (0.72.0).
        if ((!q.aborts.empty() || !q.exits.empty() || !q.chain.empty()) && !q.committedOps.empty() &&
            q.procCommits.empty() &&
            !(q.RayFlags() & 1)) {
            q.track = true;
            q.needsRecordConstants = true;
        }
        // Tracked procedural (0.71.0): see _reject_function in rayquery.py.
        if (q.NeedsBoth()) {
            bool triCmt = false;
            for (const auto& label : ReachAs(fn, q, 0)) {
                const llm::Block* blk = fn.FindBlock(label);
                if (!blk) continue;
                for (const auto& i : blk->instrs)
                    if (IsCommittedOp(i.DxOp())) triCmt = true;
            }
            if (!q.chain.empty())
                return "Proceed() without a loop in a query that commits procedural hits or "
                       "writes where they reach";
            // A break or return ends the search the same way (0.72.0).
            if (!q.aborts.empty() || !q.exits.empty() || triCmt) {
                if (!q.StaticFlags())
                    return std::string(!q.exits.empty() && q.aborts.empty()
                                           ? "A break or return" : "Abort()") +
                           " or committed state read for a triangle candidate in a "
                           "Proceed loop that commits procedural hits, with ray flags "
                           "computed at run time; the exact lowering needs the query's own "
                           "opacity and cull flags as constants in its intersection shader";
                if (q.exits.size() > 100)
                    return std::to_string(q.exits.size()) + " ways out of a Proceed loop that "
                           "becomes an intersection shader; it says which in a hit kind, and "
                           "100 are free";
                q.trackProc = true;
                q.track = true;
                q.needsRecordConstants = true;
            }
        }
        // CandidateProceduralPrimitiveNonOpaque() in the intersection shader
        // (0.70.0): see _reject_function in rayquery.py.
        if (!q.procCommits.empty()) {
            for (const auto& c : q.candidateOps)
                if (c.second->DxOp() == kCandidateProcNonOpaque &&
                    q.loop.body.count(c.first->label)) {
                    q.needsRecordConstants = true;
                    if (!q.trackProc) q.rayFlagsInLoop = true;
                    break;
                }
        }
        // Its intersection shader has no payload to read committed state
        // from (0.69.0).
        if (q.procEffects && q.committedInLoop)
            return "committed state is read inside a Proceed loop whose body also writes "
                   "for procedural candidates; the intersection shader that serves them "
                   "has no payload to read it from";
    }
    return "";
}

// Several queries in one entry point, 0.43.0. See _check_queries in
// rayquery.py, which this mirrors message for message.
// Which search each op of a query traced more than once reads (0.73.0):
// reaching definitions, a trace killing the rest; traces that reach one op
// together are one search. See _trace_owner in rayquery.py.
static std::string TraceOwners(const llm::Function& fn, const std::string& handle,
                               const std::vector<const llm::Instr*>& traces,
                               std::map<const llm::Instr*, std::set<int>>& owner,
                               std::map<std::string, std::set<int>>& outSets,
                               std::vector<std::set<int>>& searches) {
    std::map<const llm::Instr*, int> idx;
    for (size_t k = 0; k < traces.size(); ++k) idx[traces[k]] = (int)k;
    const auto preds = fn.Preds();
    std::map<std::string, int> last;
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (idx.count(&i)) last[b.label] = idx[&i];
    std::map<std::string, std::set<int>> inn;
    for (const auto& b : fn.blocks) {
        inn[b.label];
        if (last.count(b.label)) outSets[b.label] = { last[b.label] };
        else outSets[b.label];
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& b : fn.blocks) {
            std::set<int> r;
            auto pit = preds.find(b.label);
            if (pit != preds.end())
                for (const auto& p : pit->second) {
                    const auto& o = outSets[p];
                    r.insert(o.begin(), o.end());
                }
            if (r != inn[b.label]) { inn[b.label] = r; changed = true; }
            std::set<int> o = last.count(b.label) ? std::set<int>{ last[b.label] } : r;
            if (o != outSets[b.label]) { outSets[b.label] = o; changed = true; }
        }
    }
    std::vector<int> parent(traces.size());
    for (size_t k = 0; k < parent.size(); ++k) parent[k] = (int)k;
    auto find = [&](int k) { while (parent[k] != k) k = parent[k]; return k; };
    std::map<const llm::Instr*, std::set<int>> reach;
    for (const auto& b : fn.blocks) {
        std::set<int> cur = inn[b.label];
        for (const auto& i : b.instrs) {
            if (idx.count(&i)) { cur = { idx[&i] }; reach[&i] = cur; continue; }
            const int op = i.DxOp();
            if (op < 0) continue;
            const auto uses = i.Uses();
            if (std::find(uses.begin(), uses.end(), handle) == uses.end()) continue;
            if (cur.empty())
                return (IsKnownOpcode(op) ? std::string(OpcodeName(op)) : std::to_string(op)) +
                       " at \"" + i.body.substr(0, 60) + "\" can be reached with no "
                       "TraceRayInline of its query before it";
            reach[&i] = cur;
            const int k0 = *cur.begin();
            for (int k : cur) if (k != k0) parent[find(k)] = find(k0);
        }
    }
    std::map<int, std::set<int>> groups;
    for (size_t k = 0; k < traces.size(); ++k) groups[find((int)k)].insert((int)k);
    std::map<int, std::set<int>> search;
    for (const auto& g : groups) for (int k : g.second) search[k] = g.second;
    for (const auto& g : groups) searches.push_back(g.second);
    std::sort(searches.begin(), searches.end(),
              [](const std::set<int>& x, const std::set<int>& y) { return *x.begin() < *y.begin(); });
    for (const auto& kv : reach) owner[kv.first] = search[*kv.second.begin()];
    return "";
}

static std::string Trim(const std::string& v) {
    const size_t a = v.find_first_not_of(" \t"), b = v.find_last_not_of(" \t");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
}

// A search a branch chooses the ray of (0.73.0): see _merge_traces in
// rayquery.py, which this mirrors message for message.
static std::string MergeTraces(const llm::Function& fn, Query& q,
                               const std::vector<const llm::Instr*>& all,
                               const std::map<std::string, std::set<int>>& outSets) {
    if (!q.hasLoop || q.top)
        return "a query traced on either arm of a branch, without a rotated Proceed "
               "loop to start the search at";
    for (const auto& t : q.traces)
        if (q.loop.body.count(t.first->label))
            return "RayQuery " + q.handle + " is traced again inside its own Proceed loop; "
                   "that loop becomes an any-hit shader, which cannot call TraceRay";
    std::vector<const llm::Block*> guard;
    for (const auto& p : q.proceeds)
        if (p.first->label != q.loop.latch) guard.push_back(p.first);
    if (guard.size() != 1)
        return "a query traced on either arm of a branch, with no guard Proceed to start "
               "the search at";
    const llm::Block* p = guard[0];
    std::map<int, const llm::Instr*> byK;
    for (const auto& t : q.traces)
        for (size_t k = 0; k < all.size(); ++k)
            if (all[k] == t.second) byK[(int)k] = t.second;
    for (const auto& t : q.traces)
        if (t.first->label == p->label)
            return "a query traced on either arm of a branch, and again in the block its "
                   "search starts at";
    const auto dom = fn.Dominators();
    for (const auto* list : { &q.proceeds, &q.commits, &q.aborts, &q.procCommits,
                              &q.candidateOps, &q.committedOps })
        for (const auto& c : *list) {
            auto d = dom.find(c.first->label);
            if (d == dom.end() || !d->second.count(p->label))
                return "a query traced on either arm of a branch is used at \"" +
                       c.second->body.substr(0, 60) + "\" before the arms meet";
        }
    const auto preds = fn.Preds();
    std::vector<std::pair<std::string, const llm::Instr*>> src;
    auto pit = preds.find(p->label);
    if (pit != preds.end())
        for (const auto& pr : pit->second) {
            const auto& o = outSets.at(pr);
            std::vector<int> r;
            for (int k : o) if (byK.count(k)) r.push_back(k);
            if (r.size() != 1 || o.size() != 1)
                return "a query traced on either arm of a branch, with " + pr +
                       " reaching the search from more than one of them";
            src.emplace_back(pr, byK[r[0]]);
        }
    const llm::Instr* first = q.traces[0].second;
    std::vector<std::string> merged = first->args;
    for (size_t j = 2; j < first->args.size(); ++j) {
        std::vector<std::string> vals;
        for (const auto& sp : src) vals.push_back(Trim(sp.second->args[j]));
        bool same = true;
        for (const auto& v : vals) if (v != vals[0]) same = false;
        // DXR_TIER11_MERGE_POISON=1 (0.73.0): the first arm's ray on every path.
        // The branch case must diverge (C++ only).
        static const bool mergePoison = std::getenv("DXR_TIER11_MERGE_POISON") != nullptr;
        if (same || mergePoison) { merged[j] = vals[0]; continue; }
        const std::string ty = vals[0].substr(0, vals[0].find(' '));
        if (ty == "%dx.types.Handle")
            return "a query traced on either arm of a branch against a different "
                   "acceleration structure on each";
        const std::string name = "%" + q.pfx + ".ta" + std::to_string(j);
        std::string phi = "  " + name + " = phi " + ty + " ";
        for (size_t k = 0; k < src.size(); ++k) {
            if (k) phi += ", ";
            std::string v = Trim(src[k].second->args[j]);
            v = Trim(v.substr(v.find(' ') + 1));
            phi += "[ " + v + ", " + src[k].first + " ]";
        }
        q.tracePhis.push_back(phi);
        merged[j] = ty + " " + name;
    }
    q.mergedArgs = merged;
    q.routeBlock = p;
    return "";
}

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
        // The same query traced again inside its own loop (0.73.0).
        for (const auto& o : qs) {
            if (&o == &q || o.handle != q.handle) continue;
            std::vector<const llm::Block*> blocks{ o.traceBlock };
            for (const auto* list : { &o.proceeds, &o.commits, &o.aborts, &o.procCommits,
                                      &o.candidateOps, &o.committedOps })
                for (const auto& c : *list) blocks.push_back(c.first);
            for (const auto* b : blocks)
                if (q.loop.body.count(b->label))
                    return "RayQuery " + q.handle + " is traced again inside its own Proceed "
                           "loop; that loop becomes an any-hit shader, which cannot call "
                           "TraceRay";
        }
    }
    return "";
}

// The pattern, or an error when a shape has no defined lowering.
std::string Classify(Query& q) {
    if (q.proceeds.empty()) {
        // Never asked to Proceed(), so never searched (0.74.0).
        q.patternNum = 1;
        q.patternDesc = "never searched (no Proceed() call)";
    } else if (q.NeedsBoth()) {
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
    for (size_t a = 0; a < allocs.size(); ++a) {
        // A query traced more than once (0.73.0): see analyze in rayquery.py.
        std::vector<const llm::Instr*> traces;
        for (const auto* t : fn.FindOp(kTraceInline)) {
            const auto uses = t->Uses();
            if (std::find(uses.begin(), uses.end(), allocs[a]->result) != uses.end())
                traces.push_back(t);
        }
        std::map<const llm::Instr*, std::set<int>> owner;
        std::map<std::string, std::set<int>> outSets;
        std::vector<std::set<int>> searches;
        const bool segmented = traces.size() > 1;
        if (segmented) {
            const std::string err =
                TraceOwners(fn, allocs[a]->result, traces, owner, outSets, searches);
            if (!err.empty()) { r.error = err; return r; }
        } else {
            searches.emplace_back();
        }
        for (const auto& t : searches) {
            Query q;
            q.fn = &fn;
            q.alloc = allocs[a];
            q.handle = allocs[a]->result;
            q.constFlags = Imm(allocs[a]->args[1]);
            const size_t k = qs.size();
            if (k) { q.pfx = "rq.q" + std::to_string(k); q.qid = (int)k; }
            if (segmented) { q.segmented = true; q.search = t; q.owner = owner; }
            std::vector<std::string> others;
            for (const auto& h : handles) if (h != q.handle) others.push_back(h);
            std::string err = Collect(fn, q, others);
            if (!err.empty()) { r.error = err; return r; }
            err = FindLoop(fn, q);
            if (!err.empty()) { r.error = err; return r; }
            if (!q.chain.empty()) {
                err = CheckChainBetween(m, fn, q);
                if (!err.empty()) { r.error = err; return r; }
            }
            if (q.traces.size() > 1) {
                err = MergeTraces(fn, q, traces, outSets);
                if (!err.empty()) { r.error = err; return r; }
            }
            MarkProceduralEffects(m, fn, q);
            qs.push_back(q);
        }
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
