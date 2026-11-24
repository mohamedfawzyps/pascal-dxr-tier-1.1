#!/usr/bin/env python3
"""Find and classify RayQuery use in a DXIL module, structurally.

The hand lowerings in phase5/hand key off exact instruction text, which works
for a fixed input and is useless for a real one. This finds the same sites the
way the recon says the IR supports:

  - follow the query handle, which is a plain i32 SSA value, so ownership is
    one def-use hop and nothing needs alias analysis
  - discriminate on the opcode IMMEDIATE, never the callee name, because DXC
    reuses one LLVM function for several opcodes
  - recognise the rotated loop through real dominator analysis, because the
    source's single `while (q.Proceed())` becomes two Proceed calls with the
    guard peeled into the preheader

Opcodes are a WHITELIST. Every rayQuery opcode this project has actually
verified against DXC output is listed; anything else makes analysis fail
loudly rather than silently lower something it does not understand. The brief
is explicit about that preference, and a wrong render is far worse than a
refusal.
"""

import re

# Verified against DXC 1.10.2605.37 output, see docs/phase5-dxil-recon.md.
ALLOCATE = 178
TRACE_INLINE = 179
PROCEED = 180
ABORT = 181
COMMIT_NON_OPAQUE = 182
COMMIT_PROCEDURAL = 183
CANDIDATE_PROC_NON_OPAQUE = 190
COMMITTED_STATUS = 184
CANDIDATE_TYPE = 185
CANDIDATE_BARY = 193
COMMITTED_BARY = 194
COMMITTED_RAY_T = 200
# Read off DXC output the same way as the rest. All three share the
# StateScalar.i32 function with 184 and 185, which is why operand 0 is the only
# safe discriminator.
COMMITTED_INSTANCE_INDEX = 207
COMMITTED_GEOMETRY_INDEX = 209
COMMITTED_PRIMITIVE_INDEX = 210

# The accessors a real engine uses, added after surveying Unreal. All read off
# DXC output, and every DXR 1.0 target was compiled and its SFI0 checked first,
# because GeometryIndex proved an intrinsic can look ordinary and be secretly
# Tier 1.1.
CANDIDATE_WORLD_TO_OBJECT = 187
CANDIDATE_FRONT_FACE = 191
COMMITTED_FRONT_FACE = 192
COMMITTED_WORLD_TO_OBJECT = 189
CANDIDATE_RAY_T = 199
CANDIDATE_INSTANCE_INDEX = 201
CANDIDATE_INSTANCE_ID = 202
CANDIDATE_PRIMITIVE_INDEX = 204
CANDIDATE_OBJECT_RAY_ORIGIN = 205
CANDIDATE_OBJECT_RAY_DIRECTION = 206
COMMITTED_INSTANCE_ID = 208

# The four the SHADER TABLE answers, because nothing in the shader can.
#
# GeometryIndex() in a DXR 1.0 hit shader is itself Tier 1.1, and HLSL exposes
# no hit-shader intrinsic for the contribution at all. Both were written off as
# impossible for reasons that only hold while the APPLICATION owns the table.
# The shim builds it, so each record carries these two numbers as local root
# signature constants and the hit shader reads them back. Measured at
# SFI0 = 0x0: phase5/cases/reference/lib_localroot_ref.hlsl.
#
# Numbers read off DXC, not inferred from the gaps around them:
# phase5/cases/reference/rq_opcodes_ref.hlsl.
CANDIDATE_GEOMETRY_INDEX = 203
CANDIDATE_INSTANCE_CONTRIB = 214
COMMITTED_INSTANCE_CONTRIB = 215

# Query-wide, neither candidate nor committed: the flags the query is tracing
# with. 59 of Unreal's shaders refused on this once GeometryIndex stopped
# blocking them first, which made it the largest single remaining bucket.
RAY_FLAGS = 195

# Opcode -> short name, for messages. Membership in this table is what makes
# an opcode supported; the numbers NOT here are deliberately absent because
# this project has not observed them and will not guess.
KNOWN = {
    ALLOCATE: 'AllocateRayQuery',
    TRACE_INLINE: 'TraceRayInline',
    PROCEED: 'Proceed',
    ABORT: 'Abort',
    COMMIT_NON_OPAQUE: 'CommitNonOpaqueTriangleHit',
    COMMIT_PROCEDURAL: 'CommitProceduralPrimitiveHit',
    CANDIDATE_PROC_NON_OPAQUE: 'CandidateProceduralPrimitiveNonOpaque',
    COMMITTED_STATUS: 'CommittedStatus',
    CANDIDATE_TYPE: 'CandidateType',
    CANDIDATE_BARY: 'CandidateTriangleBarycentrics',
    COMMITTED_BARY: 'CommittedTriangleBarycentrics',
    COMMITTED_RAY_T: 'CommittedRayT',
    COMMITTED_INSTANCE_INDEX: 'CommittedInstanceIndex',
    COMMITTED_GEOMETRY_INDEX: 'CommittedGeometryIndex',
    COMMITTED_PRIMITIVE_INDEX: 'CommittedPrimitiveIndex',
    CANDIDATE_WORLD_TO_OBJECT: 'CandidateWorldToObject',
    CANDIDATE_FRONT_FACE: 'CandidateTriangleFrontFace',
    COMMITTED_FRONT_FACE: 'CommittedTriangleFrontFace',
    COMMITTED_WORLD_TO_OBJECT: 'CommittedWorldToObject',
    CANDIDATE_RAY_T: 'CandidateTriangleRayT',
    CANDIDATE_INSTANCE_INDEX: 'CandidateInstanceIndex',
    CANDIDATE_INSTANCE_ID: 'CandidateInstanceID',
    CANDIDATE_PRIMITIVE_INDEX: 'CandidatePrimitiveIndex',
    CANDIDATE_OBJECT_RAY_ORIGIN: 'CandidateObjectRayOrigin',
    CANDIDATE_OBJECT_RAY_DIRECTION: 'CandidateObjectRayDirection',
    COMMITTED_INSTANCE_ID: 'CommittedInstanceID',
    RAY_FLAGS: 'RayFlags',
    CANDIDATE_GEOMETRY_INDEX: 'CandidateGeometryIndex',
    CANDIDATE_INSTANCE_CONTRIB: 'CandidateInstanceContributionToHitGroupIndex',
    COMMITTED_INSTANCE_CONTRIB: 'CommittedInstanceContributionToHitGroupIndex',
}

