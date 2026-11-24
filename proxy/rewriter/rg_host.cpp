// RayQuery in an APPLICATION's raygen (0.80.0): rghost.py in phase5/rewriter
// says what this does and why; this is its port, held byte-identical.
#include "rg_host.h"

#include <algorithm>
#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "geom_index.h"
#include "ll_model.h"

namespace rq {
namespace {

const long long kRt11Flag = 0x2000000;

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> out;
    size_t at = 0;
    while (true) {
        size_t c = s.find(',', at);
        out.push_back(Trim(s.substr(at, c == std::string::npos ? std::string::npos : c - at)));
        if (c == std::string::npos) break;
        at = c + 1;
    }
    return out;
}

std::string JoinComma(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += ", "; o += v[i]; }
    return o;
}

std::string Join(const std::vector<std::string>& v) {
    std::string o;
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += "\n"; o += v[i]; }
    return o;
}

// line index and body of every `!N = !{...}`
std::map<int, std::pair<size_t, std::string>> Md(const std::vector<std::string>& lines) {
    static const std::regex kNode(R"(^!(\d+) = (?:distinct )?!\{(.*)\}\s*$)");
    std::map<int, std::pair<size_t, std::string>> md;
    std::smatch m;
    for (size_t n = 0; n < lines.size(); ++n)
        if (std::regex_match(lines[n], m, kNode)) md[std::stoi(m[1].str())] = { n, m[2].str() };
    return md;
}

bool StartsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

std::vector<std::string> DropUnusedDeclarations(const std::vector<std::string>& lines) {
    std::string body;
    for (const auto& l : lines)
        if (!StartsWith(l, "declare ")) { body += l; body += "\n"; }
    if (!body.empty()) body.pop_back();
    static const std::regex kDecl(R"RX(^declare [^@]*@("[^"]+"|[\w.$-]+)\()RX");
    std::vector<std::string> out;
    std::smatch m;
    for (const auto& l : lines) {
        if (std::regex_search(l, m, kDecl) && body.find("@" + m[1].str() + "(") == std::string::npos) {
            if (!out.empty() && StartsWith(out.back(), "; Function Attrs:")) out.pop_back();
            continue;
        }
        out.push_back(l);
    }
    return out;
}

std::vector<std::string> DropUnusedResources(std::vector<std::string> lines) {
    std::string body;
    bool first = true;
    for (const auto& l : lines)
        if (StartsWith(l, "  ")) { if (!first) body += "\n"; body += l; first = false; }
    auto used = [&](const std::string& g) {
        return body.find(g + ",") != std::string::npos || body.find(g + " ") != std::string::npos;
    };
    auto md = Md(lines);
    std::set<std::string> gone;
    std::vector<bool> dead(lines.size(), false);
    static const std::regex kGlobal(R"RX(^(@"[^"]+"|@[\w.$-]+) = external constant )RX");
    std::smatch m;
    for (size_t n = 0; n < lines.size(); ++n)
        if (std::regex_search(lines[n], m, kGlobal) && !used(m[1].str())) {
            gone.insert(m[1].str());
            dead[n] = true;
        }
    if (gone.empty()) return lines;
    int top = -1;
    for (size_t n = 0; n < lines.size(); ++n)
        if (!dead[n] && StartsWith(lines[n], "!dx.resources = ")) {
            static const std::regex kId(R"(!(\d+))");
            std::smatch im;
            if (std::regex_search(lines[n], im, kId)) top = std::stoi(im[1].str());
            break;
        }
    if (top >= 0) {
        auto groups = SplitComma(md[top].second);
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const std::string g = groups[gi];
            if (g == "null") continue;
            const auto gn = md[std::stoi(g.substr(1))];
            const auto ids = SplitComma(gn.second);
            std::vector<std::string> keep;
            for (const auto& x : ids) {
                const std::string& rb = md[std::stoi(x.substr(1))].second;
                bool hit = false;
                for (const auto& r : gone)
                    if (rb.find(r + ",") != std::string::npos || rb.find(r + " to ") != std::string::npos) hit = true;
                if (!hit) keep.push_back(x);
            }
            static const std::regex kFirst(R"(^i32 \d+)");
            for (size_t k = 0; k < keep.size(); ++k) {
                const auto& rr = md[std::stoi(keep[k].substr(1))];
                lines[rr.first] = "!" + keep[k].substr(1) + " = !{" +
                                  std::regex_replace(rr.second, kFirst, "i32 " + std::to_string(k),
                                                     std::regex_constants::format_first_only) + "}";
            }
            for (const auto& x : ids)
                if (std::find(keep.begin(), keep.end(), x) == keep.end())
                    dead[md[std::stoi(x.substr(1))].first] = true;
            if (!keep.empty()) {
                lines[gn.first] = "!" + g.substr(1) + " = !{" + JoinComma(keep) + "}";
            } else {
                dead[gn.first] = true;
                groups[gi] = "null";
            }
        }
        lines[md[top].first] = "!" + std::to_string(top) + " = !{" + JoinComma(groups) + "}";
    }
    std::vector<std::string> out;
    for (size_t n = 0; n < lines.size(); ++n) if (!dead[n]) out.push_back(lines[n]);
    return out;
}

