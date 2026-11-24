// The split at a query (0.84.0): split.py in phase5/rewriter says what this
// does and why; this is its port, held byte-identical.
#include "split.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "ll_model.h"

namespace rq {

// rq_lower.cpp, rg_host.cpp.
bool SplitIsSm66(const std::string& t);
std::set<std::string> SplitTypeNames(const std::string& t);
std::vector<std::string> SplitOperands(const llm::Instr& i, const std::set<std::string>& t);
std::string ResolveAttrs(const std::string& in, std::string* err);
std::vector<std::string> RgDropUnusedDeclarations(const std::vector<std::string>& lines);

namespace {

const unsigned kSpace = 0x7FFF0000u;
const char* kResRet = "%dx.types.ResRet.i32";
const char* kRab = "%struct.RWByteAddressBuffer";
const char* kHandle = "%dx.types.Handle";
const unsigned kHdr = 16;

bool QueryOp(int op) { return op >= 178 && op < 216; }
bool RebuildOp(int op) {
    static const std::set<int> k = { 57, 59, 160, 216, 217, 218, 93, 94, 95, 96 };
    return k.count(op) != 0;
}
bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
std::string Join(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += "\n"; o += v[i]; }
    return o;
}
std::string Strip(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string TypeOf(const llm::Instr& i) {
    std::string b = i.body;
    const size_t cut = b.find(';');
    if (cut != std::string::npos) b = b.substr(0, cut);
    b = Strip(b);
    std::smatch m;
    static const std::regex kCall(R"RX(^call\s+(?:fast\s+)?(\S+)\s+@)RX");
    static const std::regex kPhiLoad(R"RX(^(?:phi|load)\s+(\S+?),?\s)RX");
    static const std::regex kExtract(R"RX(^extractvalue\s+(%[\w.$-]+)\s)RX");
    static const std::regex kCmp(R"RX(^(?:icmp|fcmp)\s)RX");
    static const std::regex kCast(R"RX(^(?:zext|sext|trunc|uitofp|sitofp|fptoui|fptosi|fpext|fptrunc|bitcast)\s+\S+\s+\S+\s+to\s+(\S+))RX");
    static const std::regex kSelect(R"RX(^select\s+i1\s+[^,]+,\s*(\S+)\s)RX");
    static const std::regex kBin(R"RX(^(?:add|sub|mul|udiv|sdiv|urem|srem|shl|lshr|ashr|and|or|xor|fadd|fsub|fmul|fdiv|frem)(?:\s+(?:nuw|nsw|exact|fast|nnan|ninf|nsz|arcp))*\s+(\S+)\s)RX");
    if (std::regex_search(b, m, kCall)) return m[1].str();
    const std::string b2 = b + " ";
    if (std::regex_search(b2, m, kPhiLoad)) return m[1].str();
    if (std::regex_search(b, m, kExtract)) {
        const std::string agg = m[1].str();
        const std::string suffix = agg.substr(agg.rfind('.') + 1);
        static const std::map<std::string, std::string> kMap = {
            { "i32", "i32" }, { "f32", "float" }, { "f16", "half" }, { "i16", "i16" },
            { "f64", "double" }, { "i64", "i64" }, { "i1", "i1" } };
        auto it = kMap.find(suffix);
        return it == kMap.end() ? "" : it->second;
    }
    if (std::regex_search(b, m, kCmp)) return "i1";
    if (std::regex_search(b, m, kCast)) return m[1].str();
    if (std::regex_search(b, m, kSelect)) return m[1].str();
    if (std::regex_search(b, m, kBin)) return m[1].str();
    return "";
}

int Dwords(const std::string& t) {
    if (t == "i1" || t == "i16" || t == "i32" || t == "half" || t == "float") return 1;
    if (t == "i64" || t == "double") return 2;
    return 0;
}

std::map<std::string, std::set<std::string>> PostDominators(const llm::Function& fn) {
    std::vector<std::string> labels;
    std::map<std::string, std::vector<std::string>> succ;
    for (const auto& b : fn.blocks) { labels.push_back(b.label); succ[b.label] = b.Successors(); }
    std::set<std::string> all(labels.begin(), labels.end());
    std::set<std::string> exits;
    for (const auto& l : labels) if (succ[l].empty()) exits.insert(l);
    std::map<std::string, std::set<std::string>> pdom;
    for (const auto& l : labels) pdom[l] = all;
    for (const auto& e : exits) pdom[e] = { e };
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = labels.rbegin(); it != labels.rend(); ++it) {
            const std::string& l = *it;
            if (exits.count(l)) continue;
            std::set<std::string> nw;
            bool firstSet = true;
            for (const auto& s : succ[l]) {
                if (!pdom.count(s)) continue;
                if (firstSet) { nw = pdom[s]; firstSet = false; }
                else {
                    std::set<std::string> x;
                    for (const auto& v : nw) if (pdom[s].count(v)) x.insert(v);
                    nw = x;
                }
            }
            nw.insert(l);
            if (nw != pdom[l]) { pdom[l] = nw; changed = true; }
        }
    }
    return pdom;
}

std::set<std::string> Reach(const llm::Function& fn, const std::string& start, const std::string* stop) {
    std::map<std::string, std::vector<std::string>> succ;
    for (const auto& b : fn.blocks) succ[b.label] = b.Successors();
    std::set<std::string> seen = { start };
    std::vector<std::string> stack = { start };
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        for (const auto& s : succ[cur]) {
            if ((stop && s == *stop) || seen.count(s)) continue;
            seen.insert(s);
            stack.push_back(s);
        }
    }
    return seen;
}