# The two read in the any-hit or intersection shader straight out of the
# record, and the two that travel home in the payload from the closest-hit.
RECORD_CANDIDATE_OPS = (CANDIDATE_GEOMETRY_INDEX, CANDIDATE_INSTANCE_CONTRIB)
RECORD_COMMITTED_OPS = (COMMITTED_GEOMETRY_INDEX, COMMITTED_INSTANCE_CONTRIB)
RECORD_OPS = RECORD_CANDIDATE_OPS + RECORD_COMMITTED_OPS

# Read in the any-hit shader, where they need no payload at all: the candidate
# under test IS what a DXR 1.0 hit-shader intrinsic reports.
CANDIDATE_OPS = (CANDIDATE_PROC_NON_OPAQUE,
                 CANDIDATE_TYPE, CANDIDATE_BARY, CANDIDATE_WORLD_TO_OBJECT,
                 CANDIDATE_FRONT_FACE, CANDIDATE_RAY_T, CANDIDATE_INSTANCE_INDEX,
                 CANDIDATE_INSTANCE_ID, CANDIDATE_PRIMITIVE_INDEX,
                 CANDIDATE_OBJECT_RAY_ORIGIN, CANDIDATE_OBJECT_RAY_DIRECTION,
                 CANDIDATE_GEOMETRY_INDEX, CANDIDATE_INSTANCE_CONTRIB,
                 RAY_FLAGS)

# Everything the generated closest-hit can put in the payload. Used by both the
# analysis, to decide what is a committed read, and the lowering, to lay the
# payload out.
COMMITTED_OPS = (COMMITTED_STATUS, COMMITTED_BARY, COMMITTED_RAY_T,
                 COMMITTED_INSTANCE_INDEX, COMMITTED_GEOMETRY_INDEX,
                 COMMITTED_PRIMITIVE_INDEX, COMMITTED_INSTANCE_ID,
                 COMMITTED_FRONT_FACE, COMMITTED_WORLD_TO_OBJECT,
                 COMMITTED_INSTANCE_CONTRIB)

# Empty, and that is the interesting part.
#
# CommittedGeometryIndex lived here for most of this project's life, because
# GeometryIndex() in a DXR 1.0 hit shader is ITSELF Tier 1.1: a library using
# it sets shader flag 0x2000000 and CreateStateObject on the GTX 1070 returns
# E_INVALIDARG. That measurement is still true. What changed is that the answer
# no longer has to come from an intrinsic: the shim builds the shader table, so
# it puts the geometry index in the record.
#
# A real Unreal 5.8 run refused 120 shaders on the four record opcodes, more
# than everything else together, which is what made it worth doing.
#
# Kept as a table rather than deleted, because the next opcode with no lowering
# will want somewhere to say so.
NO_LOWERING = {}

RAY_FLAG = [
    (0x001, 'FORCE_OPAQUE'),
    (0x002, 'FORCE_NON_OPAQUE'),
    (0x004, 'ACCEPT_FIRST_HIT_AND_END_SEARCH'),
    (0x008, 'SKIP_CLOSEST_HIT_SHADER'),
    (0x010, 'CULL_BACK_FACING_TRIANGLES'),
    (0x020, 'CULL_FRONT_FACING_TRIANGLES'),
    (0x040, 'CULL_OPAQUE'),
    (0x080, 'CULL_NON_OPAQUE'),
    (0x100, 'SKIP_TRIANGLES'),
    (0x200, 'SKIP_PROCEDURAL_PRIMITIVES'),
]


def flag_names(value):
    names = [n for bit, n in RAY_FLAG if value & bit]
    return '|'.join(names) if names else 'NONE'


class Unsupported(Exception):
    """This shader has no valid lowering, or one this project cannot do yet.

    Raised rather than returned, because the only correct response is to stop.
    A shim that claims Tier 1.1 and then renders the wrong thing is worse than
    one that refuses."""