std::string FnRef(const std::string& name) {
    static const std::regex kPlain(R"(^[\w.$-]+$)");
    return std::regex_match(name, kPlain) ? "@" + name : "@\"" + name + "\"";
}

const llm::Function* FunctionOf(const llm::Module& m, const std::string& raygen) {
    for (const auto& f : m.functions)
        if (ExportName(f.name) == raygen) return &f;
    return nullptr;
}

}  // namespace

Exports RaygenExports(const std::string& raygen) {
    const std::string pre = "dxr11_rq_" + raygen + "_";
    Exports e;
    e.raygen = raygen;
    e.anyhit = pre + "AnyHit";
    e.closesthit = pre + "ClosestHit";
    e.miss = pre + "Miss";
    e.intersection = pre + "Isect";
    e.closesthitproc = pre + "ClosestHitProc";
    e.anyhitnull = pre + "AnyHitNull";
    e.isectnull = pre + "IsectNull";
    e.anyhitproc = pre + "AnyHitProc";
    return e;
}

bool RaygenAsCs(const std::string& in, const std::string& raygen, std::string* out,
                std::string* why) {
    std::string text = in;
    static const std::regex kAppPayload(R"(^%struct\.Payload = type )");
    {
        bool has = false;
        for (const auto& l : llm::SplitLines(text)) if (std::regex_search(l, kAppPayload)) has = true;
        if (has) {
            static const std::regex kName(R"(struct\.Payload(?![\w.]))");
            text = std::regex_replace(text, kName, "struct.Payload.app");
        }
    }
    llm::Module m(text);
    const llm::Function* fn = FunctionOf(m, raygen);
    if (!fn) { *why = "the library exports no function " + raygen; return false; }
    std::vector<std::string> lines = llm::SplitLines(text);
    std::vector<bool> keep(lines.size(), true);
    for (const auto& f : m.functions) {
        if (&f == fn) continue;
        for (int n = f.index; n <= f.endIndex; ++n) keep[n] = false;
        if (f.index > 0 && StartsWith(lines[f.index - 1], "; Function Attrs:")) keep[f.index - 1] = false;
    }
    {
        std::vector<std::string> k;
        for (size_t n = 0; n < lines.size(); ++n) if (keep[n]) k.push_back(lines[n]);
        lines.swap(k);
    }
    const std::string old = FnRef(fn->name);
    for (auto& l : lines)
        if (StartsWith(l, "define ")) {
            size_t at = l.find(old + "(");
            if (at != std::string::npos) l.replace(at, old.size() + 1, "@main(");
        }
    auto md = Md(lines);
    std::string ep;
    for (const auto& l : lines) if (StartsWith(l, "!dx.entryPoints = ")) { ep = l; break; }
    if (ep.empty()) { *why = "the library has no entry points"; return false; }
    const size_t ob = ep.find('{'), cb = ep.rfind('}');
    int nullId = -1;
    for (const auto& x : SplitComma(ep.substr(ob + 1, cb - ob - 1))) {
        const int id = std::stoi(x.substr(1));
        if (StartsWith(md[id].second, "null")) { nullId = id; break; }
    }
    if (nullId < 0) { *why = "the library declares no module entry"; return false; }
    const auto fields = SplitComma(md[nullId].second);
    const std::string res = fields[3], props = fields[4];
    long long flags = 0;
    if (props != "null") {
        static const std::regex kFlags(R"(i32 0, i64 (-?\d+))");
        std::smatch fm;
        const std::string pb = md[std::stoi(props.substr(1))].second;
        if (std::regex_search(pb, fm, kFlags)) flags = std::stoll(fm[1].str());
    }
    const int top = md.rbegin()->first + 1;
    static const std::regex kNodeStart(R"(^!\d+ = )");
    static const std::regex kFnRef(R"(\)\* @)");
    std::vector<std::string> o;
    static const std::regex kSm(R"RX(^(!\d+) = !\{!"lib", (i32 \d+, i32 \d+)\}\s*$)RX");
    for (auto l : lines) {
        if (StartsWith(l, "!dx.typeAnnotations = ")) continue;
        if (std::regex_search(l, kNodeStart) && std::regex_search(l, kFnRef)) continue;
        if (StartsWith(l, "!dx.entryPoints = ")) l = "!dx.entryPoints = !{!" + std::to_string(top + 2) + "}";
        std::smatch sm;
        if (std::regex_match(l, sm, kSm)) l = sm[1].str() + " = !{!\"cs\", " + sm[2].str() + "}";
        o.push_back(l);
    }
    o = DropUnusedDeclarations(o);
    o = DropUnusedResources(o);
    std::string r = Join(o);
    while (!r.empty() && r.back() == '\n') r.pop_back();
    r += "\n!" + std::to_string(top) + " = !{i32 1, i32 1, i32 1}\n";
    r += "!" + std::to_string(top + 1) + " = !{i32 0, i64 " + std::to_string(flags) + ", i32 4, !" +
         std::to_string(top) + "}\n";
    r += "!" + std::to_string(top + 2) + " = !{void ()* @main, !\"main\", null, " + res + ", !" +
         std::to_string(top + 1) + "}\n";
    *out = r;
    return true;
}

