// A query used inside another query's Proceed loop (0.79.0, plan step 3 l).
// nest.py in phase5/rewriter says what this does and why; this is its port,
// held byte-identical by the rewriter suite.
#include "nest_step.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ll_model.h"
#include "rq_analyze.h"

namespace rq {
namespace {

struct Scalar { const char* ty; int cand; int comm; };

const std::map<std::string, Scalar>& Scalars() {
    static const std::map<std::string, Scalar> m = {
        { "t", { "float", kCandidateRayT, kCommittedRayT } },
        { "inst", { "i32", kCandidateInstanceIndex, kCommittedInstanceIndex } },
        { "id", { "i32", kCandidateInstanceID, kCommittedInstanceID } },
        { "geom", { "i32", kCandidateGeometryIndex, kCommittedGeometryIndex } },
        { "prim", { "i32", kCandidatePrimitiveIndex, kCommittedPrimitiveIndex } },
        { "contrib", { "i32", kCandidateInstanceContrib, kCommittedInstanceContrib } },
        { "ff", { "i1", kCandidateFrontFace, kCommittedFrontFace } },
    };
    return m;
}

const char* const kKey[] = { "t", "inst", "geom", "prim" };

struct Unsupported { std::string why; };

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::string LastWord(const std::string& s) {
    const std::string t = Trim(s);
    size_t sp = t.find_last_of(" \t");
    return sp == std::string::npos ? t : t.substr(sp + 1);
}

std::string Handle(const llm::Instr& i) {
    return i.args.size() > 1 ? LastWord(i.args[1]) : std::string();
}

std::string Digits(const std::string& arg, const char* ty) {
    std::smatch m;
    const std::regex re(std::string(ty) + R"(\s+(\d+))");
    const std::string a = Trim(arg);
    return std::regex_search(a, m, re) ? m[1].str() : std::string();
}

// (field, is_committed), or field empty.
std::pair<std::string, bool> FieldOf(const llm::Instr& i) {
    const int op = i.DxOp();
    for (const auto& kv : Scalars()) {
        if (op == kv.second.cand) return { kv.first, false };
        if (op == kv.second.comm) return { kv.first, true };
    }
    if (op == kCandidateBary || op == kCommittedBary)
        return { "b" + Digits(i.args[2], "i8"), op == kCommittedBary };
    if (op == kCandidateWorldToObject || op == kCommittedWorldToObject)
        return { "w" + Digits(i.args[2], "i32") + Digits(i.args[3], "i8"),
                 op == kCommittedWorldToObject };
    if (op == kCandidateObjectRayOrigin || op == kCandidateObjectRayDirection)
        return { std::string(op == kCandidateObjectRayOrigin ? "o" : "d") +
                     Digits(i.args[2], "i8"), false };
    return { "", false };
}

std::string FType(const std::string& f) {
    auto it = Scalars().find(f);
    return it != Scalars().end() ? it->second.ty : "float";
}

std::string Read(const std::string& f, bool committed, const std::string& h,
                 const std::string& res) {
    auto it = Scalars().find(f);
    if (it != Scalars().end()) {
        const std::string ty = it->second.ty;
        const int op = committed ? it->second.comm : it->second.cand;
        return "  " + res + " = call " + ty + " @dx.op.rayQuery_StateScalar." + ty + "(i32 " +
               std::to_string(op) + ", i32 " + h + ")";
    }
    if (f[0] == 'b') {
        const int op = committed ? kCommittedBary : kCandidateBary;
        return "  " + res + " = call float @dx.op.rayQuery_StateVector.f32(i32 " +
               std::to_string(op) + ", i32 " + h + ", i8 " + f.substr(1, 1) + ")";
    }
    if (f[0] == 'w') {
        const int op = committed ? kCommittedWorldToObject : kCandidateWorldToObject;
        return "  " + res + " = call float @dx.op.rayQuery_StateMatrix.f32(i32 " +
               std::to_string(op) + ", i32 " + h + ", i32 " + f.substr(1, 1) + ", i8 " +
               f.substr(2, 1) + ")";
    }
    const int op = f[0] == 'o' ? kCandidateObjectRayOrigin : kCandidateObjectRayDirection;
    return "  " + res + " = call float @dx.op.rayQuery_StateVector.f32(i32 " +
           std::to_string(op) + ", i32 " + h + ", i8 " + f.substr(1, 1) + ")";
}

std::string Decl(const std::pair<std::string, std::string>& key) {
    if (key.first == "StateScalar" && key.second == "i32")
        return "declare i32 @dx.op.rayQuery_StateScalar.i32(i32, i32)";
    if (key.first == "StateScalar" && key.second == "float")
        return "declare float @dx.op.rayQuery_StateScalar.f32(i32, i32)";
    if (key.first == "StateScalar" && key.second == "i1")
        return "declare i1 @dx.op.rayQuery_StateScalar.i1(i32, i32)";
    if (key.first == "StateVector")
        return "declare float @dx.op.rayQuery_StateVector.f32(i32, i32, i8)";
    return "declare float @dx.op.rayQuery_StateMatrix.f32(i32, i32, i32, i8)";
}

bool IsCoreOp(int op) {
    return op == kTraceInline || op == kProceed || op == kAbort || op == kCommitNonOpaque ||
           op == kCommitProcedural;
}

std::vector<std::pair<const llm::Block*, const llm::Instr*>> Owned(const llm::Function& fn,
                                                                  const std::string& h) {
    std::vector<std::pair<const llm::Block*, const llm::Instr*>> out;
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs) {
            const int op = i.DxOp();
            if (op == kAllocate && i.result == h) out.push_back({ &b, &i });
            else if ((IsKnownOpcode(op) || IsCoreOp(op)) && op != kAllocate && Handle(i) == h)
                out.push_back({ &b, &i });
        }
    return out;
}

void CheckBoxes(const llm::Function& fn, const Query& q) {
    if (!q.hasLoop) return;
    const std::string& header = q.loop.header;
    const std::string& latch = q.loop.latch;
    const auto& body = q.loop.body;
    const std::set<std::string> reach = ReachAsKind(fn, q, 1);
    std::set<std::string> types;
    for (const auto& label : body) {
        const llm::Block* b = fn.FindBlock(label);
        if (!b) continue;
        for (const auto& i : b->instrs)
            if (i.DxOp() == kCandidateType && Handle(i) == q.handle) types.insert(i.result);
    }
    auto why = [&](const std::string& what) {
        return Unsupported{ "a procedural candidate reaches " + what + " in the Proceed loop of " +
                            q.handle + ", a query with another query inside its loop; that "
                            "query is searched for triangles only (recording a box needs loop "
                            "state in an intersection shader)" };
    };
    static const std::regex kIcmp(R"(^icmp (?:eq|ne) i32 (\S+), (\S+)$)");
    for (const auto& label : reach) {
        const llm::Block* blk = fn.FindBlock(label);
        if (!blk) continue;
        const llm::Instr* term = blk->Terminator();
        for (const auto& i : blk->instrs) {
            if (i.isPhi || &i == term) continue;
            const int op = i.DxOp();
            if ((op == kCandidateType || op == kProceed) && Handle(i) == q.handle) continue;
            std::smatch m;
            if (std::regex_match(i.body, m, kIcmp) &&
                (types.count(m[1].str()) || types.count(m[2].str())))
                continue;
            throw why(!i.result.empty() ? i.result : i.body.substr(0, 40));
        }
        bool decides = false;
        for (const auto& i : blk->instrs)
            if (i.DxOp() == kProceed && Handle(i) == q.handle) decides = true;
        if (decides) continue;
        for (const auto& s : blk->Successors())
            if (!body.count(s) && s != header) throw why(s);
    }
    const llm::Block* state = fn.FindBlock(q.top ? latch : header);
    std::map<std::string, std::pair<const llm::Block*, const llm::Instr*>> defs;
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (!i.result.empty()) defs[i.result] = { &b, &i };
    for (const auto& h : state->instrs) {
        if (!h.isPhi) continue;
        for (const auto& in : h.phiIncoming) {
            if (!reach.count(in.second)) continue;
            std::string v = in.first;
            std::set<std::string> seen;
            while (v != h.result && defs.count(v) && !seen.count(v)) {
                seen.insert(v);
                const llm::Block* b = defs[v].first;
                const llm::Instr* d = defs[v].second;
                if (!d->isPhi || !reach.count(b->label) || b->label == state->label) break;
                std::set<std::string> vals;
                for (const auto& x : d->phiIncoming)
                    if (reach.count(x.second)) vals.insert(x.first);
                if (vals.size() != 1) break;
                v = *vals.begin();
            }
            if (v != h.result) throw why(h.result);
        }
    }
}

struct Vars {
    std::map<std::string, std::pair<std::string, std::string>> var;   // name -> (alloca, type)
    std::string Store(const std::string& name, const std::string& val) const {
        const auto& a = var.at(name);
        return "  store " + a.second + " " + val + ", " + a.second + "* " + a.first + ", align 4";
    }
    std::string Load(const std::string& name, const std::string& res) const {
        const auto& a = var.at(name);
        return "  " + res + " = load " + a.second + ", " + a.second + "* " + a.first + ", align 4";
    }
};

std::string Join(const std::vector<std::string>& v) {
    std::string out;
    for (size_t k = 0; k < v.size(); ++k) {
        if (k) out += "\n";
        out += v[k];
    }
    return out;
}

std::string Declare(const llm::Module& mod, const std::vector<std::string>& cand,
                    const std::vector<std::string>& comm) {
    std::string text = mod.text;
    std::set<std::pair<std::string, std::string>> need = {
        { "StateScalar", "i32" }, { "StateScalar", "float" } };
    std::set<std::string> all(cand.begin(), cand.end());
    all.insert(comm.begin(), comm.end());
    for (const auto& f : all) {
        if (Scalars().count(f)) need.insert({ "StateScalar", FType(f) });
        else if (f[0] == 'w') need.insert({ "StateMatrix", "float" });
        else need.insert({ "StateVector", "float" });
    }
    std::string grp;
    static const std::regex kState(R"(^declare \S+ @dx\.op\.rayQuery_State\w+\.\w+\([^)]*\) (#\d+))");
    for (const auto& line : mod.lines) {
        std::smatch m;
        if (std::regex_search(line, m, kState)) { grp = m[1].str(); break; }
    }
    if (grp.empty()) {
        int maxn = -1;
        static const std::regex kAttr(R"(^attributes #(\d+) = )");
        static const std::regex kRo(R"(^attributes (#\d+) = \{ nounwind readonly \}$)");
        for (const auto& line : llm::SplitLines(text)) {
            std::smatch m;
            if (std::regex_search(line, m, kAttr)) maxn = std::max(maxn, std::stoi(m[1].str()));
        }
        for (const auto& line : mod.lines) {
            std::smatch m;
            if (std::regex_match(line, m, kRo)) grp = m[1].str();
        }
        if (grp.empty()) {
            grp = "#" + std::to_string(maxn + 1);
            while (!text.empty() && text.back() == '\n') text.pop_back();
            text += "\n\nattributes " + grp + " = { nounwind readonly }\n";
        }
    }
    std::vector<std::string> add;
    const auto lines = llm::SplitLines(text);
    for (const auto& key : need) {
        const std::string d = Decl(key);
        const size_t at = d.find('@');
        const std::string name = d.substr(at + 1, d.find('(') - at - 1);
        bool have = false;
        for (const auto& line : lines)
            if (line.compare(0, 8, "declare ") == 0 &&
                line.find("@" + name + "(") != std::string::npos &&
                line.find('\n') == std::string::npos)
                have = true;
        if (!have) add.push_back(d + " " + grp);
    }
    if (!add.empty()) {
        size_t pos = 0, end = std::string::npos;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            const std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            if (line.compare(0, 8, "declare ") == 0 &&
                line.find("@dx.op.rayQuery_Proceed") != std::string::npos) {
                end = nl == std::string::npos ? text.size() : nl;
                break;
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (end != std::string::npos) text = text.substr(0, end) + "\n" + Join(add) + text.substr(end);
    }
    return text;
}

std::string ProceedSite(const llm::Module& mod, const llm::Function& fn, const llm::Block* blk,
                        const llm::Instr* pr, const std::string& p, int n,
                        const std::string& accel, const std::vector<std::string>& cand,
                        const std::vector<std::string>& comm, const Vars& V) {
    const std::string s = p + "s" + std::to_string(n);
    const std::string q = p + "q" + std::to_string(n) + ".";
    auto lab = [&](const std::string& x) { return p.substr(1) + "q" + std::to_string(n) + "." + x; };
    auto L = [&](const std::string& x) { return "%" + lab(x); };
    std::vector<std::string> out = { "  br label " + L("a"), "", lab("a") + ":" };
    out.push_back(V.Load("ab", q + "ab"));
    out.push_back("  " + q + "abn = icmp ne i32 " + q + "ab, 0");
    out.push_back(V.Load("cs", q + "cs"));
    out.push_back("  " + q + "csn = icmp ne i32 " + q + "cs, 0");
    out.push_back(V.Load("af", q + "af"));
    out.push_back("  " + q + "afn = icmp ne i32 " + q + "af, 0");
    out.push_back("  " + q + "fst = and i1 " + q + "csn, " + q + "afn");
    out.push_back("  " + q + "stop = or i1 " + q + "abn, " + q + "fst");
    out.push_back("  br i1 " + q + "stop, label " + L("no") + ", label " + L("t"));
    out.push_back("");
    out.push_back(lab("t") + ":");
    for (const char* f : { "ct", "ox", "oy", "oz", "tn", "dx", "dy", "dz", "rf", "mk", "hl" })
        out.push_back(V.Load(f, q + "r." + f));
    for (const char* f : kKey) out.push_back(V.Load(std::string("l.") + f, q + "l." + f));
    out.push_back("  call void @dx.op.rayQuery_TraceRayInline(i32 179, i32 " + s + ", " + accel +
                  ", i32 " + q + "r.rf, i32 " + q + "r.mk, float " + q + "r.ox, float " + q +
                  "r.oy, float " + q + "r.oz, float " + q + "r.tn, float " + q + "r.dx, float " +
                  q + "r.dy, float " + q + "r.dz, float " + q + "r.ct)");
    out.push_back("  br label " + L("h"));
    out.push_back("");
    out.push_back(lab("h") + ":");
    std::vector<std::string> fields = { "f" };
    fields.insert(fields.end(), cand.begin(), cand.end());
    auto stTy = [&](const std::string& f) {
        std::string ty = f == "f" ? "i32" : FType(f);
        return ty == "i1" ? std::string("i32") : ty;
    };
    for (const auto& f : fields) {
        const std::string ty = stTy(f);
        const std::string z = ty == "float" ? "0.000000e+00" : "0";
        out.push_back("  " + q + "h." + f + " = phi " + ty + " [ " + z + ", " + L("t") + " ], [ " +
                      q + "h." + f + ", " + L("b") + " ], [ " + q + "n." + f + ", " + L("c") + " ]");
    }
    out.push_back("  " + q + "pr = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 " + s + ")");
    out.push_back("  br i1 " + q + "pr, label " + L("b") + ", label " + L("x"));
    out.push_back("");
    out.push_back(lab("b") + ":");
    out.push_back("  " + q + "ty = call i32 @dx.op.rayQuery_StateScalar.i32(i32 " +
                  std::to_string(kCandidateType) + ", i32 " + s + ")");
    out.push_back("  " + q + "tri = icmp eq i32 " + q + "ty, 0");
    out.push_back("  br i1 " + q + "tri, label " + L("c") + ", label " + L("h"));
    out.push_back("");
    out.push_back(lab("c") + ":");
    for (const auto& f : cand) {
        if (FType(f) == "i1") {
            out.push_back(Read(f, false, s, q + "g." + f + ".b"));
            out.push_back("  " + q + "g." + f + " = zext i1 " + q + "g." + f + ".b to i32");
        } else {
            out.push_back(Read(f, false, s, q + "g." + f));
        }
    }
    auto after = [&](const std::string& pa, const std::string& pb, const std::string& tag) {
        std::vector<std::string> o;
        const std::string t = q + tag;
        o.push_back("  " + t + ".t1 = fcmp ogt float " + pa + "t, " + pb + "t");
        o.push_back("  " + t + ".t0 = fcmp oeq float " + pa + "t, " + pb + "t");
        for (const char* f : { "inst", "geom", "prim" }) {
            o.push_back("  " + t + "." + f + "1 = icmp ugt i32 " + pa + f + ", " + pb + f);
            o.push_back("  " + t + "." + f + "0 = icmp eq i32 " + pa + f + ", " + pb + f);
        }
        o.push_back("  " + t + ".gp = and i1 " + t + ".geom0, " + t + ".prim1");
        o.push_back("  " + t + ".g = or i1 " + t + ".geom1, " + t + ".gp");
        o.push_back("  " + t + ".ig = and i1 " + t + ".inst0, " + t + ".g");
        o.push_back("  " + t + ".i = or i1 " + t + ".inst1, " + t + ".ig");
        o.push_back("  " + t + ".ti = and i1 " + t + ".t0, " + t + ".i");
        o.push_back("  " + t + " = or i1 " + t + ".t1, " + t + ".ti");
        return o;
    };
    for (const auto& l : after(q + "g.", q + "l.", "past")) out.push_back(l);
    out.push_back("  " + q + "nol = icmp eq i32 " + q + "r.hl, 0");
    out.push_back("  " + q + "pst = or i1 " + q + "nol, " + q + "past");
    for (const auto& l : after(q + "h.", q + "g.", "near")) out.push_back(l);
    out.push_back("  " + q + "nof = icmp eq i32 " + q + "h.f, 0");
    out.push_back("  " + q + "nr = or i1 " + q + "nof, " + q + "near");
    out.push_back("  " + q + "tk = and i1 " + q + "pst, " + q + "nr");
    out.push_back("  " + q + "n.f = select i1 " + q + "tk, i32 1, i32 " + q + "h.f");
    for (const auto& f : cand) {
        const std::string ty = stTy(f);
        out.push_back("  " + q + "n." + f + " = select i1 " + q + "tk, " + ty + " " + q + "g." + f +
                      ", " + ty + " " + q + "h." + f);
    }
    out.push_back("  br label " + L("h"));
    out.push_back("");
    out.push_back(lab("x") + ":");
    out.push_back("  " + q + "sc = call i32 @dx.op.rayQuery_StateScalar.i32(i32 " +
                  std::to_string(kCommittedStatus) + ", i32 " + s + ")");
    out.push_back("  " + q + "so = icmp eq i32 " + q + "sc, 1");
    out.push_back(Read("t", true, s, q + "st"));
    out.push_back("  " + q + "fd = icmp ne i32 " + q + "h.f, 0");
    out.push_back("  " + q + "cl = fcmp olt float " + q + "h.t, " + q + "st");
    out.push_back("  " + q + "nso = xor i1 " + q + "so, true");
    out.push_back("  " + q + "c1 = or i1 " + q + "nso, " + q + "cl");
    out.push_back("  " + q + "yl = and i1 " + q + "fd, " + q + "c1");
    out.push_back("  br i1 " + q + "yl, label " + L("y") + ", label " + L("o"));
    out.push_back("");
    out.push_back(lab("y") + ":");
    for (const auto& f : cand) out.push_back(V.Store("c." + f, q + "h." + f));
    for (const char* f : kKey) out.push_back(V.Store(std::string("l.") + f, q + "h." + f));
    out.push_back(V.Store("hl", "1"));
    out.push_back("  br label " + L("k"));
    out.push_back("");
    out.push_back(lab("o") + ":");
    out.push_back("  " + q + "nt = fcmp olt float " + q + "st, " + q + "r.ct");
    // DXR_TIER11_NEST_POISON=1: an opaque hit a search's traversal found is not
    // committed for the stepped query, so it never ends the search either.
    static const bool poison = std::getenv("DXR_TIER11_NEST_POISON") != nullptr;
    out.push_back("  " + q + "ac = and i1 " + q + "so, " + (poison ? std::string("false") : q + "nt"));
    out.push_back("  br i1 " + q + "ac, label " + L("m") + ", label " + L("no"));
    out.push_back("");
    out.push_back(lab("m") + ":");
    for (const auto& f : comm) {
        if (FType(f) == "i1") {
            out.push_back(Read(f, true, s, q + "m." + f + ".b"));
            out.push_back("  " + q + "m." + f + " = zext i1 " + q + "m." + f + ".b to i32");
        } else {
            out.push_back(Read(f, true, s, q + "m." + f));
        }
        out.push_back(V.Store("k." + f, q + "m." + f));
    }
    out.push_back(V.Store("ct", q + "st"));
    out.push_back(V.Store("cs", "1"));
    out.push_back("  br label " + L("a"));
    out.push_back("");
    out.push_back(lab("no") + ":");
    out.push_back("  br label " + L("k"));
    out.push_back("");
    out.push_back(lab("k") + ":");
    out.push_back("  " + pr->result + " = phi i1 [ true, " + L("y") + " ], [ false, " + L("no") + " ]");
    std::map<int, std::optional<std::string>> edits;
    edits[pr->index] = Join(out);
    if (!blk->label.empty()) {
        std::string esc;
        for (char c : blk->label) {
            if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos) esc += '\\';
            esc += c;
        }
        const std::regex re(esc + R"((?=\s*\]))");
        for (const auto& sname : blk->Successors()) {
            const llm::Block* sb = fn.FindBlock(sname);
            if (!sb) continue;
            for (const auto& i : sb->instrs) {
                if (!i.isPhi) break;
                edits[i.index] = std::regex_replace(i.line, re, L("k"));
            }
        }
    }
    return llm::Render(mod, edits);
}

std::string StepQuery(const llm::Module& mod, const std::string& h, int k) {
    const llm::Function& fn = mod.functions[0];
    const auto ops = Owned(fn, h);
    std::vector<const llm::Instr*> alloc, traces;
    for (const auto& o : ops) {
        if (o.second->DxOp() == kAllocate) alloc.push_back(o.second);
        if (o.second->DxOp() == kTraceInline) traces.push_back(o.second);
    }
    if (alloc.size() != 1 || traces.empty())
        throw Unsupported{ "RayQuery " + h + ", with another query inside its loop, has no "
                           "single allocation and trace" };
    std::set<std::string> accel;
    for (const auto* t : traces) accel.insert(Trim(t->args[2]));
    if (accel.size() != 1)
        throw Unsupported{ "RayQuery " + h + ", with another query inside its loop, traces "
                           "more than one acceleration structure" };
    const std::string cflags = LastWord(alloc[0]->args[1]);
    const std::string p = "%ns" + std::to_string(k) + ".";
    std::set<std::string> cand(std::begin(kKey), std::end(kKey)), comm;
    for (const auto& o : ops) {
        const int op = o.second->DxOp();
        if (op == kAllocate || op == kTraceInline || op == kProceed || op == kAbort ||
            op == kCommitNonOpaque || op == kCommitProcedural || op == kCandidateType ||
            op == kCandidateProcNonOpaque || op == kCommittedStatus || op == kRayFlags)
            continue;
        const auto fc = FieldOf(*o.second);
        if (fc.first.empty())
            throw Unsupported{ "RayQuery " + h + ", with another query inside its loop, reads " +
                               std::string(OpcodeName(op)) + ", which a stepped query does not "
                               "keep" };
        (fc.second ? comm : cand).insert(fc.first);
    }
    for (const auto& f : comm)
        if (f[0] == 'o' || f[0] == 'd')
            throw Unsupported{ "RayQuery " + h + " reads a committed object-space ray" };
    cand.insert(comm.begin(), comm.end());
    const std::vector<std::string> candv(cand.begin(), cand.end()), commv(comm.begin(), comm.end());
    Vars V;
    std::vector<std::string> allocs;
    auto mk = [&](const std::string& name, const std::string& ty) {
        const std::string st = ty == "i1" ? "i32" : ty;
        V.var[name] = { p + name, st };
        allocs.push_back("  " + p + name + " = alloca " + st + ", align 4");
    };
    for (const auto& f : candv) mk("c." + f, FType(f));
    for (const auto& f : commv) mk("k." + f, FType(f));
    for (const char* f : kKey) mk(std::string("l.") + f, FType(f));
    const std::pair<const char*, const char*> fixed[] = {
        { "hl", "i32" }, { "cs", "i32" }, { "ct", "float" }, { "ab", "i32" }, { "af", "i32" },
        { "rf", "i32" }, { "ox", "float" }, { "oy", "float" }, { "oz", "float" }, { "tn", "float" },
        { "dx", "float" }, { "dy", "float" }, { "dz", "float" }, { "mk", "i32" } };
    for (const auto& f : fixed) mk(f.first, f.second);

    std::map<int, std::optional<std::string>> edits;
    const llm::Instr& entryFirst = fn.blocks[0].instrs[0];
    size_t nProceed = 0;
    for (const auto& o : ops) {
        const llm::Instr& i = *o.second;
        const int op = i.DxOp();
        const std::string ix = std::to_string(i.index);
        if (op == kAllocate) {
            edits[i.index] = std::nullopt;
        } else if (op == kTraceInline) {
            std::vector<std::string> a;
            for (const auto& x : i.args) a.push_back(LastWord(x));
            std::vector<std::string> lines = {
                V.Store("hl", "0"), V.Store("cs", "0"), V.Store("ab", "0"), V.Store("ct", a[12]),
                V.Store("ox", a[5]), V.Store("oy", a[6]), V.Store("oz", a[7]), V.Store("tn", a[8]),
                V.Store("dx", a[9]), V.Store("dy", a[10]), V.Store("dz", a[11]), V.Store("mk", a[4]),
                "  " + p + "f" + ix + " = or i32 " + cflags + ", " + a[3],
                V.Store("rf", p + "f" + ix),
                "  " + p + "a" + ix + " = and i32 " + p + "f" + ix + ", 4",
                V.Store("af", p + "a" + ix) };
            edits[i.index] = Join(lines);
        } else if (op == kAbort) {
            edits[i.index] = V.Store("ab", "1");
        } else if (op == kCommitProcedural) {
            edits[i.index] = std::nullopt;
        } else if (op == kCommitNonOpaque) {
            std::vector<std::string> lines;
            for (const auto& f : commv) {
                lines.push_back(V.Load("c." + f, p + "v" + ix + "." + f));
                lines.push_back(V.Store("k." + f, p + "v" + ix + "." + f));
            }
            lines.push_back(V.Load("c.t", p + "v" + ix + ".ct"));
            lines.push_back(V.Store("ct", p + "v" + ix + ".ct"));
            lines.push_back(V.Store("cs", "1"));
            edits[i.index] = Join(lines);
        } else if (op == kCandidateType) {
            edits[i.index] = "  " + i.result + " = add i32 0, 0";
        } else if (op == kCandidateProcNonOpaque) {
            edits[i.index] = "  " + i.result + " = icmp eq i32 0, 1";
        } else if (op == kCommittedStatus) {
            edits[i.index] = V.Load("cs", i.result);
        } else if (op == kRayFlags) {
            edits[i.index] = V.Load("rf", i.result);
        } else if (op == kProceed) {
            ++nProceed;
        } else {
            const auto fc = FieldOf(i);
            const std::string name = (fc.second ? "k." : "c.") + fc.first;
            if (FType(fc.first) == "i1")
                edits[i.index] = V.Load(name, i.result + ".b") + "\n  " + i.result +
                                 " = icmp ne i32 " + i.result + ".b, 0";
            else
                edits[i.index] = V.Load(name, i.result);
        }
    }
    std::vector<std::string> head = allocs;
    for (size_t n = 0; n < nProceed; ++n)
        head.push_back("  " + p + "s" + std::to_string(n) +
                       " = call i32 @dx.op.allocateRayQuery(i32 178, i32 " + cflags + ")");
    {
        auto it = edits.find(entryFirst.index);
        const std::optional<std::string> first =
            it != edits.end() ? it->second : std::optional<std::string>(entryFirst.line);
        edits[entryFirst.index] = first ? Join(head) + "\n" + *first : Join(head);
    }
    std::string text = Declare(llm::Module(llm::Render(mod, edits)), candv, commv);
    for (size_t n = 0; n < nProceed; ++n) {
        llm::Module m2(text);
        const llm::Function& fn2 = m2.functions[0];
        const llm::Block* sb = nullptr;
        const llm::Instr* si = nullptr;
        for (const auto& o : Owned(fn2, h))
            if (o.second->DxOp() == kProceed) { sb = o.first; si = o.second; break; }
        text = ProceedSite(m2, fn2, sb, si, p, (int)n, *accel.begin(), candv, commv, V);
    }
    return text;
}

}  // namespace

bool StepNested(const std::string& in, std::string* out, std::string* why) {
    std::string text = in;
    try {
        for (int n = 0; n < 16; ++n) {
            llm::Module m(text);
            const auto a = Analyze(m);
            if (a.ok || !a.nested) break;
            CheckBoxes(m.functions[0], a.outer);
            text = StepQuery(m, a.outer.handle, n);
        }
    } catch (const Unsupported& u) {
        *why = u.why;
        return false;
    }
    *out = text;
    return true;
}

}  // namespace rq