class Query(object):
    """One RayQuery object and everything reachable from its handle."""

    def __init__(self, fn, alloc_block, alloc):
        self.fn = fn
        self.handle = alloc.result
        self.alloc = alloc
        self.alloc_block = alloc_block
        self.const_flags = _imm(alloc.args[1])
        self.trace = None
        self.trace_block = None
        self.proceeds = []          # [(block, instr)]
        self.commits = []
        self.aborts = []
        self.proc_commits = []
        self.candidate_ops = []
        self.committed_ops = []
        self.loop = None            # (header, latch, body) or None
        # A loop left top-tested (0.72.0): the Proceed is in the block that
        # loops, so `latch` is that block and `header` its body's first.
        self.top = False
        # Edges leaving the loop body other than through the latch, a `break`
        # or `return` (0.72.0): [(block, target)], numbered from 1 in order.
        self.exits = []
        # Does any accessor need the answer that only the shader TABLE has?
        # Set by the collect pass; drives the local root signature and the
        # wider hit records, both of which cost something and are therefore
        # only added for a shader that asks.
        self.needs_record_constants = False
        # Set by the lowering: is RayFlags read INSIDE the Proceed loop?
        # Only then does a generated hit shader call dx.op.rayFlags, and
        # only then may it be declared, since an unused declare is itself a
        # validation error.
        self.rayflags_in_loop = False
        # Committed state read inside the Proceed loop (0.66.0). With `track`
        # the any-hit keeps it in the payload as it changes; see lower.py.
        self.committed_in_loop = False
        self.track = False
        # A triangle query whose body WRITES where a procedural candidate
        # reaches it (0.69.0): see _mark_procedural_effects.
        self.proc_effects = False
        # A procedural query with Abort() or committed state read for a
        # triangle candidate (0.71.0): see _reject_function.
        self.track_proc = False
        # Several queries in one entry point, 0.43.0: the first is returned and
        # carries the rest here. Each names its raygen values with its own
        # prefix, so two traces never define the same %name; the first keeps
        # "rq", so a one-query shader lowers byte for byte as before.
        self.others = []
        self.pfx = 'rq'
        self.qid = 0
        # Several queries where one needs an intersection shader (0.77.0):
        # each query gets hit shaders and records of its own, its TraceRay
        # passing its query id as the ray contribution, because an
        # intersection shader has no payload to read a query id from.
        self.per_query = False
        self.rec_leaves = set()     # carried indices a handle chain starts from (0.78.0)
        # One query traced more than once (0.73.0): each search is a query of
        # its own here, owning the ops it reaches. (its traces, owner
        # function); the traces that start it, more than one where a branch
        # chooses; and then the ray as the phis at the block where the search
        # starts make it, that block, and the phis.
        self.segment = None
        # Proceed() without a loop (0.74.0): one entry per call, in order,
        # with the region its true edge guards (see _find_chain).
        self.chain = None
        self.chain_region = {}
        # Abort() outside the Proceed loop (0.75.0): before the search, so it
        # searches nothing; or after it, where it changes nothing.
        self.pre_aborts = []
        self.post_aborts = []
        self.traces = []
        self.merged_args = None
        self.route_block = None
        self.trace_phis = []

    # --- derived ---------------------------------------------------------

    @property
    def dyn_flags(self):
        """The flags passed to TraceRayInline, which OR with the template's."""
        return _imm(self.args[3])

    @property
    def args(self):
        """TraceRayInline's operands; for a search a branch chooses the trace
        of, the phis that merge them (0.73.0)."""
        return self.merged_args or self.trace.args

    @property
    def ray_flags(self):
        """The flags KNOWN AT COMPILE TIME. Used for classification only.

        When TraceRayInline is given a runtime value, dyn_flags is None and
        this is just the template's flags. That is the honest answer for
        deciding whether traversal is provably fixed-function: it is not, so a
        query with no Proceed loop and no FORCE_OPAQUE template is still
        refused. What must NOT use this is the traceRay call itself, which
        needs flags_operand."""
        return (self.const_flags or 0) | (self.dyn_flags or 0)

    @property
    def flags_operand(self):
        """The RayFlags operand text for traceRay, and any setup it needs.

        (setup_lines, operand_text). Static flags are a literal; a runtime
        value is OR'd with the template's flags, because RayQuery<FLAGS> means
        both apply and TraceRay takes only one operand."""
        if self.dyn_flags is not None:
            return [], 'i32 %d' % self.ray_flags
        from dxil import operand_name
        dyn = operand_name(self.args[3])
        return (['  %%%s.flags = or i32 %s, %d' % (self.pfx, dyn, self.const_flags or 0)],
                'i32 %%%s.flags' % self.pfx)

    @property
    def as_handle(self):
        from dxil import operand_name
        return operand_name(self.args[2])

    @property
    def ray_args(self):
        """mask, origin xyz, tmin, direction xyz, tmax, as operand text."""
        a = self.args
        return {'mask': a[4], 'origin': a[5:8], 'tmin': a[8],
                'direction': a[9:12], 'tmax': a[12]}

    @property
    def needs_anyhit(self):
        """A Proceed loop means candidates are being inspected in the shader,
        which is exactly what an any-hit shader does."""
        return self.loop is not None

    @property
    def needs_intersection(self):
        """A procedural commit means the loop body is an INTERSECTION shader."""
        return bool(self.proc_commits)

    @property
    def needs_both(self):
        """A procedural query: the loop body becomes TWO shaders, 0.69.0.

        Until 0.69.0 this meant commits of BOTH kinds. A query committing only
        procedural hits got an empty triangle hit group, but RayQuery treats
        triangles too: Proceed() yields a non-opaque one to the body (whose
        writes and Abort() then apply to it) and commits an opaque one
        itself. Both were lost, measured: a body counting every candidate
        missed 14450 triangle candidates, and an opaque triangle came back as
        nothing committed. So every procedural query gets the triangle
        any-hit made from its body and the triangle closest-hit as well.

        Commits of BOTH kinds, so the loop body becomes TWO shaders.

        A hit group is either triangles or procedural, never both, so this
        needs two of them, and two closest-hits as well, because one writes
        committed status 1 and the other 2. The scene side of it is the shim's
        problem: which record a geometry resolves to is chosen by
        InstanceContributionToHitGroupIndex, which the APPLICATION set. That is
        handled by the typed shader table, and is refused only when the
        application routed both kinds to the SAME record.
        """
        return bool(self.proc_commits) or self.proc_effects

    def pattern(self):
        if not self.proceeds:
            # Never asked to Proceed(), so never searched (0.74.0): nothing is
            # committed, whatever the flags.
            return 1, 'never searched (no Proceed() call)'
        if self.needs_both:
            return 5, 'both triangle and procedural commits (two hit groups)'
        if self.needs_intersection:
            return 4, 'procedural primitives (generated intersection shader)'
        if self.needs_anyhit:
            return 3, 'alpha-tested closest hit (generated any-hit shader)'
        if self.ray_flags & 0x004:
            return 2, 'shadow / visibility (miss shader only)'
        if self.ray_flags & 0x001:
            return 1, 'opaque closest hit (no any-hit shader)'
        # No loop and not forced opaque: traversal would have yielded
        # candidates the shader never inspected, so the source never committed
        # them. Refuse rather than guess what was meant.
        raise Unsupported(
            'query has no Proceed loop but is not FORCE_OPAQUE '
            '(flags %s); no lowering is defined for this' % flag_names(self.ray_flags))


_ICMP_CONST = re.compile(r'^icmp (eq|ne) i32 (\S+), (\S+?)\s*$')


def attr_groups(module):
    """callee name -> the attribute group TEXT its declaration carries: a
    bare `nounwind` writes memory, `readnone` and `readonly` do not. The same
    reader as _attr_groups in lower.py."""
    attrs = {}
    for line in module.lines:
        m = re.match(r'^attributes\s+#(\d+)\s*=\s*\{(.*)\}\s*$', line)
        if m:
            attrs[m.group(1)] = m.group(2)
    out = {}
    for line in module.lines:
        m = re.match(r'^declare\s+.*@([\w.$]+)\s*\(.*\)\s*#(\d+)\s*$', line)
        if m:
            out[m.group(1)] = attrs.get(m.group(2), '')
    return out


def _mark_procedural_effects(module, fn, q):
    """A triangle query whose body WRITES where a procedural candidate
    reaches it gets an intersection shader made from its body too (0.69.0).
    RayQuery yields procedural candidates to every body; the empty
    intersection shader such a query had made those writes vanish: a body
    counting every candidate missed 7396 procedural ones, measured."""
    if not q.loop or q.proc_commits:
        return
    groups = attr_groups(module)
    for label in sorted(reach_as(fn, q, 1)):
        for i in fn.block(label).instrs:
            if not i.callee or not i.callee.startswith('dx.op.'):
                continue
            if (i.callee.startswith('dx.op.rayQuery_')
                    or i.callee == 'dx.op.allocateRayQuery'):
                continue
            attr = groups.get(i.callee)
            if attr is not None and ('readnone' in attr or 'readonly' in attr):
                continue
            q.proc_effects = True
            return