bool StripFunction(const std::string& in, const std::string& raygen, std::string* out,
                   bool* empty, std::string* why) {
    *empty = false;
    llm::Module m(in);
    const llm::Function* fn = FunctionOf(m, raygen);
    if (!fn) { *why = "the library exports no function " + raygen; return false; }
    std::vector<std::string> lines = llm::SplitLines(in);
    std::vector<bool> keep(lines.size(), true);
    for (int n = fn->index; n <= fn->endIndex; ++n) keep[n] = false;
    if (fn->index > 0 && StartsWith(lines[fn->index - 1], "; Function Attrs:")) keep[fn->index - 1] = false;
    {
        std::vector<std::string> k;
        for (size_t n = 0; n < lines.size(); ++n) if (keep[n]) k.push_back(lines[n]);
        lines.swap(k);
    }
    const std::string ref = FnRef(fn->name);
    auto md = Md(lines);
    size_t ep = 0;
    for (size_t n = 0; n < lines.size(); ++n) if (StartsWith(lines[n], "!dx.entryPoints = ")) { ep = n; break; }
    const size_t ob = lines[ep].find('{'), cb = lines[ep].rfind('}');
    const auto ids = SplitComma(lines[ep].substr(ob + 1, cb - ob - 1));
    std::vector<std::string> mine, rest;
    for (const auto& x : ids)
        (md[std::stoi(x.substr(1))].second.find(ref) != std::string::npos ? mine : rest).push_back(x);
    bool any = false;
    for (const auto& x : rest) if (!StartsWith(md[std::stoi(x.substr(1))].second, "null")) any = true;
    if (!any) { *empty = true; return true; }
    lines[ep] = "!dx.entryPoints = !{" + JoinComma(rest) + "}";
    std::vector<bool> dead(lines.size(), false);
    for (const auto& x : mine) dead[md[std::stoi(x.substr(1))].first] = true;
    static const std::regex kAnn(R"(^!\d+ = !\{i32 1, )");
    std::string esc;
    for (char c : ref) {
        if (std::string(".^$|()[]{}*+?\\").find(c) != std::string::npos) esc += '\\';
        esc += c;
    }
    // `void (...)* @f, !n`: parameter types inside for a closest-hit or miss (0.82.0).
    const std::regex kPair(", void \\([^)]*\\)\\* " + esc + ", !\\d+");
    for (size_t n = 0; n < lines.size(); ++n) {
        if (dead[n] || !std::regex_search(lines[n], kAnn)) continue;
        lines[n] = std::regex_replace(lines[n], kPair, "");
    }
    {
        std::vector<std::string> k;
        for (size_t n = 0; n < lines.size(); ++n) if (!dead[n]) k.push_back(lines[n]);
        lines.swap(k);
    }
    lines = DropUnusedDeclarations(lines);
    lines = DropUnusedResources(lines);
    static const std::regex kRqDecl(R"(^declare .*@dx\.op\.rayQuery_)");
    bool rq = false;
    for (const auto& l : lines) if (std::regex_search(l, kRqDecl)) rq = true;
    if (!rq) {
        static const std::regex kFl(R"(^(!\d+ = !\{i32 0, i64 )(-?\d+)(.*)$)");
        for (auto& l : lines) {
            std::smatch fm;
            if (std::regex_match(l, fm, kFl))
                l = fm[1].str() + std::to_string(std::stoll(fm[2].str()) & ~kRt11Flag) + fm[3].str();
        }
    }
    *out = Join(lines);
    return true;
}