std::map<int, std::string> Md(const std::string& text) {
    static const std::regex kNode(R"(^!(\d+) = (?:distinct )?!\{(.*)\}\s*$)");
    std::map<int, std::string> md;
    std::smatch m;
    for (const auto& l : llm::SplitLines(text))
        if (std::regex_match(l, m, kNode)) md[std::stoi(m[1].str())] = m[2].str();
    return md;
}

bool NumThreads(const std::string& text, unsigned out[3], std::string* why) {
    static const std::regex kEp(R"(^!dx\.entryPoints = !\{!(\d+)\})");
    std::smatch m;
    int ep = -1;
    for (const auto& l : llm::SplitLines(text))
        if (std::regex_search(l, m, kEp)) { ep = std::stoi(m[1].str()); break; }
    auto md = Md(text);
    if (ep >= 0 && md.count(ep)) {
        auto entry = llm::SplitArgs(md[ep]);
        if (entry.size() >= 5 && entry[4] != "null") {
            auto props = llm::SplitArgs(md[std::stoi(entry[4].substr(1))]);
            for (size_t k = 0; k + 1 < props.size(); k += 2)
                if (props[k] == "i32 4") {
                    auto v = llm::SplitArgs(md[std::stoi(props[k + 1].substr(1))]);
                    for (int c = 0; c < 3 && c < (int)v.size(); ++c)
                        out[c] = (unsigned)std::stoul(v[c].substr(v[c].find(' ') + 1));
                    return true;
                }
        }
    }
    *why = "the compute shader declares no numthreads";
    return false;
}

// What the "; Function Attrs:" line before an added declaration says, which
// the lowering reads when it drops a declaration it no longer needs.
std::string AttrText(const std::string& ph) {
    if (ph == "#RQRO") return "nounwind readonly";
    if (ph == "#RQNW") return "nounwind";
    if (ph == "#RQNONE") return "nounwind readnone";
    return "noduplicate nounwind";   // #SPBAR
}

// A resource the part makes no handle for leaves its records, the rest
// renumbered (at 6.5 their handles' range ids with them): lowered into a
// library an unused one is an unused global, which the validator refuses.
std::string DropUnusedResources(const std::string& text, const std::string& fbody, bool sm66) {
    auto md = Md(text);
    static const std::regex kRes(R"(^!dx\.resources = !\{!(\d+)\})");
    int rid = -1;
    std::smatch m;
    for (const auto& l : llm::SplitLines(text))
        if (std::regex_search(l, m, kRes)) { rid = std::stoi(m[1].str()); break; }
    if (rid < 0) return text;
    auto groups = llm::SplitArgs(md[rid]);
    std::map<int, std::string> nodes;
    std::map<std::pair<int, int>, int> remap;
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ", "; s += v[i]; }
        return s;
    };
    auto field = [](const std::string& f) { return f.substr(f.find(' ') + 1); };
    for (int c = 0; c < (int)groups.size(); ++c) {
        if (groups[c] == "null") continue;
        const int gid = std::stoi(groups[c].substr(1));
        const auto recs = llm::SplitArgs(md[gid]);
        std::vector<std::string> kept;
        for (const auto& r : recs) {
            auto f = llm::SplitArgs(md[std::stoi(r.substr(1))]);
            const int old = std::stoi(field(f[0]));
            bool used;
            if (sm66) {
                const std::string lo = field(f[4]), space = field(f[3]);
                const std::regex pat("%dx\\.types\\.ResBind \\{ i32 " + lo + ", i32 [^,]+, i32 " + space +
                                     ", i8 " + std::to_string(c) + " \\}");
                used = std::regex_search(fbody, pat) ||
                       (lo == "0" && space == "0" && c == 0 &&
                        fbody.find("%dx.types.ResBind zeroinitializer") != std::string::npos);
            } else {
                used = fbody.find("@dx.op.createHandle(i32 57, i8 " + std::to_string(c) + ", i32 " +
                                  std::to_string(old) + ",") != std::string::npos;
            }
            if (!used) continue;
            const int nw = (int)kept.size();
            kept.push_back(r);
            remap[{ c, old }] = nw;
            if (nw != old) {
                f[0] = "i32 " + std::to_string(nw);
                nodes[std::stoi(r.substr(1))] = join(f);
            }
        }
        if (kept.size() != recs.size()) {
            if (!kept.empty()) nodes[gid] = join(kept);
            else groups[c] = "null";
        }
    }
    nodes[rid] = join(groups);
    static const std::regex kNodeHead(R"(^!(\d+) = !\{)");
    std::vector<std::string> out;
    for (auto l : llm::SplitLines(text)) {
        if (std::regex_search(l, m, kNodeHead)) {
            auto it = nodes.find(std::stoi(m[1].str()));
            if (it != nodes.end()) l = "!" + m[1].str() + " = !{" + it->second + "}";
        }
        out.push_back(l);
    }
    std::string t = Join(out);
    if (!sm66) {
        static const std::regex kCh(R"((@dx\.op\.createHandle\(i32 57, i8 (\d), i32 )(\d+),)");
        std::string r;
        auto begin = t.cbegin();
        std::smatch hm;
        while (std::regex_search(begin, t.cend(), hm, kCh)) {
            r.append(begin, hm[0].first);
            const int c = std::stoi(hm[2].str()), old = std::stoi(hm[3].str());
            auto it = remap.find({ c, old });
            r += hm[1].str() + std::to_string(it == remap.end() ? old : it->second) + ",";
            begin = hm[0].second;
        }
        r.append(begin, t.cend());
        t = r;
    }
    return t;
}