def reach_as_triangle(fn, q):
    """The loop-body blocks a TRIANGLE candidate can reach (0.69.0)."""
    return reach_as(fn, q, 0)


def reach_as(fn, q, kind):
    """The loop-body blocks a candidate of `kind` can reach (0.69.0): 0 a
    non-opaque triangle, 1 a procedural primitive.

    CandidateType() folds to `kind` in the shader made for that kind, so an
    `icmp eq|ne` of it against a constant is known, and a branch on one has a
    dead edge. Followed from the loop header; anything this cannot decide
    counts as reachable."""
    header, _, body = q.loop
    starts = [c['entry'] for c in q.chain if c['entry']] if q.chain else [header]
    types = set()
    for label in body:
        for i in fn.block(label).instrs:
            if i.dxop == CANDIDATE_TYPE and i.result:
                types.add(i.result)
    known = {}
    for label in body:
        for i in fn.block(label).instrs:
            m = _ICMP_CONST.match(i.body) if i.result else None
            if not m:
                continue
            a, b = m.group(2), m.group(3)
            if a in types and re.match(r'^-?\d+$', b):
                k = int(b)
            elif b in types and re.match(r'^-?\d+$', a):
                k = int(a)
            else:
                continue
            known[i.result] = (k == kind) == (m.group(1) == 'eq')
    seen, todo = set(), list(starts)
    while todo:
        label = todo.pop()
        if label in seen or label not in body:
            continue
        seen.add(label)
        blk = fn.block(label)
        term = blk.terminator
        m = re.match(r'^br i1 (\S+), label (\S+), label (\S+)$', term.body) if term else None
        if m and m.group(1) in known:
            todo.append(m.group(2) if known[m.group(1)] else m.group(3))
        else:
            todo.extend(blk.successors())
    return seen


def _imm(arg):
    m = re.match(r'^i32\s+(-?\d+)$', arg.strip())
    return int(m.group(1)) if m else None


def analyze(module, fn_name=None):
    """Return the single Query in the module, or raise Unsupported."""
    _reject_module(module)

    fn = module.function(fn_name) if fn_name else None
    if fn is None:
        cands = [f for f in module.functions if f.find(ALLOCATE)]
        if not cands:
            raise Unsupported('no dx.op.allocateRayQuery in this module')
        if len(cands) > 1:
            raise Unsupported('RayQuery used in %d functions; only one entry '
                              'point is supported' % len(cands))
        fn = cands[0]

    allocs = fn.find(ALLOCATE)
    handles = [a.result for _, a in allocs]
    queries = []
    for block, alloc in allocs:
        # A query traced more than once (0.73.0): TraceRayInline again starts a
        # new search and forgets the old one, so each trace is lowered as a
        # query of its own, with its own TraceRay and payload, as several
        # queries already are (0.43.0).
        traces = [i for _, i in fn.find(TRACE_INLINE) if alloc.result in i.uses()]
        owner, searches = _trace_owner(fn, alloc.result, traces) if len(traces) > 1 \
            else (None, [None])
        for t in searches:
            q = Query(fn, block, alloc)
            k = len(queries)
            if k:
                q.pfx, q.qid = 'rq.q%d' % k, k
            if owner:
                q.segment = (t, owner)
            _collect(fn, q, [h for h in handles if h != q.handle])
            _find_loop(fn, q)
            if q.chain:
                _check_chain_between(module, fn, q)
            if len(q.traces) > 1:
                _merge_traces(fn, q, owner)
            _mark_procedural_effects(module, fn, q)
            queries.append(q)
    # Before the per-query checks, so a query nested in another's loop is
    # refused for THAT, not for whatever it happens to read there.
    if len(queries) > 1:
        _check_queries(fn, queries)
        if any(x.needs_both for x in queries):
            # Its own records per query (0.77.0): the shim's own layout keeps
            # up to 15 ray contributions per scene copy.
            if len(queries) > MAX_PER_QUERY:
                raise Unsupported(
                    '%d RayQuery objects in %s, one needing an intersection shader; '
                    'records of their own are made for at most %d'
                    % (len(queries), fn.name, MAX_PER_QUERY))
            for x in queries:
                x.per_query = True
    for q in queries:
        _reject_function(fn, q)
        # Classify here rather than on demand, so that a shape with no defined
        # lowering is refused by analysis itself. Leaving it lazy meant a caller
        # that never asked for the pattern got a Query back for a shader that
        # cannot be lowered at all.
        q.pattern_num, q.pattern_desc = q.pattern()
    queries[0].others = queries[1:]
    return queries[0]


def _trace_owner(fn, handle, traces):
    """Which search each op of a query traced more than once reads (0.73.0).

    A TraceRayInline starts a search and forgets the one before, so an op
    reads the search of the traces that reach it with no other trace of the
    query between: reaching definitions, a trace killing the rest. Traces
    that reach one op together are one search, a branch choosing its ray.
    Returns owner(block, instr) -> that search (a frozenset of trace
    indices), and the searches in order of their first trace."""
    labels = [b.label for b in fn.blocks]
    preds = fn.preds()
    idx = dict((id(t), k) for k, t in enumerate(traces))
    last = {}                      # block -> its last trace
    for b in fn.blocks:
        for i in b.instrs:
            if id(i) in idx:
                last[b.label] = idx[id(i)]
    inn = dict((l, frozenset()) for l in labels)
    out = dict((l, frozenset([last[l]]) if l in last else frozenset()) for l in labels)
    changed = True
    while changed:
        changed = False
        for l in labels:
            r = frozenset().union(*[out[p] for p in preds[l]]) if preds[l] else frozenset()
            if r != inn[l]:
                inn[l] = r
                changed = True
            o = frozenset([last[l]]) if l in last else r
            if o != out[l]:
                out[l] = o
                changed = True
    parent = list(range(len(traces)))

    def find(k):
        while parent[k] != k:
            k = parent[k]
        return k

    reach = {}
    for b in fn.blocks:
        cur = inn[b.label]
        for i in b.instrs:
            if id(i) in idx:
                cur = frozenset([idx[id(i)]])
                reach[id(i)] = cur
                continue
            if i.dxop is None or handle not in i.uses():
                continue
            if not cur:
                raise Unsupported(
                    '%s at "%s" can be reached with no TraceRayInline of its query '
                    'before it' % (KNOWN.get(i.dxop, i.dxop), i.body[:60]))
            reach[id(i)] = cur
            ks = sorted(cur)
            for k in ks[1:]:
                parent[find(k)] = find(ks[0])
    groups = {}
    for k in range(len(traces)):
        groups.setdefault(find(k), []).append(k)
    search = dict((k, frozenset(g)) for g in groups.values() for k in g)
    searches = sorted(set(search.values()), key=min)

    def owner(block, instr):
        return search[min(reach[id(instr)])]
    owner.out = out
    owner.traces = traces
    return owner, searches


