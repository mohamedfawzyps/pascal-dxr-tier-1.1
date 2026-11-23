#include "rq_lower.h"

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

namespace rq {
namespace {

const char* kPayload = "%struct.Payload";
const char* kAttrs = "%struct.BuiltInTriangleIntersectionAttributes";

// One payload shape carrying every committed accessor the whitelist supports.
// Tailoring it per shader would save a few bytes and cost a lot of ways to get
// the offsets wrong; the brief puts correctness well ahead of that.
//   0 float t | 1 <2 x float> bary | 2 i32 hit | 3 inst | 4 prim | 5 geom
//   0 t | 1 bary | 2 hit | 3 inst | 4 prim | 5 unused | 6 instanceID
//   7 hitKind | 8 worldToObject, 12 floats | 9 aborted
//
// The LAYOUT is fixed even though the matrix is only sometimes read: variable
// offsets across two implementations is a good way to get one of them subtly
// wrong. The per-invocation COST is what is made conditional instead.
const char* kPayloadType =
    "{ float, <2 x float>, i32, i32, i32, i32, i32, i32, [12 x float], i32, i32 }";
const int kPayloadBytes = 92;
// DRIVER FACTS, 0.67.0, GTX 1070: a payload of exactly 124 bytes whose last
// dword an any-hit reads crashes the driver inside CreateStateObject; one of
// 128 bytes builds and removes the device when dispatched. 120 and 132 to 260
// are fine. So 124 and 128 get unused fields up to 132. See _payload_pad in
// lower.py.
const int kDriverSafePayloadBytes = 132;
int PayloadBytesFor(bool multi, int carried) {
    const int n = kPayloadBytes + 4 * ((multi ? 1 : 0) + carried);
    return (n == 124 || n == 128) ? kDriverSafePayloadBytes : n;
}
// Values the loop body reads from before it and the hit shader cannot rebuild
// ride after the fixed fields, 4 bytes each. See _plan_carry in lower.py.
const int kCarryField0 = 11;
// 2024 since 0.76.0 (16 before): the payload then stays at or under 8192 bytes,
// the largest the GTX 1070 was measured right at (every 4 bytes from 132 to
// 1032, and 1500, 2048, 3072, 4096 and 8192, against WARP). See lower.py.
const int kMaxCarry = 2024;
// With several queries, field 11 says which one a trace serves.
const int kQidField = 11;

// Carried in other shapes (0.76.0): see _OTHER_CARRY in lower.py.
bool CarryType(const std::string& ty) {
    return ty == "i32" || ty == "float" || ty == "i1" || ty == "half" || ty == "i16" ||
           ty == "i64" || ty == "double";
}
bool OtherCarry(const std::string& ty) {
    return ty == "half" || ty == "i16" || ty == "i64" || ty == "double";
}
int Words(const std::string& ty) { return (ty == "i64" || ty == "double") ? 2 : 1; }
std::vector<std::string> Slots(const std::string& ty) {
    if (ty == "float" || ty == "half") return { "float" };
    if (ty == "i64" || ty == "double") return { "i32", "i32" };
    return { "i32" };
}
int CarryWords(const std::map<std::string, std::pair<std::string, int>>& f) {
    int n = 0;
    for (const auto& kv : f) n += Words(kv.second.first);
    return n;
}
const char* kPayloadName = "%struct.Payload";
std::vector<std::string> CarryStore(const std::string& ty, const std::string& v, int f,
                                    const std::string& p0, const std::string& basep) {
    const std::string t = p0 + ".w";
    if (ty == "half")
        return { "  " + t + " = fpext half " + v + " to float",
                 "  store float " + t + ", float* " + p0 + ", align 4" };
    if (ty == "i16")
        return { "  " + t + " = zext i16 " + v + " to i32",
                 "  store i32 " + t + ", i32* " + p0 + ", align 4" };
    const std::string p1 = p0 + ".h";
    std::vector<std::string> out{ "  " + p1 + " = getelementptr inbounds " + kPayloadName + ", " +
                                  kPayloadName + "* " + basep + ", i32 0, i32 " + std::to_string(f + 1) };
    if (ty == "i64") {
        out.push_back("  " + t + ".l = trunc i64 " + v + " to i32");
        out.push_back("  " + t + ".s = lshr i64 " + v + ", 32");
        out.push_back("  " + t + ".u = trunc i64 " + t + ".s to i32");
    } else {
        out.push_back("  " + t + ".d = call %dx.types.splitdouble @dx.op.splitDouble.f64(i32 102, double " +
                      v + ")  ; SplitDouble(value)");
        out.push_back("  " + t + ".l = extractvalue %dx.types.splitdouble " + t + ".d, 0");
        out.push_back("  " + t + ".u = extractvalue %dx.types.splitdouble " + t + ".d, 1");
    }
    // DXR_TIER11_CARRY_POISON=1 (0.76.0): the high word of a carried i64 or
    // double is dropped. The carried-types case must diverge (C++ only).
    static const bool carryPoison = std::getenv("DXR_TIER11_CARRY_POISON") != nullptr;
    out.push_back("  store i32 " + t + ".l, i32* " + p0 + ", align 4");
    out.push_back("  store i32 " + (carryPoison ? std::string("0") : t + ".u") + ", i32* " + p1 + ", align 4");
    return out;
}
std::vector<std::string> CarryLoad(const std::string& ty, const std::string& name, int f,
                                   const std::string& p0, const std::string& basep) {
    const std::string t = p0 + ".w";
    if (ty == "half")
        return { "  " + t + " = load float, float* " + p0 + ", align 4",
                 "  " + name + " = fptrunc float " + t + " to half" };
    if (ty == "i16")
        return { "  " + t + " = load i32, i32* " + p0 + ", align 4",
                 "  " + name + " = trunc i32 " + t + " to i16" };
    const std::string p1 = p0 + ".h";
    std::vector<std::string> out{
        "  " + p1 + " = getelementptr inbounds " + kPayloadName + ", " + kPayloadName + "* " + basep +
            ", i32 0, i32 " + std::to_string(f + 1),
        "  " + t + ".l = load i32, i32* " + p0 + ", align 4",
        "  " + t + ".u = load i32, i32* " + p1 + ", align 4" };
    if (ty == "i64") {
        out.push_back("  " + t + ".a = zext i32 " + t + ".l to i64");
        out.push_back("  " + t + ".b = zext i32 " + t + ".u to i64");
        out.push_back("  " + t + ".c = shl i64 " + t + ".b, 32");
        out.push_back("  " + name + " = or i64 " + t + ".c, " + t + ".a");
    } else {
        out.push_back("  " + name + " = call double @dx.op.makeDouble.f64(i32 101, i32 " + t + ".l, i32 " +
                      t + ".u)  ; MakeDouble(lo,hi)");
    }
    return out;
}

// The record's own two numbers, and how the generated shaders reach them.
//
// Not a dx.op call like everything in kChSource: there is no intrinsic for
// either, which is exactly why they were refused. They come out of a RAW
// BUFFER bound by a LOCAL root signature root SRV at t0, space1, so the value
// is per hit-group-record and the shim chose it when it built the table.
//
// In a Shader Model 6.6 module the handle MUST be annotated: a hit shader
// reaching a LOCAL root signature resource through a bare createHandleForLib
// is what crashed the Pascal driver, cbuffer or raw buffer alike, and
// annotating it was 0 of 15 for both. See lower.py and
// phase5/cases/driver-crash/.
const char* kRecordType = "%rq_record";
const char* kRecordGlobal = "@rq_record";
const char* kRecordHandleFn = "dx.op.createHandleForLib.rq_record";
const char* kResRet = "%dx.types.ResRet.i32";
// Shader flag for raw and structured buffers, required once a module reads one.
const unsigned long long kRawBufferFlag = 0x10ull;
// HitKind() is an integer; the RayQuery form is a bool.
const int kHitKindFront = 254;

struct Field { int idx; const char* ty; int align; };

// Emitted per use rather than hoisted. The handle and the load are pure and
// the driver folds the duplicates; hoisting them by hand would mean finding a
// place that dominates every use, which is a CFG question this pass has no
// reason to ask.
std::vector<std::string> RecordRead(const std::string& tag, int word,
                                    const std::string& result, bool sm66) {
    const std::string t = std::string(kRecordType);
    std::vector<std::string> out;
    if (sm66) {
        const std::string h = "%dx.types.Handle";
        out.push_back("  %rq.cbv" + tag + " = load " + h + ", " + h + "* " +
                      kRecordGlobal + ", align 4");
        out.push_back("  %rq.cbh" + tag + ".lib = call " + h +
                      " @dx.op.createHandleForLib.dx.types.Handle(i32 160, " + h +
                      " %rq.cbv" + tag + ")  ; CreateHandleForLib(Resource)");
        out.push_back("  %rq.cbh" + tag + " = call " + h + " @dx.op.annotateHandle(i32 216, " +
                      h + " %rq.cbh" + tag + ".lib, %dx.types.ResourceProperties "
                      "{ i32 11, i32 0 })  ; AnnotateHandle(res,props)  resource: ByteAddressBuffer");
    } else {
        out.push_back("  %rq.cbv" + tag + " = load " + t + ", " + t + "* " + kRecordGlobal +
                      ", align 4");
        out.push_back("  %rq.cbh" + tag + " = call %dx.types.Handle @" + kRecordHandleFn +
                      "(i32 160, " + t + " %rq.cbv" + tag + ")  ; CreateHandleForLib(Resource)");
    }
    // Word 2, whether the record's geometry is opaque (0.66.0), is a load of
    // its own at offset 8.
    const std::vector<std::string> rest = {
        "  %rq.cbr" + tag + " = call " + kResRet +
            " @dx.op.rawBufferLoad.i32(i32 139, %dx.types.Handle %rq.cbh" + tag +
            (word == 2 ? ", i32 8, i32 undef, i8 1, i32 4)" : ", i32 0, i32 undef, i8 3, i32 4)") +
            "  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)",
        "  " + result + " = extractvalue " + kResRet + " %rq.cbr" + tag + ", " +
            std::to_string(word == 2 ? 0 : word),
    };
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

// Shader Model 6.6 or later, where every handle is annotated.
bool IsSm66(const std::string& text) {
    static const std::regex kSm(R"RX(^!\d+ = !\{!"(?:cs|lib)", i32 (\d+), i32 (\d+)\})RX");
    for (const auto& line : llm::SplitLines(text)) {
        std::smatch mm;
        if (std::regex_search(line, mm, kSm)) {
            const int maj = std::stoi(mm[1].str()), min = std::stoi(mm[2].str());
            return maj > 6 || (maj == 6 && min >= 6);
        }
    }
    return false;
}

std::optional<Field> PayloadField(int op) {
    switch (op) {
        case kCommittedRayT:            return Field{ 0, "float", 8 };
        case kCommittedBary:            return Field{ 1, "<2 x float>", 4 };
        case kCommittedStatus:          return Field{ 2, "i32", 4 };
        case kCommittedInstanceIndex:   return Field{ 3, "i32", 4 };
        case kCommittedPrimitiveIndex:  return Field{ 4, "i32", 4 };
        case kCommittedInstanceID:      return Field{ 6, "i32", 4 };
        case kCommittedFrontFace:       return Field{ 7, "i32", 4 };
        case kCommittedWorldToObject:   return Field{ 8, "[12 x float]", 4 };
        // 9 is the abort flag. 10 came with the record constants; the geometry
        // index needed no new field because slot 5 had been reserved for it all
        // along and left unused while the accessor was refused.
        case kCommittedGeometryIndex:   return Field{ 5, "i32", 4 };
        case kCommittedInstanceContrib: return Field{ 10, "i32", 4 };
        default:                        return std::nullopt;
    }
}

// Which dx.op gives each field in a DXR 1.0 closest-hit, read off DXC output.
// Field 5, geometry index, is deliberately absent: emitting dx.op.geometryIndex
// would set shader flag 0x2000000 and the driver would refuse the state object.
const std::pair<int, const char*> kChSource[] = {
    { 3, "call i32 @dx.op.instanceIndex.i32(i32 142)" },
    { 4, "call i32 @dx.op.primitiveIndex.i32(i32 161)" },
    { 6, "call i32 @dx.op.instanceID.i32(i32 141)" },
    { 7, "call i32 @dx.op.hitKind.i32(i32 143)" },
};

// RayQuery -> DXR 1.0, for accessors read in the ANY-HIT shader. Measured
// from DXC output on both sides. The operand shape is identical apart from the
// query handle, which simply goes away.
struct CandMap { int op; const char* ty; const char* callee; int dxop; int extra; };
const CandMap kCandidateMap[] = {
    { kCandidateInstanceIndex,  "i32",   "dx.op.instanceIndex.i32",     142, 0 },
    { kCandidateInstanceID,     "i32",   "dx.op.instanceID.i32",        141, 0 },
    { kCandidatePrimitiveIndex, "i32",   "dx.op.primitiveIndex.i32",    161, 0 },
    { kCandidateRayT,           "float", "dx.op.rayTCurrent.f32",       154, 0 },
    { kCandidateObjectRayOrigin,    "float", "dx.op.objectRayOrigin.f32",    149, 1 },
    { kCandidateObjectRayDirection, "float", "dx.op.objectRayDirection.f32", 150, 1 },
    { kCandidateWorldToObject,      "float", "dx.op.worldToObject.f32",      152, 2 },
    // Legal in an any-hit and an intersection shader, NOT in a raygen.
    // A raygen read is folded to a constant instead, see EditRayFlags.
    { kRayFlags,                    "i32",   "dx.op.rayFlags.i32",           144, 0 },
};
const CandMap* FindCand(int op) {
    for (const auto& c : kCandidateMap) if (c.op == op) return &c;
    return nullptr;
}

// Shader Model 6.6 reaches a resource by BINDING, not by a range index.
//
// Unreal's RayQuery shaders are cs_6_6 and every one of them does this. The
// rewriter was built against 6.5, and that single difference caused 124 of the
// 157 refusals a real Unreal run produced.
//
//   cs_6_6   createHandleFromBinding (217) -> annotateHandle (216)
//   lib_6_6  createHandleForLib      (160) -> annotateHandle (216)
//
// So the conversion is 217 -> 160, and the annotateHandle after it is kept
// untouched. Read off DXC, see phase5/cases/reference/lib_sm66_binding_ref.hlsl.
const int kBindHandle = 217;
const int kAnnotateHandle = 216;
const int kHeapHandle = 218;
// The library form's global is a HANDLE, not the resource type, and the
// overload is named after the handle type too. That is the part that cannot be
// guessed from the 6.5 path, where both are the resource type.
const char kHandleType[] = "%dx.types.Handle";

// The global a Shader Model 6.6 binding lowers to: a handle, or for a
// resource ARRAY an array of handles, `[4 x %dx.types.Handle]` (read off DXC,
// phase5/cases/reference/lib_sm66_dynarray_ref.hlsl).
// Where the part of the trace text that runs at the search's start begins:
// the TraceRay, or the Abort() flag's read before it (0.75.0).
size_t TraceCut(const std::string& joined, const std::string& pfx) {
    const size_t k = joined.find("  %" + pfx + ".rav = load");
    return k != std::string::npos ? k : joined.find("  call void @dx.op.traceRay.");
}

std::string BindingGty(const std::string& gty, const std::string& elem) {
    static const std::regex kN(R"(^\[(\d+) x )");
    std::smatch m;
    if (gty != elem && std::regex_search(gty, m, kN)) return "[" + m[1].str() + " x " + kHandleType + "]";
    return kHandleType;
}

// 0x2000000 is the raytracing tier 1.1 shader flag: measured on GeometryIndex,
// which sets it and makes CreateStateObject refuse the library on a GTX 1070.
// Every RayQuery op is gone after lowering, so the flag must go with them.
const unsigned long long kRt11ShaderFlag = 0x2000000ull;

struct ResRec {
    int nid = 0;
    std::string gty;      // global type, "[4 x %..]" for an array
    std::string elem;     // element type
    bool isArray = false;
    std::string name;
};

struct GlobalInfo {
    int cls = 0;
    int nid = 0;
    std::string sym;
    std::string gty;
    std::string elem;
    std::string name;
};

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> SplitTop(const std::string& s) { return llm::SplitArgs(s); }

std::map<int, std::string> Metadata(const std::string& text) {
    // `distinct` counts. A [branch] or [loop] hint makes DXC emit
    // `!16 = distinct !{!16, !"dx.controlflow.hints", i32 1}`, and a
    // pattern that misses it leaves that id invisible. The fresh-id
    // counter starts at max+1, so it then hands out an id the module
    // already uses and the assembler says "Metadata id is already used".
    // Unreal uses those hints constantly.
    static const std::regex re(R"(!(\d+) = (?:distinct )?!\{(.*)\}\s*)");
    std::map<int, std::string> out;
    for (const auto& line : llm::SplitLines(text)) {
        std::smatch m;
        if (std::regex_match(line, m, re))
            out[std::stoi(m[1].str())] = m[2].str();
    }
    return out;
}

// The createHandleForLib overload name for a resource type. DXC names it after
// the type with the leading % dropped, and quotes the whole symbol when the
// type does.
std::string HandleFn(const std::string& llvmType) {
    std::string bare = llvmType.substr(1);
    if (!bare.empty() && bare.front() == '"') {
        std::string inner = bare.substr(1, bare.size() - 2);
        return "\"dx.op.createHandleForLib." + inner + "\"";
    }
    return "dx.op.createHandleForLib." + bare;
}

// Every block this line branches to.
std::vector<std::string> Block_successors(const std::string& line) {
    static const std::regex re(R"(label\s+(%[\w.$-]+))");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(line.begin(), line.end(), re);
         it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1].str());
    return out;
}

std::string Replace(std::string s, const std::string& from, const std::string& to) {
    size_t p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
    return s;
}


// --- what a generated hit shader can rebuild for itself ---------------------
//
// Mirrors _type_names / _operands / _recomputable / _needed_chain in lower.py,
// which is the reference. See there for why each list is what it is.

// Every %name an instruction READS, not just its call arguments.
//
// Instr::Uses() decodes call arguments and phi incomings, which is what the
// rest of this pass needs. It is not enough for the isolation check: a value
// can reach the loop body through ordinary arithmetic, `fcmp float %bary,
// %thresh`, and Uses() cannot see it. The check then let it through, the
// generated hit shader referenced a value it never defined, and the ASSEMBLER
// reported it, as "use of undefined value". Loud, but nowhere near the cause.
std::set<std::string> TypeNames(const std::string& text) {
    // A name is a TYPE exactly when the module declares it one. Guessing from
    // the shape of the name does not work: %rq.pl and %dx.types.Handle look
    // alike.
    static const std::regex kTypedef(R"RX(^(%"[^"]*"|%[\w.$-]+)\s*=\s*type\s)RX");
    std::set<std::string> out;
    for (const auto& line : llm::SplitLines(text)) {
        std::smatch m;
        if (std::regex_search(line, m, kTypedef)) out.insert(m[1].str());
    }
    return out;
}

std::vector<std::string> Operands(const llm::Instr& i,
                                  const std::set<std::string>& types) {
    // A phi writes its predecessors as `[ %val, %bb12 ]`, with no `label`
    // keyword to mark them, so the general scan below counts %bb12 as a value
    // read. Unreal's loop bodies are full of phis, and the check then refused
    // 118 shaders for "reading" their own predecessor blocks.
    //
    // Uses() already decodes a phi correctly: incoming VALUES, which do count,
    // and not the blocks, which do not.
    if (i.isPhi) return i.Uses();

    std::string body = i.body;
    const size_t cut = body.find(';');
    if (cut != std::string::npos) body = body.substr(0, cut);

    // A quoted name first, because %"class.RWStructuredBuffer<Result>" holds
    // characters an unquoted one cannot and would be cut at the quote.
    static const std::regex kName(R"RX(%"[^"]*"|%[\w.$-]+)RX");
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), kName);
         it != std::sregex_iterator(); ++it) {
        const std::string n = it->str();
        if (types.count(n) || n == i.result) continue;
        // `label %bb42` is a branch target, not a value.
        std::string before = body.substr(0, it->position());
        while (!before.empty() && std::isspace((unsigned char)before.back()))
            before.pop_back();
        if (before.size() >= 5 && before.compare(before.size() - 5, 5, "label") == 0)
            continue;
        out.push_back(n);
    }
    return out;
}

bool PureInstruction(const llm::Instr& i) {
    static const std::set<std::string> kPure = {
        "add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr",
        "fadd", "fsub", "fmul", "fdiv", "fneg",
        "icmp", "fcmp", "select", "extractvalue", "extractelement",
        "zext", "sext", "trunc", "bitcast", "sitofp", "uitofp", "fptosi",
        "fptoui", "fpext", "fptrunc", "getelementptr",
    };
    static const std::regex kOp(R"(^\s*(\w+))");
    std::smatch m;
    if (!std::regex_search(i.body, m, kOp)) return false;
    return kPure.count(m[1].str()) != 0;
}

// 57 createHandle, 59 cbufferLoadLegacy, 93 threadId, plus the Shader Model
// 6.6 handle pair 216/217 and the bindless form 218. See lower.py for what is
// deliberately absent: loads, phis, integer division, any other dx.op.
//
// 218 was left off deliberately and wrongly. The 0.25.0 note said a heap
// handle used inside the Proceed loop is refused because 218 is not on this
// list, and filed that as correct; it was measured on a shader that kept all
// 16 of its heap handles in the raygen, so nothing counted the in-loop case.
// Counted: 58 of the 157 refusals from one Escher run are exactly this.
//
// It is recomputable for the reason the other handles are, one step further
// out. The descriptor heap is set on the command list and is the same heap in
// the any-hit as in the raygen, and the index reaches it from a cbuffer, which
// holds the same bytes for the whole dispatch. The fixpoint is what enforces
// "from a cbuffer": 218 is admitted only when its index is ALREADY
// recomputable, so an index built from a UAV read or a phi still refuses. It
// needs no conversion either, unlike 57 and 217, because a heap handle means
// the same thing in a library, so it is emitted verbatim.
//
// 94, 95 and 96 are the group forms of 93, and the hit shader rebuilds them
// from DispatchRaysIndex exactly as the raygen does. See ThreadIndexText.
bool PureDxOp(int op) {
    return op == 57 || op == 59 || op == 93 ||
           op == kGroupId || op == kThreadIdInGroup || op == kFlatThreadIdInGroup ||
           op == kAnnotateHandle || op == kBindHandle || op == kHeapHandle;
}