std::string ScratchRecord(const std::string& text, std::string* why) {
    auto md = Md(text);
    int nxt = md.empty() ? 0 : md.rbegin()->first + 1;
    std::vector<std::string> added;
    auto node = [&](const std::string& body) {
        const int nid = nxt++;
        added.push_back("!" + std::to_string(nid) + " = !{" + body + "}");
        return nid;
    };
    auto rewrite = [](const std::string& t, int nid, const std::string& body) {
        const std::regex pat("^!" + std::to_string(nid) + " = !\\{");
        std::vector<std::string> out;
        for (const auto& l : llm::SplitLines(t))
            out.push_back(std::regex_search(l, pat) ? "!" + std::to_string(nid) + " = !{" + body + "}" : l);
        return Join(out);
    };
    static const std::regex kRes(R"(^!dx\.resources = !\{!(\d+)\})");
    int rid = -1;
    std::smatch m;
    for (const auto& l : llm::SplitLines(text))
        if (std::regex_search(l, m, kRes)) { rid = std::stoi(m[1].str()); break; }
    if (rid < 0) { *why = "a compute shader with no resources"; return ""; }
    auto groups = llm::SplitArgs(md[rid]);
    std::vector<std::string> uavs;
    if (groups[1] != "null") uavs = llm::SplitArgs(md[std::stoi(groups[1].substr(1))]);
    const int rec = node("i32 " + std::to_string(uavs.size()) + ", " + kRab + "* undef, !\"\", i32 " +
                         std::to_string(kSpace) + ", i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null");
    std::string t = text;
    const std::string id = std::to_string(uavs.size());
    for (size_t at = t.find("@UAVID@"); at != std::string::npos; at = t.find("@UAVID@", at))
        t.replace(at, 7, id);
    std::vector<std::string> list = uavs;
    list.push_back("!" + std::to_string(rec));
    std::string joined;
    for (size_t i = 0; i < list.size(); ++i) { if (i) joined += ", "; joined += list[i]; }
    const int lst = node(joined);
    groups[1] = "!" + std::to_string(lst);
    std::string gj;
    for (size_t i = 0; i < groups.size(); ++i) { if (i) gj += ", "; gj += groups[i]; }
    t = rewrite(t, rid, gj);
    while (!t.empty() && t.back() == '\n') t.pop_back();
    return t + "\n" + Join(added) + "\n";
}