def _merge_traces(fn, q, owner):
    """A search a branch chooses the ray of (0.73.0): the traces on each arm
    become phis at the block where the search starts, the first Proceed's,
    and the TraceRay goes there. That block's predecessors each have to come
    from exactly one of the traces, and everything else of the query has to
    come after it."""
    if not q.loop or q.top:
        raise Unsupported(
            'a query traced on either arm of a branch, without a rotated Proceed '
            'loop to start the search at')
    if any(b.label in q.loop[2] for b, _ in q.traces):
        raise Unsupported(
            'RayQuery %s is traced again inside its own Proceed loop; that loop '
            'becomes an any-hit shader, which cannot call TraceRay' % q.handle)
    guard =[(b, i) for b, i in q.proceeds if b.label != q.loop[1]]
    if len(guard) != 1:
        raise Unsupported('a query traced on either arm of a branch, with no '
                          'guard Proceed to start the search at')
    p = guard[0][0]
    mine = dict((id(t), t) for _b, t in q.traces)
    ks = dict((id(t), k) for k, t in enumerate(owner.traces))
    by_k = dict((ks[id(t)], t) for t in mine.values())
    if any(b.label == p.label for b, _ in q.traces):
        raise Unsupported('a query traced on either arm of a branch, and again in '
                          'the block its search starts at')
    dom = fn.dominators()
    for b, i in q.proceeds + q.commits + q.aborts + q.proc_commits + \
            q.candidate_ops + q.committed_ops:
        if p.label not in dom.get(b.label, ()):
            raise Unsupported(
                'a query traced on either arm of a branch is used at "%s" before '
                'the arms meet' % i.body[:60])
    preds = fn.preds()[p.label]
    src = []
    for pr in preds:
        r = [k for k in owner.out[pr] if k in by_k]
        if len(r) != 1 or len(owner.out[pr]) != 1:
            raise Unsupported(
                'a query traced on either arm of a branch, with %s reaching the '
                'search from more than one of them' % pr)
        src.append((pr, by_k[r[0]]))
    first = q.traces[0][1]
    merged = list(first.args)
    for j in range(2, len(first.args)):
        vals = [t.args[j].strip() for _pr, t in src]
        if all(v == vals[0] for v in vals):
            merged[j] = vals[0]
            continue
        ty = vals[0].split()[0]
        if ty == '%dx.types.Handle':
            raise Unsupported(
                'a query traced on either arm of a branch against a different '
                'acceleration structure on each')
        name = '%%%s.ta%d' % (q.pfx, j)
        q.trace_phis.append('  %s = phi %s %s' % (name, ty, ', '.join(
            '[ %s, %s ]' % (t.args[j].strip().split(None, 1)[1], pr) for pr, t in src)))
        merged[j] = '%s %s' % (ty, name)
    q.merged_args = merged
    q.route_block = p


# Queries with records of their own (0.77.0): one scene copy holds 15 ray
# contributions (shimscene::PairsPerStructure).
MAX_PER_QUERY = 15


def _check_queries(fn, queries):
    """Several queries in one entry point, 0.43.0.

    Each becomes its own TraceRay with its own payload, and ONE any-hit serves
    them all, choosing the loop body by a payload field the raygen sets before
    each trace. 33 of Unreal's Lumen and Niagara shaders are this shape, two
    queries one after the other. The check this replaces counted
    ALLOCATIONS and called them concurrent; what actually matters is below."""
    for q in queries:
        if not q.loop:
            continue
        others = [x.handle for x in queries if x is not q and x.handle != q.handle]
        for label in q.loop[2]:
            for instr in fn.block(label).instrs:
                used = instr.uses()
                for h in others:
                    if h in used:
                        raise Unsupported(
                            'RayQuery %s is used inside the Proceed loop of %s; '
                            'that loop becomes an any-hit shader, which cannot '
                            'call TraceRay' % (h, q.handle))
        # The same query traced again inside its own loop (0.73.0).
        for x in queries:
            if x is q or x.handle != q.handle:
                continue
            if any(b.label in q.loop[2] for b, _ in _ops(x)):
                raise Unsupported(
                    'RayQuery %s is traced again inside its own Proceed loop; that '
                    'loop becomes an any-hit shader, which cannot call TraceRay'
                    % q.handle)


def _ops(q):
    """Every (block, instr) the query owns."""
    return ([(q.trace_block, q.trace)] + q.proceeds + q.commits + q.aborts +
            q.proc_commits + q.candidate_ops + q.committed_ops)


def _collect(fn, q, others=()):
    """Walk every dx.op call that reads the query handle. `others` are the
    handles of the entry point's other queries, whose ops belong to them."""
    for block, instr in fn.instrs():
        op = instr.dxop
        if op is None:
            continue
        # Only rayQuery ops take the handle; anything else is ordinary code.
        if q.handle not in instr.uses():
            if op in KNOWN and op != ALLOCATE and not any(
                    h in instr.uses() for h in others):
                raise Unsupported('%s at "%s" does not use the query handle'
                                  % (KNOWN[op], instr.body[:60]))
            continue
        # Traced more than once (0.73.0): another search.
        if q.segment and q.segment[1](block, instr) != q.segment[0]:
            continue
        if op not in KNOWN:
            raise Unsupported(
                'unrecognised rayQuery opcode %d at "%s". This project has '
                'only verified %s. Refusing rather than guessing.'
                % (op, instr.body[:60], ', '.join(str(k) for k in sorted(KNOWN))))
        if op == TRACE_INLINE:
            if q.trace is not None and not q.segment:
                raise Unsupported('more than one TraceRayInline on one query')
            if q.trace is None:
                q.trace, q.trace_block = instr, block
            q.traces.append((block, instr))
        elif op == PROCEED:
            q.proceeds.append((block, instr))
        elif op == COMMIT_NON_OPAQUE:
            q.commits.append((block, instr))
        elif op == ABORT:
            q.aborts.append((block, instr))
        elif op == COMMIT_PROCEDURAL:
            q.proc_commits.append((block, instr))
        elif op in CANDIDATE_OPS:
            q.candidate_ops.append((block, instr))
            if op in RECORD_OPS:
                q.needs_record_constants = True
        elif op in COMMITTED_OPS:
            if op in NO_LOWERING:
                raise Unsupported(NO_LOWERING[op])
            q.committed_ops.append((block, instr))
            # The state object then needs a local root signature and every hit
            # record two more words. Both cost something, so they are added
            # only for a shader that actually asks.
            if op in RECORD_OPS:
                q.needs_record_constants = True

    if q.trace is None:
        raise Unsupported('query is allocated but never traced')

    # Ray flags computed at runtime are FINE. dx.op.traceRay takes its
    # RayFlags as an ordinary i32 operand, not an immediate: measured, see
    # phase5/cases/reference/lib_dynflags_ref.hlsl, where DXC emits `i32 %7`
    # and the container signs.
    #
    # They were refused for one version, because `(dyn_flags or 0)` had been
    # silently folding an unknown value to 0 and tracing with the wrong flags.
    # Refusing was the right answer to THAT; passing the value through is the
    # right answer to the question. Unreal computes its ray flags at runtime in
    # 118 shaders, so folding was never going to be enough.