// The compute thread-index ops. None is legal in a ray tracing stage, and a
// raygen has no thread group at all. But the shim launches exactly
// groups * numthreads rays, one per thread, so DispatchRaysIndex IS the
// thread's SV_DispatchThreadID and the other three follow from it:
//
//   93  SV_DispatchThreadID.c  = DispatchRaysIndex.c
//   94  SV_GroupID.c           = DispatchRaysIndex.c / numthreads.c
//   95  SV_GroupThreadID.c     = DispatchRaysIndex.c % numthreads.c
//   96  SV_GroupIndex          = (gt.z * ny + gt.y) * nx + gt.x, gt the above
//
// Exact. What a group actually SHARES, groupshared and barriers, is refused by
// the analysis. The divisors are constants of at least 1, so nothing hoisted
// can divide by zero. The Python original is _thread_index_text in lower.py.
bool IsThreadOp(int op) {
    return op == kThreadId || op == kGroupId || op == kThreadIdInGroup ||
           op == kFlatThreadIdInGroup;
}

const char* kNoNumThreads =
    "the entry point declares no numthreads, so the thread group index cannot "
    "be rebuilt";

std::string ThreadIndexText(const llm::Instr& i, const llm::Module& m,
                            std::string* err) {
    const std::string& r = i.result;
    const std::string tag = r.substr(1);
    auto Dri = [](const std::string& name, int comp) {
        return "  " + name + " = call i32 @dx.op.dispatchRaysIndex.i32(i32 145, i8 " +
               std::to_string(comp) + ")  ; DispatchRaysIndex(col)";
    };
    const int op = i.DxOp();
    int nt[3] = { 1, 1, 1 };

    if (op == kThreadId || op == kGroupId || op == kThreadIdInGroup) {
        static const std::regex kCol(R"RX(^i32\s+(\d+))RX");
        std::smatch cm;
        const std::string a1 = i.args.size() > 1 ? Trim(i.args[1]) : std::string();
        const int comp = std::regex_search(a1, cm, kCol) ? std::stoi(cm[1].str()) : 0;
        if (comp > 2) {
            *err = "thread index component " + std::to_string(comp) + " out of range";
            return std::string();
        }
        if (op == kThreadId) return Dri(r, comp);
        if (!NumThreads(m, nt)) { *err = kNoNumThreads; return std::string(); }
        const std::string d = "%rq.dri." + tag;
        return Dri(d, comp) + "\n  " + r + " = " +
               (op == kGroupId ? "udiv" : "urem") + " i32 " + d + ", " +
               std::to_string(nt[comp]);
    }

    if (!NumThreads(m, nt)) { *err = kNoNumThreads; return std::string(); }
    std::string out;
    const char* axes = "xyz";
    for (int c = 0; c < 3; ++c) {
        const std::string ax(1, axes[c]);
        out += Dri("%rq.dri." + tag + "." + ax, c) + "\n";
        out += "  %rq.gt." + tag + "." + ax + " = urem i32 %rq.dri." + tag + "." + ax +
               ", " + std::to_string(nt[c]) + "\n";
    }
    out += "  %rq.gi." + tag + ".a = mul i32 %rq.gt." + tag + ".z, " +
           std::to_string(nt[1]) + "\n";
    out += "  %rq.gi." + tag + ".b = add i32 %rq.gi." + tag + ".a, %rq.gt." + tag + ".y\n";
    out += "  %rq.gi." + tag + ".c = mul i32 %rq.gi." + tag + ".b, " +
           std::to_string(nt[0]) + "\n";
    out += "  " + r + " = add i32 %rq.gi." + tag + ".c, %rq.gt." + tag + ".x";
    return out;
}

// Loads, 0.42.0: a value read from a READ-ONLY buffer or texture before the
// loop and used inside it, which the hit shader reads again. Exact, because a
// dispatch cannot write what it reads as an SRV. A UAV load is not re-read: it
// travels in the payload instead. See LOAD_OPS and _plan_carry in lower.py.
bool IsLoadOp(int op) { return op == 66 || op == 68 || op == 139; }
const int kUavBit = 0x1000;   // ResourceProperties, first word: IsUAV

// "srv", "uav" or "", read from how the handle was made.
std::string HandleClass(const std::map<std::string, const llm::Instr*>& defs,
                        const std::string& name) {
    auto it = defs.find(name);
    if (it == defs.end()) return "";
    const llm::Instr& i = *it->second;
    std::smatch m;
    if (i.DxOp() == kAnnotateHandle) {
        static const std::regex kProps(R"RX(%dx\.types\.ResourceProperties \{ i32 (\d+),)RX");
        if (!std::regex_search(i.line, m, kProps)) return "";
        return (std::stoll(m[1].str()) & kUavBit) ? "uav" : "srv";
    }
    std::string cls;
    if (i.DxOp() == kCreateHandle) {
        static const std::regex kCls(R"RX(^i8 (\d+))RX");
        const std::string a1 = i.args.size() > 1 ? Trim(i.args[1]) : std::string();
        if (!std::regex_search(a1, m, kCls)) return "";
        cls = m[1].str();
    } else if (i.DxOp() == kBindHandle) {
        static const std::regex kBind(
            R"RX(ResBind \{ i32 -?\d+, i32 -?\d+, i32 -?\d+, i8 (\d+) \})RX");
        if (!std::regex_search(i.line, m, kBind)) return "";
        cls = m[1].str();
    } else {
        return "";
    }
    return cls == "0" ? "srv" : cls == "1" ? "uav" : "";
}

std::map<std::string, const llm::Instr*> Recomputable(
        const llm::Function& fn, const std::set<std::string>& types) {
    std::map<std::string, const llm::Instr*> ok, defs;
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (!i.result.empty()) defs[i.result] = &i;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& b : fn.blocks) {
            for (const auto& i : b.instrs) {
                if (i.result.empty() || ok.count(i.result)) continue;
                const int op = i.DxOp();
                if (IsLoadOp(op)) {
                    std::string h;
                    if (i.args.size() > 1) {
                        const std::string a = Trim(i.args[1]);
                        size_t sp = a.find_last_of(" \t");
                        h = sp == std::string::npos ? a : a.substr(sp + 1);
                    }
                    const std::string cls = HandleClass(defs, h);
                    if (!ok.count(h) || cls != "srv") continue;
                }
                else if (op >= 0) { if (!PureDxOp(op)) continue; }
                else if (!PureInstruction(i)) continue;
                bool all = true;
                for (const auto& u : Operands(i, types))
                    if (!ok.count(u) && u != i.result) { all = false; break; }
                if (!all) continue;
                ok[i.result] = &i;
                changed = true;
            }
        }
    }
    return ok;
}

// The LLVM type of the value an instruction defines, or "". _result_type in
// lower.py, which this mirrors.
std::string ResultType(const llm::Instr& i) {
    static const std::set<std::string> kFlags = {
        "nuw", "nsw", "exact", "fast", "nnan", "ninf", "nsz", "arcp", "contract",
        "afn", "reassoc"};
    std::string body = i.body;
    size_t cut = body.find(';');
    if (cut != std::string::npos) body = body.substr(0, cut);
    std::vector<std::string> words;
    {
        std::istringstream ws(body);
        std::string w;
        while (ws >> w) words.push_back(w);
    }
    if (words.empty()) return "";
    const std::string& op = words[0];
    std::vector<std::string> rest;
    for (size_t k = 1; k < words.size(); ++k)
        if (!kFlags.count(words[k])) rest.push_back(words[k]);
    std::smatch m;
    if (op == "icmp" || op == "fcmp") return "i1";
    if (op == "call" || op == "load" || op == "phi") {
        if (rest.empty()) return "";
        std::string t = rest[0];
        while (!t.empty() && t.back() == ',') t.pop_back();
        return t;
    }
    if (op == "select") {
        static const std::regex kSel(R"RX(^select\s+i1\s+[^,]+,\s+(\S+)\s)RX");
        return std::regex_search(body, m, kSel) ? m[1].str() : "";
    }
    static const std::set<std::string> kCasts = {
        "zext", "sext", "trunc", "bitcast", "sitofp", "uitofp", "fptosi",
        "fptoui", "fpext", "fptrunc"};
    if (kCasts.count(op)) {
        static const std::regex kTo(R"RX(\sto\s+(\S+)\s*$)RX");
        const std::string t = Trim(body);
        if (std::regex_search(t, m, kTo)) return m[1].str();
        return rest.empty() ? "" : rest[0];
    }
    if (op == "extractvalue") {
        static const std::regex kEv(
            R"RX(^extractvalue\s+%dx\.types\.(ResRet|CBufRet)\.(\w+)\s+\S+,\s*(\d+))RX");
        if (!std::regex_search(body, m, kEv)) return "";
        if (m[1].str() == "ResRet" && m[3].str() == "4") return "i32";
        static const std::map<std::string, std::string> kSuffix = {
            {"i32", "i32"}, {"f32", "float"}, {"i16", "i16"}, {"f16", "half"},
            {"i64", "i64"}, {"f64", "double"}};
        auto it = kSuffix.find(m[2].str());
        return it == kSuffix.end() ? "" : it->second;
    }
    if (op == "extractelement") {
        static const std::regex kEe(R"RX(^extractelement\s+<\d+ x (\S+)>)RX");
        return std::regex_search(body, m, kEe) ? m[1].str() : "";
    }
    return rest.empty() ? "" : rest[0];
}