bool MakeModule(const std::string& text, const llm::Function& fn, std::vector<std::string> body, bool sm66,
                std::string* result, std::string* why) {
    auto lines = llm::SplitLines(text);
    std::vector<std::string> out(lines.begin(), lines.begin() + fn.index + 1);
    while (!body.empty() && body.back().empty()) body.pop_back();
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), lines.begin() + fn.endIndex, lines.end());
    const std::string have = Join(out);
    const std::string H = kHandle;
    std::vector<std::pair<std::string, std::string>> decls = {
        { "@dx.op.rawBufferLoad.i32(", std::string("declare ") + kResRet + " @dx.op.rawBufferLoad.i32(i32, " + H +
                                       ", i32, i32, i8, i32) #RQRO" },
        { "@dx.op.rawBufferStore.i32(", "declare void @dx.op.rawBufferStore.i32(i32, " + H +
                                        ", i32, i32, i32, i32, i32, i32, i8, i32) #RQNW" },
        { "@dx.op.groupId.i32(", "declare i32 @dx.op.groupId.i32(i32, i32) #RQNONE" },
        { "@dx.op.flattenedThreadIdInGroup.i32(", "declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #RQNONE" },
        { "@dx.op.barrier(", "declare void @dx.op.barrier(i32, i32) #SPBAR" },
    };
    if (sm66) {
        decls.push_back({ "@dx.op.createHandleFromBinding(", "declare " + H +
                          " @dx.op.createHandleFromBinding(i32, %dx.types.ResBind, i32, i1) #RQNONE" });
        decls.push_back({ "@dx.op.annotateHandle(", "declare " + H + " @dx.op.annotateHandle(i32, " + H +
                          ", %dx.types.ResourceProperties) #RQNONE" });
    } else {
        decls.push_back({ "@dx.op.createHandle(", "declare " + H + " @dx.op.createHandle(i32, i8, i32, i32, i1) #RQRO" });
    }
    std::vector<std::string> adds;
    for (const auto& kd : decls) {
        bool declared = false;
        for (const auto& l : out) if (StartsWith(l, "declare ") && l.find(kd.first) != std::string::npos) declared = true;
        if (!declared && have.find("call ") != std::string::npos && have.find(kd.first) != std::string::npos) {
            adds.push_back("");
            adds.push_back("; Function Attrs: " + AttrText(kd.second.substr(kd.second.rfind(' ') + 1)));
            adds.push_back(kd.second);
        }
    }
    out = RgDropUnusedDeclarations(out);
    const std::string fbody = Join(body);
    static const std::regex kGs(R"RX(^(@"[^"]+"|@[\w.$-]+) = .*addrspace\(3\) global)RX");
    {
        std::vector<std::string> kept;
        std::smatch m;
        for (const auto& l : out) {
            if (std::regex_search(l, m, kGs) && fbody.find(m[1].str()) == std::string::npos) continue;
            kept.push_back(l);
        }
        out = kept;
    }
    int at = -1;
    for (int k = 0; k < (int)out.size(); ++k) if (StartsWith(out[k], "declare ") || out[k] == "}") at = k;
    out.insert(out.begin() + at + 1, adds.begin(), adds.end());
    std::vector<std::pair<std::string, std::string>> tys = { { kResRet, std::string(kResRet) + " = type { i32, i32, i32, i32, i32 }" } };
    if (sm66) {
        tys.push_back({ "%dx.types.ResBind", "%dx.types.ResBind = type { i32, i32, i32, i8 }" });
        tys.push_back({ "%dx.types.ResourceProperties", "%dx.types.ResourceProperties = type { i32, i32 }" });
    }
    tys.push_back({ kRab, std::string(kRab) + " = type { i32 }" });
    std::vector<std::string> types;
    for (const auto& t : tys) {
        bool has = false;
        for (const auto& l : out) if (StartsWith(l, t.first + " = type")) has = true;
        if (!has) types.push_back(t.second);
    }
    static const std::regex kType(R"RX(^%[\w."<>, -]+ = type )RX");
    int lastType = -1;
    for (int k = 0; k < (int)out.size(); ++k) if (std::regex_search(out[k], kType)) lastType = k;
    out.insert(out.begin() + lastType + 1, types.begin(), types.end());
    std::string t2 = ScratchRecord(DropUnusedResources(Join(out), fbody, sm66), why);
    if (t2.empty()) return false;
    if (t2.find("#SPBAR") != std::string::npos) {
        static const std::regex kAm(R"(^attributes (#\d+) = \{ noduplicate nounwind \})");
        std::string rep;
        std::smatch m;
        for (const auto& l : llm::SplitLines(t2))
            if (std::regex_search(l, m, kAm)) { rep = m[1].str(); break; }
        if (rep.empty()) {
            static const std::regex kId(R"(^attributes #(\d+) = )");
            int mx = -1, last = -1;
            auto ls = llm::SplitLines(t2);
            for (int k = 0; k < (int)ls.size(); ++k)
                if (std::regex_search(ls[k], m, kId)) { mx = (std::max)(mx, std::stoi(m[1].str())); last = k; }
            rep = "#" + std::to_string(mx + 1);
            ls.insert(ls.begin() + last + 1, "attributes " + rep + " = { noduplicate nounwind }");
            t2 = Join(ls);
        }
        for (size_t p = t2.find("#SPBAR"); p != std::string::npos; p = t2.find("#SPBAR", p))
            t2.replace(p, 6, rep);
    }
    std::string err;
    *result = ResolveAttrs(t2, &err);
    if (!err.empty()) { *why = err; return false; }
    return true;
}

}  // namespace