bool FixFlags(const std::string& in, const std::string& said, std::string* out) {
    static const std::regex kSaid(R"(Flags declared=(\d+), actual=(\d+))");
    std::smatch m;
    if (!std::regex_search(said, m, kSaid)) return false;
    const std::regex kLine("^(!\\d+ = !\\{i32 0, i64 )" + m[1].str() + "(\\}.*)$");
    auto lines = llm::SplitLines(in);
    for (auto& l : lines) {
        std::smatch fm;
        if (std::regex_match(l, fm, kLine)) {
            l = fm[1].str() + m[2].str() + fm[2].str();
            *out = Join(lines);
            return true;
        }
    }
    return false;
}

// The lowering's own TraceRay takes its miss index at run time (0.81.0):
// a GeometryIndex() read, which LowerGeometryIndex turns into the constant at
// b0, space kGeomIndexSpace. shim_miss in rghost.py.
std::string ShimMiss(const std::string& in) {
    static const std::regex kTrace(
        R"RX(^(\s*)call void @dx\.op\.traceRay\.struct\.Payload\(i32 157, (%dx\.types\.Handle [^,]+, i32 [^,]+, i32 [^,]+, i32 [^,]+, i32 [^,]+), i32 ([^,]+), (.*)$)RX");
    std::vector<std::string> out;
    int n = 0;
    std::smatch m;
    for (const auto& l : llm::SplitLines(in)) {
        if (std::regex_match(l, m, kTrace)) {
            const std::string v = "%rgq.ms" + std::to_string(n++);
            out.push_back(m[1].str() + v + " = call i32 @dx.op.geometryIndex.i32(i32 213)");
            out.push_back(m[1].str() + "call void @dx.op.traceRay.struct.Payload(i32 157, " + m[2].str() +
                          ", i32 " + v + ", " + m[4].str());
            continue;
        }
        out.push_back(l);
    }
    return Join(out);
}