def _find_loop(fn, q):
    _find_loop_shape(fn, q)
    _place_aborts(fn, q)


def _place_aborts(fn, q):
    """Abort() outside the Proceed loop (0.75.0). It ends the search; only
    the Proceed() calls after it see that. One that can reach the call where
    the search starts makes that search empty, so the TraceRay is traced with
    instance mask 0 when it ran; one that cannot is after the search and
    changes nothing; one between two calls of a chain is refused."""
    body = q.loop[2] if q.loop else set()
    outer = [(b, i) for b, i in q.aborts if b.label not in body]
    q.aborts = [(b, i) for b, i in q.aborts if b.label in body]
    if not outer:
        return
    # Where the search starts.
    if not q.proceeds:
        start = None
    elif q.chain:
        start = (q.chain[0]['block'], q.chain[0]['instr'])
    elif q.loop and q.top:
        start = (q.trace_block, q.trace_block.terminator)
    elif q.loop:
        start = [(b, i) for b, i in q.proceeds if b.label != q.loop[1]][0]
    else:
        start = q.proceeds[0]
    traces = [i for _b, i in (q.traces or [(q.trace_block, q.trace)])]
    succ = dict((b.label, b.successors()) for b in fn.blocks)

    def reaches(ab, ai, rb, ri):
        if ab.label == rb.label and ai.index < ri.index:
            return not any(ai.index < t.index < ri.index for t in traces
                           if t in ab.instrs)
        if any(t.index > ai.index for t in traces if t in ab.instrs):
            return False
        seen, todo = set(), list(succ[ab.label])
        while todo:
            l = todo.pop()
            if l in seen:
                continue
            seen.add(l)
            blk = fn.block(l)
            ts = [t for t in traces if t in blk.instrs]
            if l == rb.label:
                if not any(t.index < ri.index for t in ts):
                    return True
                continue
            if ts:
                continue
            todo.extend(succ[l])
        return False
    for b, i in outer:
        if start is not None and reaches(b, i, start[0], start[1]):
            q.pre_aborts.append((b, i))
        elif q.chain and any(reaches(b, i, c['block'], c['instr']) for c in q.chain[1:]):
            raise Unsupported(
                'Abort() between two Proceed() calls without a loop; whether the next call '
                'yields depends on code that runs after the search in the lowering')
        else:
            q.post_aborts.append((b, i))


def _find_loop_shape(fn, q):
    """Locate the rotated Proceed loop, if there is one.

    The source's `while (q.Proceed())` becomes two Proceed calls: a guard in
    the preheader and the real test in the latch. So look for a natural loop
    whose latch contains a Proceed on this handle."""
    proceed_blocks = {b.label for b, _ in q.proceeds}
    found = []
    for header, latch, body in fn.natural_loops():
        if latch in proceed_blocks:
            found.append((header, latch, body))
    if len(found) > 1:
        raise Unsupported('more than one Proceed loop on one query')
    if not found:
        # Top-tested (0.72.0): DXC leaves a loop unrotated when its latch
        # already exits, `while (q.Proceed()) { ...; if (c) break; }`, so the
        # one Proceed sits in the header and the body's last block loops back.
        # The header is then the latch: an edge into it ends an iteration.
        for header, latch, body in fn.natural_loops():
            if header not in proceed_blocks:
                continue
            hdr = fn.block(header)
            pr = [i for b, i in q.proceeds if b.label == header][0]
            m = re.match(r'^br i1 (\S+), label (\S+), label (\S+)$',
                         hdr.terminator.body) if hdr.terminator else None
            if (not m or m.group(1) != pr.result or m.group(2) not in body
                    or m.group(3) in body):
                raise Unsupported('a Proceed loop tested at its top has an '
                                  'unexpected header shape')
            for i in hdr.instrs:
                if not (i.is_phi or i is pr or i is hdr.terminator):
                    raise Unsupported(
                        'the Proceed loop header computes %s itself; the any-hit '
                        'shader has no header to compute it in' % (i.result or i.body[:40]))
            first = m.group(2)
            if fn.preds()[first] != [header]:
                raise Unsupported('a Proceed loop tested at its top has an '
                                  'unexpected body entry')
            # A `continue` is a second edge back, so a second natural loop on
            # the same header: one loop, both bodies.
            same = [x for x in found if x[1] == header]
            if same:
                same[0][2].update(body)
                continue
            found.append((first, header, set(body)))
            q.top = True
        if len(found) > 1:
            raise Unsupported('more than one Proceed loop on one query')
    if found:
        q.loop = found[0]
        _absorb_exits(fn, q)
        return

    # No loop. One bare Proceed is the FORCE_OPAQUE shape, where traversal is
    # entirely fixed function and Proceed never yields.
    if len(q.proceeds) == 1 and q.ray_flags & 1:
        return
    if q.proceeds:
        _find_chain(fn, q)