SplitResult SplitAtQuery(const std::string& text) {
    SplitResult res;
    llm::Module m(text);
    if (m.functions.size() != 1) { res.error = "a compute shader with more than one function"; return res; }
    const llm::Function& fn = m.functions[0];
    const auto types = SplitTypeNames(text);
    const bool sm66 = SplitIsSm66(text);
    unsigned nt[3] = { 1, 1, 1 };
    if (!NumThreads(text, nt, &res.error)) return res;
    const unsigned NT = nt[0] * nt[1] * nt[2];
    auto header = [&](const llm::Block& b) { return b.index >= 0 ? m.lines[(size_t)b.index] : std::string(); };

    std::vector<std::string> qblocks;
    const llm::Block* S = nullptr;
    size_t cut1 = 0;
    for (const auto& b : fn.blocks)
        for (size_t k = 0; k < b.instrs.size(); ++k)
            if (QueryOp(b.instrs[k].DxOp())) {
                if (!S) { S = &b; cut1 = k; }
                if (std::find(qblocks.begin(), qblocks.end(), b.label) == qblocks.end()) qblocks.push_back(b.label);
            }
    if (!S) { res.error = "no RayQuery in the shader"; return res; }
    auto dom = fn.Dominators();
    for (const auto& l : qblocks)
        if (!dom[l].count(S->label)) { res.error = "a query op the first query does not dominate"; return res; }
    auto pdom = PostDominators(fn);
    std::set<std::string> cands = pdom[qblocks[0]];
    for (const auto& l : qblocks) {
        std::set<std::string> x;
        for (const auto& c : cands) if (pdom[l].count(c)) x.insert(c);
        cands = x;
    }
    for (const auto& l : qblocks) cands.erase(l);
    std::string P;
    bool found = false;
    for (const auto& c : cands) {
        bool all = true;
        for (const auto& o : cands) if (!(o == c || pdom[c].count(o))) all = false;
        if (all) { P = c; found = true; }
    }
    if (!found) { res.error = "the query region has no single exit"; return res; }
    const auto R = Reach(fn, S->label, &P);
    for (const auto& l : R) {
        const llm::Block* b = fn.FindBlock(l);
        for (const auto& s : b->Successors())
            if (s == S->label) {
                res.error = "a query inside a loop, beside groupshared memory, barriers or wave ops";
                return res;
            }
    }
    for (const auto& l : R)
        if (!dom[l].count(S->label)) { res.error = "a block of the query region the query does not dominate"; return res; }
    const auto Bpart = Reach(fn, P, nullptr);
    bool overlap = Bpart.count(S->label) != 0;
    for (const auto& l : Bpart) if (R.count(l)) overlap = true;
    if (overlap) { res.error = "the query region is reached again after its exit"; return res; }
    const llm::Block* Pb = fn.FindBlock(P);

    std::map<const llm::Instr*, char> where;
    std::map<std::string, const llm::Instr*> defs;
    for (const auto& b : fn.blocks)
        for (size_t k = 0; k < b.instrs.size(); ++k) {
            const llm::Instr& i = b.instrs[k];
            char p;
            if (R.count(b.label)) p = (&b == S && k < cut1) ? 'A' : 'Q';
            else if (b.label == P) p = i.isPhi ? 'Q' : 'B';
            else if (Bpart.count(b.label)) p = 'B';
            else p = 'A';
            where[&i] = p;
            if (!i.result.empty()) defs[i.result] = &i;
        }
    static const std::regex kWave(R"(dx\.op\.(wave|quad))");
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs) {
            if (where[&i] != 'Q') continue;
            if (i.DxOp() == 80 || (!i.callee.empty() && std::regex_search(i.callee, kWave))) {
                res.error = "a barrier or wave op inside the query region";
                return res;
            }
            if (i.body.find("addrspace(3)") != std::string::npos) {
                res.error = "groupshared memory inside the query region";
                return res;
            }
        }

    std::map<char, std::set<std::string>> rebuild;
    std::map<char, std::vector<std::string>> store, loads;
    std::string err;
    std::function<bool(const std::string&, char)> need = [&](const std::string& u, char Y) -> bool {
        auto it = defs.find(u);
        if (it == defs.end()) { err = "a value with no definition crosses the split: " + u; return false; }
        const llm::Instr* d = it->second;
        const char X = where[d];
        if (X == Y) return true;
        if (rebuild[Y].count(u) || std::find(loads[Y].begin(), loads[Y].end(), u) != loads[Y].end()) return true;
        bool rb = RebuildOp(d->DxOp());
        if (!rb && StartsWith(d->body, "extractvalue ")) {
            const auto ops = SplitOperands(*d, types);
            auto jt = ops.empty() ? defs.end() : defs.find(ops[0]);
            rb = RebuildOp((jt == defs.end() ? d : jt->second)->DxOp());
        }
        // A groupshared address is rebuilt (B holds the same arrays, given
        // back from the group area when B reads one A wrote).
        if (!rb && StartsWith(d->body, "getelementptr ") && d->body.find("addrspace(3)") != std::string::npos)
            rb = true;
        if (rb) {
            rebuild[Y].insert(u);
            for (const auto& o : SplitOperands(*d, types))
                if (defs.count(o) && !need(o, Y)) return false;
            return true;
        }
        const std::string t = TypeOf(*d);
        if (!Dwords(t)) { err = "a value of type " + (t.empty() ? std::string("None") : t) + " crosses the split: " + u; return false; }
        if (std::find(store[X].begin(), store[X].end(), u) == store[X].end()) store[X].push_back(u);
        loads[Y].push_back(u);
        return true;
    };
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs) {
            const char Y = where[&i];
            if (Y == 'A') continue;
            for (const auto& u : SplitOperands(i, types))
                if (defs.count(u) && where[defs[u]] != Y && !need(u, Y)) { res.error = err; return res; }
        }
    std::map<std::string, unsigned> field;
    unsigned K = 1;
    for (char X : { 'A', 'Q' })
        for (const auto& u : store[X]) { field[u] = K; K += (unsigned)Dwords(TypeOf(*defs[u])); }

    // Groupshared arrays B reads that A wrote.
    struct Gs { std::string g, ty; unsigned n; std::string el; unsigned off; };
    std::vector<Gs> gs, saved;
    {
        static const std::regex kG(R"RX(^(@"[^"]+"|@[\w.$-]+) = (?:external )?addrspace\(3\) global (\[(\d+) x (i32|float)\]|i32|float))RX");
        static const std::regex kAny(R"RX(^@\S+ = .*addrspace\(3\) global)RX");
        std::smatch gm;
        for (const auto& line : llm::SplitLines(text)) {
            if (std::regex_search(line, gm, kG)) {
                Gs x;
                x.g = gm[1].str();
                x.ty = gm[2].str();
                x.n = gm[3].matched ? (unsigned)std::stoul(gm[3].str()) : 0u;
                x.el = gm[4].matched ? gm[4].str() : gm[2].str();
                x.off = 0;
                gs.push_back(x);
            } else if (std::regex_search(line, kAny)) {
                res.error = "a groupshared variable of a shape the split does not save: " + line.substr(0, 80);
                return res;
            }
        }
    }
    unsigned G = 0;
    for (auto x : gs) {
        bool inA = false, inB = false;
        for (const auto& b : fn.blocks)
            for (const auto& i : b.instrs) {
                if (i.body.find(x.g) == std::string::npos) continue;
                if (where[&i] == 'A') inA = true;
                if (where[&i] == 'B' || rebuild['B'].count(i.result)) inB = true;
            }
        if (inA && inB) { x.off = G; saved.push_back(x); G += (std::max)(x.n, 1u); }
    }

    int tmp = 0;
    auto fresh = [&](const char* p) { return "%sp." + std::string(p) + std::to_string(++tmp); };
    const std::string H = kHandle;
    auto rawLoad = [&](std::vector<std::string>& o, const std::string& off, const std::string& name) {
        const std::string r = fresh("r");
        o.push_back("  " + r + " = call " + kResRet + " @dx.op.rawBufferLoad.i32(i32 139, " + H + " %sp.h, i32 " +
                    off + ", i32 undef, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)");
        o.push_back("  " + name + " = extractvalue " + kResRet + " " + r + ", 0");
    };
    auto rawStore = [&](std::vector<std::string>& o, const std::string& off, const std::string& v) {
        o.push_back("  call void @dx.op.rawBufferStore.i32(i32 140, " + H + " %sp.h, i32 " + off + ", i32 undef, i32 " +
                    v + ", i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,"
                    "value0,value1,value2,value3,mask,alignment)");
    };
    auto addr = [&](std::vector<std::string>& o, unsigned f) {
        const std::string a = fresh("a");
        o.push_back("  " + a + " = add i32 %sp.tb, " + std::to_string(4 * f));
        return a;
    };
    auto toWords = [&](std::vector<std::string>& o, const std::string& u, const std::string& t) {
        std::vector<std::string> ws;
        if (t == "i32") { ws.push_back(u); return ws; }
        if (t == "float") { auto w = fresh("w"); o.push_back("  " + w + " = bitcast float " + u + " to i32"); ws.push_back(w); return ws; }
        if (t == "i1" || t == "i16") { auto w = fresh("w"); o.push_back("  " + w + " = zext " + t + " " + u + " to i32"); ws.push_back(w); return ws; }
        if (t == "half") {
            auto f = fresh("w"); o.push_back("  " + f + " = fpext half " + u + " to float");
            auto w = fresh("w"); o.push_back("  " + w + " = bitcast float " + f + " to i32");
            ws.push_back(w); return ws;
        }
        std::string v = u;
        if (t == "double") { v = fresh("w"); o.push_back("  " + v + " = bitcast double " + u + " to i64"); }
        auto lo = fresh("w"); o.push_back("  " + lo + " = trunc i64 " + v + " to i32");
        auto sh = fresh("w"); o.push_back("  " + sh + " = lshr i64 " + v + ", 32");
        auto hi = fresh("w"); o.push_back("  " + hi + " = trunc i64 " + sh + " to i32");
        ws.push_back(lo); ws.push_back(hi);
        return ws;
    };
    auto fromWords = [&](std::vector<std::string>& o, const std::string& u, const std::string& t,
                         const std::vector<std::string>& ws) {
        if (t == "i32") o.push_back("  " + u + " = add i32 " + ws[0] + ", 0");
        else if (t == "float") o.push_back("  " + u + " = bitcast i32 " + ws[0] + " to float");
        else if (t == "i1") o.push_back("  " + u + " = icmp ne i32 " + ws[0] + ", 0");
        else if (t == "i16") o.push_back("  " + u + " = trunc i32 " + ws[0] + " to i16");
        else if (t == "half") {
            auto f = fresh("w"); o.push_back("  " + f + " = bitcast i32 " + ws[0] + " to float");
            o.push_back("  " + u + " = fptrunc float " + f + " to half");
        } else {
            auto lo = fresh("w"); o.push_back("  " + lo + " = zext i32 " + ws[0] + " to i64");
            auto hi = fresh("w"); o.push_back("  " + hi + " = zext i32 " + ws[1] + " to i64");
            auto hs = fresh("w"); o.push_back("  " + hs + " = shl i64 " + hi + ", 32");
            if (t == "i64") o.push_back("  " + u + " = or i64 " + lo + ", " + hs);
            else {
                auto v = fresh("w"); o.push_back("  " + v + " = or i64 " + lo + ", " + hs);
                o.push_back("  " + u + " = bitcast i64 " + v + " to double");
            }
        }
    };
    auto prologue = [&](std::vector<std::string>& o) {
        if (sm66) {
            o.push_back("  %sp.hb = call " + H + " @dx.op.createHandleFromBinding(i32 217, %dx.types.ResBind { i32 0, "
                        "i32 0, i32 " + std::to_string(kSpace) + ", i8 1 }, i32 0, i1 false)  ; CreateHandleFromBinding("
                        "bind,index,nonUniformIndex)");
            o.push_back("  %sp.h = call " + H + " @dx.op.annotateHandle(i32 216, " + H + " %sp.hb, "
                        "%dx.types.ResourceProperties { i32 4107, i32 0 })  ; AnnotateHandle(res,props)  resource: "
                        "RWByteAddressBuffer");
        } else {
            o.push_back("  %sp.h = call " + H + " @dx.op.createHandle(i32 57, i8 1, i32 @UAVID@, i32 0, i1 false)  "
                        "; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)");
        }
        rawLoad(o, "0", "%sp.gx");
        rawLoad(o, "4", "%sp.gy");
        rawLoad(o, "8", "%sp.gz");
        for (int c = 0; c < 3; ++c)
            o.push_back("  %sp.g" + std::to_string(c) + " = call i32 @dx.op.groupId.i32(i32 94, i32 " +
                        std::to_string(c) + ")  ; GroupId(component)");
        o.push_back("  %sp.fi = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)  ; FlattenedThreadIdInGroup()");
        for (const std::string& l : std::vector<std::string>{
                 "  %sp.l1 = mul i32 %sp.g2, %sp.gy", "  %sp.l2 = add i32 %sp.l1, %sp.g1",
                 "  %sp.l3 = mul i32 %sp.l2, %sp.gx", "  %sp.gl = add i32 %sp.l3, %sp.g0",
                 "  %sp.s1 = mul i32 %sp.gl, " + std::to_string(NT), "  %sp.sl = add i32 %sp.s1, %sp.fi",
                 "  %sp.s2 = mul i32 %sp.sl, " + std::to_string(4 * K), "  %sp.tb = add i32 %sp.s2, " + std::to_string(kHdr) })
            o.push_back(l);
        if (G)
            for (const std::string& l : std::vector<std::string>{
                     "  %sp.n1 = mul i32 %sp.gx, %sp.gy", "  %sp.n2 = mul i32 %sp.n1, %sp.gz",
                     "  %sp.n3 = mul i32 %sp.n2, " + std::to_string(4 * K * NT), "  %sp.n4 = add i32 %sp.n3, " + std::to_string(kHdr),
                     "  %sp.n5 = mul i32 %sp.gl, " + std::to_string(4 * G), "  %sp.gb = add i32 %sp.n4, %sp.n5" })
                o.push_back(l);
    };
    auto rebuilt = [&](std::vector<std::string>& o, char Y) {
        for (const auto& b : fn.blocks)
            for (const auto& i : b.instrs)
                if (!i.result.empty() && rebuild[Y].count(i.result)) o.push_back(i.line);
    };
    auto loadAll = [&](std::vector<std::string>& o, char Y) {
        for (const auto& u : loads[Y]) {
            const std::string t = TypeOf(*defs[u]);
            std::vector<std::string> ws;
            for (int d = 0; d < Dwords(t); ++d) {
                const std::string w = fresh("l");
                rawLoad(o, addr(o, field[u] + (unsigned)d), w);
                ws.push_back(w);
            }
            fromWords(o, u, t, ws);
        }
    };
    auto gsCopy = [&](std::vector<std::string>& o, bool out, std::string pred) {
        for (const auto& x : saved) {
            const std::string lab = fresh("c").substr(1);
            if (!x.n) {
                const std::string p = x.g;
                if (out) {
                    auto v = fresh("x"); o.push_back("  " + v + " = load " + x.el + ", " + x.el + " addrspace(3)* " + p + ", align 4");
                    auto w = toWords(o, v, x.el)[0];
                    auto a = fresh("a"); o.push_back("  " + a + " = add i32 %sp.gb, " + std::to_string(4 * x.off));
                    rawStore(o, a, w);
                } else {
                    auto a = fresh("a"); o.push_back("  " + a + " = add i32 %sp.gb, " + std::to_string(4 * x.off));
                    auto w = fresh("l"); rawLoad(o, a, w);
                    auto v = fresh("x"); fromWords(o, v, x.el, { w });
                    o.push_back("  store " + x.el + " " + v + ", " + x.el + " addrspace(3)* " + p + ", align 4");
                }
                continue;
            }
            const std::string start = out ? "0" : "%sp.fi";
            const unsigned step = out ? 1 : NT;
            if (!out) {
                auto c0 = fresh("k"); o.push_back("  " + c0 + " = icmp ult i32 %sp.fi, " + std::to_string(x.n));
                o.push_back("  br i1 " + c0 + ", label %" + lab + ", label %" + lab + ".done");
            } else {
                o.push_back("  br label %" + lab);
            }
            o.push_back("");
            o.push_back(lab + ":");
            auto i = fresh("i"), inx = fresh("i");
            o.push_back("  " + i + " = phi i32 [ " + start + ", %" + pred + " ], [ " + inx + ", %" + lab + " ]");
            auto p = fresh("p");
            o.push_back("  " + p + " = getelementptr " + x.ty + ", " + x.ty + " addrspace(3)* " + x.g + ", i32 0, i32 " + i);
            auto q = fresh("a"); o.push_back("  " + q + " = add i32 " + i + ", " + std::to_string(x.off));
            auto q2 = fresh("a"); o.push_back("  " + q2 + " = shl i32 " + q + ", 2");
            auto a = fresh("a"); o.push_back("  " + a + " = add i32 %sp.gb, " + q2);
            if (out) {
                auto v = fresh("x"); o.push_back("  " + v + " = load " + x.el + ", " + x.el + " addrspace(3)* " + p + ", align 4");
                rawStore(o, a, toWords(o, v, x.el)[0]);
            } else {
                auto w = fresh("l"); rawLoad(o, a, w);
                auto v = fresh("x"); fromWords(o, v, x.el, { w });
                o.push_back("  store " + x.el + " " + v + ", " + x.el + " addrspace(3)* " + p + ", align 4");
            }
            o.push_back("  " + inx + " = add i32 " + i + ", " + std::to_string(step));
            auto c = fresh("k"); o.push_back("  " + c + " = icmp ult i32 " + inx + ", " + std::to_string(x.n));
            o.push_back("  br i1 " + c + ", label %" + lab + ", label %" + lab + ".done");
            o.push_back("");
            o.push_back(lab + ".done:");
            pred = lab + ".done";
        }
        return pred;
    };
    const std::string barrier = "  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)";

    // A path that leaves part A for a return without the query (an early
    // return) joins the query's path at sp.ajoin instead, so every thread of
    // the group takes A's barrier; it leaves the alive flag 0, so Q and B
    // return for it, as the original did.
    std::set<std::string> leave;
    for (const auto& b : fn.blocks) {
        if (R.count(b.label) || b.label == P || Bpart.count(b.label)) continue;
        for (const auto& s : b.Successors()) {
            if (s != P && !Bpart.count(s)) continue;
            const llm::Block* t = fn.FindBlock(s);
            if (!t || t->instrs.size() != 1 || !StartsWith(t->instrs[0].body, "ret void")) {
                res.error = "a path around the query that rejoins after it";
                return res;
            }
            leave.insert(s);
        }
    }
    auto retarget = [&](std::string line) {
        for (const auto& s : leave) {
            const std::string pat = "label " + s;
            for (size_t at = line.find(pat); at != std::string::npos; at = line.find(pat, at)) {
                const size_t e = at + pat.size();
                if (e < line.size() && (std::isalnum((unsigned char)line[e]) || std::strchr("_.$-", line[e]))) {
                    at = e;
                    continue;
                }
                line.replace(at, pat.size(), "label %sp.ajoin");
                at += 15;
            }
        }
        return line;
    };

    // --- A ---
    std::vector<std::string> A;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const llm::Block& b = fn.blocks[bi];
        if (R.count(b.label) && &b != S) continue;
        if (b.label == P || Bpart.count(b.label)) continue;
        if (b.index >= 0) A.push_back(header(b));
        if (bi == 0) {
            prologue(A);
            rawStore(A, "%sp.tb", "0");
        }
        if (&b == S) {
            for (size_t k = 0; k < b.instrs.size() && k < cut1; ++k) A.push_back(b.instrs[k].line);
            A.push_back("  br label %sp.atail");
            A.push_back("");
            A.push_back("sp.atail:");
            rawStore(A, "%sp.tb", "1");
            for (const auto& u : store['A']) {
                auto ws = toWords(A, u, TypeOf(*defs[u]));
                for (size_t d = 0; d < ws.size(); ++d) rawStore(A, addr(A, field[u] + (unsigned)d), ws[d]);
            }
            for (const char* l : { "  br label %sp.ajoin", "", "sp.ajoin:" }) A.push_back(l);
            if (!saved.empty()) {
                A.push_back(barrier);
                gsCopy(A, true, "sp.ajoin");
            }
            A.push_back("  ret void");
        } else {
            for (const auto& i : b.instrs) A.push_back(retarget(i.line));
        }
        A.push_back("");
    }

    // --- Q ---
    std::vector<std::string> Q = { "sp.qentry:" };
    prologue(Q);
    rawLoad(Q, "%sp.tb", "%sp.al");
    for (const char* l : { "  %sp.alc = icmp ne i32 %sp.al, 0", "  br i1 %sp.alc, label %sp.qgo, label %sp.qret", "",
                           "sp.qret:", "  ret void", "", "sp.qgo:" })
        Q.push_back(l);
    loadAll(Q, 'Q');
    rebuilt(Q, 'Q');
    const std::string sl = S->label.empty() ? "sp.s" : S->label.substr(1);
    Q.push_back("  br label %" + sl);
    Q.push_back("");
    Q.push_back(sl + ":");
    for (size_t k = cut1; k < S->instrs.size(); ++k) Q.push_back(S->instrs[k].line);
    Q.push_back("");
    for (const auto& b : fn.blocks)
        if (R.count(b.label) && &b != S) {
            Q.push_back(header(b));
            for (const auto& i : b.instrs) Q.push_back(i.line);
            Q.push_back("");
        }
    Q.push_back(header(*Pb));
    for (const auto& i : Pb->instrs) if (i.isPhi) Q.push_back(i.line);
    for (const auto& u : store['Q']) {
        auto ws = toWords(Q, u, TypeOf(*defs[u]));
        for (size_t d = 0; d < ws.size(); ++d) rawStore(Q, addr(Q, field[u] + (unsigned)d), ws[d]);
    }
    Q.push_back("  ret void");
    Q.push_back("");

    // --- B ---
    std::vector<std::string> Bt = { "sp.bentry:" };
    prologue(Bt);
    std::string pred = "sp.bentry";
    if (!saved.empty()) {
        pred = gsCopy(Bt, false, pred);
        Bt.push_back(barrier);
    }
    rawLoad(Bt, "%sp.tb", "%sp.al");
    for (const char* l : { "  %sp.alc = icmp ne i32 %sp.al, 0", "  br i1 %sp.alc, label %sp.bgo, label %sp.bret", "",
                           "sp.bret:", "  ret void", "", "sp.bgo:" })
        Bt.push_back(l);
    loadAll(Bt, 'B');
    rebuilt(Bt, 'B');
    Bt.push_back("  br label %" + P.substr(1));
    Bt.push_back("");
    Bt.push_back(header(*Pb));
    for (const auto& i : Pb->instrs) if (!i.isPhi) Bt.push_back(i.line);
    Bt.push_back("");
    for (const auto& b : fn.blocks)
        if (Bpart.count(b.label) && b.label != P) {
            Bt.push_back(header(b));
            for (const auto& i : b.instrs) Bt.push_back(i.line);
            Bt.push_back("");
        }

    res.K = K;
    res.G = G;
    for (int c = 0; c < 3; ++c) res.threads[c] = nt[c];
    res.stored = (unsigned)(store['A'].size() + store['Q'].size());
    for (const auto& x : saved) res.saved.push_back(x.g);
    if (!MakeModule(text, fn, A, sm66, &res.a, &res.error) || !MakeModule(text, fn, Q, sm66, &res.q, &res.error) ||
        !MakeModule(text, fn, Bt, sm66, &res.b, &res.error))
        return res;
    res.ok = true;
    return res;
}

}  // namespace rq