bool PointRaygenQueries(const std::string& in, unsigned queries, unsigned mult,
                        const std::vector<unsigned>& slotOf, unsigned slots,
                        const std::vector<unsigned>& caps, std::string* out, std::string* why,
                        unsigned base, unsigned total) {
    std::string gi;
    std::vector<std::string> fns;
    if (!LowerGeometryIndex(ShimMiss(in), &gi, &fns, why)) return false;
    std::vector<std::pair<unsigned, unsigned>> pairs;
    if (!total) total = queries;
    for (unsigned j = 0; j < total; ++j) pairs.push_back({ 15u, 2u + j });
    for (unsigned k = 0; k < queries; ++k) pairs[base + k] = { k, mult };
    int calls = 0;
    if (!RetraceToShimScene(gi, pairs, 0, slotOf, slots, caps, out, &calls, why, "struct.Payload"))
        return false;
    if (!calls) {
        *why = "the lowered raygen has no TraceRay of its own to point at the shim's scene";
        return false;
    }
    return true;
}

// A closest-hit or miss hosted as a compute shader, given back its own
// parameters, function type, entry properties and annotations (0.82.0).
// restore_function in rghost.py.
bool RestoreFunction(const std::string& lowered, const std::string& cs, const std::string& orig,
                     const std::string& exportName, std::string* out, std::string* why) {
    static const std::regex kMain(R"RX(^define void @main\((.*)\) #\d+ \{$)RX");
    std::string params;
    {
        std::smatch m;
        for (const auto& l : llm::SplitLines(cs))
            if (std::regex_match(l, m, kMain)) { params = m[1].str(); break; }
    }
    if (Trim(params).empty()) { *out = lowered; return true; }
    std::string ftype = "void (";
    {
        const auto ps = llm::SplitArgs(params);
        for (size_t i = 0; i < ps.size(); ++i) {
            const std::string p = Trim(ps[i]);
            ftype += (i ? ", " : "") + p.substr(0, p.find(' '));
        }
        ftype += ")*";
    }
    const std::string* name = nullptr;
    std::string fnName;
    {
        llm::Module m(orig);
        for (const auto& f : m.functions)
            if (ExportName(f.name) == exportName) { fnName = f.name; name = &fnName; break; }
    }
    if (!name) { *why = "the library exports no function " + exportName; return false; }
    const std::string ref = FnRef(fnName);
    std::string props, ann;
    for (const auto& kv : Md(llm::SplitLines(orig))) {
        const auto f = llm::SplitArgs(kv.second.second);
        auto ends = [&](const std::string& s) {
            return s.size() > ref.size() && s.compare(s.size() - ref.size() - 1, std::string::npos, " " + ref) == 0;
        };
        if (f.size() >= 5 && ends(f[0]) && f[1].rfind("!\"", 0) == 0) props = f[4];
        if (!f.empty() && f[0] == "i32 1")
            for (size_t k = 1; k + 1 < f.size(); k += 2)
                if (ends(f[k])) ann = f[k + 1];
    }
    if (props.empty() || ann.empty()) {
        *why = "the entry or annotation of " + exportName + " is not where it was looked for";
        return false;
    }
    const std::string me = FnRef(exportName);
    const std::string def = "define void " + me + "() ";
    auto esc = [](const std::string& s) {
        static const std::regex kSpecial(R"([.^$|()\[\]{}*+?\\])");
        return std::regex_replace(s, kSpecial, "\\$&");
    };
    const std::regex kEntry("(" + esc(ftype) + " " + esc(me) + ", !\"[^\"]*\", null, null, )!\\d+\\}");
    const std::regex kAnn("void \\(\\)\\* " + esc(me) + ", !\\d+");
    std::vector<std::string> lines;
    for (std::string l : llm::SplitLines(lowered)) {
        if (l.rfind(def, 0) == 0) {
            l = "define void " + me + "(" + params + ") " + l.substr(def.size());
        } else if (!l.empty() && l[0] == '!') {
            const std::string from = "void ()* " + me + ", !\"";
            for (size_t at = l.find(from); at != std::string::npos; at = l.find(from, at + 1))
                l.replace(at, from.size(), ftype + " " + me + ", !\"");
            l = std::regex_replace(l, kEntry, "$1" + props + "}");
            l = std::regex_replace(l, kAnn, ftype + " " + me + ", " + ann);
        }
        lines.push_back(l);
    }
    *out = Join(lines);
    return true;
}

}  // namespace rq