def _find_chain(fn, q):
    """Proceed() called without a loop, once or more (0.74.0).

    Each call yields the next candidate, and the code its true edge guards is
    that candidate's body; after the last call the shader never asks again,
    so the search stops there. So the calls are one virtual loop: the any-hit
    counts candidates in the payload and runs the k-th call's body for the
    k-th, ending the search after the last; the raygen reads each call's
    result as whether that many candidates came. Lowered with the loop
    machinery, over a header and latch of its own."""
    labels = [b.label for b in fn.blocks]
    dom = fn.dominators()
    preds = fn.preds()
    succ = dict((b.label, b.successors()) for b in fn.blocks)
    calls = sorted(q.proceeds, key=lambda bi: bi[1].index)

    def before(a, b):
        (ba, ia), (bb, ib) = a, b
        if ba.label == bb.label:
            return ia.index < ib.index
        return ba.label in dom.get(bb.label, ())

    for k in range(len(calls) - 1):
        if not before(calls[k], calls[k + 1]):
            raise Unsupported('Proceed() calls without a loop that are not one after '
                              'another on every path')
    chain = []
    for k, (b, p) in enumerate(calls):
        term = b.terminator
        m = re.match(r'^br i1 (\S+), label (\S+), label (\S+)$', term.body) if term else None
        entry = None
        if m and m.group(1) == p.result and m.group(2) != m.group(3) and \
                preds.get(m.group(2)) == [b.label]:
            entry = m.group(2)
        region = set()
        if entry:
            region = set(l for l in labels if entry in dom.get(l, ()))
            if k + 1 < len(calls) and calls[k + 1][0].label in region:
                raise Unsupported('a Proceed() inside the body of the Proceed() before '
                                  'it, without a loop')
        exits = [(l, s) for l in sorted(region, key=labels.index) for s in succ[l]
                 if s not in region]
        rets = [l for l in sorted(region, key=labels.index)
                if fn.block(l).terminator and fn.block(l).terminator.body.strip() == 'ret void']
        targets = sorted(set(s for _, s in exits), key=labels.index)
        if len(targets) > 1 or (targets and rets):
            raise Unsupported('the body of a Proceed() without a loop has more than one '
                              'way out')
        chain.append({'block': b, 'instr': p, 'entry': entry, 'region': region,
                      'exit': targets[0] if targets else None, 'exits': exits,
                      'rets': rets})
    # After a call's body, the next call is made on every path, or the search
    # would stop there on some.
    for k in range(len(chain) - 1):
        c, nb = chain[k], chain[k + 1]['block'].label
        start = c['exit'] if c['entry'] else None
        if c['entry'] is None:
            if c['block'].label == nb:
                continue
            todo = [s for s in succ[c['block'].label]]
        elif start is None:
            continue           # the body returns: the search ends with it
        else:
            todo = [start]
        seen = set()
        while todo:
            l = todo.pop()
            if l == nb or l in seen:
                continue
            seen.add(l)
            t = fn.block(l).terminator
            if t is not None and t.body.strip() == 'ret void':
                raise Unsupported('a Proceed() without a loop that is not always followed '
                                  'by the next one')
            todo.extend(succ[l])
    body = set()
    for k, c in enumerate(chain):
        body |= c['region']
        for l in c['region']:
            q.chain_region[l] = k
    for b, i in q.commits + q.proc_commits + q.candidate_ops:
        if i.dxop == RAY_FLAGS:
            continue
        if b.label not in body:
            raise Unsupported(
                'the candidate is used at "%s", where no Proceed() result guards it'
                % i.body[:60])
    q.chain = chain
    q.loop = ('%%rq.pd%d' % q.qid, '%%rq.pn%d' % q.qid, body)


def _check_chain_between(module, fn, q):
    """Code between two Proceed() calls runs once in the raygen, after the
    search; the later calls' bodies run during it. So a write there before a
    body that touches memory would change order (0.74.0)."""
    if len(q.chain) < 2:
        return
    groups = attr_groups(module)

    def touches(i, write):
        if not i.callee or not i.callee.startswith('dx.op.') or \
                i.callee.startswith('dx.op.rayQuery_') or i.callee == 'dx.op.allocateRayQuery':
            return False
        g = groups.get(i.callee, '')
        if write:
            return 'readnone' not in g and 'readonly' not in g
        return 'readnone' not in g
    later = [i for c in q.chain[1:] for l in c['region'] for i in fn.block(l).instrs
             if touches(i, False)]
    if not later:
        return
    dom = fn.dominators()
    first = q.chain[0]
    lastb = q.chain[-1]['block'].label
    succ = dict((b.label, b.successors()) for b in fn.blocks)

    def reaches(a, target):
        seen, todo = set(), [a]
        while todo:
            l = todo.pop()
            if l == target:
                return True
            if l in seen:
                continue
            seen.add(l)
            todo.extend(succ[l])
        return False
    for b in fn.blocks:
        if b.label in q.loop[2] or first['block'].label not in dom.get(b.label, ()):
            continue
        if not reaches(b.label, lastb):
            continue
        for i in b.instrs:
            if b.label == first['block'].label and i.index <= first['instr'].index:
                continue
            if b.label == lastb and i.index >= q.chain[-1]['instr'].index:
                continue
            if touches(i, True):
                raise Unsupported(
                    'code between two Proceed() calls writes memory (%s) that a later '
                    'call\'s body may read; the body runs during the search, before it'
                    % i.callee)


def _absorb_exits(fn, q):
    """The loop's exits other than the latch's, 0.72.0.

    A `break` or `return` leaves the body without reaching the latch, and DXC
    sinks what the source did just before it (a commit, a candidate read)
    into the block that leaves, OUTSIDE the natural loop. Those blocks are
    the candidate's, so they join the body; every other edge out of the body
    is an exit. An exit ends the search with the committed state as it
    stands, as Abort() does, and the raygen goes on where the edge went."""
    header, latch, body = q.loop
    body = set(body)
    labels = [b.label for b in fn.blocks]
    succ = {b.label: b.successors() for b in fn.blocks}
    preds = fn.preds()
    need = set(b.label for b, i in q.commits + q.proc_commits)
    need |= set(b.label for b, i in q.candidate_ops if i.dxop != RAY_FLAGS)
    need -= body
    # Outside blocks the body reaches without coming back to it, other than
    # through the latch: running out of candidates leaves with none current.
    reach, todo = set(), [s for l in body if l != latch for s in succ[l] if s not in body]
    while todo:
        l = todo.pop()
        if l in reach or l in body:
            continue
        reach.add(l)
        todo.extend(succ[l])
    need &= reach
    if need:
        # And every block on the way to one.
        back, todo = set(), list(need)
        while todo:
            l = todo.pop()
            if l in back or l in body or l not in reach:
                continue
            back.add(l)
            todo.extend(preds[l])
        for l in sorted(back, key=labels.index):
            for p in preds[l]:
                if p == latch or (p not in body and p not in back):
                    raise Unsupported(
                        'the candidate is read or committed in %s, after the Proceed '
                        'loop, which code that did not leave the loop also reaches' % l)
            for b, i in q.committed_ops:
                if b.label == l:
                    raise Unsupported(
                        'committed state is read in %s, on the way out of the Proceed '
                        'loop after the candidate is used there' % l)
        body |= back
    # An Abort() DXC sank into the way out joins too (0.75.0), where only the
    # way out reaches it; anywhere else it is an Abort() after the loop.
    for ab in sorted(set(b.label for b, i in q.aborts) - body, key=labels.index):
        if ab not in reach:
            continue
        back, todo = set(), [ab]
        while todo:
            l = todo.pop()
            if l in back or l in body or l not in reach:
                continue
            back.add(l)
            todo.extend(preds[l])
        if all(p != latch and (p in body or p in back) for l in back for p in preds[l]):
            body |= back
    q.loop = (header, latch, body)
    q.exits = [(l, s) for l in sorted(body, key=labels.index) if l != latch
               for s in succ[l] if s not in body]