// `skip` is what the loop body defines for itself. Without it the closure walks
// straight back into the body and rebuilds its instructions in the prologue as
// well, which is a duplicate definition.
std::vector<const llm::Instr*> NeededChain(
        const llm::Function& fn,
        const std::map<std::string, const llm::Instr*>& rec,
        const std::set<std::string>& wanted,
        const std::set<std::string>& types,
        const std::set<std::string>& skip) {
    std::set<std::string> need;
    std::vector<std::string> frontier;
    for (const auto& w : wanted) if (!skip.count(w)) frontier.push_back(w);
    while (!frontier.empty()) {
        const std::string r = frontier.back();
        frontier.pop_back();
        if (need.count(r) || skip.count(r)) continue;
        auto it = rec.find(r);
        if (it == rec.end()) continue;
        need.insert(r);
        for (const auto& u : Operands(*it->second, types)) frontier.push_back(u);
    }
    std::vector<const llm::Instr*> out;
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (need.count(i.result)) out.push_back(&i);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

bool UsesBinding(const llm::Module& m) {
    if (m.functions.empty()) return false;
    for (const auto& b : m.functions[0].blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == kBindHandle) return true;
    return false;
}

struct Bind {
    int cls = 0;
    int space = 0;
    int lower = 0;
    bool operator<(const Bind& o) const {
        if (cls != o.cls) return cls < o.cls;
        if (space != o.space) return space < o.space;
        return lower < o.lower;
    }
};

// `%dx.types.ResBind { i32 lower, i32 upper, i32 space, i8 class }`, read off
// real DXC output: `{ i32 4, i32 4, i32 0, i8 2 }` is cbuffer b4, space 0.
bool ResBind(const llm::Instr& i, Bind* out) {
    static const std::regex re(
        R"(\{\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),\s*i32\s+(-?\d+),\s*i8\s+(-?\d+)\s*\})");
    if (i.args.size() < 2) return false;
    // SRV t0 space0 is all zeroes, and LLVM prints an all-zero struct as
    // `zeroinitializer` rather than writing the fields out. Nothing in the
    // Unreal shaders was bound there, so only an independently written case
    // reached this.
    if (i.args[1].find("zeroinitializer") != std::string::npos) {
        out->cls = 0; out->space = 0; out->lower = 0;
        return true;
    }
    std::smatch m;
    if (!std::regex_search(i.args[1], m, re)) return false;
    out->lower = std::stoi(m[1].str());
    out->space = std::stoi(m[3].str());
    out->cls = std::stoi(m[4].str());
    return true;
}

// {class, space, lowerBound} -> record node id. A record is
// `{id, global, name, space, lowerBound, rangeSize, ...}`, so the binding in
// the call is matched by space and lower bound rather than by the range index
// the 6.5 form carries.
std::map<Bind, int> BindingMap(const std::string& text,
                               const std::map<int, std::string>& md) {
    std::map<Bind, int> out;
    static const std::regex kRes(R"(!dx\.resources\s*=\s*!\{!(\d+)\})");
    std::smatch rm;
    if (!std::regex_search(text, rm, kRes)) return out;
    auto groups = SplitTop(md.at(std::stoi(rm[1].str())));
    static const std::regex kI32(R"(i32\s+(-?\d+))");
    for (size_t cls = 0; cls < groups.size(); ++cls) {
        const std::string g = Trim(groups[cls]);
        if (g == "null") continue;
        for (const auto& idTok : SplitTop(md.at(std::stoi(g.substr(1))))) {
            const int nid = std::stoi(Trim(idTok).substr(1));
            auto f = SplitTop(md.at(nid));
            if (f.size() < 5) continue;
            std::smatch sm, lm;
            const std::string sp = Trim(f[3]), lo = Trim(f[4]);
            if (!std::regex_search(sp, sm, kI32)) continue;
            if (!std::regex_search(lo, lm, kI32)) continue;
            Bind b;
            b.cls = static_cast<int>(cls);
            b.space = std::stoi(sm[1].str());
            b.lower = std::stoi(lm[1].str());
            out[b] = nid;
        }
    }
    return out;
}

// The module-level shader flags, tag 0 of the entry point's properties.
//
// The lowering used to hardcode 16 here, which happened to be right for every
// shader this project had ever seen: they all declared 0x2000010, and the only
// bit the lowering removes is the tier 1.1 flag. An Unreal shader declares
// 0x42000010, so 16 was wrong by exactly the bit it did not know about and the
// validator said "Flags must match usage".
// Attribute group NUMBERS are a fact about the input module, not a constant.
// See lower.py for what Unreal's numbering did to AnyHitNull and why the
// message pointed at the `unreachable` rather than at the missing noreturn.
struct AttrPlaceholder { const char* tag; const char* body; };
const AttrPlaceholder kAttrPlaceholders[] = {
    { "#RQNONE", "nounwind readnone" },
    { "#RQNW",   "nounwind" },
    { "#RQRO",   "nounwind readonly" },
    { "#RQNR",   "noreturn nounwind" },
};

std::string ResolveAttrs(const std::string& in, std::string* err) {
    auto lines = llm::SplitLines(in);
    static const std::regex kAttr(R"(attributes #(\d+) = \{ (.*) \}\s*)");
    std::map<std::string, int> have;
    std::set<int> used;
    int last = -1;
    for (size_t n = 0; n < lines.size(); ++n) {
        std::smatch m;
        if (!std::regex_match(lines[n], m, kAttr)) continue;
        const int id = std::stoi(m[1].str());
        used.insert(id);
        // First wins, matching the Python.
        have.emplace(Trim(m[2].str()), id);
        last = static_cast<int>(n);
    }
    std::vector<AttrPlaceholder> wanted;
    for (const auto& a : kAttrPlaceholders)
        if (in.find(a.tag) != std::string::npos) wanted.push_back(a);

    std::vector<std::string> added;
    for (const auto& a : wanted) {
        if (have.count(a.body)) continue;
        int id = 0;
        while (used.count(id)) ++id;
        used.insert(id);
        have[a.body] = id;
        added.push_back("attributes #" + std::to_string(id) + " = { " + a.body + " }");
    }
    std::string out = in;
    if (!added.empty()) {
        if (last < 0) {
            *err = "module declares no attribute groups";
            return in;
        }
        lines.insert(lines.begin() + last + 1, added.begin(), added.end());
        out = llm::JoinLines(lines);
    }
    // Replace ALL of them. The Replace helper here does the first occurrence
    // only, where Python's str.replace does every one, and a placeholder
    // appears once per generated function.
    for (const auto& a : wanted) {
        const std::string tag = a.tag;
        const std::string num = "#" + std::to_string(have[a.body]);
        for (size_t p = out.find(tag); p != std::string::npos;
             p = out.find(tag, p + num.size()))
            out.replace(p, tag.size(), num);
    }
    return out;
}

unsigned long long ModuleFlags(const std::string& text,
                               const std::map<int, std::string>& md) {
    static const std::regex kEp(R"(!dx\.entryPoints\s*=\s*!\{!(\d+)\})");
    std::smatch m;
    if (!std::regex_search(text, m, kEp)) return 0;
    auto it = md.find(std::stoi(m[1].str()));
    if (it == md.end()) return 0;
    auto fields = SplitTop(it->second);
    if (fields.size() < 5) return 0;
    const std::string p4 = Trim(fields[4]);
    if (p4.empty() || p4[0] != '!') return 0;
    auto pit = md.find(std::stoi(p4.substr(1)));
    if (pit == md.end()) return 0;
    auto props = SplitTop(pit->second);
    static const std::regex kI64(R"(^i64\s+(\d+))");
    for (size_t i = 0; i + 1 < props.size(); i += 2) {
        if (Trim(props[i]) != "i32 0") continue;
        std::smatch vm;
        const std::string v = Trim(props[i + 1]);
        if (std::regex_search(v, vm, kI64)) return std::stoull(vm[1].str());
    }
    return 0;
}

// The any-hit's first act when it tracks committed state (0.66.0); see
// _opacity in lower.py.
std::vector<std::string> Opacity(const Query& q, bool sm66) {
    const std::string t = "o" + std::to_string(q.qid);
    const std::string n = "%rq." + t;
    std::vector<std::string> out = RecordRead(t, 2, n + ".w", sm66);
    const std::vector<std::string> rest = {
        "  " + n + ".fo = and i32 %rq.oflags, 1",
        "  " + n + ".fn = and i32 %rq.oflags, 2",
        "  " + n + ".a = icmp ne i32 " + n + ".fo, 0",
        "  " + n + ".b = icmp ne i32 " + n + ".fn, 0",
        "  " + n + ".c = icmp ne i32 " + n + ".w, 0",
        "  " + n + ".d = select i1 " + n + ".b, i1 false, i1 " + n + ".c",
        "  " + n + ".op = select i1 " + n + ".a, i1 true, i1 " + n + ".d",
        "  " + n + ".co = and i32 %rq.oflags, 64",
        "  " + n + ".cn = and i32 %rq.oflags, 128",
        "  " + n + ".cu = select i1 " + n + ".op, i32 " + n + ".co, i32 " + n + ".cn",
        "  " + n + ".cl = icmp ne i32 " + n + ".cu, 0",
        "  br i1 " + n + ".cl, label %rq.reject, label " + n + ".k",
        "",
        "rq." + t + ".k:",
        "  br i1 " + n + ".op, label %rq.accept, label " + q.loop.header,
    };
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

// What the closest-hit would record, on every accept; see _record_commit.
// %rq.<t>.op, whether this procedural candidate is opaque (0.70.0), with
// the query's own flags as constants once traced forced non-opaque (0.71.0).
// See _isect_opacity in lower.py.
std::vector<std::string> IsectOpacity(const Query& q, const std::string& t, bool sm66) {
    const std::string n = "%rq." + t;
    std::vector<std::string> out = RecordRead(t, 2, n + ".w", sm66);
    if (q.trackProc)
        out.push_back("  " + n + ".f = add i32 " + std::to_string(q.RayFlags()) + ", 0");
    else
        out.push_back("  " + n + ".f = call i32 @dx.op.rayFlags.i32(i32 144)  ; RayFlags()");
    out.push_back("  " + n + ".fo = and i32 " + n + ".f, 1");
    out.push_back("  " + n + ".fn = and i32 " + n + ".f, 2");
    out.push_back("  " + n + ".a = icmp ne i32 " + n + ".fo, 0");
    out.push_back("  " + n + ".b = icmp ne i32 " + n + ".fn, 0");
    out.push_back("  " + n + ".c = icmp ne i32 " + n + ".w, 0");
    out.push_back("  " + n + ".d = select i1 " + n + ".b, i1 false, i1 " + n + ".c");
    out.push_back("  " + n + ".op = select i1 " + n + ".a, i1 true, i1 " + n + ".d");
    return out;
}

std::vector<std::string> RecordCommit(const Query& q, bool sm66, int status = 1) {
    const std::string pl = kPayload;
    auto gep = [&](int idx) {
        return "  %rq.cp" + std::to_string(idx) + " = getelementptr inbounds " + pl + ", " + pl +
               "* %p, i32 0, i32 " + std::to_string(idx);
    };
    std::vector<std::string> out = {
        "  %rq.ct = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()",
        gep(0), "  store float %rq.ct, float* %rq.cp0, align 4",
        gep(1), "  store <2 x float> %rq.ab, <2 x float>* %rq.cp1, align 4",
        gep(2), "  store i32 " + std::to_string(status) + ", i32* %rq.cp2, align 4",
    };
    for (const auto& kv : kChSource) {
        const std::string idx = std::to_string(kv.first);
        out.push_back("  %rq.ci" + idx + " = " + kv.second);
        out.push_back(gep(kv.first));
        out.push_back("  store i32 %rq.ci" + idx + ", i32* %rq.cp" + idx + ", align 4");
    }
    if (q.needsRecordConstants) {
        for (const auto& l : RecordRead("c", 0, "%rq.cg", sm66)) out.push_back(l);
        out.push_back(gep(5));
        out.push_back("  store i32 %rq.cg, i32* %rq.cp5, align 4");
        out.push_back(std::string("  %rq.cc = extractvalue ") + kResRet + " %rq.cbrc, 1");
        out.push_back(gep(10));
        out.push_back("  store i32 %rq.cc, i32* %rq.cp10, align 4");
    }
    bool w2o = false;
    auto scan = [&](const Query& x) {
        for (const auto& c : x.committedOps)
            if (c.second->DxOp() == kCommittedWorldToObject) w2o = true;
    };
    scan(q);
    for (const auto& o : q.others) scan(o);
    if (w2o) {
        out.push_back(gep(8));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) {
                const std::string s = std::to_string(r * 4 + c);
                out.push_back("  %rq.cw" + s + " = call float @dx.op.worldToObject.f32(i32 152, i32 " +
                              std::to_string(r) + ", i8 " + std::to_string(c) +
                              ")  ; WorldToObject(row,col)");
                out.push_back("  %rq.cq" + s + " = getelementptr inbounds [12 x float], [12 x float]* %rq.cp8, "
                              "i32 0, i32 " + s);
                out.push_back("  store float %rq.cw" + s + ", float* %rq.cq" + s + ", align 4");
            }
    }
    return out;
}

// A committed accessor read inside the loop, from the payload; see
// _committed_read.
std::vector<std::string> CommittedRead(const llm::Instr& i) {
    const auto f = PayloadField(i.DxOp());
    const std::string r = i.result.substr(1);
    const std::string ptr = "%rq.cq" + r;
    const std::string pl = kPayload;
    std::vector<std::string> out = {
        "  " + ptr + " = getelementptr inbounds " + pl + ", " + pl + "* %p, i32 0, i32 " + std::to_string(f->idx)
    };
    if (i.DxOp() == kCommittedFrontFace) {
        out.push_back("  %rq.cf" + r + " = load i32, i32* " + ptr + ", align 4");
        out.push_back("  " + i.result + " = icmp eq i32 %rq.cf" + r + ", " + std::to_string(kHitKindFront));
    } else if (i.DxOp() == kCommittedWorldToObject) {
        static const std::regex kRowRe(R"(i32\s+(\d+))");
        static const std::regex kColRe(R"(i8\s+(\d+))");
        std::smatch rm2, cm2;
        std::string ar = Trim(i.args[2]), ac = Trim(i.args[3]);
        std::regex_search(ar, rm2, kRowRe);
        std::regex_search(ac, cm2, kColRe);
        const int slot = std::stoi(rm2[1].str()) * 4 + std::stoi(cm2[1].str());
        out.push_back("  %rq.cw" + r + " = getelementptr inbounds [12 x float], [12 x float]* " + ptr +
                      ", i32 0, i32 " + std::to_string(slot));
        out.push_back("  " + i.result + " = load float, float* %rq.cw" + r + ", align 4");
    } else if (i.DxOp() == kCommittedBary) {
        static const std::regex kI8(R"(i8\s+(\d+))");
        std::smatch mm;
        std::string a2 = Trim(i.args[2]);
        std::regex_search(a2, mm, kI8);
        out.push_back("  %rq.cb" + r + " = load <2 x float>, <2 x float>* " + ptr + ", align 4");
        out.push_back("  " + i.result + " = extractelement <2 x float> %rq.cb" + r + ", i32 " + mm[1].str());
    } else {
        out.push_back("  " + i.result + " = load " + f->ty + ", " + f->ty + "* " + ptr + ", align " +
                      std::to_string(f->align));
    }
    return out;
}

LowerResult Lower(const llm::Module& m, const Query& q, const Exports& e) {
    struct Carry { std::string name, ty; int field; };
    // A loop's state and what leaves it, in the payload (0.65.0). See
    // _plan_state in lower.py, which this mirrors.
    struct StatePhi { std::string name, ty; int field; std::string init, next; };
    struct StateOut { std::string name, ty; int field; };
    struct LoopState {
        std::vector<StatePhi> phis;
        std::vector<StateOut> outs;
        // Top-tested (0.72.0): escaping header phis, loaded from their state.
        std::vector<StateOut> souts;
        int iter = -1;
        std::string guardFalse, guard;
        std::map<std::string, std::map<std::string, std::string>> latchPhis;   // phi -> pred -> value
        std::vector<std::pair<std::string, std::string>> tails;                // (block, accept/reject)
        // A break or return (0.72.0): which way out, the exits as (block,
        // target, k), and the any-hit's tails for them as (block, tail, k).
        int exitField = -1;
        // Proceed() without a loop (0.74.0): the candidate count, and the
        // any-hit's tails that end the search, as (block, tail).
        int pcount = -1;
        std::vector<std::pair<std::string, std::string>> etails;
        std::vector<std::tuple<std::string, std::string, int>> exits, xtails;
        std::map<std::string, std::string> defBlock;
        std::map<std::string, std::set<std::string>> dom;
    };
    std::map<const Query*, LoopState> states;
    // Every value carried by any query, in field order, once all are planned.
    std::vector<Carry> carry;
    auto CarryWordsOf = [](const std::vector<Carry>& cs) {
        int n = 0;
        for (const auto& c : cs) n += Words(c.ty);
        return n;
    };
    // Several queries in one entry point, 0.43.0: each its own TraceRay and
    // payload, one any-hit choosing the loop body from the query id. See
    // lower.py. A value two loop bodies read travels in one field.
    std::vector<const Query*> queries{ &q };
    for (const auto& o : q.others) queries.push_back(&o);
    const bool multi = queries.size() > 1;
    std::map<std::string, std::pair<std::string, int>> carryFields;
    const int carryBase = kCarryField0 + (multi ? 1 : 0);
    LowerResult r;
    const std::string& text0 = m.text;
    const auto md = Metadata(text0);

    // --- resource table ----------------------------------------------------
    std::smatch rm;
    static const std::regex kRes(R"(!dx\.resources\s*=\s*!\{!(\d+)\})");
    if (!std::regex_search(text0, rm, kRes)) {
        r.error = "module has no !dx.resources";
        return r;
    }
    const std::string resId = rm[1].str();
    std::map<int, std::vector<ResRec>> table;   // class -> records, in order
    {
        auto groups = SplitTop(md.at(std::stoi(resId)));
        static const std::regex kType(R"RX((\[[^\]]+\]|%[\w.$-]+|%"[^"]+")\*\s+\S+)RX");
        static const std::regex kArr(R"(^\[(\d+) x (.+)\]$)");
        static const std::regex kName(R"RX(^!"([^"]*)")RX");
        for (size_t cls = 0; cls < groups.size(); ++cls) {
            const std::string g = Trim(groups[cls]);
            if (g == "null") continue;
            std::vector<ResRec> recs;
            for (const auto& idTok : SplitTop(md.at(std::stoi(g.substr(1))))) {
                const int nid = std::stoi(Trim(idTok).substr(1));
                auto fields = SplitTop(md.at(nid));
                std::smatch tm;
                if (fields.size() < 3 || !std::regex_match(fields[1], tm, kType)) {
                    r.error = "resource record !" + std::to_string(nid) +
                              " has an unexpected shape";
                    return r;
                }
                ResRec rec;
                rec.nid = nid;
                rec.gty = tm[1].str();
                std::smatch am;
                if (std::regex_match(rec.gty, am, kArr)) {
                    rec.isArray = true;
                    rec.elem = am[2].str();
                } else {
                    rec.elem = rec.gty;
                }
                std::smatch nm;
                if (std::regex_search(fields[2], nm, kName)) rec.name = nm[1].str();
                recs.push_back(rec);
            }
            table[static_cast<int>(cls)] = recs;
        }
    }

    const bool binding = UsesBinding(m);
    // How the record handle is written; see kRecordType.
    const bool sm66 = IsSm66(text0);
    const std::map<Bind, int> binds = binding ? BindingMap(text0, md)
                                              : std::map<Bind, int>();

    // --- one global per resource, in the same order Python builds them -----
    std::vector<GlobalInfo> globals;
    {
        static const std::regex kIdent(R"(^[A-Za-z_]\w*$)");
        const char* clsName[] = { "srv", "uav", "cbv", "smp" };
        for (const auto& kv : table) {
            for (size_t n = 0; n < kv.second.size(); ++n) {
                const ResRec& rec = kv.second[n];
                GlobalInfo g;
                g.cls = kv.first;
                g.nid = rec.nid;
                if (!rec.name.empty() && std::regex_match(rec.name, kIdent))
                    g.sym = rec.name;
                else
                    g.sym = std::string("rq_") +
                            (kv.first >= 0 && kv.first < 4 ? clsName[kv.first] : "res") +
                            std::to_string(n);
                g.gty = rec.gty;
                g.elem = rec.elem;
                g.name = rec.name.empty() ? g.sym : rec.name;
                globals.push_back(g);
            }
        }
    }
    auto findGlobal = [&](int cls, int nid) -> const GlobalInfo* {
        for (const auto& g : globals)
            if (g.cls == cls && g.nid == nid) return &g;
        return nullptr;
    };

    // The load + createHandleForLib that replaces one createHandle. Shared,
    // because the any-hit shader has to recreate any resource handle the
    // Proceed loop body used: a handle is not caller state, it names a
    // resource, and every shader in the library can reach it.
    std::string handleErr;
    auto handleText = [&](const llm::Instr& instr) -> std::string {
        static const std::regex kI8(R"(i8\s+(\d+))");
        static const std::regex kI32(R"(i32\s+(\d+))");
        static const std::regex kI32End(R"(^i32\s+(\d+)$)");
        std::smatch mm;

        if (instr.DxOp() == kBindHandle) {
            // Shader Model 6.6. The binding names the resource directly, so
            // the record is found by space and lower bound rather than by a
            // range index. The annotateHandle that follows is left exactly as
            // it was: it is legal in a library and carries the properties.
            Bind b;
            if (!ResBind(instr, &b)) {
                handleErr = "cannot read the ResBind of " + instr.body.substr(0, 60);
                return "";
            }
            auto bit = binds.find(b);
            if (bit == binds.end()) {
                handleErr = "createHandleFromBinding names class " +
                            std::to_string(b.cls) + " space " +
                            std::to_string(b.space) + " register " +
                            std::to_string(b.lower) +
                            ", which !dx.resources does not describe";
                return "";
            }
            const GlobalInfo* gb = findGlobal(b.cls, bit->second);
            if (!gb) {
                handleErr = "createHandleFromBinding names a resource with no global";
                return "";
            }
            const std::string a2b = instr.args.size() > 2 ? Trim(instr.args[2])
                                                          : std::string();
            static const std::regex kI32Signed(R"(^i32\s+(-?\d+)$)");
            const std::string rawb = "%rq.raw." + instr.result.substr(1);
            const std::string h = kHandleType;
            std::string preb, srcb;
            std::smatch bm;
            const bool constIdx = std::regex_match(a2b, bm, kI32Signed);
            if (gb->gty == gb->elem) {
                if (!constIdx) {
                    handleErr = "createHandleFromBinding indexes a resource that is not an array (" + a2b + ")";
                    return "";
                }
                srcb = h + "* @" + gb->sym;
            } else {
                // A resource ARRAY (0.65.0; until then refused dynamically
                // and, indexed by a constant, silently bound its first
                // element). The binding's index is the REGISTER, the array's
                // element is that less the range's lower bound; read off DXC,
                // phase5/cases/reference/lib_sm66_dynarray_ref.hlsl.
                const std::string arr = BindingGty(gb->gty, gb->elem);
                if (constIdx) {
                    srcb = h + "* getelementptr inbounds (" + arr + ", " + arr + "* @" + gb->sym + ", i32 0, i32 " +
                           std::to_string(std::stoll(bm[1].str()) - b.lower) + ")";
                } else {
                    std::string ix = a2b;
                    if (b.lower) {
                        const std::string sub = "%rq.ix." + instr.result.substr(1);
                        const size_t sp = a2b.find_last_of(' ');
                        preb += "  " + sub + " = sub i32 " + a2b.substr(sp + 1) + ", " +
                                std::to_string(b.lower) + "\n";
                        ix = "i32 " + sub;
                    }
                    const std::string a3b = instr.args.size() > 3 ? Trim(instr.args[3]) : std::string("i1 false");
                    const bool nub = a3b != "i1 false" && a3b != "i1 0";
                    const std::string gep = "%rq.gep." + instr.result.substr(1);
                    preb += "  " + gep + " = getelementptr inbounds " + arr + ", " + arr + "* @" + gb->sym +
                            ", i32 0, " + ix + (nub ? ", !dx.nonuniform !RQNU" : "") + "\n";
                    srcb = h + "* " + gep;
                }
            }
            return preb + "  " + rawb + " = load " + h + ", " + srcb + ", align 4\n"
                   "  " + instr.result + " = call " + h +
                   " @dx.op.createHandleForLib.dx.types.Handle(i32 160, " + h +
                   " " + rawb + ")  ; CreateHandleForLib(Resource)";
        }

        std::string a1 = Trim(instr.args[1]), a2 = Trim(instr.args[2]);
        std::regex_search(a1, mm, kI8);
        const int cls = std::stoi(mm[1].str());
        std::regex_search(a2, mm, kI32);
        const int rid = std::stoi(mm[1].str());

        auto it = table.find(cls);
        if (it == table.end() || rid >= static_cast<int>(it->second.size())) {
            handleErr = "createHandle names range " + std::to_string(rid) +
                        " of class " + std::to_string(cls) +
                        ", which !dx.resources does not describe";
            return "";
        }
        const GlobalInfo* g = findGlobal(cls, it->second[rid].nid);
        const std::string fn = HandleFn(g->elem);
        const std::string raw = "%rq.raw." + instr.result.substr(1);

        std::string src;
        std::string pre;
        if (g->gty == g->elem) {
            src = g->elem + "* @" + g->sym;
        } else {
            // A resource array. DXC reaches the element through a
            // getelementptr on the array global and hands createHandleForLib
            // the ELEMENT type. Matched against a DXC-built library, see
            // phase5/cases/reference/lib_array_ref.hlsl for the constant form
            // and lib_dynarray_ref.hlsl for the dynamic one.
            const std::string a3 = Trim(instr.args[3]);
            const std::string nonuni = Trim(instr.args[4]);
            const bool nu = (nonuni != "i1 false" && nonuni != "i1 0");
            std::smatch im;
            if (std::regex_match(a3, im, kI32End)) {
                // A constant index folds into a constant getelementptr
                // EXPRESSION, inline in the call. Nothing runs at runtime.
                src = g->elem + "* getelementptr inbounds (" + g->gty + ", " +
                      g->gty + "* @" + g->sym + ", i32 0, i32 " + im[1].str() + ")";
            } else {
                // A dynamic index cannot be a constant expression, so it
                // becomes a real getelementptr INSTRUCTION on the same global,
                // with the index operand carried across untouched.
                const std::string gep = "%rq.gep." + instr.result.substr(1);
                // NonUniformResourceIndex shows up as !dx.nonuniform on the
                // getelementptr, and dropping it would be a silently wrong
                // lowering rather than a missing feature. The node id is not
                // known until the metadata is rebuilt, so a placeholder stands
                // in until then.
                pre = "  " + gep + " = getelementptr inbounds " + g->gty + ", " +
                      g->gty + "* @" + g->sym + ", i32 0, " + a3 +
                      (nu ? ", !dx.nonuniform !RQNU" : "") + "\n";
                src = g->elem + "* " + gep;
            }
        }
        return pre + "  " + raw + " = load " + g->elem + ", " + src + ", align 4\n" +
               "  " + instr.result + " = call %dx.types.Handle @" + fn +
               "(i32 160, " + g->elem + " " + raw + ")  ; CreateHandleForLib(Resource)";
    };

    std::map<int, std::optional<std::string>> edits;
    const llm::Function& fn = *q.fn;

    // --- resources ---------------------------------------------------------
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (i.DxOp() == kCreateHandle || i.DxOp() == kBindHandle) {
                const std::string t = handleText(i);
                if (!handleErr.empty()) { r.error = handleErr; return r; }
                edits[i.index] = t;
            }

    // --- no compute thread-index op is legal in a raygen ---------------------
    for (const auto& b : fn.blocks)
        for (const auto& i : b.instrs)
            if (IsThreadOp(i.DxOp())) {
                std::string terr;
                const std::string t = ThreadIndexText(i, m, &terr);
                if (!terr.empty()) { r.error = terr; return r; }
                edits[i.index] = t;
            }

    auto editQuery = [&](const Query& x) -> bool {
    std::vector<Carry> qcarry;
    // --- the payload alloca has to be in the entry block --------------------
    {
        const llm::Block& entry = fn.blocks.front();
        if (entry.instrs.empty()) { r.error = "entry block is empty"; return false; }
        const llm::Instr& first = entry.instrs.front();
        std::string pending = first.line;
        auto it = edits.find(first.index);
        if (it != edits.end() && it->second.has_value()) pending = *it->second;
        if (!x.preAborts.empty())   // Abort() before the search (0.75.0)
            pending = "  %" + x.pfx + ".rab = alloca i32, align 4\n" + pending;
        edits[first.index] = std::string("  %" + x.pfx + ".pl = alloca ") + kPayload +
                             ", align 8\n" + pending;
    }

    edits[x.alloc->index] = std::nullopt;

    // --- the loop, if there is one -----------------------------------------
    std::string bareRoute;
    std::string exitLabel;
    std::string preheader;
    if (x.hasLoop) {
        const llm::Block* latchBlk = fn.FindBlock(x.loop.latch);
        std::vector<std::string> outs;
        if (latchBlk)
            for (const auto& s : latchBlk->Successors())
                if (!x.loop.body.count(s)) outs.push_back(s);
        if (x.chain.empty() && outs.size() != 1) {
            r.error = "Proceed loop has " + std::to_string(outs.size()) +
                      " exits; expected one";
            return false;
        }
        if (x.chain.empty()) exitLabel = outs[0];

        // The any-hit shader is a separate invocation with only the payload
        // for shared state, so the loop body must not read caller locals or
        // leak values back out. A resource HANDLE is exempt: it names a
        // resource the any-hit shader can reach for itself.
        const std::set<std::string> types = TypeNames(m.text);
        std::set<std::string> defined, used, handles;
        for (const auto& lbl : x.loop.body) {
            const llm::Block* b = fn.FindBlock(lbl);
            if (!b) continue;
            for (const auto& i : b->instrs) {
                if (!i.result.empty()) defined.insert(i.result);
                // A header phi is loop STATE (0.65.0): its start is stored by
                // the raygen and its next value by each iteration, so what it
                // reads is not read by the any-hit.
                if (lbl == x.loop.header && i.isPhi) continue;
                if (x.top && lbl == x.loop.latch && i.isPhi) {
                    // Top-tested (0.72.0): its header is the latch and its
                    // phis the state; only what an iteration hands back is
                    // read here.
                    static const std::regex kName(R"RX(%"[^"]*"|%[\w.$-]+)RX");
                    for (const auto& in : i.phiIncoming) {
                        if (!x.loop.body.count(in.second)) continue;
                        for (std::sregex_iterator it(in.first.begin(), in.first.end(), kName), end;
                             it != end; ++it)
                            used.insert(it->str());
                    }
                    continue;
                }
                for (const auto& u : Operands(i, types)) used.insert(u);
            }
        }
        // A resource handle is not caller state, and neither is anything
        // else the hit shader can work out for itself: a cbuffer read, the ray
        // index, arithmetic on those. See Recomputable above.
        for (const auto& kv : Recomputable(fn, types)) handles.insert(kv.first);

        // But that exemption holds only for a handle the MODULE fully
        // determines. A dynamically indexed one depends on a value computed in
        // the raygen, and recreating it in the any-hit would need that value,
        // which is exactly the caller state the payload cannot carry. So this
        // one IS refused, precisely, rather than being swept up by the generic
        // message below.
        static const std::regex kConstIdx(R"(^i32\s+\d+$)");
        for (const auto& b : fn.blocks) {
            for (const auto& i : b.instrs) {
                if (i.DxOp() != kCreateHandle || i.result.empty()) continue;
                if (!used.count(i.result) || i.args.size() < 4) continue;
                const std::string a3 = Trim(i.args[3]);
                if (std::regex_match(a3, kConstIdx)) continue;
                r.error = "resource handle " + i.result + " is indexed dynamically (" +
                          a3 + ") and used inside the Proceed loop; the index is "
                          "computed in the raygen and the any-hit shader is a "
                          "separate invocation that cannot see it";
                return false;
            }
        }
        std::vector<std::string> outside;
        for (const auto& u : used)
            if (!defined.count(u) && u != x.handle && !handles.count(u))
                outside.push_back(u);
        // A value the loop defines and code after it reads leaves through
        // the payload (0.65.0; until then refused).
        std::vector<std::string> escapes;
        for (const auto& b : fn.blocks) {
            if (x.loop.body.count(b.label)) continue;
            for (const auto& i : b.instrs)
                for (const auto& u : Operands(i, types))
                    if (defined.count(u) && std::find(escapes.begin(), escapes.end(), u) == escapes.end())
                        escapes.push_back(u);
        }

        // What the hit shader cannot rebuild travels in the PAYLOAD, 0.42.0:
        // the raygen stores it before TraceRay and the any-hit loads it back
        // under its original name. See _plan_carry in lower.py.
        if (!outside.empty()) {
            std::string names;
            for (size_t k = 0; k < outside.size(); ++k) {
                if (k) names += ", ";
                names += outside[k];
            }
            if (x.NeedsBoth()) {
                r.error = "Proceed loop body reads values defined outside it (" + names +
                          ") and becomes an intersection shader, which has no payload "
                          "to carry them in";
                return false;
            }
            if ((int)outside.size() > kMaxCarry) {
                r.error = "Proceed loop body reads " + std::to_string(outside.size()) +
                          " values defined outside it (" + names + "); more than the " +
                          std::to_string(kMaxCarry) + " the payload carries";
                return false;
            }
            std::map<std::string, std::pair<const llm::Block*, const llm::Instr*>> defs;
            for (const auto& b : fn.blocks)
                for (const auto& i : b.instrs)
                    if (!i.result.empty()) defs[i.result] = {&b, &i};
            const auto dom = fn.Dominators();
            // Where the TraceRay goes: the trace, or for a search a branch
            // chooses the ray of, the block where it starts (0.73.0).
            const std::string tb = (x.routeBlock ? x.routeBlock : x.traceBlock)->label;
            const int at = x.routeBlock ? x.routeBlock->Terminator()->index : x.trace->index;
            for (size_t k = 0; k < outside.size(); ++k) {
                const std::string& name = outside[k];
                auto it = defs.find(name);
                if (it == defs.end()) {
                    r.error = "Proceed loop body reads " + name + ", which no instruction "
                              "defines; the any-hit shader is a separate invocation";
                    return false;
                }
                const llm::Block* db = it->second.first;
                const llm::Instr* di = it->second.second;
                bool before = false;
                if (db->label == tb) before = di->index < at;
                else {
                    auto d = dom.find(tb);
                    before = d != dom.end() && d->second.count(db->label);
                }
                if (!before) {
                    r.error = "Proceed loop body reads " + name + ", which is computed "
                              "after the trace; the raygen cannot put it in the payload "
                              "before TraceRay";
                    return false;
                }
                const std::string ty = ResultType(*di);
                if (!CarryType(ty)) {
                    r.error = "Proceed loop body reads " + name + ", of type " +
                              (ty.empty() ? std::string("None") : ty) +
                              ", from before it; the payload carries i32, float, i1, half, i16, i64 and double";
                    return false;
                }
                auto fit = carryFields.find(name);
                if (fit == carryFields.end())
                    fit = carryFields.emplace(name, std::make_pair(
                        ty, carryBase + CarryWords(carryFields))).first;
                qcarry.push_back({name, ty, fit->second.second});
            }
            if (CarryWords(carryFields) > kMaxCarry) {
                r.error = "the Proceed loops read " + std::to_string(CarryWords(carryFields)) +
                          " values defined outside them between them; more than the " +
                          std::to_string(kMaxCarry) + " the payload carries";
                return false;
            }
        }

        // The loop's state and what leaves it (0.65.0): see _plan_state.
        // Proceed() without a loop (0.74.0): see _plan_chain_state.
        if (!x.chain.empty()) {
            if (x.NeedsBoth() && !escapes.empty()) {
                std::string names;
                for (const auto& s : escapes) names += (names.empty() ? "" : ", ") + s;
                r.error = "Proceed loop carries state across candidates or out of the loop (" +
                          names + ") and becomes an intersection shader, which has no payload "
                          "to carry it in";
                return false;
            }
            std::map<std::string, const llm::Instr*> defs;
            for (const auto& b : fn.blocks)
                for (const auto& i : b.instrs)
                    if (!i.result.empty()) defs[i.result] = &i;
            auto field = [&](const std::string& key, const std::string& ty) {
                auto it = carryFields.find(key);
                if (it == carryFields.end())
                    it = carryFields.emplace(key, std::make_pair(ty, carryBase + CarryWords(carryFields))).first;
                return it->second.second;
            };
            LoopState& st = states[&x];
            for (const auto& s : escapes) {
                auto d = defs.find(s);
                const std::string ty = d == defs.end() ? std::string() : ResultType(*d->second);
                if (!CarryType(ty)) {
                    r.error = "Proceed loop state " + s + " is of type " +
                              (ty.empty() ? std::string("None") : ty) +
                              "; the payload carries i32, float, i1, half, i16, i64 and double";
                    return false;
                }
                st.outs.push_back({s, ty, field(s + ".out", ty)});
            }
            st.pcount = field("%rq.pc" + std::to_string(x.qid) + ".out", "i32");
            if (CarryWords(carryFields) > kMaxCarry) {
                r.error = "the Proceed loops carry " + std::to_string(CarryWords(carryFields)) +
                          " values between them; more than the " + std::to_string(kMaxCarry) +
                          " the payload carries";
                return false;
            }
            for (const auto& b : fn.blocks)
                for (const auto& i : b.instrs)
                    if (!i.result.empty()) st.defBlock[i.result] = b.label;
            st.dom = fn.Dominators();
        } else
        {
            const llm::Block* hdr = fn.FindBlock(x.top ? x.loop.latch : x.loop.header);
            std::vector<const llm::Instr*> phis;
            if (hdr)
                for (const auto& i : hdr->instrs)
                    if (i.isPhi) phis.push_back(&i);
            if (!phis.empty() || !escapes.empty() || !x.exits.empty()) {
                if (x.NeedsBoth() && (!phis.empty() || !escapes.empty())) {
                    std::string names;
                    for (const auto* p : phis) names += (names.empty() ? "" : ", ") + p->result;
                    for (const auto& s : escapes) names += (names.empty() ? "" : ", ") + s;
                    r.error = "Proceed loop carries state across candidates or out of the loop (" +
                              names + ") and becomes an intersection shader, which has no payload "
                              "to carry it in";
                    return false;
                }
                std::map<std::string, const llm::Instr*> defs;
                for (const auto& b : fn.blocks)
                    for (const auto& i : b.instrs)
                        if (!i.result.empty()) defs[i.result] = &i;
                const llm::Block* lat = fn.FindBlock(x.loop.latch);
                for (const auto& i : lat->instrs)
                    if (!i.isPhi && i.DxOp() != kProceed && i.body.rfind("br ", 0) != 0) {
                        r.error = "the Proceed loop latch computes " +
                                  (i.result.empty() ? i.body.substr(0, 40) : i.result) +
                                  " itself; the any-hit shader has no latch to compute it in";
                        return false;
                    }
                std::string terr;
                auto typed = [&](const std::string& name) -> std::string {
                    auto d = defs.find(name);
                    const std::string ty = d == defs.end() ? std::string() : ResultType(*d->second);
                    if (!CarryType(ty))
                        terr = "Proceed loop state " + name + " is of type " +
                               (ty.empty() ? std::string("None") : ty) +
                               "; the payload carries i32, float, i1, half, i16, i64 and double";
                    return ty;
                };
                auto field = [&](const std::string& key, const std::string& ty) {
                    auto it = carryFields.find(key);
                    if (it == carryFields.end())
                        it = carryFields.emplace(key, std::make_pair(ty, carryBase + CarryWords(carryFields))).first;
                    return it->second.second;
                };
                LoopState& st = states[&x];
                for (const auto* p : phis) {
                    const std::string ty = typed(p->result);
                    if (!terr.empty()) { r.error = terr; return false; }
                    std::vector<std::string> inits, nexts;
                    if (x.top) {
                        // Top-tested (0.72.0): the phi is the state itself.
                        for (const auto& in : p->phiIncoming)
                            if (!x.loop.body.count(in.second)) inits.push_back(Trim(in.first));
                        nexts.push_back(p->result);
                    } else {
                        for (const auto& in : p->phiIncoming)
                            (in.second == x.loop.latch ? nexts : inits).push_back(Trim(in.first));
                    }
                    if (inits.size() != 1 || nexts.size() != 1) {
                        r.error = "Proceed loop header phi " + p->result + " has an unexpected shape";
                        return false;
                    }
                    st.phis.push_back({p->result, ty, field(p->result, ty), inits[0], nexts[0]});
                }
                std::map<std::string, int> stateField;
                for (const auto& p : st.phis) stateField[p.name] = p.field;
                for (const auto& s : escapes) {
                    const std::string ty = typed(s);
                    if (!terr.empty()) { r.error = terr; return false; }
                    if (x.top && stateField.count(s))
                        st.souts.push_back({s, ty, stateField[s]});
                    else
                        st.outs.push_back({s, ty, field(s + ".out", ty)});
                }
                if (!x.exits.empty()) {
                    // Which way out (0.72.0): 0 through the latch, k through exit k.
                    st.exitField = field("%rq.ex" + std::to_string(x.qid) + ".out", "i32");
                    for (size_t k = 0; k < x.exits.size(); ++k)
                        st.exits.emplace_back(x.exits[k].first, x.exits[k].second, (int)k + 1);
                }
                if (!x.top) st.iter = field("%rq.iter", "i32");
                if (CarryWords(carryFields) > kMaxCarry) {
                    r.error = "the Proceed loops carry " + std::to_string(CarryWords(carryFields)) +
                              " values between them; more than the " + std::to_string(kMaxCarry) +
                              " the payload carries";
                    return false;
                }
                for (const auto& b : fn.blocks)
                    for (const auto& i : b.instrs)
                        if (!i.result.empty()) st.defBlock[i.result] = b.label;
                st.dom = fn.Dominators();
                for (const auto& i : lat->instrs) {
                    if (!i.isPhi) continue;
                    auto& mp = st.latchPhis[i.result];
                    for (const auto& in : i.phiIncoming) mp[in.second] = Trim(in.first);
                }
                // The guard: the Proceed before the loop, whose other edge
                // skips it. A top-tested loop has none.
                std::string pre;
                const llm::Block* guard = nullptr;
                if (!x.top) {
                for (const auto& p : x.proceeds)
                    if (p.first->label != x.loop.latch) { guard = p.first; break; }
                if (guard)
                    for (const auto& s : guard->Successors()) {
                        if (x.loop.body.count(s)) continue;
                        const llm::Block* blk = fn.FindBlock(s);
                        if (blk) {
                            auto succ = blk->Successors();
                            if (succ.size() == 1 && succ[0] == x.loop.header) { pre = s; break; }
                        }
                    }
                if (!guard) {
                    r.error = "Proceed loop carries state but has no guard Proceed";
                    return false;
                }
                std::vector<std::string> others;
                for (const auto& s : guard->Successors())
                    if (!x.loop.body.count(s) && s != pre) others.push_back(s);
                if (others.size() != 1) {
                    r.error = "the guard Proceed has an unexpected shape";
                    return false;
                }
                st.guardFalse = others[0];
                st.guard = guard->label;
                }
                // The loop's exit block now follows the trace block; with
                // exits (0.72.0), the blocks the trace block routes through.
                // See _edit_exit_phis in lower.py.
                // The block the routing ends up in: the guard's (0.75.0).
                std::string tb = (x.routeBlock ? x.routeBlock : x.traceBlock)->label;
                if (!x.top)
                    for (const auto& gp : x.proceeds)
                        if (gp.first->label != x.loop.latch) { tb = gp.first->label; break; }
                std::vector<std::tuple<std::string, std::string, std::string>> ren;
                if (!st.exits.empty()) {
                    const std::string norm = "%" + x.pfx + ".xn";
                    ren.emplace_back(exitLabel, x.loop.latch, norm);
                    if (!st.guardFalse.empty()) ren.emplace_back(st.guardFalse, st.guard, norm);
                    for (const auto& ex : st.exits)
                        ren.emplace_back(std::get<1>(ex), std::get<0>(ex),
                                         "%" + x.pfx + ".x" + std::to_string(std::get<2>(ex)));
                } else {
                    ren.emplace_back(exitLabel, x.loop.latch, tb);
                }
                for (const auto& rn : ren) {
                    const llm::Block* ex = fn.FindBlock(std::get<0>(rn));
                    const std::regex kIn("\\" + std::get<1>(rn) + "(\\s*\\])");
                    for (const auto& i : ex->instrs) {
                        std::string line = i.line;
                        auto ed = edits.find(i.index);
                        if (ed != edits.end()) {
                            if (!ed->second.has_value()) continue;
                            line = *ed->second;
                        }
                        if (!i.isPhi || !std::regex_search(line, kIn)) continue;
                        if (std::get<2>(rn).empty()) {
                            r.error = "the Proceed loop exit has a phi and the trace is in the entry "
                                      "block, which has no label to name";
                            return false;
                        }
                        edits[i.index] = std::regex_replace(line, kIn, std::get<2>(rn) + "$1");
                    }
                }
            }
        }

        if (x.track) {
            if (!carryFields.count("%rq.oflags"))
                carryFields.emplace("%rq.oflags", std::make_pair(std::string("i32"),
                                                                 carryBase + CarryWords(carryFields)));
            if (CarryWords(carryFields) > kMaxCarry) {
                r.error = "the Proceed loops carry " + std::to_string(CarryWords(carryFields)) +
                          " values between them; more than the " + std::to_string(kMaxCarry) +
                          " the payload carries";
                return false;
            }
        }

        // A Proceed loop body that WRITES is not transplantable.
        //
        // The check above guards what the body READS. Nothing guarded what it
        // writes, and the two are not the same question. The any-hit shader
        // runs a different number of times than the loop body does, by design:
        // with RAY_FLAG_FORCE_OPAQUE no candidate is ever yielded so it never
        // runs at all, and with FORCE_NON_OPAQUE it runs once per candidate in
        // an implementation-defined order. So a store, an append or an atomic
        // in that body means something different after lowering, silently.
        //
        // Found on RayTracingDebugMainCS, a real Unreal shader that appends a
        // debug record per candidate. It lowered, validated, signed, and then
        // killed the device inside CreateStateObject. Whether the write is
        // also what the driver choked on is NOT established, and this refusal
        // does not rest on it.
        //
        // DXC marks every dx.op declaration with one of three attribute
        // groups and the module says which is which: readnone is pure,
        // readonly loads, a bare nounwind WRITES. Reading that beats a
        // hand-kept list of store opcodes, which would be incomplete the day
        // DXIL grows another one. The numbering is resolved per module rather
        // than assumed, see the attribute group bug in 0.26.0.
        //
        // LIMIT, stated rather than hidden: this reads dx.op calls. A plain
        // `store` reaches only an alloca or groupshared in practice, and
        // groupshared around a query is already refused.
        {
            std::map<std::string, std::string> groupText;
            static const std::regex kAttrLine(
                R"(^attributes\s+#(\d+)\s*=\s*\{(.*)\}\s*$)");
            static const std::regex kDeclLine(
                R"(^declare\s+.*@([\w.$]+)\s*\(.*\)\s*#(\d+)\s*$)");
            std::map<std::string, std::string> byNum;
            std::smatch mm;
            for (const auto& line : m.lines)
                if (std::regex_match(line, mm, kAttrLine))
                    byNum[mm[1].str()] = mm[2].str();
            for (const auto& line : m.lines)
                if (std::regex_match(line, mm, kDeclLine)) {
                    auto it = byNum.find(mm[2].str());
                    groupText[mm[1].str()] = it == byNum.end() ? "" : it->second;
                }
            // One exception, 0.41.0: an APPEND, a counter update and stores
            // indexed by the value it returned. Order-independent, and the
            // proxy makes the COUNT equal by setting NO_DUPLICATE_ANYHIT_
            // INVOCATION on every bottom-level geometry. See the Python.
            std::vector<const llm::Instr*> writes;
            for (const auto& b : fn.blocks) {
                if (!x.loop.body.count(b.label)) continue;
                for (const auto& i : b.instrs) {
                    if (i.callee.compare(0, 6, "dx.op.") != 0) continue;
                    // The RayQuery ops are mutators too and are all declared
                    // nounwind. They are not transplanted, they are REWRITTEN,
                    // and the analysis already refuses an opcode it does not
                    // know.
                    if (i.callee.compare(0, 15, "dx.op.rayQuery_") == 0 ||
                        i.callee == "dx.op.allocateRayQuery")
                        continue;
                    auto g = groupText.find(i.callee);
                    if (g != groupText.end() &&
                        (g->second.find("readnone") != std::string::npos ||
                         g->second.find("readonly") != std::string::npos))
                        continue;
                    writes.push_back(&i);
                }
            }
            // Any write, 0.67.0, and in an intersection shader too, 0.68.0
            // (the spec: no redundant intersection invocations with the
            // NO_DUPLICATE_ANYHIT_INVOCATION flag the proxy sets on every
            // geometry). See _check_loop_side_effects in lower.py. Refused:
            // a write beside an Abort(), which an intersection shader cannot
            // act on.
            if (!writes.empty() && x.NeedsBoth() && !x.aborts.empty() && !x.trackProc) {
                r.error = "Proceed loop body writes (" + writes[0]->callee +
                          ") and calls Abort() and becomes an intersection shader, which "
                          "cannot stop traversal, so the write would happen for procedural "
                          "candidates met after the abort";
                return false;
            }
            r.appends = !writes.empty();
        }

        // The block the guard branches through to reach the loop, if any.
        for (const auto& p : x.proceeds) {
            if (p.first->label == x.loop.latch) continue;
            for (const auto& s : p.first->Successors()) {
                if (x.loop.body.count(s)) continue;
                const llm::Block* blk = fn.FindBlock(s);
                if (blk) {
                    auto succ = blk->Successors();
                    if (succ.size() == 1 && succ[0] == x.loop.header) preheader = s;
                }
            }
            break;
        }
    }

    // --- payload init plus the TraceRay that replaces the inline query ------
    {
        std::vector<std::string> lines;
        const struct { int idx; const char* ty; int align; } init[] = {
            { 0, "float", 8 }, { 1, "<2 x float>", 4 }, { 2, "i32", 4 },
            { 3, "i32", 4 }, { 4, "i32", 4 }, { 5, "i32", 4 },
            { 6, "i32", 4 }, { 7, "i32", 4 }, { 8, "[12 x float]", 4 },
            { 9, "i32", 4 }, { 10, "i32", 4 },
        };
        for (const auto& f : init) {
            std::string zero =
                std::string(f.ty) == "float" ? "0.000000e+00"
                : ((f.ty[0] == '<' || f.ty[0] == '[') ? "zeroinitializer" : "0");
            if (f.idx == 0 && (x.track || x.proceeds.empty())) {
                // CommittedRayT() is TMax until something is committed.
                const std::string tmx = x.RayArgs()[8];
                const std::string on = llm::OperandName(tmx);
                zero = on.empty() ? tmx.substr(tmx.find_last_of(' ') + 1) : on;
            }
            lines.push_back("  %" + x.pfx + ".pl" + std::to_string(f.idx) +
                            " = getelementptr inbounds " + kPayload + ", " + kPayload +
                            "* %" + x.pfx + ".pl, i32 0, i32 " + std::to_string(f.idx));
            lines.push_back(std::string("  store ") + f.ty + " " + zero + ", " + f.ty +
                            "* %" + x.pfx + ".pl" + std::to_string(f.idx) + ", align " +
                            std::to_string(f.align));
        }
        if (multi) {
            // Which query this trace serves, read by the one any-hit they share.
            lines.push_back("  %" + x.pfx + ".plq = getelementptr inbounds " + kPayload +
                            ", " + kPayload + "* %" + x.pfx + ".pl, i32 0, i32 " +
                            std::to_string(kQidField));
            // DXR_TIER11_RETRACE_POISON=1 (0.73.0): every trace of a query
            // traced more than once runs the first trace's loop body. The
            // retrace cases must diverge (C++ only).
            static const bool retracePoison = std::getenv("DXR_TIER11_RETRACE_POISON") != nullptr;
            const int qidStored = (retracePoison && x.segmented) ? q.qid : x.qid;
            lines.push_back("  store i32 " + std::to_string(qidStored) + ", i32* %" + x.pfx +
                            ".plq, align 4");
        }
        for (const auto& c : qcarry) {
            const std::string f = std::to_string(c.field);
            lines.push_back("  %" + x.pfx + ".plc" + f + " = getelementptr inbounds " + kPayload + ", " +
                            kPayload + "* %" + x.pfx + ".pl, i32 0, i32 " + f);
            if (c.ty == "i1") {
                lines.push_back("  %" + x.pfx + ".plz" + f + " = zext i1 " + c.name + " to i32");
                lines.push_back("  store i32 %" + x.pfx + ".plz" + f + ", i32* %" + x.pfx + ".plc" + f + ", align 4");
            } else if (OtherCarry(c.ty)) {
                for (const auto& l : CarryStore(c.ty, c.name, c.field, "%" + x.pfx + ".plc" + f, "%" + x.pfx + ".pl"))
                    lines.push_back(l);
            } else {
                lines.push_back("  store " + c.ty + " " + c.name + ", " + c.ty + "* %" + x.pfx + ".plc" +
                                f + ", align 4");
            }
        }
        auto sit = states.find(&x);
        const LoopState* st = sit == states.end() ? nullptr : &sit->second;
        if (st) {
            // Loop state (0.65.0): each header phi starts at its value from
            // before the loop, and the iteration flag at 0.
            for (const auto& p : st->phis) {
                const std::string f = std::to_string(p.field);
                lines.push_back("  %" + x.pfx + ".plc" + f + " = getelementptr inbounds " + kPayload + ", " +
                                kPayload + "* %" + x.pfx + ".pl, i32 0, i32 " + f);
                if (p.ty == "i1") {
                    lines.push_back("  %" + x.pfx + ".plz" + f + " = zext i1 " + p.init + " to i32");
                    lines.push_back("  store i32 %" + x.pfx + ".plz" + f + ", i32* %" + x.pfx + ".plc" + f + ", align 4");
                } else if (OtherCarry(p.ty)) {
                    for (const auto& l : CarryStore(p.ty, p.init, p.field, "%" + x.pfx + ".plc" + f, "%" + x.pfx + ".pl"))
                        lines.push_back(l);
                } else {
                    lines.push_back("  store " + p.ty + " " + p.init + ", " + p.ty + "* %" + x.pfx + ".plc" + f +
                                    ", align 4");
                }
            }
            for (int fi : { st->iter, st->exitField, st->pcount }) {
                if (fi < 0) continue;
                const std::string f = std::to_string(fi);
                lines.push_back("  %" + x.pfx + ".plc" + f + " = getelementptr inbounds " + kPayload + ", " +
                                kPayload + "* %" + x.pfx + ".pl, i32 0, i32 " + f);
                lines.push_back("  store i32 0, i32* %" + x.pfx + ".plc" + f + ", align 4");
            }
        }
        const auto ra = x.RayArgs();
        // The RayFlags operand, which may be a runtime value. The OR that
        // combines it with the template's flags has to land before the call.
        auto flags = x.FlagsOperand();
        if (!flags.first.empty()) lines.push_back(flags.first);
        if (x.track) {
            // The original flags go to the any-hit, which applies the opacity
            // ones itself; the ray is traced forced non-opaque (0.66.0).
            const std::string f = std::to_string(carryFields.at("%rq.oflags").second);
            const std::string v = flags.second.substr(flags.second.find_last_of(' ') + 1);
            lines.push_back("  %" + x.pfx + ".plc" + f + " = getelementptr inbounds " + kPayload + ", " +
                            kPayload + "* %" + x.pfx + ".pl, i32 0, i32 " + f);
            lines.push_back("  store i32 " + v + ", i32* %" + x.pfx + ".plc" + f + ", align 4");
            static const std::regex kInt(R"(^-?\d+$)");
            // DXR_TIER11_ABORT_POISON=1: an aborting query keeps its own flags,
            // so opaque candidates skip the any-hit and traversal commits them
            // after an Abort(), as before 0.67.0. The abort cases must diverge.
            static const bool abortPoison = std::getenv("DXR_TIER11_ABORT_POISON") != nullptr;
            if (abortPoison && !x.aborts.empty()) {
            } else if (std::regex_match(v, kInt)) {
                flags.second = "i32 " + std::to_string((int)(((unsigned)std::stoll(v) & ~0xC3u) | 2u));
            } else {
                lines.push_back("  %" + x.pfx + ".tf0 = and i32 " + v + ", -196");
                lines.push_back("  %" + x.pfx + ".tf = or i32 %" + x.pfx + ".tf0, 2");
                flags.second = "i32 %" + x.pfx + ".tf";
            }
        }
        std::string mask = ra[0];
        if (!x.preAborts.empty()) {
            // Abort() before the search (0.75.0): see _trace_block in lower.py.
            // DXR_TIER11_PREABORT_POISON=1: the flag is never read, so an
            // aborted query searches anyway. The cases must diverge (C++ only).
            static const bool preAbortPoison = std::getenv("DXR_TIER11_PREABORT_POISON") != nullptr;
            lines.push_back("  store i32 0, i32* %" + x.pfx + ".rab, align 4");
            if (!x.proceeds.empty()) {
                const std::string on = llm::OperandName(ra[0]);
                const std::string mv = on.empty() ? ra[0].substr(ra[0].find_last_of(' ') + 1) : on;
                lines.push_back("  %" + x.pfx + ".rav = load i32, i32* %" + x.pfx + ".rab, align 4");
                lines.push_back("  %" + x.pfx + ".rac = icmp ne i32 %" + x.pfx + ".rav, 0");
                lines.push_back("  %" + x.pfx + ".ram = select i1 %" + x.pfx + ".rac, i32 " +
                                (preAbortPoison ? mv : std::string("0")) + ", i32 " + mv);
                mask = "i32 %" + x.pfx + ".ram";
            }
        }
        std::ostringstream tr;
        tr << "  call void @dx.op.traceRay." << std::string(kPayload).substr(1)
           << "(i32 157, %dx.types.Handle " << x.AsHandle() << ", " << flags.second
           << ", " << mask << ", i32 0, i32 "
           << (x.needsRecordConstants ? 1 : 0)
           << ", i32 0, " << ra[1] << ", " << ra[2]
           << ", " << ra[3] << ", " << ra[4] << ", " << ra[5] << ", " << ra[6]
           << ", " << ra[7] << ", " << ra[8] << ", " << kPayload
           << "* nonnull %" + x.pfx + ".pl)  ; TraceRay(...)";
        // Never asked to Proceed(), so never searched: no TraceRay (0.74.0).
        if (!x.proceeds.empty()) lines.push_back(tr.str());
        if (st) {
            // What left the loop, as the last iteration stored it; then the
            // guard's two ways on, by whether any iteration ran.
            std::vector<StateOut> loads = st->outs;
            loads.insert(loads.end(), st->souts.begin(), st->souts.end());
            for (size_t li = 0; li < loads.size(); ++li) {
                const auto& o = loads[li];
                // A top-tested loop's state field has its GEP from the start
                // stores already (0.76.0: it was defined twice).
                const std::string c = li < st->outs.size() ? "plc" : "pls";
                const std::string f = std::to_string(o.field);
                lines.push_back("  %" + x.pfx + "." + c + f + " = getelementptr inbounds " + kPayload + ", " +
                                kPayload + "* %" + x.pfx + ".pl, i32 0, i32 " + f);
                if (o.ty == "i1") {
                    lines.push_back("  %" + x.pfx + ".plv" + f + " = load i32, i32* %" + x.pfx + "." + c + f +
                                    ", align 4");
                    lines.push_back("  " + o.name + " = icmp ne i32 %" + x.pfx + ".plv" + f + ", 0");
                } else if (OtherCarry(o.ty)) {
                    for (const auto& l : CarryLoad(o.ty, o.name, o.field, "%" + x.pfx + "." + c + f, "%" + x.pfx + ".pl"))
                        lines.push_back(l);
                } else {
                    lines.push_back("  " + o.name + " = load " + o.ty + ", " + o.ty + "* %" + x.pfx + "." + c +
                                    f + ", align 4");
                }
            }
            if (!st->exits.empty()) {
                // Which way the loop was left (0.72.0): through exit k, the
                // raygen goes on where that edge went.
                const std::string& p = x.pfx;
                lines.push_back("  %" + p + ".ex = load i32, i32* %" + p + ".plc" +
                                std::to_string(st->exitField) + ", align 4");
                for (const auto& ex : st->exits) {
                    const std::string k = std::to_string(std::get<2>(ex));
                    lines.push_back("  %" + p + ".xc" + k + " = icmp eq i32 %" + p + ".ex, " + k);
                    lines.push_back("  br i1 %" + p + ".xc" + k + ", label %" + p + ".x" + k +
                                    ", label %" + p + ".xk" + k);
                    lines.push_back("");
                    lines.push_back(p + ".x" + k + ":");
                    lines.push_back("  br label " + std::get<1>(ex));
                    lines.push_back("");
                    lines.push_back(p + ".xk" + k + ":");
                }
                lines.push_back("  br label %" + p + ".xn");
                lines.push_back("");
                lines.push_back(p + ".xn:");
            }
            if (st->iter >= 0) {
                lines.push_back("  %" + x.pfx + ".it = load i32, i32* %" + x.pfx + ".plc" +
                                std::to_string(st->iter) + ", align 4");
                lines.push_back("  %" + x.pfx + ".itc = icmp ne i32 %" + x.pfx + ".it, 0");
                lines.push_back("  br i1 %" + x.pfx + ".itc, label " + exitLabel + ", label " +
                                st->guardFalse);
            } else if (!exitLabel.empty()) {
                lines.push_back("  br label " + exitLabel);
            }
        } else if (!exitLabel.empty()) {
            lines.push_back("  br label " + exitLabel);
        }
        std::string joined;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) joined += "\n";
            joined += lines[i];
        }
        // Where traversal starts (0.72.0): at the first Proceed, so what the
        // shader does between it and TraceRayInline happens before any
        // candidate is met. See _edit_query in lower.py.
        const llm::Instr* route = nullptr;
        if (x.hasLoop && x.top) {
            // Top-tested: the trace's block branches into the loop's header,
            // which goes with the loop; the trace block now routes.
            const llm::Instr* term = x.traceBlock->Terminator();
            if (!term || Trim(term->body) != "br label " + x.loop.latch) {
                r.error = "the trace does not branch straight into the Proceed loop "
                          "tested at its top";
                return false;
            }
            route = term;
        } else if (x.hasLoop) {
            static const std::regex kBr(R"(^\s*br\s+i1\s+(\S+),)");
            for (const auto& p : x.proceeds) {
                const llm::Instr* term = p.first->Terminator();
                // The guard's block, wherever it is (0.75.0).
                if (p.first->label == x.loop.latch ||
                    (x.routeBlock && p.first != x.routeBlock) || !term ||
                    term == p.second)
                    continue;
                std::smatch bm;
                if (std::regex_search(term->line, bm, kBr) && bm[1].str() == p.second->result)
                    route = term;
            }
        }
        if (!x.chain.empty()) {
            // Proceed() without a loop (0.74.0): see _edit_chain in lower.py.
            const std::string& p = x.pfx;
            static const std::regex kChainBr(R"(^(\s*br i1 \S+, label )(\S+)(, label \S+\s*)$)");
            static const std::regex kPhiTy(R"(^\s*\S+\s*=\s*phi\s+(\S+))");
            for (size_t k = 0; k < x.chain.size(); ++k) {
                const auto& c = x.chain[k];
                if (c.entry.empty()) continue;
                const std::string fwd = "%" + p + ".pk" + std::to_string(k);
                const llm::Instr* term = c.block->Terminator();
                std::string line = term->line;
                auto ed = edits.find(term->index);
                if (ed != edits.end() && ed->second.has_value()) line = *ed->second;
                std::smatch bm;
                if (!std::regex_match(line, bm, kChainBr) || bm[2].str() != c.entry) {
                    r.error = "a Proceed() without a loop with an unexpected branch";
                    return false;
                }
                std::string tail3 = bm[3].str();
                while (!tail3.empty() && (tail3.back() == ' ' || tail3.back() == '\t')) tail3.pop_back();
                const std::string nxt = c.exit.empty() ? "  ret void" : "  br label " + c.exit;
                edits[term->index] = bm[1].str() + fwd + tail3 + "\n\n" + fwd.substr(1) + ":\n" + nxt;
                if (c.exit.empty()) continue;
                std::set<std::string> leaving;
                for (const auto& e : c.exits) leaving.insert(e.first);
                for (const auto& i : fn.FindBlock(c.exit)->instrs) {
                    if (!i.isPhi) continue;
                    std::vector<std::pair<std::string, std::string>> ents;
                    std::map<std::string, std::string> vals;
                    for (const auto& in : i.phiIncoming) {
                        const std::string v = Trim(in.first);
                        if (leaving.count(in.second)) {
                            if (vals.count(fwd) && vals[fwd] != v) {
                                r.error = "the body of a Proceed() without a loop leaves it from two "
                                          "blocks with different values for " + i.result;
                                return false;
                            }
                            if (!vals.count(fwd)) { vals[fwd] = v; ents.emplace_back(v, fwd); }
                        } else {
                            ents.emplace_back(v, in.second);
                        }
                    }
                    std::string cur = i.line;
                    auto e2 = edits.find(i.index);
                    if (e2 != edits.end() && e2->second.has_value()) cur = *e2->second;
                    std::smatch pm;
                    std::regex_search(cur, pm, kPhiTy);
                    std::string phi = "  " + i.result + " = phi " + pm[1].str() + " ";
                    for (size_t n = 0; n < ents.size(); ++n) {
                        if (n) phi += ", ";
                        phi += "[ " + ents[n].first + ", " + ents[n].second + " ]";
                    }
                    edits[i.index] = phi;
                }
            }
            const size_t cut = TraceCut(joined, x.pfx);
            std::string head = joined.substr(0, cut);
            while (!head.empty() && head.back() == '\n') head.pop_back();
            edits[x.trace->index] = head;
            std::string call = joined.substr(cut) + "\n  %" + p + ".pc = load i32, i32* %" + p +
                               ".plc" + std::to_string(states.at(&x).pcount) + ", align 4";
            for (size_t k = 0; k < x.chain.size(); ++k) {
                const llm::Instr* i = x.chain[k].instr;
                std::string res;
                if (!i->result.empty())
                    res = "  " + i->result + " = icmp ugt i32 %" + p + ".pc, " + std::to_string(k);
                if (k == 0) {
                    if (!res.empty()) call += "\n" + res;
                    edits[i->index] = call;
                } else if (!res.empty()) {
                    edits[i->index] = res;
                } else {
                    edits[i->index] = std::nullopt;
                }
            }
        } else if (x.routeBlock) {
            // A search a branch chooses the ray of (0.73.0): the traces go,
            // the phis open the block where the search starts, and all of it
            // is there.
            if (!route) {
                r.error = "a query traced on either arm of a branch, where its guard Proceed "
                          "does not branch on its own result";
                return false;
            }
            for (const auto& t : x.traces) edits[t.second->index] = std::nullopt;
            std::string head = m.lines[x.routeBlock->index];
            for (const auto& ph : x.tracePhis) head += "\n" + ph;
            edits[x.routeBlock->index] = head;
            edits[route->index] = joined;
        } else if (route) {
            const size_t cut = TraceCut(joined, x.pfx);
            std::string head = joined.substr(0, cut);
            while (!head.empty() && head.back() == '\n') head.pop_back();
            edits[x.trace->index] = head;
            edits[route->index] = joined.substr(cut);
        } else {
            edits[x.trace->index] = joined;
        }
        if (x.chain.empty() && !x.hasLoop && !x.preAborts.empty() && !x.proceeds.empty()) {
            // One bare Proceed() with an Abort() before it (0.75.0).
            const size_t cut = TraceCut(joined, x.pfx);
            std::string head = joined.substr(0, cut);
            while (!head.empty() && head.back() == '\n') head.pop_back();
            edits[x.trace->index] = head;
            bareRoute = joined.substr(cut);
        }
        // Abort() outside the loop (0.75.0): before the search it sets the
        // flag, after it changes nothing.
        for (const auto& a : x.preAborts)
            edits[a.second->index] = "  store i32 1, i32* %" + x.pfx + ".rab, align 4";
        for (const auto& a : x.postAborts) edits[a.second->index] = std::nullopt;
    }

    // --- Proceed calls vanish ----------------------------------------------
    // (A chain of them is edited above, 0.74.0.)
    for (const auto& p : x.proceeds) {
        if (!x.chain.empty()) break;
        edits[p.second->index] = std::nullopt;
        if (x.hasLoop && p.first->label == x.loop.latch) continue;
        const llm::Instr* term = p.first->Terminator();
        if (term && term != p.second && !edits.count(term->index)) {
            static const std::regex kBr(R"(^\s*br\s+i1\s+(\S+),)");
            std::smatch bm;
            if (std::regex_search(term->line, bm, kBr) && bm[1].str() == p.second->result)
                edits[term->index] = std::nullopt;   // folded into the trace replacement
        }
    }
    if (!bareRoute.empty()) edits[x.proceeds[0].second->index] = bareRoute;

    // --- the loop blocks go --------------------------------------------------
    if (x.hasLoop) {
        std::set<std::string> gone = x.loop.body;
        if (!preheader.empty()) gone.insert(preheader);
        for (const auto& lbl : gone) {
            const llm::Block* b = fn.FindBlock(lbl);
            if (!b) continue;
            if (b->index >= 0) edits[b->index] = std::nullopt;
            for (const auto& i : b->instrs) edits[i.index] = std::nullopt;
        }
    }

    // --- RayFlags read in the RAYGEN becomes the constant it must be --------
    //
    // dx.op.rayFlags is legal in a hit or miss shader, not in a raygen, so the
    // loop-body uses go through kCandidateMap and these do not. The value is
    // the same one the lowering hands TraceRay, and the analysis refuses a
    // query whose flags are not a compile-time constant, so there is nothing to
    // look up at runtime.
    //
    // An SSA name needs an instruction to define it, hence `add N, 0`, which
    // the assembler folds away.
    for (const auto& c : x.candidateOps) {
        const llm::Instr& i = *c.second;
        if (i.DxOp() != kRayFlags) continue;
        // A read inside the loop already has an edit: the whole block is marked
        // for deletion and the body is re-emitted into the any-hit, where the
        // intrinsic IS legal. Overwriting that would resurrect it in the raygen
        // as well.
        if (edits.count(i.index)) {
            // With `track` the any-hit reads the query's flags from the
            // payload instead (0.66.0).
            if (!x.track) x.rayFlagsInLoop = true;
            continue;
        }
        if (x.StaticFlags()) {
            edits[i.index] = "  " + i.result + " = add i32 " +
                             std::to_string(x.RayFlags()) + ", 0";
        } else {
            // RayFlags() can only be called after TraceRayInline, so the
            // runtime operand dominates this point.
            edits[i.index] = "  " + i.result + " = or i32 " +
                             llm::OperandName(x.Args()[3]) + ", " +
                             std::to_string(x.constFlags);
        }
    }

    // --- committed accessors become payload reads ---------------------------
    for (const auto& c : x.committedOps) {
        const llm::Instr& i = *c.second;
        if (x.hasLoop && x.loop.body.count(c.first->label)) continue;   // a generated shader's, 0.66.0
        const auto f = PayloadField(i.DxOp());
        if (!f) continue;
        if (i.DxOp() == kCommittedFrontFace) {
            const std::string tmp = "%rq.ff" + i.result.substr(1);
            edits[i.index] = "  " + tmp + " = load i32, i32* %" + x.pfx + ".pl7, align 4\n  " +
                i.result + " = icmp eq i32 " + tmp + ", " +
                std::to_string(kHitKindFront);
        } else if (i.DxOp() == kCommittedWorldToObject) {
            static const std::regex kRowRe(R"(i32\s+(\d+))");
            static const std::regex kColRe(R"(i8\s+(\d+))");
            std::smatch rm2, cm2;
            std::string ar = Trim(i.args[2]), ac = Trim(i.args[3]);
            std::regex_search(ar, rm2, kRowRe);
            std::regex_search(ac, cm2, kColRe);
            const int slot = std::stoi(rm2[1].str()) * 4 + std::stoi(cm2[1].str());
            const std::string ptr = "%rq.w" + i.result.substr(1);
            edits[i.index] =
                "  " + ptr + " = getelementptr inbounds [12 x float], [12 x float]* "
                "%" + x.pfx + ".pl8, i32 0, i32 " + std::to_string(slot) + "\n  " +
                i.result + " = load float, float* " + ptr + ", align 4";
        } else if (i.DxOp() == kCommittedBary) {
            static const std::regex kI8(R"(i8\s+(\d+))");
            std::smatch mm;
            std::string a2 = Trim(i.args[2]);
            std::regex_search(a2, mm, kI8);
            const std::string tmp = "%rq.bv" + i.result.substr(1);
            edits[i.index] = "  " + tmp +
                " = load <2 x float>, <2 x float>* %" + x.pfx + ".pl1, align 4\n  " + i.result +
                " = extractelement <2 x float> " + tmp + ", i32 " + mm[1].str();
        } else {
            edits[i.index] = "  " + i.result + " = load " + f->ty + ", " + f->ty +
                "* %" + x.pfx + ".pl" + std::to_string(f->idx) + ", align " +
                std::to_string(f->align);
        }
    }

    return true;
    };
    for (const Query* qp : queries)
        if (!editQuery(*qp)) return r;
    {
        std::vector<std::pair<int, std::pair<std::string, std::string>>> byField;
        for (const auto& kv : carryFields)
            byField.push_back({kv.second.second, {kv.first, kv.second.first}});
        std::sort(byField.begin(), byField.end());
        for (const auto& bf : byField)
            carry.push_back({bf.second.first, bf.second.second, bf.first});
    }

    edits[fn.index] = "define void @" + e.raygen + "() #RQNW {";
    bool anyLoop = false;
    for (const Query* qp : queries) anyLoop = anyLoop || qp->hasLoop;

    std::string out = llm::Render(m, edits);

    // --- types and globals --------------------------------------------------
    {
        std::vector<std::string> decls;
        std::string ptype = kPayloadType;
        ptype = ptype.substr(0, ptype.size() - 2);
        if (multi) ptype += ", i32";
        int carryWords = 0;
        for (const auto& c : carry) {
            for (const auto& sl : Slots(c.ty)) ptype += ", " + sl;
            carryWords += Words(c.ty);
        }
        for (int n = kPayloadBytes + 4 * ((multi ? 1 : 0) + carryWords);
             n < PayloadBytesFor(multi, carryWords); n += 4)
            ptype += ", i32";
        ptype += " }";
        decls.push_back(std::string(kPayload) + " = type " + ptype);
        decls.push_back(std::string(kAttrs) + " = type { <2 x float> }");
        if (q.needsRecordConstants) {
            // The raw buffer's element type, as DXC declares a ByteAddressBuffer.
            // ResRet is added only when the application's module lacks it.
            decls.push_back(std::string(kRecordType) + " = type { i32 }");
            if (out.find(std::string(kResRet) + " = type") == std::string::npos)
                decls.push_back(std::string(kResRet) + " = type { i32, i32, i32, i32, i32 }");
            if (sm66 && out.find("%dx.types.ResourceProperties = type") == std::string::npos)
                decls.push_back("%dx.types.ResourceProperties = type { i32, i32 }");
        }
        decls.push_back("");
        // At 6.6 the global holds a HANDLE and the record bitcasts it, as DXC's.
        if (q.needsRecordConstants)
            decls.push_back(std::string(kRecordGlobal) + " = external constant " +
                            (sm66 ? std::string(kHandleType) : std::string(kRecordType)) +
                            ", align 4");
        std::set<std::string> seen;
        for (const auto& g : globals) {
            if (!seen.insert(g.sym).second) continue;
            // In the binding form the global holds a HANDLE whatever the
            // resource is; the RECORD bitcasts it back to the resource type.
            // Copied from DXC: `@CB = external constant %dx.types.Handle`.
            decls.push_back("@" + g.sym + " = external constant " +
                            (binding ? BindingGty(g.gty, g.elem) : g.gty) +
                            ", align 4");
        }
        static const std::regex kAnchor(R"(%dx\.types\.Handle = type .*)");
        auto lines = llm::SplitLines(out);
        const int at = llm::FindLine(lines, kAnchor);
        if (at < 0) {
            r.error = "cannot find %dx.types.Handle to anchor declarations";
            return r;
        }
        std::string joined;
        for (size_t i = 0; i < decls.size(); ++i) {
            if (i) joined += "\n";
            joined += decls[i];
        }
        llm::InsertAfter(lines, at, joined);
        out = llm::JoinLines(lines);
    }

    // --- declarations --------------------------------------------------------
    {
        const char* kill[] = {
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.rayQuery_[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.allocateRayQuery[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandle\(i32, i8[^\n]*\n)",
            // also removes threadIdInGroup, which shares the prefix
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.threadId[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.groupId[^\n]*\n)",
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.flattenedThreadIdInGroup[^\n]*\n)",
            // The 6.6 handle creation is converted away, so its declare goes
            // too: an unused declare is itself a validation error.
            R"(\n; Function Attrs:[^\n]*\ndeclare [^\n]*@dx\.op\.createHandleFromBinding[^\n]*\n)",
        };
        for (const char* k : kill) out = std::regex_replace(out, std::regex(k), "\n");

        // DispatchRaysIndex replaces threadId, so a shader that never read
        // SV_DispatchThreadID never calls it, and an unused declare is itself
        // a validation error. See lower.py.
        bool usesThreadId = false;
        for (const auto& b : q.fn->blocks)
            for (const auto& i : b.instrs)
                if (IsThreadOp(i.DxOp())) usesThreadId = true;

        std::vector<std::string> add;
        if (usesThreadId) {
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readnone");
            add.push_back("declare i32 @dx.op.dispatchRaysIndex.i32(i32, i8) #RQNONE");
        }
        // Only where something is traced (0.74.0): see lower.py.
        bool traces = !q.proceeds.empty();
        for (const auto& o : q.others) traces = traces || !o.proceeds.empty();
        if (traces) {
            add.push_back("");
            add.push_back("; Function Attrs: nounwind");
            add.push_back("declare void @dx.op.traceRay." + std::string(kPayload).substr(1) +
                          "(i32, %dx.types.Handle, i32, i32, i32, i32, i32, float, float, float, "
                          "float, float, float, float, float, " + kPayload + "*) #RQNW");
        }
        const std::vector<std::string> rest = {
            "", "; Function Attrs: nounwind readonly",
            "declare float @dx.op.rayTCurrent.f32(i32) #RQRO",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.instanceIndex.i32(i32) #RQNONE",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.primitiveIndex.i32(i32) #RQNONE",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.instanceID.i32(i32) #RQNONE",
            "", "; Function Attrs: nounwind readnone",
            "declare i32 @dx.op.hitKind.i32(i32) #RQNONE",
        };
        add.insert(add.end(), rest.begin(), rest.end());
        // An UNUSED declare is itself a validation error, so these three are
        // emitted only when something calls them. The rest are always used,
        // because the generated closest-hit reads them every time.
        std::set<int> usedOps;
        bool rayFlagsInLoop = false;
        for (const Query* qp : queries) {
            for (const auto& c : qp->candidateOps) usedOps.insert(c.second->DxOp());
            for (const auto& c : qp->committedOps) usedOps.insert(c.second->DxOp());
            rayFlagsInLoop = rayFlagsInLoop || qp->rayFlagsInLoop;
        }
        auto addDecl = [&](bool want, const char* d) {
            if (!want) return;
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readnone");
            add.push_back(d);
        };
        // Only reached from a generated hit shader. A raygen read of RayFlags
        // became a constant, so a shader reading it only there declares
        // nothing, and an unused declare is itself a validation error.
        addDecl(rayFlagsInLoop, "declare i32 @dx.op.rayFlags.i32(i32) #RQNONE");
        addDecl(usedOps.count(kCandidateWorldToObject) ||
                usedOps.count(kCommittedWorldToObject),
                "declare float @dx.op.worldToObject.f32(i32, i32, i8) #RQNONE");
        addDecl(usedOps.count(kCandidateObjectRayOrigin) != 0,
                "declare float @dx.op.objectRayOrigin.f32(i32, i8) #RQNONE");
        addDecl(usedOps.count(kCandidateObjectRayDirection) != 0,
                "declare float @dx.op.objectRayDirection.f32(i32, i8) #RQNONE");

        if (q.needsRecordConstants && sm66) {
            // The binding form declares this overload itself, further down.
            if (!binding &&
                out.find("@dx.op.createHandleForLib.dx.types.Handle(") == std::string::npos) {
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readonly");
                add.push_back(std::string("declare ") + kHandleType +
                              " @dx.op.createHandleForLib.dx.types.Handle(i32, " +
                              kHandleType + ") #RQRO");
            }
            if (out.find("@dx.op.annotateHandle(") == std::string::npos) {
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readnone");
                add.push_back(std::string("declare ") + kHandleType +
                              " @dx.op.annotateHandle(i32, " + kHandleType +
                              ", %dx.types.ResourceProperties) #RQNONE");
            }
        } else if (q.needsRecordConstants) {
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readonly");
            add.push_back(std::string("declare %dx.types.Handle @") + kRecordHandleFn +
                          "(i32, " + kRecordType + ") #RQRO");
        }
        if (q.needsRecordConstants) {
            if (out.find("@dx.op.rawBufferLoad.i32(") == std::string::npos) {
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readonly");
                add.push_back(std::string("declare ") + kResRet +
                              " @dx.op.rawBufferLoad.i32"
                              "(i32, %dx.types.Handle, i32, i32, i8, i32) #RQRO");
            }
        }
        if (q.NeedsIntersection() || (q.trackProc && (!q.aborts.empty() || !q.exits.empty()))) {
            add.push_back("");
            add.push_back("; Function Attrs: nounwind");
            add.push_back("declare i1 @dx.op.reportHit." +
                          std::string(kAttrs).substr(1) + "(i32, float, i32, " +
                          kAttrs + "*) #RQNW");
        }
        // Unconditional now: the generated any-hit may or may not exist, but
        // AnyHitNull always does and it is nothing but an IgnoreHit.
        add.push_back("");
        add.push_back("; Function Attrs: noreturn nounwind");
        add.push_back("declare void @dx.op.ignoreHit(i32) #RQNR");
        {
            bool anyAbort = false;
            for (const Query* qp : queries)
                anyAbort = anyAbort ||
                           (qp->hasLoop && (!qp->aborts.empty() || !qp->exits.empty() || !qp->chain.empty()));
            if ((anyAbort && (!q.NeedsIntersection() || q.NeedsBoth())) || q.trackProc) {
                add.push_back("");
                add.push_back("; Function Attrs: noreturn nounwind");
                add.push_back("declare void @dx.op.acceptHitAndEndSearch(i32) #RQNR");
            }
        }
        if (binding) {
            // One overload serves every resource in the 6.6 form, because the
            // global is a handle whatever the resource is.
            add.push_back("");
            add.push_back("; Function Attrs: nounwind readonly");
            add.push_back(std::string("declare ") + kHandleType +
                          " @dx.op.createHandleForLib.dx.types.Handle(i32, " +
                          kHandleType + ") #RQRO");
        } else {
            std::set<std::string> seenFn;
            for (const auto& g : globals) {
                const std::string f = HandleFn(g.elem);
                if (!seenFn.insert(f).second) continue;
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readonly");
                add.push_back("declare %dx.types.Handle @" + f + "(i32, " + g.elem + ") #RQRO");
            }
        }
        const size_t anchor = out.find("\nattributes #0 =");
        std::string joined;
        for (size_t i = 0; i < add.size(); ++i) {
            if (i) joined += "\n";
            joined += add[i];
        }
        out = out.substr(0, anchor) + "\n" + joined + out.substr(anchor);

    }

    // --- generated shaders ---------------------------------------------------
    {
        std::vector<std::string> fns;
        // ONE loop body, TWO shaders when the query commits both kinds. The
        // substitution of CandidateType is what separates them: folded to
        // CANDIDATE_PROCEDURAL_PRIMITIVE the triangle arm dies and this is an
        // intersection shader, folded to CANDIDATE_NON_OPAQUE_TRIANGLE the
        // procedural arm dies and it is an any-hit. Each keeps the other's
        // dead arm as valid but unreachable IR.
        if (q.NeedsBoth()) {
            // The Proceed loop body re-rooted as an INTERSECTION shader. Same
            // body as the any-hit case, with one substitution changed: an
            // any-hit folds CandidateType to CANDIDATE_NON_OPAQUE_TRIANGLE (0)
            // and this folds it to CANDIDATE_PROCEDURAL_PRIMITIVE (1), so the
            // triangle branch dies and the procedural branch survives. One
            // body, two substitutions, two shaders.
            //
            // CommitProceduralPrimitiveHit becomes ReportHit. An intersection
            // shader has no accept or reject terminator: reporting IS
            // accepting and returning reports nothing, so every way out of the
            // loop body is a plain ret void.
            std::map<std::string, std::string> subst;
            for (const auto& lbl : q.loop.body) {
                if (lbl == q.loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs) {
                    if (i.DxOp() == kCandidateType) subst[i.result] = "1";
                    // Triangle barycentrics have NO source in an intersection
                    // shader: there is no attributes parameter to read them
                    // from. HitKind() is not available either. Both appear
                    // only in the triangle arm, which CandidateType has just
                    // folded away, so they are dead. Constants keep the IR
                    // valid without pretending to a value. Substituted in this
                    // pre-pass, so it does not depend on the order blocks
                    // happen to be emitted in.
                    else if (i.DxOp() == kCandidateBary)
                        subst[i.result] = "0.000000e+00";
                    else if (i.DxOp() == kCandidateFrontFace)
                        subst[i.result] = "false";
                }
            }
            std::vector<std::string> order{ q.loop.header };
            {
                std::vector<std::string> rest;
                for (const auto& l : q.loop.body)
                    if (l != q.loop.header && l != q.loop.latch) rest.push_back(l);
                std::sort(rest.begin(), rest.end());
                for (const auto& l : rest) order.push_back(l);
            }
            std::vector<std::string> is;
            is.push_back("define void @" + e.intersection + "() #RQNW {");
            is.push_back(std::string("  %rq.at = alloca ") + kAttrs + ", align 8");
            if (q.trackProc && (!q.aborts.empty() || !q.exits.empty())) {
                is.push_back("  %rq.iab = alloca i32, align 4");
                is.push_back("  store i32 0, i32* %rq.iab, align 4");
            }
            if (q.trackProc && (q.RayFlags() & 0xC0)) {
                // Traced forced non-opaque: the query's culls, here (0.71.0).
                for (const auto& l : IsectOpacity(q, "icw", sm66)) is.push_back(l);
                is.push_back("  %rq.icw.co = and i32 %rq.icw.f, 64");
                is.push_back("  %rq.icw.cn = and i32 %rq.icw.f, 128");
                is.push_back("  %rq.icw.cu = select i1 %rq.icw.op, i32 %rq.icw.co, i32 %rq.icw.cn");
                is.push_back("  %rq.icw.cl = icmp ne i32 %rq.icw.cu, 0");
                is.push_back("  br i1 %rq.icw.cl, label %rq.done, label %rq.ic");
                is.push_back("");
                is.push_back("rq.ic:");
            }

            const std::set<std::string> types = TypeNames(m.text);
            std::set<std::string> usedNames, inBody;
            for (const auto& lbl : q.loop.body) {
                if (lbl == q.loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs) {
                    for (const auto& u : Operands(i, types)) usedNames.insert(u);
                    if (!i.result.empty()) inBody.insert(i.result);
                }
            }
            // Everything the body reads from outside itself, rebuilt here in
            // source order. A resource handle becomes its library form; a
            // thread id becomes DispatchRaysIndex, which is the SAME ray so the
            // same value; everything else is pure and is emitted as it stood.
            // The isolation check has already refused anything not on the list.
            for (const llm::Instr* pi :
                 NeededChain(fn, Recomputable(fn, types), usedNames, types, inBody)) {
                const llm::Instr& i = *pi;
                if (i.DxOp() == kCreateHandle || i.DxOp() == kBindHandle) {
                    const std::string ht = handleText(i);
                    if (!handleErr.empty()) { r.error = handleErr; return r; }
                    is.push_back(ht);
                } else if (IsThreadOp(i.DxOp())) {
                    std::string terr;
                    const std::string t = ThreadIndexText(i, m, &terr);
                    if (!terr.empty()) { r.error = terr; return r; }
                    is.push_back(t);
                } else {
                    is.push_back(i.line);
                }
            }

            for (const auto& lbl : order) {
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                if (lbl != q.loop.header) {
                    is.push_back("");
                    is.push_back(lbl.substr(1) + ":");
                }
                for (const auto& i : b->instrs) {
                    const int op = i.DxOp();
                    // CandidateProceduralPrimitiveNonOpaque, computed (0.70.0):
                    // see _intersection in lower.py.
                    if (op == kCandidateProcNonOpaque) {
                        const std::string t = "pn" + i.result.substr(1);
                        for (const auto& l : IsectOpacity(q, t, sm66)) is.push_back(l);
                        is.push_back("  " + i.result + " = xor i1 %rq." + t + ".op, true");
                        continue;
                    }
                    if (op == kCandidateType ||
                        op == kCandidateBary || op == kCandidateFrontFace) continue;
                    // A TRIANGLE commit, in the arm CandidateType folded away.
                    // Dead, and an intersection shader has nothing to lower it
                    // onto, so it simply goes.
                    if (op == kCommitNonOpaque) continue;
                    if (op == kCommitProcedural) {
                        const std::string tv = llm::OperandName(i.args[2]);
                        is.push_back("  %rq.rh" + std::to_string(i.index) +
                                     " = call i1 @dx.op.reportHit." +
                                     std::string(kAttrs).substr(1) +
                                     "(i32 158, float " + tv + ", i32 0, " + kAttrs +
                                     "* nonnull %rq.at)"
                                     "  ; ReportHit(THit,HitKind,Attributes)");
                        continue;
                    }
                    // Abort: NOT EXACT here, traversal continues (0.68.0). See
                    // _intersection in lower.py for when that is still legal.
                    // EXACT once tracked procedural (0.71.0): noted, and
                    // reported at the one exit as a hit of kind 127.
                    if (op == kAbort) {
                        if (q.trackProc) is.push_back("  store i32 1, i32* %rq.iab, align 4");
                        continue;
                    }
                    // DXR_TIER11_WRITE_POISON=1 (0.68.0): the stores of a
                    // procedural body are dropped from the intersection
                    // shader. The writes case must diverge.
                    static const bool writePoison = std::getenv("DXR_TIER11_WRITE_POISON") != nullptr;
                    if (writePoison && i.result.empty() &&
                        (op == 67 || op == 69 || op == 140))   // textureStore, bufferStore, rawBufferStore
                        continue;
                    if (op == kCommittedRayT) {
                        // In an intersection shader RayTCurrent() IS the
                        // committed t: the closest hit so far, or TMax (0.66.0).
                        is.push_back("  " + i.result + " = call float @dx.op.rayTCurrent.f32(i32 154)"
                                     "  ; RayTCurrent()");
                        continue;
                    }
                    if (RecordWord(op) >= 0) {
                        // The record answers, because nothing in the shader
                        // can. The local root signature bound this record's own
                        // constants.
                        for (const auto& l :
                             RecordRead(i.result.substr(1), RecordWord(op), i.result, sm66))
                            is.push_back(l);
                        continue;
                    }
                    if (IsCommittedOp(op) && op != kCommittedRayT) {
                        // Dead here (0.71.0): see _intersection in lower.py.
                        const std::string kind = i.callee.substr(i.callee.rfind('.') + 1);
                        if (kind == "i32") is.push_back("  " + i.result + " = add i32 0, 0");
                        else if (kind == "f32") is.push_back("  " + i.result + " = fadd float 0.000000e+00, 0.000000e+00");
                        else is.push_back("  " + i.result + " = icmp eq i32 0, 0");
                        continue;
                    }
                    if (const CandMap* cmap = FindCand(op)) {
                        std::string cargs = "i32 " + std::to_string(cmap->dxop);
                        for (int k = 0; k < cmap->extra; ++k)
                            cargs += ", " + Trim(i.args[2 + k]);
                        is.push_back("  " + i.result + " = call " + cmap->ty + " @" +
                                     cmap->callee + "(" + cargs + ")");
                        continue;
                    }
                    std::string line = i.line;
                    for (const auto& kv : subst)
                        line = std::regex_replace(
                            line, std::regex("\\" + kv.first + "\\b"), kv.second);
                    line = std::regex_replace(
                        line, std::regex("label\\s+\\" + q.loop.latch + "\\b"),
                        "label %rq.done");
                    for (size_t k = 0; k < q.exits.size(); ++k)
                        if (q.exits[k].first == lbl)
                            // A break or return (0.72.0): noted, and reported
                            // at the one exit as a hit of kind 127 - k.
                            line = std::regex_replace(
                                line, std::regex("label\\s+\\" + q.exits[k].second + "\\b"),
                                "label %rq.ix" + std::to_string(k + 1));
                    for (const auto& s2 : Block_successors(line))
                        if (!q.loop.body.count(s2) && s2 != "%rq.done" &&
                            s2.rfind("%rq.ix", 0) != 0) {
                            r.error = "Proceed loop body branches to " + s2 +
                                      ", outside the loop; no lowering is defined "
                                      "for that";
                            return r;
                        }
                    is.push_back(line);
                }
            }
            for (size_t k = 0; k < q.exits.size(); ++k) {
                // Which way out (0.72.0): 1 is an Abort(), k + 1 exit k.
                is.push_back("");
                is.push_back("rq.ix" + std::to_string(k + 1) + ":");
                is.push_back("  store i32 " + std::to_string(k + 2) + ", i32* %rq.iab, align 4");
                is.push_back("  br label %rq.done");
            }
            is.push_back("");
            is.push_back("rq.done:");
            if (q.trackProc && (!q.aborts.empty() || !q.exits.empty())) {
                // The one exit (0.71.0): see _intersection in lower.py.
                // DXR_TIER11_PROCABORT_POISON=1 leaves the abort unreported,
                // so traversal runs on: the abort cases must diverge.
                static const bool procAbortPoison =
                    std::getenv("DXR_TIER11_PROCABORT_POISON") != nullptr;
                is.push_back("  %rq.ida = load i32, i32* %rq.iab, align 4");
                is.push_back("  %rq.idc = icmp ne i32 %rq.ida, 0");
                is.push_back("  br i1 %rq.idc, label %rq.iabt, label %rq.iret");
                is.push_back("");
                is.push_back("rq.iabt:");
                std::string kind = "i32 127";
                if (!q.exits.empty()) {
                    // Exit k as a hit of kind 127 - k (0.72.0).
                    is.push_back("  %rq.idk = sub i32 128, %rq.ida");
                    kind = "i32 %rq.idk";
                }
                is.push_back("  %rq.idt = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()");
                if (!procAbortPoison)
                    is.push_back("  %rq.idr = call i1 @dx.op.reportHit." +
                                 std::string(kAttrs).substr(1) + "(i32 158, float %rq.idt, " + kind + ", " +
                                 kAttrs + "* nonnull %rq.at)  ; ReportHit(THit,HitKind,Attributes)");
                is.push_back("  br label %rq.iret");
                is.push_back("");
                is.push_back("rq.iret:");
            }
            is.push_back("  ret void");
            is.push_back("}");
            std::string joinedIs;
            for (size_t i = 0; i < is.size(); ++i) {
                if (i) joinedIs += "\n";
                joinedIs += is[i];
            }
            fns.push_back(joinedIs);
        }
        // Not an else: a both-kinds query emits the intersection shader above
        // AND the any-hit below, from the same loop body.
        if (anyLoop && (q.NeedsBoth() || !q.NeedsIntersection())) {
            // AnyHit IS the Proceed loop body, re-rooted. Accept is falling off
            // the end; reject is IgnoreHit. The polarity is the reverse of the
            // RayQuery form, where committing is the special path. With several
            // queries it holds every loop body and picks one by the query id.
            std::vector<const Query*> loops;
            for (const Query* qp : queries) if (qp->hasLoop) loops.push_back(qp);

            std::map<std::string, std::string> subst;
            for (const Query* xp : loops) for (const auto& lbl : xp->loop.body) {
                if (lbl == xp->loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs)
                    if (i.DxOp() == kCandidateType) subst[i.result] = "0";
                    // Meaningless for a triangle candidate, and in the arm
                    // CandidateType has just folded away. Dead, but it still
                    // has to be a value.
                    else if (i.DxOp() == kCandidateProcNonOpaque)
                        subst[i.result] = "false";
            }

            // Abort() has no direct DXR 1.0 equivalent. An any-hit shader
            // can only IgnoreHit (reject and CONTINUE) or
            // AcceptHitAndEndSearch (accept and stop); there is no "reject and
            // stop", which is what a bare Abort needs.
            //
            // So it becomes a payload flag, covering both shapes uniformly.
            // Abort sets it and every later invocation ignores its candidate
            // at once. Commit-then-abort still accepts, because the control
            // flow still falls through; a bare abort still rejects. Either
            // way nothing further is committed, which is what stopping
            // traversal means for the result.
            //
            // Traversal itself carries on, so this is slower than the ideal.
            // AcceptHitAndEndSearch would be exact for commit-then-abort, but
            // only after proving the commit dominates the abort in the same
            // iteration, and correctness comes first.
            bool aborts = false;
            // A break or return ends the search as Abort() does (0.72.0).
            for (const Query* xp : loops)
                aborts = aborts || !xp->aborts.empty() || !xp->exits.empty() || !xp->chain.empty();
            bool track = false;
            for (const Query* xp : loops) track = track || xp->track;
            auto entryOf = [](const Query* xp) {
                return xp->track ? "%rq.e" + std::to_string(xp->qid) : xp->loop.header;
            };

            std::vector<std::string> ah;
            ah.push_back("define void @" + e.anyhit + "(" + kPayload +
                         "* noalias nocapture %p, " + kAttrs +
                         "* nocapture readonly %attr) #RQNW {");
            ah.push_back(std::string("  %rq.ap = getelementptr inbounds ") + kAttrs +
                         ", " + kAttrs + "* %attr, i32 0, i32 0");
            ah.push_back("  %rq.ab = load <2 x float>, <2 x float>* %rq.ap, align 4");
            for (const auto& c : carry) {
                // Written by each iteration and read by the raygen.
                if ((c.name.size() > 4 && c.name.compare(c.name.size() - 4, 4, ".out") == 0) ||
                    c.name == "%rq.iter")
                    continue;
                const std::string f = std::to_string(c.field);
                ah.push_back("  %rq.pac" + f + " = getelementptr inbounds " +
                             std::string(kPayload) + ", " + kPayload + "* %p, i32 0, i32 " + f);
                if (c.ty == "i1") {
                    ah.push_back("  %rq.pav" + f + " = load i32, i32* %rq.pac" + f + ", align 4");
                    ah.push_back("  " + c.name + " = icmp ne i32 %rq.pav" + f + ", 0");
                } else if (OtherCarry(c.ty)) {
                    for (const auto& l : CarryLoad(c.ty, c.name, c.field, "%rq.pac" + f, "%p")) ah.push_back(l);
                } else {
                    ah.push_back("  " + c.name + " = load " + c.ty + ", " + c.ty + "* %rq.pac" +
                                 f + ", align 4");
                }
            }


            // Recreate every resource handle the body uses, under the SAME SSA
            // name it had in the raygen, so the transplanted instructions need
            // no rewriting.
            const std::set<std::string> types = TypeNames(m.text);
            std::set<std::string> usedNames, inBody;
            for (const Query* xp : loops) for (const auto& lbl : xp->loop.body) {
                if (lbl == xp->loop.latch) continue;
                const llm::Block* b = fn.FindBlock(lbl);
                if (!b) continue;
                for (const auto& i : b->instrs) {
                    for (const auto& u : Operands(i, types)) usedNames.insert(u);
                    if (!i.result.empty()) inBody.insert(i.result);
                }
            }
            // Everything the body reads from outside itself, rebuilt here in
            // source order. A resource handle becomes its library form; a
            // thread id becomes DispatchRaysIndex, which is the SAME ray so the
            // same value; everything else is pure and is emitted as it stood.
            // The isolation check has already refused anything not on the list.
            for (const llm::Instr* pi :
                 NeededChain(fn, Recomputable(fn, types), usedNames, types, inBody)) {
                const llm::Instr& i = *pi;
                if (i.DxOp() == kCreateHandle || i.DxOp() == kBindHandle) {
                    const std::string ht = handleText(i);
                    if (!handleErr.empty()) { r.error = handleErr; return r; }
                    ah.push_back(ht);
                } else if (IsThreadOp(i.DxOp())) {
                    std::string terr;
                    const std::string t = ThreadIndexText(i, m, &terr);
                    if (!terr.empty()) { r.error = terr; return r; }
                    ah.push_back(t);
                } else {
                    ah.push_back(i.line);
                }
            }

            if (aborts) {
                ah.push_back(std::string("  %rq.pab = getelementptr inbounds ") +
                             kPayload + ", " + kPayload + "* %p, i32 0, i32 9");
                ah.push_back("  %rq.abv = load i32, i32* %rq.pab, align 4");
                ah.push_back("  %rq.abc = icmp ne i32 %rq.abv, 0");
                ah.push_back("  br i1 %rq.abc, label %rq.reject, label %rq.body");
                ah.push_back("");
                ah.push_back("rq.body:");
            }

            if (multi) {
                // Which query's trace this is, set by the raygen before TraceRay.
                ah.push_back(std::string("  %rq.pq = getelementptr inbounds ") + kPayload +
                             ", " + kPayload + "* %p, i32 0, i32 " +
                             std::to_string(kQidField));
                ah.push_back("  %rq.qid = load i32, i32* %rq.pq, align 4");
                for (size_t n = 1; n < loops.size(); ++n) {
                    const std::string ns = std::to_string(n);
                    ah.push_back("  %rq.qc" + ns + " = icmp eq i32 %rq.qid, " +
                                 std::to_string(loops[n]->qid));
                    ah.push_back("  br i1 %rq.qc" + ns + ", label " + entryOf(loops[n]) +
                                 ", label %rq.qn" + ns);
                    ah.push_back("");
                    ah.push_back("rq.qn" + ns + ":");
                }
                ah.push_back("  br label " + entryOf(loops[0]));
            }
            for (const Query* xp : loops) {
                if (!xp->track) continue;
                if (multi) {
                    ah.push_back("");
                    ah.push_back("rq.e" + std::to_string(xp->qid) + ":");
                }
                for (const auto& l : Opacity(*xp, sm66)) ah.push_back(l);
            }

            for (const Query* xp : loops) {
                const Query& x = *xp;
                std::set<std::string> commitBlocks;
                for (const auto& c : x.commits) commitBlocks.insert(c.first->label);

                std::vector<std::string> order{ x.loop.header };
                {
                    std::vector<std::string> rest;
                    for (const auto& l : x.loop.body)
                        if (l != x.loop.header && l != x.loop.latch) rest.push_back(l);
                    std::sort(rest.begin(), rest.end());
                    for (const auto& l : rest) order.push_back(l);
                }

                auto sit = states.find(&x);
                LoopState* st = sit == states.end() ? nullptr : &sit->second;
                // A phi in the body naming the header as a predecessor needs
                // the header to be a block of its own (0.65.0).
                bool named = x.track;
                for (const auto& l : order) {
                    if (l == x.loop.header) continue;
                    const llm::Block* nb = fn.FindBlock(l);
                    if (!nb) continue;
                    for (const auto& i : nb->instrs)
                        if (i.isPhi)
                            for (const auto& in : i.phiIncoming)
                                if (in.second == x.loop.header) named = true;
                }
                for (const auto& lbl : order) {
                    if (!x.chain.empty() && lbl == x.loop.header) {
                        // Proceed() without a loop (0.74.0): which call's body.
                        // See _chain_dispatch in lower.py.
                        if (!multi && !x.track) ah.push_back("  br label " + x.loop.header);
                        ah.push_back("");
                        ah.push_back(x.loop.header.substr(1) + ":");
                        const std::string t = std::to_string(x.qid);
                        const size_t n = x.chain.size();
                        ah.push_back("  %rq.pcp" + t + " = getelementptr inbounds " + std::string(kPayload) +
                                     ", " + kPayload + "* %p, i32 0, i32 " + std::to_string(st->pcount));
                        ah.push_back("  %rq.pck" + t + " = load i32, i32* %rq.pcp" + t + ", align 4");
                        ah.push_back("  %rq.pcn" + t + " = add i32 %rq.pck" + t + ", 1");
                        ah.push_back("  store i32 %rq.pcn" + t + ", i32* %rq.pcp" + t + ", align 4");
                        // DXR_TIER11_CHAIN_POISON=1 (0.74.0): every candidate runs
                        // the first call's body and nothing ends the search. The
                        // chain cases must diverge (C++ only).
                        static const bool chainPoison = std::getenv("DXR_TIER11_CHAIN_POISON") != nullptr;
                        for (size_t k = 0; k < n; ++k) {
                            const std::string ks = std::to_string(k);
                            const size_t kt = chainPoison ? 0 : k;
                            const std::string tgt = x.chain[kt].entry.empty()
                                ? "%rq.pce" + t + "." + std::to_string(kt) : x.chain[kt].entry;
                            ah.push_back("  %rq.pcc" + t + "." + ks + " = icmp eq i32 %rq.pck" + t + ", " + ks);
                            ah.push_back("  br i1 %rq.pcc" + t + "." + ks + ", label " + tgt +
                                         ", label %rq.pcs" + t + "." + ks);
                            ah.push_back("");
                            ah.push_back("rq.pcs" + t + "." + ks + ":");
                        }
                        ah.push_back(chainPoison ? "  br label " + (x.chain[0].entry.empty()
                                         ? "%rq.pce" + t + ".0" : x.chain[0].entry)
                                                 : std::string("  br label %rq.reject"));
                        for (size_t k = 0; k < n; ++k) {
                            if (!x.chain[k].entry.empty()) continue;
                            ah.push_back("");
                            ah.push_back("rq.pce" + t + "." + std::to_string(k) + ":");
                            if (k == n - 1 && !chainPoison) ah.push_back("  store i32 1, i32* %rq.pab, align 4");
                            ah.push_back("  br label %rq.reject");
                        }
                        continue;
                    }
                    const llm::Block* b = fn.FindBlock(lbl);
                    if (!b) continue;
                    if (!multi && lbl == x.loop.header && named && !x.track)
                        ah.push_back("  br label " + x.loop.header);
                    if (multi || lbl != x.loop.header || named) {
                        ah.push_back("");
                        ah.push_back(lbl.substr(1) + ":");
                    }
                    if (lbl == x.loop.header && st && st->iter >= 0) {
                        // An iteration ran: the body, not an opaque candidate
                        // committed before it (0.66.0), and not an aborted one.
                        const std::string f = std::to_string(st->iter);
                        const std::string qn = std::to_string(x.qid);
                        ah.push_back("  %rq.pit." + qn + " = getelementptr inbounds " + std::string(kPayload) +
                                     ", " + kPayload + "* %p, i32 0, i32 " + f);
                        ah.push_back("  store i32 1, i32* %rq.pit." + qn + ", align 4");
                    }
                    for (const auto& i : b->instrs) {
                        if (st && lbl == x.loop.header && i.isPhi) continue;   // loaded at entry
                        const int op = i.DxOp();
                        if (op == kCandidateType || op == kCommitNonOpaque ||
                            op == kCandidateProcNonOpaque) continue;
                        // A PROCEDURAL commit, in the arm CandidateType folded
                        // away. Dead, and an any-hit shader cannot report a
                        // procedural hit, so it simply goes.
                        if (op == kCommitProcedural) continue;
                        if (op == kAbort) {
                            ah.push_back("  store i32 1, i32* %rq.pab, align 4");
                            continue;
                        }
                        if (op == kCandidateFrontFace) {
                            // RayQuery returns a bool; HitKind() is an integer.
                            const std::string hk = "%rq.hk" + i.result.substr(1);
                            ah.push_back("  " + hk +
                                         " = call i32 @dx.op.hitKind.i32(i32 143)"
                                         "  ; HitKind()");
                            ah.push_back("  " + i.result + " = icmp eq i32 " + hk + ", " +
                                         std::to_string(kHitKindFront));
                            continue;
                        }
                        // A procedural query's are dead here (0.69.0).
                        if ((x.track || x.NeedsBoth()) && PayloadField(op)) {
                            for (const auto& l : CommittedRead(i)) ah.push_back(l);
                            continue;
                        }
                        if (x.track && op == kRayFlags) {
                            // The query's flags, as the raygen had them.
                            ah.push_back("  " + i.result + " = add i32 %rq.oflags, 0");
                            continue;
                        }
                        if (RecordWord(op) >= 0) {
                            // The record answers, because nothing in the shader
                            // can. The local root signature bound this record's own
                            // constants.
                            for (const auto& l :
                                 RecordRead(i.result.substr(1), RecordWord(op), i.result, sm66))
                                ah.push_back(l);
                            continue;
                        }
                        if (const CandMap* cmap = FindCand(op)) {
                            // Same operands as the RayQuery form, minus the handle.
                            std::string cargs = "i32 " + std::to_string(cmap->dxop);
                            for (int k = 0; k < cmap->extra; ++k)
                                cargs += ", " + Trim(i.args[2 + k]);
                            ah.push_back("  " + i.result + " = call " + cmap->ty + " @" +
                                         cmap->callee + "(" + cargs + ")");
                            continue;
                        }
                        if (op == kCandidateBary) {
                            static const std::regex kI8(R"(i8\s+(\d+))");
                            std::smatch mm;
                            std::string a2 = Trim(i.args[2]);
                            std::regex_search(a2, mm, kI8);
                            ah.push_back("  " + i.result +
                                         " = extractelement <2 x float> %rq.ab, i32 " +
                                         mm[1].str());
                            continue;
                        }
                        std::string line = i.line;
                        for (const auto& kv : subst)
                            line = std::regex_replace(
                                line, std::regex(kv.first.substr(0, 1) == "%"
                                                 ? "\\" + kv.first + "\\b" : kv.first),
                                kv.second);
                        if (!x.chain.empty()) {
                            // Proceed() without a loop (0.74.0): see _anyhit_body
                            // in lower.py.
                            const int ck = x.chainRegion.at(lbl);
                            const auto& c = x.chain[ck];
                            const bool last = ck == (int)x.chain.size() - 1;
                            const std::string end = "%rq.ce" + std::to_string(x.qid) + "." + lbl.substr(1);
                            const std::regex kToExit("label\\s+\\" + (c.exit.empty() ? std::string("%rq.none") : c.exit) + "\\b");
                            if (Trim(line) == "ret void") {
                                line = "  br label " + end;
                                const auto et = std::make_pair(lbl, end);
                                if (std::find(st->etails.begin(), st->etails.end(), et) == st->etails.end())
                                    st->etails.push_back(et);
                            } else if (!c.exit.empty() && std::regex_search(line, kToExit)) {
                                if (last) {
                                    line = std::regex_replace(line, kToExit, "label " + end);
                                    const auto et = std::make_pair(lbl, end);
                                    if (std::find(st->etails.begin(), st->etails.end(), et) == st->etails.end())
                                        st->etails.push_back(et);
                                } else {
                                    line = std::regex_replace(line, kToExit, "label " + x.loop.latch);
                                }
                            }
                        }
                        std::string target =
                            commitBlocks.count(lbl) ? "%rq.accept" : "%rq.reject";
                        const std::regex kToLatch("label\\s+" + std::string("\\") + x.loop.latch + "\\b");
                        if (st && std::regex_search(line, kToLatch)) {
                            // Through a tail that stores the state first.
                            const std::string tail = "%rq.t" + std::to_string(x.qid) + "." + lbl.substr(1);
                            const auto tp = std::make_pair(lbl, target);
                            if (std::find(st->tails.begin(), st->tails.end(), tp) == st->tails.end())
                                st->tails.push_back(tp);
                            line = std::regex_replace(line, kToLatch, "label " + tail);
                            target = tail;
                        }
                        line = std::regex_replace(line, kToLatch, "label " + target);
                        if (st)
                            for (const auto& ex : st->exits) {
                                if (std::get<0>(ex) != lbl) continue;
                                // A break or return (0.72.0): through a tail that
                                // says which, ends the search and keeps what was
                                // committed.
                                const std::string tail = "%rq.x" + std::to_string(x.qid) + "." +
                                                         std::to_string(std::get<2>(ex));
                                line = std::regex_replace(
                                    line, std::regex("label\\s+\\" + std::get<1>(ex) + "\\b"),
                                    "label " + tail);
                                const auto xt = std::make_tuple(lbl, tail, std::get<2>(ex));
                                if (std::find(st->xtails.begin(), st->xtails.end(), xt) == st->xtails.end())
                                    st->xtails.push_back(xt);
                            }
                        const std::string tailPfx = "%rq.t" + std::to_string(x.qid) + ".";
                        const std::string xtailPfx = "%rq.x" + std::to_string(x.qid) + ".";
                        const std::string etailPfx = "%rq.ce" + std::to_string(x.qid) + ".";
                        for (const auto& s2 : Block_successors(line))
                            if (!x.loop.body.count(s2) && s2 != "%rq.accept" &&
                                s2 != "%rq.reject" && s2.rfind(tailPfx, 0) != 0 &&
                                s2.rfind(xtailPfx, 0) != 0 && s2.rfind(etailPfx, 0) != 0) {
                                r.error = "Proceed loop body branches to " + s2 +
                                          ", outside the loop; no lowering is defined "
                                          "for that";
                                return r;
                            }
                        ah.push_back(line);
                    }
                }
            }
            // Per edge into the latch: the loop state as this iteration
            // leaves it, then accept or reject as the edge did (0.65.0).
            for (const Query* xp : loops) {
                auto sit = states.find(xp);
                if (sit == states.end()) continue;
                const LoopState& st = sit->second;
                const std::string qs = std::to_string(xp->qid);
                // A value leaves only where it exists (0.72.0): an exit can be
                // taken before a value it does not use is computed.
                auto dominates = [&](const std::string& v, const std::string& label) {
                    auto d = st.defBlock.find(v);
                    if (d == st.defBlock.end()) return true;
                    auto dm = st.dom.find(label);
                    return dm != st.dom.end() && dm->second.count(d->second) != 0;
                };
                for (const auto& tl : st.tails) {
                    const std::string bl = tl.first.substr(1);
                    ah.push_back("");
                    ah.push_back("rq.t" + qs + "." + bl + ":");
                    std::vector<std::tuple<int, std::string, std::string>> stores;
                    for (const auto& p : st.phis) stores.emplace_back(p.field, p.ty, p.next);
                    for (const auto& o : st.outs) stores.emplace_back(o.field, o.ty, o.name);
                    // DXR_TIER11_STATE_POISON=1: no iteration stores its state, the
                    // sensitivity check for rayquery_carryout (C++ only).
                    static const bool statePoison = std::getenv("DXR_TIER11_STATE_POISON") != nullptr;
                    if (statePoison) stores.clear();
                    for (const auto& s : stores) {
                        const std::string f = std::to_string(std::get<0>(s));
                        const std::string& ty = std::get<1>(s);
                        std::string v = std::get<2>(s);
                        auto lp = st.latchPhis.find(v);
                        if (lp != st.latchPhis.end()) {
                            auto e2 = lp->second.find(tl.first);
                            if (e2 != lp->second.end()) v = e2->second;
                        }
                        if (!dominates(v, tl.first)) continue;
                        const std::string ptr = "%rq.s" + f + "." + qs + "." + bl;
                        ah.push_back("  " + ptr + " = getelementptr inbounds " + std::string(kPayload) + ", " +
                                     kPayload + "* %p, i32 0, i32 " + f);
                        if (ty == "i1") {
                            const std::string z = "%rq.z" + f + "." + qs + "." + bl;
                            ah.push_back("  " + z + " = zext i1 " + v + " to i32");
                            ah.push_back("  store i32 " + z + ", i32* " + ptr + ", align 4");
                        } else if (OtherCarry(ty)) {
                            for (const auto& l : CarryStore(ty, v, std::get<0>(s), ptr, "%p")) ah.push_back(l);
                        } else {
                            ah.push_back("  store " + ty + " " + v + ", " + ty + "* " + ptr + ", align 4");
                        }
                    }
                    ah.push_back("  br label " + tl.second);
                }
                std::set<std::string> commitBlocks;
                for (const auto& c : xp->commits) commitBlocks.insert(c.first->label);
                for (const auto& xt : st.xtails) {
                    const std::string& label = std::get<0>(xt);
                    const std::string k = std::to_string(std::get<2>(xt));
                    ah.push_back("");
                    ah.push_back(std::get<1>(xt).substr(1) + ":");
                    for (const auto& o : st.outs) {
                        if (!dominates(o.name, label)) continue;
                        const std::string f = std::to_string(o.field);
                        const std::string ptr = "%rq.s" + f + "." + qs + ".x" + k;
                        ah.push_back("  " + ptr + " = getelementptr inbounds " + std::string(kPayload) + ", " +
                                     kPayload + "* %p, i32 0, i32 " + f);
                        if (o.ty == "i1") {
                            const std::string z = "%rq.z" + f + "." + qs + ".x" + k;
                            ah.push_back("  " + z + " = zext i1 " + o.name + " to i32");
                            ah.push_back("  store i32 " + z + ", i32* " + ptr + ", align 4");
                        } else if (OtherCarry(o.ty)) {
                            for (const auto& l : CarryStore(o.ty, o.name, o.field, ptr, "%p")) ah.push_back(l);
                        } else {
                            ah.push_back("  store " + o.ty + " " + o.name + ", " + o.ty + "* " + ptr + ", align 4");
                        }
                    }
                    // DXR_TIER11_EXIT_POISON=1 (0.72.0): an exit neither says
                    // which nor ends the search, as before 0.72.0 had it lowered.
                    // The exit cases must diverge (C++ only).
                    static const bool exitPoison = std::getenv("DXR_TIER11_EXIT_POISON") != nullptr;
                    if (!exitPoison) {
                        ah.push_back("  %rq.xe" + qs + "." + k + " = getelementptr inbounds " +
                                     std::string(kPayload) + ", " + kPayload + "* %p, i32 0, i32 " +
                                     std::to_string(st.exitField));
                        ah.push_back("  store i32 " + k + ", i32* %rq.xe" + qs + "." + k + ", align 4");
                        ah.push_back("  store i32 1, i32* %rq.pab, align 4");
                    }
                    ah.push_back(std::string("  br label ") +
                                 (commitBlocks.count(label) ? "%rq.accept" : "%rq.reject"));
                }
                for (const auto& et : st.etails) {
                    // The end of a chain of Proceed() calls (0.74.0).
                    const std::string& label = et.first;
                    const std::string bl = label.substr(1);
                    ah.push_back("");
                    ah.push_back(et.second.substr(1) + ":");
                    for (const auto& o : st.outs) {
                        if (!dominates(o.name, label)) continue;
                        const std::string f = std::to_string(o.field);
                        const std::string ptr = "%rq.s" + f + "." + qs + ".e" + bl;
                        ah.push_back("  " + ptr + " = getelementptr inbounds " + std::string(kPayload) + ", " +
                                     kPayload + "* %p, i32 0, i32 " + f);
                        if (o.ty == "i1") {
                            const std::string z = "%rq.z" + f + "." + qs + ".e" + bl;
                            ah.push_back("  " + z + " = zext i1 " + o.name + " to i32");
                            ah.push_back("  store i32 " + z + ", i32* " + ptr + ", align 4");
                        } else if (OtherCarry(o.ty)) {
                            for (const auto& l : CarryStore(o.ty, o.name, o.field, ptr, "%p")) ah.push_back(l);
                        } else {
                            ah.push_back("  store " + o.ty + " " + o.name + ", " + o.ty + "* " + ptr + ", align 4");
                        }
                    }
                    static const bool chainPoison2 = std::getenv("DXR_TIER11_CHAIN_POISON") != nullptr;
                    if (!chainPoison2) ah.push_back("  store i32 1, i32* %rq.pab, align 4");
                    ah.push_back(std::string("  br label ") +
                                 (commitBlocks.count(label) ? "%rq.accept" : "%rq.reject"));
                }
            }
            ah.push_back("");
            ah.push_back("rq.accept:");
            if (track) for (const auto& l : RecordCommit(q, sm66)) ah.push_back(l);
            if (aborts) {
                // Committed, then Abort(): this hit, and the search ends (0.67.0).
                ah.push_back("  %rq.aa = load i32, i32* %rq.pab, align 4");
                ah.push_back("  %rq.ac = icmp ne i32 %rq.aa, 0");
                ah.push_back("  br i1 %rq.ac, label %rq.end, label %rq.ret");
                ah.push_back("");
                ah.push_back("rq.ret:");
            }
            ah.push_back("  ret void");
            ah.push_back("");
            ah.push_back("rq.reject:");
            bool abortTracked = false;
            for (const Query* xp : loops)
                abortTracked = abortTracked ||
                               ((!xp->aborts.empty() || !xp->exits.empty() || !xp->chain.empty()) && xp->track);
            if (aborts && (q.trackProc || !abortTracked)) {
                // A bare Abort() in a query that never reads committed state:
                // ending the search commits a hit nothing reads (0.67.0).
                ah.push_back("  %rq.ra = load i32, i32* %rq.pab, align 4");
                ah.push_back("  %rq.rc = icmp ne i32 %rq.ra, 0");
                ah.push_back("  br i1 %rq.rc, label %rq.end, label %rq.ign");
                ah.push_back("");
                ah.push_back("rq.ign:");
            }
            ah.push_back("  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()");
            ah.push_back("  unreachable");
            if (aborts) {
                ah.push_back("");
                ah.push_back("rq.end:");
                ah.push_back("  call void @dx.op.acceptHitAndEndSearch(i32 156)  ; AcceptHitAndEndSearch()");
                ah.push_back("  unreachable");
            }
            ah.push_back("}");
            std::string joined;
            for (size_t i = 0; i < ah.size(); ++i) {
                if (i) joined += "\n";
                joined += ah[i];
            }
            fns.push_back(joined);
        }

        // ClosestHit. A both-kinds query needs TWO of these: a triangle hit
        // reports COMMITTED_TRIANGLE_HIT and a procedural one
        // COMMITTED_PROCEDURAL_PRIMITIVE_HIT, and one shader cannot say both,
        // because it does not know which hit group resolved to it.
        auto closestHit = [&](const std::string& name, const char* status) {
            std::vector<std::string> ch;
            ch.push_back("define void @" + name + "(" + kPayload +
                         "* noalias nocapture %p, " + kAttrs +
                         "* nocapture readonly %attr) #RQNW {");
            if (q.trackProc) {
                // An abort ended the search (0.71.0): see _closesthit.
                ch.push_back(std::string("  %rq.cga = getelementptr inbounds ") + kPayload + ", " +
                             kPayload + "* %p, i32 0, i32 9");
                ch.push_back("  %rq.cgv = load i32, i32* %rq.cga, align 4");
                ch.push_back("  %rq.cgc = icmp ne i32 %rq.cgv, 0");
                ch.push_back("  br i1 %rq.cgc, label %rq.cgr, label %rq.cgw");
                ch.push_back("");
                ch.push_back("rq.cgr:");
                ch.push_back("  ret void");
                ch.push_back("");
                ch.push_back("rq.cgw:");
            }
            ch.push_back("  %t = call float @dx.op.rayTCurrent.f32(i32 154)  ; RayTCurrent()");
            ch.push_back(std::string("  %ap = getelementptr inbounds ") + kAttrs + ", " +
                         kAttrs + "* %attr, i32 0, i32 0");
            ch.push_back("  %b = load <2 x float>, <2 x float>* %ap, align 4");
            ch.push_back(std::string("  %pt = getelementptr inbounds ") + kPayload + ", " +
                         kPayload + "* %p, i32 0, i32 0");
            ch.push_back("  store float %t, float* %pt, align 4");
            ch.push_back(std::string("  %pb = getelementptr inbounds ") + kPayload + ", " +
                         kPayload + "* %p, i32 0, i32 1");
            ch.push_back("  store <2 x float> %b, <2 x float>* %pb, align 4");
            ch.push_back(std::string("  %ph = getelementptr inbounds ") + kPayload + ", " +
                         kPayload + "* %p, i32 0, i32 2");
            ch.push_back(std::string("  store i32 ") + status +
                         ", i32* %ph, align 4");
            for (const auto& s : kChSource) {
                ch.push_back("  %id" + std::to_string(s.first) + " = " + s.second);
                ch.push_back("  %pi" + std::to_string(s.first) +
                             " = getelementptr inbounds " + kPayload + ", " + kPayload +
                             "* %p, i32 0, i32 " + std::to_string(s.first));
                ch.push_back("  store i32 %id" + std::to_string(s.first) + ", i32* %pi" +
                             std::to_string(s.first) + ", align 4");
            }

            // The record's two numbers, when the shader asked for either.
            // Both are stored whenever either is read: one load answers both,
            // so splitting them would cost a branch and save nothing.
            if (q.needsRecordConstants) {
                for (const auto& l : RecordRead("g", 0, "%rq.geo", sm66)) ch.push_back(l);
                ch.push_back(std::string("  %rq.gp = getelementptr inbounds ") +
                             kPayload + ", " + kPayload + "* %p, i32 0, i32 5");
                ch.push_back("  store i32 %rq.geo, i32* %rq.gp, align 4");
                ch.push_back(std::string("  %rq.con = extractvalue ") + kResRet +
                             " %rq.cbrg, 1");
                ch.push_back(std::string("  %rq.cp = getelementptr inbounds ") +
                             kPayload + ", " + kPayload + "* %p, i32 0, i32 10");
                ch.push_back("  store i32 %rq.con, i32* %rq.cp, align 4");
            }
            // Twelve fetches and twelve stores, so they are emitted ONLY when
            // the shader actually reads the matrix. The payload field exists
            // either way, because a layout that changes shape is a layout that
            // gets an offset wrong; it is the per-invocation work worth
            // avoiding.
            bool wantsW2O = false;
            for (const Query* qp : queries) for (const auto& cw : qp->committedOps)
                if (cw.second->DxOp() == kCommittedWorldToObject) wantsW2O = true;
            if (wantsW2O) {
                ch.push_back(std::string("  %pw = getelementptr inbounds ") + kPayload +
                             ", " + kPayload + "* %p, i32 0, i32 8");
                for (int rr = 0; rr < 3; ++rr) {
                    for (int cc = 0; cc < 4; ++cc) {
                        const int sl = rr * 4 + cc;
                        ch.push_back("  %w" + std::to_string(sl) +
                                     " = call float @dx.op.worldToObject.f32(i32 152, i32 " +
                                     std::to_string(rr) + ", i8 " + std::to_string(cc) +
                                     ")  ; WorldToObject(row,col)");
                        ch.push_back("  %pw" + std::to_string(sl) +
                                     " = getelementptr inbounds [12 x float], "
                                     "[12 x float]* %pw, i32 0, i32 " + std::to_string(sl));
                        ch.push_back("  store float %w" + std::to_string(sl) +
                                     ", float* %pw" + std::to_string(sl) + ", align 4");
                    }
                }
            }
            ch.push_back("  ret void");
            ch.push_back("}");
            std::string joined;
            for (size_t i = 0; i < ch.size(); ++i) {
                if (i) joined += "\n";
                joined += ch[i];
            }
            fns.push_back(joined);
        };

        if (q.NeedsBoth()) {
            closestHit(e.closesthit, "1");
            closestHit(e.closesthitproc, "2");
            if (q.trackProc) {
                // See _anyhit_proc in lower.py.
                const std::string pl = kPayload, at = kAttrs;
                std::vector<std::string> ap = {
                    "define void @" + e.anyhitproc + "(" + pl + "* noalias nocapture %p, " + at +
                        "* nocapture readonly %attr) #RQNW {",
                    "  %rq.ap = getelementptr inbounds " + at + ", " + at + "* %attr, i32 0, i32 0",
                    "  %rq.ab = load <2 x float>, <2 x float>* %rq.ap, align 4",
                    "  %rq.pab = getelementptr inbounds " + pl + ", " + pl + "* %p, i32 0, i32 9",
                    "  %rq.phk = call i32 @dx.op.hitKind.i32(i32 143)  ; HitKind()",
                };
                // Kinds 127 - n to 127 end the search, and say which way out
                // (0.72.0).
                if (!q.exits.empty())
                    ap.push_back("  %rq.pak = icmp ugt i32 %rq.phk, " +
                                 std::to_string(126 - (int)q.exits.size()));
                else
                    ap.push_back("  %rq.pak = icmp eq i32 %rq.phk, 127");
                for (const char* l : { "  br i1 %rq.pak, label %rq.pend, label %rq.prec", "",
                                       "rq.pend:", "  store i32 1, i32* %rq.pab, align 4" })
                    ap.push_back(l);
                if (!q.exits.empty()) {
                    ap.push_back("  %rq.pxi = sub i32 127, %rq.phk");
                    ap.push_back("  %rq.pxp = getelementptr inbounds " + pl + ", " + pl +
                                 "* %p, i32 0, i32 " + std::to_string(states.at(&q).exitField));
                    ap.push_back("  store i32 %rq.pxi, i32* %rq.pxp, align 4");
                }
                for (const char* l : { "  call void @dx.op.acceptHitAndEndSearch(i32 156)  ; AcceptHitAndEndSearch()",
                                       "  unreachable", "", "rq.prec:" })
                    ap.push_back(l);
                for (const auto& l : RecordCommit(q, sm66, 2)) ap.push_back(l);
                ap.push_back("  ret void");
                ap.push_back("}");
                std::string joinedAp;
                for (size_t i = 0; i < ap.size(); ++i) {
                    if (i) joinedAp += "\n";
                    joinedAp += ap[i];
                }
                fns.push_back(joinedAp);
            }
        } else {
            closestHit(e.closesthit, q.NeedsIntersection() ? "2" : "1");
        }

        // Miss
        fns.push_back("define void @" + e.miss + "(" + std::string(kPayload) +
                      "* noalias nocapture %p) #RQNW {\n"
                      "  %ph = getelementptr inbounds " + kPayload + ", " + kPayload +
                      "* %p, i32 0, i32 2\n"
                      "  store i32 0, i32* %ph, align 4\n"
                      "  ret void\n}");

        // The two never-commit stubs. Shapes taken from DXC, see
        // phase5/cases/reference/lib_null_ref.hlsl, which also confirms both
        // are SFI0=0x0 and so carry no Tier 1.1 feature flag.
        //
        // Rejecting every candidate is how a TRIANGLES hit group produces no
        // hit: traversal carries on past that geometry as if the shader had
        // never seen it. #3 is noreturn nounwind, as DXC marks its own.
        fns.push_back("define void @" + e.anyhitnull + "(" + std::string(kPayload) +
                      "* noalias nocapture %p, " + kAttrs +
                      "* nocapture readnone %attr) #RQNR {\n"
                      "  call void @dx.op.ignoreHit(i32 155)  ; IgnoreHit()\n"
                      "  unreachable\n}");

        // And reporting nothing is how a PROCEDURAL hit group produces no hit:
        // an intersection shader that returns has found nothing.
        fns.push_back("define void @" + e.isectnull + "() #RQNW {\n"
                      "  ret void\n}");

        static const std::regex kEnd(R"(\}\s*)");
        auto lines = llm::SplitLines(out);
        const int at = llm::FindLine(lines, kEnd);
        if (at < 0) {
            r.error = "cannot find the end of the entry function";
            return r;
        }
        std::string joined;
        for (size_t i = 0; i < fns.size(); ++i) {
            if (i) joined += "\n\n";
            joined += fns[i];
        }
        llm::InsertAfter(lines, at, "\n" + joined);
        out = llm::JoinLines(lines);
    }

    // --- metadata ------------------------------------------------------------
    {
        for (const auto& g : globals) {
            auto fields = SplitTop(md.at(g.nid));
            if (binding) {
                // `%CB* bitcast (%dx.types.Handle* @CB to %CB*)`, copied from
                // DXC. The global holds a handle; the record still has to name
                // the resource type, so it casts.
                fields[1] = g.gty + "* bitcast (" + BindingGty(g.gty, g.elem) + "* @" + g.sym +
                            " to " + g.gty + "*)";
            } else {
                fields[1] = std::regex_replace(fields[1], std::regex(R"(\*\s+\S+$)"),
                                               "* @" + g.sym);
            }
            fields[2] = "!\"" + g.name + "\"";
            std::string body;
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i) body += ", ";
                body += fields[i];
            }
            {
                const std::regex re("!" + std::to_string(g.nid) + R"( = !\{.*\}\s*)");
                auto lines = llm::SplitLines(out);
                const int at = llm::FindLine(lines, re);
                if (at >= 0)
                    lines[at] = "!" + std::to_string(g.nid) + " = !{" + body + "}";
                out = llm::JoinLines(lines);
            }
        }

        {
            static const std::regex re(
                R"RX(!(\d+) = !\{!"cs", i32 (\d+), i32 (\d+)\}\s*)RX");
            auto lines = llm::SplitLines(out);
            const int at = llm::FindLine(lines, re);
            if (at >= 0) {
                std::smatch cm;
                std::regex_match(lines[at], cm, re);
                lines[at] = "!" + cm[1].str() + " = !{!\"lib\", i32 " + cm[2].str() +
                            ", i32 " + cm[3].str() + "}";
            }
            out = llm::JoinLines(lines);
        }

        std::smatch epm;
        std::regex_search(out, epm, std::regex(R"(!dx\.entryPoints\s*=\s*!\{!(\d+)\})"));
        const std::string oldEntry = epm[1].str();

        int counter = 0;
        for (const auto& kv : md) counter = std::max(counter, kv.first);
        ++counter;
        std::vector<std::string> nodes;
        auto node = [&](const std::string& body) {
            const int id = counter++;
            nodes.push_back("!" + std::to_string(id) + " = !{" + body + "}");
            return id;
        };

        // The record SRV, added to whatever the application already had.
        //
        // Shape copied from DXC, not invented: an SRV record is
        // {id, global, name, space, lowerBound, rangeSize, kind, sampleCount,
        // extra}, and kind 11 is a raw buffer. space 1 keeps it clear of
        // anything the application's own global root signature binds. See
        // phase5/cases/reference/lib_localsrv_ref.hlsl.
        if (q.needsRecordConstants) {
            std::vector<std::string> groups;
            for (const auto& g : SplitTop(md.at(std::stoi(resId))))
                groups.push_back(Trim(g));
            while (groups.size() < 4) groups.push_back("null");
            std::vector<std::string> have;
            if (groups[0] != "null")
                for (const auto& x : SplitTop(md.at(std::stoi(groups[0].substr(1)))))
                    have.push_back(Trim(x));
            const std::string gref = sm66
                ? std::string(kRecordType) + "* bitcast (" + kHandleType + "* " +
                  kRecordGlobal + " to " + kRecordType + "*)"
                : std::string(kRecordType) + "* " + kRecordGlobal;
            const int rec = node("i32 " + std::to_string(have.size()) + ", " + gref +
                                 ", !\"rq_record\", i32 1, i32 0, i32 1, i32 11, i32 0, null");
            std::string grp;
            for (const auto& h : have) grp += h + ", ";
            grp += "!" + std::to_string(rec);
            groups[0] = "!" + std::to_string(node(grp));
            std::string all;
            for (size_t i = 0; i < groups.size(); ++i) {
                if (i) all += ", ";
                all += groups[i];
            }
            // Rewritten in place, so !dx.resources keeps pointing at the same
            // node and nothing else has to learn a new id.
            auto lines = llm::SplitLines(out);
            const int at = llm::FindLine(
                lines, std::regex("^!" + resId + R"( = !\{.*\}\s*$)"));
            if (at >= 0) lines[at] = "!" + resId + " = !{" + all + "}";
            out = llm::JoinLines(lines);
        }

        const std::string sigH = "(" + std::string(kPayload) + "*, " + kAttrs + "*)";
        const std::string sigP = "(" + std::string(kPayload) + "*)";

        const int empty = node("");
        const int ret = node("i32 1, !" + std::to_string(empty) + ", !" + std::to_string(empty));
        const int parPayload = node("i32 2, !" + std::to_string(empty) + ", !" + std::to_string(empty));
        const int parAttrs = node("i32 0, !" + std::to_string(empty) + ", !" + std::to_string(empty));
        const int annRaygen = node("!" + std::to_string(ret));
        const int annHit = node("!" + std::to_string(ret) + ", !" + std::to_string(parPayload) +
                                ", !" + std::to_string(parAttrs));
        const int annMiss = node("!" + std::to_string(ret) + ", !" + std::to_string(parPayload));

        std::vector<std::string> ann{ "i32 1", "void ()* @" + e.raygen,
                                      "!" + std::to_string(annRaygen) };
        if (q.NeedsBoth()) {
            // An intersection shader is void(), so it annotates like a raygen.
            ann.push_back("void ()* @" + e.intersection);
            ann.push_back("!" + std::to_string(annRaygen));
            ann.push_back("void " + sigH + "* @" + e.anyhit);
            ann.push_back("!" + std::to_string(annHit));
        } else if (q.NeedsIntersection()) {
            // An intersection shader is void(), so it annotates like a raygen.
            ann.push_back("void ()* @" + e.intersection);
            ann.push_back("!" + std::to_string(annRaygen));
        } else if (anyLoop) {
            ann.push_back("void " + sigH + "* @" + e.anyhit);
            ann.push_back("!" + std::to_string(annHit));
        }
        ann.push_back("void " + sigH + "* @" + e.closesthit);
        ann.push_back("!" + std::to_string(annHit));
        if (q.NeedsBoth()) {
            ann.push_back("void " + sigH + "* @" + e.closesthitproc);
            ann.push_back("!" + std::to_string(annHit));
        }
        ann.push_back("void " + sigP + "* @" + e.miss);
        ann.push_back("!" + std::to_string(annMiss));
        ann.push_back("void " + sigH + "* @" + e.anyhitnull);
        ann.push_back("!" + std::to_string(annHit));
        if (q.trackProc) {
            ann.push_back("void " + sigH + "* @" + e.anyhitproc);
            ann.push_back("!" + std::to_string(annHit));
        }
        // An intersection shader is void(), so it annotates like the raygen.
        ann.push_back("void ()* @" + e.isectnull);
        ann.push_back("!" + std::to_string(annRaygen));
        std::string annJoined;
        for (size_t i = 0; i < ann.size(); ++i) {
            if (i) annJoined += ", ";
            annJoined += ann[i];
        }
        const int annId = node(annJoined);

        const int zero = node("i32 0");
        const int flags = node(
            "i32 0, i64 " +
            std::to_string((ModuleFlags(text0, md) & ~kRt11ShaderFlag) |
                           (q.needsRecordConstants ? kRawBufferFlag : 0ull)));
        std::vector<int> eps{ node("null, !\"\", null, !" + resId + ", !" +
                                   std::to_string(flags)) };

        auto entry = [&](const std::string& name, const std::string& sig, int kind,
                         bool payload, bool attrs) {
            std::vector<std::string> props{ "i32 8", "i32 " + std::to_string(kind) };
            if (payload) {
                props.push_back("i32 6");
                props.push_back("i32 " + std::to_string(
                    PayloadBytesFor(multi, CarryWordsOf(carry))));
            }
            if (attrs) { props.push_back("i32 7"); props.push_back("i32 8"); }
            props.push_back("i32 5");
            props.push_back("!" + std::to_string(zero));
            std::string pj;
            for (size_t i = 0; i < props.size(); ++i) {
                if (i) pj += ", ";
                pj += props[i];
            }
            const int pid = node(pj);
            return node("void " + sig + "* @" + name + ", !\"" + name +
                        "\", null, null, !" + std::to_string(pid));
        };

        // Shader kind 8. An intersection shader carries neither a payload
        // size nor an attribute size, exactly as DXC emits it.
        if (q.NeedsBoth()) {
            eps.push_back(entry(e.intersection, "()", 8, false, false));
            eps.push_back(entry(e.anyhit, sigH, 9, true, true));
        } else if (q.NeedsIntersection())
            eps.push_back(entry(e.intersection, "()", 8, false, false));
        else if (anyLoop) eps.push_back(entry(e.anyhit, sigH, 9, true, true));
        eps.push_back(entry(e.closesthit, sigH, 10, true, true));
        if (q.NeedsBoth())
            eps.push_back(entry(e.closesthitproc, sigH, 10, true, true));
        eps.push_back(entry(e.miss, sigP, 11, true, false));
        eps.push_back(entry(e.anyhitnull, sigH, 9, true, true));
        if (q.trackProc) eps.push_back(entry(e.anyhitproc, sigH, 9, true, true));
        eps.push_back(entry(e.isectnull, "()", 8, false, false));
        eps.push_back(entry(e.raygen, "()", 7, false, false));

        std::string epJoined;
        for (size_t i = 0; i < eps.size(); ++i) {
            if (i) epJoined += ", ";
            epJoined += "!" + std::to_string(eps[i]);
        }
        // Allocated LAST, so a shader that does not index dynamically keeps
        // every other node id exactly where it was.
        if (out.find("!RQNU") != std::string::npos)
            out = Replace(out, "!RQNU", "!" + std::to_string(node("i32 1")));

        out = Replace(out, "!dx.entryPoints = !{!" + oldEntry + "}",
                      "!dx.typeAnnotations = !{!" + std::to_string(annId) + "}\n"
                      "!dx.entryPoints = !{" + epJoined + "}");
        {
            const std::regex re("!" + oldEntry +
                                R"( = !\{void \(\)\* @\w+.*\}\s*)");
            auto lines = llm::SplitLines(out);
            const int at = llm::FindLine(lines, re);
            if (at >= 0) lines[at] = "";
            out = llm::JoinLines(lines);
        }

        while (!out.empty() && out.back() == '\n') out.pop_back();
        std::string nodeJoined;
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (i) nodeJoined += "\n";
            nodeJoined += nodes[i];
        }
        out += "\n" + nodeJoined + "\n";
    }

    {
        bool dbl = false;
        for (const auto& c : carry) dbl = dbl || c.ty == "double";
        if (dbl) {
            // A carried double travels as two words (0.76.0): see lower().
            if (out.find("%dx.types.splitdouble = type") == std::string::npos) {
                const size_t k = out.find(std::string(kPayload) + " = type ");
                out.insert(k, "%dx.types.splitdouble = type { i32, i32 }\n");
            }
            std::vector<std::string> ls;
            {
                size_t a = 0;
                while (true) {
                    const size_t b2 = out.find('\n', a);
                    ls.push_back(out.substr(a, b2 == std::string::npos ? std::string::npos : b2 - a));
                    if (b2 == std::string::npos) break;
                    a = b2 + 1;
                }
            }
            bool haveSplit = false, haveMake = false;
            int lastDecl = -1;
            for (size_t n = 0; n < ls.size(); ++n) {
                if (ls[n].rfind("declare %dx.types.splitdouble @dx.op.splitDouble.f64(", 0) == 0) haveSplit = true;
                if (ls[n].rfind("declare double @dx.op.makeDouble.f64(", 0) == 0) haveMake = true;
                if (ls[n].rfind("declare ", 0) == 0) lastDecl = (int)n;
            }
            std::vector<std::string> add;
            if (!haveSplit) {
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readnone");
                add.push_back("declare %dx.types.splitdouble @dx.op.splitDouble.f64(i32, double) #RQNONE");
            }
            if (!haveMake) {
                add.push_back("");
                add.push_back("; Function Attrs: nounwind readnone");
                add.push_back("declare double @dx.op.makeDouble.f64(i32, i32, i32) #RQNONE");
            }
            if (!add.empty()) {
                ls.insert(ls.begin() + lastDecl + 1, add.begin(), add.end());
                out.clear();
                for (size_t n = 0; n < ls.size(); ++n) {
                    if (n) out += "\n";
                    out += ls[n];
                }
            }
        }
    }
    {
        std::string attrErr;
        out = ResolveAttrs(out, &attrErr);
        if (!attrErr.empty()) { r.error = attrErr; return r; }
    }

    r.ok = true;
    r.text = out;
    r.payloadBytes = PayloadBytesFor(multi, CarryWordsOf(carry));
    r.carried = (int)carry.size();
    return r;
}

}  // namespace rq