def _reject_module(module):
    """Cases the brief says have no valid lowering, checked before anything."""
    m = re.search(r'!dx\.shaderModel\s*=\s*!\{!(\d+)\}', module.text)
    if m:
        node = re.search(r'^!%s = !\{!"(\w+)", i32 (\d+), i32 (\d+)\}'
                         % m.group(1), module.text, re.M)
        if node and node.group(1) != 'cs':
            raise Unsupported(
                'RayQuery in a "%s" shader. DispatchRays only launches raygen, '
                'so there is no promotion path from any other stage.'
                % node.group(1))

    if re.search(r'addrspace\(3\)', module.text):
        raise Unsupported('shader uses groupshared memory; a raygen shader has '
                          'no thread group')


def _reject_function(fn, q):
    """Checks that need the query, so they run after collection."""
    for _, instr in fn.instrs():
        op = instr.dxop
        if op is None:
            continue
        if op == 80:                      # dx.op.barrier
            raise Unsupported('shader uses a group barrier; a raygen shader '
                              'has no thread group')
        if instr.callee and '.wave' in instr.callee.lower():
            raise Unsupported('shader uses wave intrinsics around the query; '
                              'promotion to raygen changes lane occupancy')


    if q.loop:
        _, _, body = q.loop
        for label in body:
            blk = fn.block(label)
            for instr in blk.instrs:
                if instr.dxop in COMMITTED_OPS:
                    q.committed_in_loop = True
        # An intersection shader has RayTCurrent(), which IS the committed t,
        # and no payload for the rest; only what a procedural candidate
        # reaches matters (0.71.0, the whole body before).
        if q.proc_commits:
            for label in sorted(reach_as(fn, q, 1)):
                for instr in fn.block(label).instrs:
                    if instr.dxop in COMMITTED_OPS and instr.dxop != COMMITTED_RAY_T:
                        raise Unsupported(
                            'committed state other than CommittedRayT() is read '
                            'inside a Proceed loop that becomes an intersection '
                            'shader, which has no payload to read it from')
        # Committed state as it stands during traversal, 0.66.0: the any-hit
        # keeps it in the payload, and since an OPAQUE candidate would be
        # committed without reaching an any-hit, the ray is traced forced
        # non-opaque and the any-hit applies the original flags itself, with
        # each record saying whether its geometry is opaque. A query forced
        # opaque never runs its loop, so has nothing to track.
        if q.committed_in_loop and not q.needs_intersection and not (q.ray_flags & 1):
            q.track = True
            q.needs_record_constants = True
        # Abort(), exactly (0.67.0). Traversal carries on after it, so an
        # OPAQUE candidate met later would be committed by traversal itself,
        # which RayQuery never does. That matters only where committed state
        # is read; there the query is tracked, so every candidate reaches the
        # any-hit, which rejects them all once aborted. Where it is not read,
        # the any-hit ends the search instead (lower.py).
        # A break or return ends the search the same way (0.72.0).
        if ((q.aborts or q.exits or q.chain) and q.committed_ops
                and not q.needs_intersection and not (q.ray_flags & 1)):
            q.track = True
            q.needs_record_constants = True
        # Tracked procedural, 0.71.0: Abort() anywhere in a procedural query's
        # loop, or committed state read where a triangle candidate reaches.
        # An intersection shader cannot stop traversal or write the payload,
        # so: the ray is traced forced non-opaque (the 0.66.0 tracking, the
        # any-hit applying opacity), every procedural commit goes through a
        # procedural any-hit that records it in the payload, an Abort() in the
        # intersection shader is reported as a hit of kind 127 that the
        # procedural any-hit turns into AcceptHitAndEndSearch, every abort
        # ends the search, and the closest-hits leave the payload alone once
        # one has. The intersection shader then sees the forced flags, so it
        # takes the query's own as constants: computed at run time, refused.
        if q.needs_both:
            tri_cmt = any(instr.dxop in COMMITTED_OPS
                          for label in reach_as(fn, q, 0)
                          for instr in fn.block(label).instrs)
            if q.chain:
                raise Unsupported('Proceed() without a loop in a query that commits '
                                  'procedural hits or writes where they reach')
            # A break or return ends the search the same way (0.72.0).
            if q.aborts or q.exits or tri_cmt:
                if q.dyn_flags is None:
                    raise Unsupported(
                        '%s or committed state read for a triangle candidate in a '
                        'Proceed loop that commits procedural hits, with ray flags '
                        'computed at run time; the exact lowering needs the query\'s own '
                        'opacity and cull flags as constants in its intersection shader'
                        % ('A break or return' if q.exits and not q.aborts else 'Abort()'))
                if len(q.exits) > 100:
                    raise Unsupported('%d ways out of a Proceed loop that becomes an '
                                      'intersection shader; it says which in a hit kind, '
                                      'and 100 are free' % len(q.exits))
                q.track_proc = True
                q.track = True
                q.needs_record_constants = True
        # CandidateProceduralPrimitiveNonOpaque() in the intersection shader
        # (0.70.0): computed from the ray's flags and the record's word 2,
        # the geometry's and instance's opacity, as traversal decides it.
        # Tracked, the flags are the query's constant ones (0.71.0).
        if q.proc_commits and any(i.dxop == CANDIDATE_PROC_NON_OPAQUE
                                  for b, i in q.candidate_ops if b.label in body):
            q.needs_record_constants = True
            if not q.track_proc:
                q.rayflags_in_loop = True
        # Its intersection shader has no payload to read committed state from
        # (0.69.0).
        if q.proc_effects and q.committed_in_loop:
            raise Unsupported(
                'committed state is read inside a Proceed loop whose body also writes '
                'for procedural candidates; the intersection shader that serves them '
                'has no payload to read it from')

