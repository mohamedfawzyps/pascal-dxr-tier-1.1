"""A query used inside another query's Proceed loop (0.79.0, plan step 3 l).

The outer loop would become an any-hit shader, and an any-hit shader cannot
call TraceRay, so the inner query had nowhere to go. Refused until 0.79.0.

The outer query is STEPPED instead: its loop stays in the raygen, and each of
its Proceed() calls becomes a search by a fresh query on the same ray that
commits nothing and keeps the nearest non-opaque candidate past the last one
yielded, by (t, instance, geometry, primitive). Front to back is a legal
RayQuery order, so the body sees what RayQuery may show it. A closer opaque hit
the search's own traversal committed is committed for the outer query before
any candidate behind it, without the body, as traversal does; the search then
runs again behind it. The outer query's state (the last key, the candidate, what
is committed, whether it was aborted) lives in local variables, so commits,
Abort() and every accessor become stores and loads.

Probed before building (phase5/cases/probe_nested_step.hlsl, the stepped form
written by hand): on WARP it gives what the nested original gives, 18496 hits,
and through the shim on the GTX 1070 the same, 0 mismatches.

The searches are ordinary queries, so the rest of the rewriter lowers them as
it lowers any other: nothing after this pass knows a query was stepped.

What a procedural candidate does is not reproduced: a search sees triangles
only, because recording a box needs loop state in an intersection shader, on
the limits list. So the outer body has to do nothing for a box (CandidateType()
folded, the blocks a box reaches holding no effect and leaving the loop state as
it was); otherwise the query is refused by name.

Runs on NORMALISED text, after the NVAPI fold, before analysis. A module where
no query is inside another's loop comes back unchanged.
"""

import re

import rayquery as rq
from dxil import Module

# What a stepped query keeps per candidate and per commit: field -> (type,
# candidate op, committed op, how the op is called).
_SCALAR = {
    't': ('float', rq.CANDIDATE_RAY_T, rq.COMMITTED_RAY_T),
    'inst': ('i32', rq.CANDIDATE_INSTANCE_INDEX, rq.COMMITTED_INSTANCE_INDEX),
    'id': ('i32', rq.CANDIDATE_INSTANCE_ID, rq.COMMITTED_INSTANCE_ID),
    'geom': ('i32', rq.CANDIDATE_GEOMETRY_INDEX, rq.COMMITTED_GEOMETRY_INDEX),
    'prim': ('i32', rq.CANDIDATE_PRIMITIVE_INDEX, rq.COMMITTED_PRIMITIVE_INDEX),
    'contrib': ('i32', rq.CANDIDATE_INSTANCE_CONTRIB, rq.COMMITTED_INSTANCE_CONTRIB),
    'ff': ('i1', rq.CANDIDATE_FRONT_FACE, rq.COMMITTED_FRONT_FACE),
}
_KEY = ('t', 'inst', 'geom', 'prim')
_NESTED = re.compile(r'^RayQuery (%[\w.]+) is (?:used inside the Proceed loop of (%[\w.]+)|'
                     r'traced again inside its own Proceed loop)')


def _field_of(i):
    """(field, is_committed) for an accessor, or None."""
    op = i.dxop
    for f, (_ty, c, k) in _SCALAR.items():
        if op == c:
            return f, False
        if op == k:
            return f, True
    if op in (rq.CANDIDATE_BARY, rq.COMMITTED_BARY):
        comp = re.match(r'i8\s+(\d+)', i.args[2].strip()).group(1)
        return 'b' + comp, op == rq.COMMITTED_BARY
    if op in (rq.CANDIDATE_WORLD_TO_OBJECT, rq.COMMITTED_WORLD_TO_OBJECT):
        r = re.match(r'i32\s+(\d+)', i.args[2].strip()).group(1)
        c = re.match(r'i8\s+(\d+)', i.args[3].strip()).group(1)
        return 'w%s%s' % (r, c), op == rq.COMMITTED_WORLD_TO_OBJECT
    if op in (rq.CANDIDATE_OBJECT_RAY_ORIGIN, rq.CANDIDATE_OBJECT_RAY_DIRECTION):
        comp = re.match(r'i8\s+(\d+)', i.args[2].strip()).group(1)
        return ('o' if op == rq.CANDIDATE_OBJECT_RAY_ORIGIN else 'd') + comp, False
    return None


def _ftype(f):
    if f in _SCALAR:
        return _SCALAR[f][0]
    return 'float'


def _read(f, committed, h, res):
    """The search's own accessor for field f, as a line defining res."""
    if f in _SCALAR:
        ty, c, k = _SCALAR[f]
        return '  %s = call %s @dx.op.rayQuery_StateScalar.%s(i32 %d, i32 %s)' % (
            res, ty, ty, k if committed else c, h)
    if f[0] == 'b':
        op = rq.COMMITTED_BARY if committed else rq.CANDIDATE_BARY
        return '  %s = call float @dx.op.rayQuery_StateVector.f32(i32 %d, i32 %s, i8 %s)' % (
            res, op, h, f[1])
    if f[0] == 'w':
        op = rq.COMMITTED_WORLD_TO_OBJECT if committed else rq.CANDIDATE_WORLD_TO_OBJECT
        return ('  %s = call float @dx.op.rayQuery_StateMatrix.f32(i32 %d, i32 %s, i32 %s, '
                'i8 %s)' % (res, op, h, f[1], f[2]))
    op = rq.CANDIDATE_OBJECT_RAY_ORIGIN if f[0] == 'o' else rq.CANDIDATE_OBJECT_RAY_DIRECTION
    return '  %s = call float @dx.op.rayQuery_StateVector.f32(i32 %d, i32 %s, i8 %s)' % (
        res, op, h, f[1])


_DECLS = {
    ('StateScalar', 'i32'): 'declare i32 @dx.op.rayQuery_StateScalar.i32(i32, i32)',
    ('StateScalar', 'float'): 'declare float @dx.op.rayQuery_StateScalar.f32(i32, i32)',
    ('StateScalar', 'i1'): 'declare i1 @dx.op.rayQuery_StateScalar.i1(i32, i32)',
    ('StateVector', 'float'): 'declare float @dx.op.rayQuery_StateVector.f32(i32, i32, i8)',
    ('StateMatrix', 'float'): 'declare float @dx.op.rayQuery_StateMatrix.f32(i32, i32, i32, i8)',
}


def _handle(i):
    return i.args[1].strip().split()[-1] if len(i.args) > 1 else None


def _owned(fn, h):
    """Every (block, instr) that operates on query handle h, alloc included."""
    out = []
    for b in fn.blocks:
        for i in b.instrs:
            if i.dxop == rq.ALLOCATE and i.result == h:
                out.append((b, i))
            elif i.dxop in rq.KNOWN or i.dxop in (rq.TRACE_INLINE, rq.PROCEED, rq.ABORT,
                                                    rq.COMMIT_NON_OPAQUE, rq.COMMIT_PROCEDURAL):
                if _handle(i) == h:
                    out.append((b, i))
    return out


def _check_boxes(fn, q):
    """A box reaching the outer body must do nothing there: the search does
    not yield boxes (see the module docstring)."""
    if not q.loop:
        return
    header, latch, body = q.loop
    reach = rq.reach_as(fn, q, 1)
    types = set()
    for label in body:
        for i in fn.block(label).instrs:
            if i.dxop == rq.CANDIDATE_TYPE and _handle(i) == q.handle:
                types.add(i.result)
    why = ('a procedural candidate reaches %s in the Proceed loop of %s, a query with '
           'another query inside its loop; that query is searched for triangles only '
           '(recording a box needs loop state in an intersection shader)')
    for label in sorted(reach):
        blk = fn.block(label)
        for i in blk.instrs:
            if i.is_phi or i is blk.terminator:
                continue
            if i.dxop in (rq.CANDIDATE_TYPE, rq.PROCEED) and _handle(i) == q.handle:
                continue
            m = re.match(r'^icmp (?:eq|ne) i32 (\S+), (\S+)$', i.body)
            if m and (m.group(1) in types or m.group(2) in types):
                continue
            raise rq.Unsupported(why % (i.result or i.body[:40], q.handle))
        # Leaving where the query's own Proceed() decides is the search ending.
        if any(i.dxop == rq.PROCEED and _handle(i) == q.handle for i in blk.instrs):
            continue
        for s in blk.successors():
            if s not in body and s != header:
                raise rq.Unsupported(why % (s, q.handle))
    # The loop state after a box's iteration is what it was before it.
    state = fn.block(latch if q.top else header)
    defs = {}
    for b in fn.blocks:
        for i in b.instrs:
            if i.result:
                defs[i.result] = (b, i)
    for h in [i for i in state.instrs if i.is_phi]:
        for v, pred in h.phi_incoming:
            if pred not in reach:
                continue
            seen = set()
            while v != h.result and v in defs and v not in seen:
                seen.add(v)
                b, d = defs[v]
                if not d.is_phi or b.label not in reach or b.label == state.label:
                    break
                vals = {x for x, p in d.phi_incoming if p in reach}
                if len(vals) != 1:
                    break
                v = vals.pop()
            if v != h.result:
                raise rq.Unsupported(why % (h.result, q.handle))


def step(text):
    """Step every query that has another query inside its loop."""
    for n in range(16):
        try:
            rq.analyze(Module(text))
            return text
        except rq.Unsupported as e:
            q = getattr(e, 'nested', None)
            if q is None:
                return text
            mod = Module(text)
            _check_boxes(mod.functions[0], q)
            text = _step_query(mod, q.handle, n)
    return text


def _step_query(mod, h, k):
    """The rewrite of one query, every site of it. Re-parsed per site, since
    each splits a block."""
    fn = mod.functions[0]
    ops = _owned(fn, h)
    alloc = [i for _b, i in ops if i.dxop == rq.ALLOCATE]
    traces = [i for _b, i in ops if i.dxop == rq.TRACE_INLINE]
    if len(alloc) != 1 or not traces:
        raise rq.Unsupported('RayQuery %s, with another query inside its loop, has no '
                             'single allocation and trace' % h)
    accel = {t.args[2].strip() for t in traces}
    if len(accel) != 1:
        raise rq.Unsupported('RayQuery %s, with another query inside its loop, traces '
                             'more than one acceleration structure' % h)
    cflags = alloc[0].args[1].strip().split()[-1]
    p = '%%ns%d.' % k
    # The fields: the key always, and every one the query reads.
    cand, comm = set(_KEY), set()
    for _b, i in ops:
        if i.dxop in (rq.ALLOCATE, rq.TRACE_INLINE, rq.PROCEED, rq.ABORT,
                      rq.COMMIT_NON_OPAQUE, rq.COMMIT_PROCEDURAL, rq.CANDIDATE_TYPE,
                      rq.CANDIDATE_PROC_NON_OPAQUE, rq.COMMITTED_STATUS, rq.RAY_FLAGS):
            continue
        fc = _field_of(i)
        if fc is None:
            raise rq.Unsupported('RayQuery %s, with another query inside its loop, reads '
                                 '%s, which a stepped query does not keep'
                                 % (h, rq.KNOWN.get(i.dxop, i.dxop)))
        (comm if fc[1] else cand).add(fc[0])
    for f in comm:
        if f[0] in 'od':
            raise rq.Unsupported('RayQuery %s reads a committed object-space ray' % h)
    cand |= comm
    cand = sorted(cand)
    comm = sorted(comm)
    # Local variables, in the entry block.
    allocs = []
    var = {}

    def mk(name, ty):
        store_ty = 'i32' if ty == 'i1' else ty
        var[name] = (p + name, store_ty)
        allocs.append('  %s = alloca %s, align 4' % (p + name, store_ty))
    for f in cand:
        mk('c.' + f, _ftype(f))
    for f in comm:
        mk('k.' + f, _ftype(f))
    for f in _KEY:
        mk('l.' + f, _ftype(f))
    for name, ty in (('hl', 'i32'), ('cs', 'i32'), ('ct', 'float'), ('ab', 'i32'),
                     ('af', 'i32'), ('rf', 'i32'), ('ox', 'float'), ('oy', 'float'),
                     ('oz', 'float'), ('tn', 'float'), ('dx', 'float'), ('dy', 'float'),
                     ('dz', 'float'), ('mk', 'i32')):
        mk(name, ty)

    def store(name, val):
        a, ty = var[name]
        return '  store %s %s, %s* %s, align 4' % (ty, val, ty, a)

    def load(name, res):
        a, ty = var[name]
        return '  %s = load %s, %s* %s, align 4' % (res, ty, ty, a)

    # One site at a time, re-parsed: the trace sites and simple accessors in
    # one pass (no block splits), then each Proceed().
    edits = {}
    entry_first = fn.blocks[0].instrs[0]
    n_search = 0
    for b, i in ops:
        op = i.dxop
        if op == rq.ALLOCATE:
            edits[i.index] = None
        elif op == rq.TRACE_INLINE:
            a = [x.strip().split()[-1] for x in i.args]
            # 179, h, accel, flags, mask, ox, oy, oz, tmin, dx, dy, dz, tmax
            lines = [store('hl', '0'), store('cs', '0'), store('ab', '0'),
                     store('ct', a[12]), store('ox', a[5]), store('oy', a[6]),
                     store('oz', a[7]), store('tn', a[8]), store('dx', a[9]),
                     store('dy', a[10]), store('dz', a[11]), store('mk', a[4]),
                     '  %sf%d = or i32 %s, %s' % (p, i.index, cflags, a[3]),
                     store('rf', '%sf%d' % (p, i.index)),
                     '  %sa%d = and i32 %sf%d, 4' % (p, i.index, p, i.index),
                     store('af', '%sa%d' % (p, i.index))]
            edits[i.index] = '\n'.join(lines)
        elif op == rq.ABORT:
            edits[i.index] = store('ab', '1')
        elif op == rq.COMMIT_PROCEDURAL:
            edits[i.index] = None     # a box is never yielded (_check_boxes)
        elif op == rq.COMMIT_NON_OPAQUE:
            lines = []
            for f in comm:
                lines.append(load('c.' + f, '%sv%d.%s' % (p, i.index, f)))
                lines.append(store('k.' + f, '%sv%d.%s' % (p, i.index, f)))
            lines.append(load('c.t', '%sv%d.ct' % (p, i.index)))
            lines.append(store('ct', '%sv%d.ct' % (p, i.index)))
            lines.append(store('cs', '1'))
            edits[i.index] = '\n'.join(lines)
        elif op == rq.CANDIDATE_TYPE:
            edits[i.index] = '  %s = add i32 0, 0' % i.result
        elif op == rq.CANDIDATE_PROC_NON_OPAQUE:
            edits[i.index] = '  %s = icmp eq i32 0, 1' % i.result
        elif op == rq.COMMITTED_STATUS:
            edits[i.index] = load('cs', i.result)
        elif op == rq.RAY_FLAGS:
            edits[i.index] = load('rf', i.result)
        elif op == rq.PROCEED:
            continue
        else:
            f, committed = _field_of(i)
            name = ('k.' if committed else 'c.') + f
            if _ftype(f) == 'i1':
                edits[i.index] = '%s\n  %s = icmp ne i32 %s.b, 0' % (
                    load(name, i.result + '.b'), i.result, i.result)
            else:
                edits[i.index] = load(name, i.result)
    # The searches' own allocations, one per Proceed() site.
    proceeds = [i for _b, i in ops if i.dxop == rq.PROCEED]
    srch = []
    for n in range(len(proceeds)):
        srch.append('  %ss%d = call i32 @dx.op.allocateRayQuery(i32 178, i32 %s)'
                    % (p, n, cflags))
    head = '\n'.join(allocs + srch)
    first = edits.get(entry_first.index, entry_first.line)
    edits[entry_first.index] = head if first is None else head + '\n' + first
    text = _declare(Module(_render(mod, edits)), cand, comm)
    for n in range(len(proceeds)):
        m2 = Module(text)
        fn2 = m2.functions[0]
        site = [(b, i) for b, i in _owned(fn2, h) if i.dxop == rq.PROCEED][0]
        text = _proceed_site(m2, fn2, site, p, n, list(accel)[0], cflags, cand, comm, var, store, load)
    return text


def _render(mod, edits):
    out = []
    for n, line in enumerate(mod.lines):
        if n in edits:
            if edits[n] is not None:
                out.append(edits[n])
        else:
            out.append(line)
    return '\n'.join(out)


def _declare(mod, cand, comm):
    """Declarations the searches call that the module may lack."""
    text = mod.text
    need = {('StateScalar', 'i32'), ('StateScalar', 'float')}
    for f in set(cand) | set(comm):
        ty = _ftype(f)
        if f in _SCALAR:
            need.add(('StateScalar', ty))
        elif f[0] == 'w':
            need.add(('StateMatrix', 'float'))
        else:
            need.add(('StateVector', 'float'))
    grp = None
    for line in mod.lines:
        m = re.match(r'^declare \S+ @dx\.op\.rayQuery_State\w+\.\w+\([^)]*\) (#\d+)', line)
        if m:
            grp = m.group(1)
            break
    if grp is None:
        nums = [int(x) for x in re.findall(r'^attributes #(\d+) = ', text, re.M)]
        for line in mod.lines:
            m = re.match(r'^attributes (#\d+) = \{ nounwind readonly \}$', line)
            if m:
                grp = m.group(1)
        if grp is None:
            grp = '#%d' % (max(nums) + 1 if nums else 0)
            text = text.rstrip('\n') + '\n\nattributes %s = { nounwind readonly }\n' % grp
    add = []
    for key in sorted(need):
        d = _DECLS[key]
        name = re.search(r'@([\w.]+)\(', d).group(1)
        if not re.search(r'^declare [^\n]*@' + re.escape(name) + r'\(', text, re.M):
            add.append('%s %s' % (d, grp))
    if add:
        # Beside the module's own rayQuery declarations.
        m = re.search(r'^declare .*@dx\.op\.rayQuery_Proceed[^\n]*$', text, re.M)
        text = text[:m.end()] + '\n' + '\n'.join(add) + text[m.end():]
    return text


def _proceed_site(mod, fn, site, p, n, accel, cflags, cand, comm, var, store, load):
    blk, pr = site
    s = '%ss%d' % (p, n)
    q = '%sq%d.' % (p, n)          # names of this site
    lab = lambda x: '%sq%d.%s' % (p[1:], n, x)
    L = lambda x: '%' + lab(x)
    out = ['  br label %s' % L('a'), '', lab('a') + ':']
    out += [load('ab', q + 'ab'), '  %sabn = icmp ne i32 %sab, 0' % (q, q),
            load('cs', q + 'cs'), '  %scsn = icmp ne i32 %scs, 0' % (q, q),
            load('af', q + 'af'), '  %safn = icmp ne i32 %saf, 0' % (q, q),
            '  %sfst = and i1 %scsn, %safn' % (q, q, q),
            '  %sstop = or i1 %sabn, %sfst' % (q, q, q),
            '  br i1 %sstop, label %s, label %s' % (q, L('no'), L('t')),
            '', lab('t') + ':']
    for f in ('ct', 'ox', 'oy', 'oz', 'tn', 'dx', 'dy', 'dz', 'rf', 'mk', 'hl'):
        out.append(load(f, q + 'r.' + f))
    for f in _KEY:
        out.append(load('l.' + f, q + 'l.' + f))
    # The search traces with the query's flags as given: the dynamic part is
    # what rf holds beyond the allocation's.
    out.append('  call void @dx.op.rayQuery_TraceRayInline(i32 179, i32 %s, %s, i32 %sr.rf, '
               'i32 %sr.mk, float %sr.ox, float %sr.oy, float %sr.oz, float %sr.tn, '
               'float %sr.dx, float %sr.dy, float %sr.dz, float %sr.ct)'
               % (s, accel, q, q, q, q, q, q, q, q, q, q))
    out += ['  br label %s' % L('h'), '', lab('h') + ':']
    fields = ['f'] + list(cand)

    def zero(f):
        ty = 'i32' if f == 'f' else _ftype(f)
        ty = 'i32' if ty == 'i1' else ty
        return ty, '0.000000e+00' if ty == 'float' else '0'
    for f in fields:
        ty, z = zero(f)
        out.append('  %sh.%s = phi %s [ %s, %s ], [ %sh.%s, %s ], [ %sn.%s, %s ]'
                   % (q, f, ty, z, L('t'), q, f, L('b'), q, f, L('c')))
    out += ['  %spr = call i1 @dx.op.rayQuery_Proceed.i1(i32 180, i32 %s)' % (q, s),
            '  br i1 %spr, label %s, label %s' % (q, L('b'), L('x')),
            '', lab('b') + ':',
            '  %sty = call i32 @dx.op.rayQuery_StateScalar.i32(i32 %d, i32 %s)'
            % (q, rq.CANDIDATE_TYPE, s),
            '  %stri = icmp eq i32 %sty, 0' % (q, q),
            '  br i1 %stri, label %s, label %s' % (q, L('c'), L('h')),
            '', lab('c') + ':']
    for f in cand:
        if _ftype(f) == 'i1':
            out.append(_read(f, False, s, q + 'g.' + f + '.b'))
            out.append('  %sg.%s = zext i1 %sg.%s.b to i32' % (q, f, q, f))
        else:
            out.append(_read(f, False, s, q + 'g.' + f))
    # past: no last, or the key after the last one; nearer: none yet, or the
    # key before the best one.
    g = lambda f: q + 'g.' + f
    lst = lambda f: q + 'l.' + f
    bst = lambda f: q + 'h.' + f

    def after(a, b, tag):
        # (t, inst, geom, prim) of a after b
        o = ['  %s%s.t1 = fcmp ogt float %s, %s' % (q, tag, a('t'), b('t')),
             '  %s%s.t0 = fcmp oeq float %s, %s' % (q, tag, a('t'), b('t'))]
        for f in ('inst', 'geom', 'prim'):
            o.append('  %s%s.%s1 = icmp ugt i32 %s, %s' % (q, tag, f, a(f), b(f)))
            o.append('  %s%s.%s0 = icmp eq i32 %s, %s' % (q, tag, f, a(f), b(f)))
        o += ['  %s%s.gp = and i1 %s%s.geom0, %s%s.prim1' % (q, tag, q, tag, q, tag),
              '  %s%s.g = or i1 %s%s.geom1, %s%s.gp' % (q, tag, q, tag, q, tag),
              '  %s%s.ig = and i1 %s%s.inst0, %s%s.g' % (q, tag, q, tag, q, tag),
              '  %s%s.i = or i1 %s%s.inst1, %s%s.ig' % (q, tag, q, tag, q, tag),
              '  %s%s.ti = and i1 %s%s.t0, %s%s.i' % (q, tag, q, tag, q, tag),
              '  %s%s = or i1 %s%s.t1, %s%s.ti' % (q, tag, q, tag, q, tag)]
        return o
    out += after(g, lst, 'past')
    out += ['  %snol = icmp eq i32 %sr.hl, 0' % (q, q),
            '  %spst = or i1 %snol, %spast' % (q, q, q)]
    out += after(bst, g, 'near')
    out += ['  %snof = icmp eq i32 %sh.f, 0' % (q, q),
            '  %snr = or i1 %snof, %snear' % (q, q, q),
            '  %stk = and i1 %spst, %snr' % (q, q, q),
            '  %sn.f = select i1 %stk, i32 1, i32 %sh.f' % (q, q, q)]
    for f in cand:
        ty = 'i32' if _ftype(f) == 'i1' else _ftype(f)
        out.append('  %sn.%s = select i1 %stk, %s %sg.%s, %s %sh.%s'
                   % (q, f, q, ty, q, f, ty, q, f))
    out += ['  br label %s' % L('h'), '', lab('x') + ':']
    # The search's own commit: the closest opaque hit its traversal found.
    out += ['  %ssc = call i32 @dx.op.rayQuery_StateScalar.i32(i32 %d, i32 %s)'
            % (q, rq.COMMITTED_STATUS, s),
            '  %sso = icmp eq i32 %ssc, 1' % (q, q),
            _read('t', True, s, q + 'st'),
            '  %sfd = icmp ne i32 %sh.f, 0' % (q, q),
            '  %scl = fcmp olt float %sh.t, %sst' % (q, q, q),
            '  %snso = xor i1 %sso, true' % (q, q),
            '  %sc1 = or i1 %snso, %scl' % (q, q, q),
            '  %syl = and i1 %sfd, %sc1' % (q, q, q),
            '  br i1 %syl, label %s, label %s' % (q, L('y'), L('o')),
            '', lab('y') + ':']
    for f in cand:
        out.append(store('c.' + f, q + 'h.' + f))
    for f in _KEY:
        out.append(store('l.' + f, q + 'h.' + f))
    out += [store('hl', '1'), '  br label %s' % L('k'), '', lab('o') + ':',
            '  %snt = fcmp olt float %sst, %sr.ct' % (q, q, q),
            '  %sac = and i1 %sso, %snt' % (q, q, q),
            '  br i1 %sac, label %s, label %s' % (q, L('m'), L('no')),
            '', lab('m') + ':']
    for f in comm:
        if _ftype(f) == 'i1':
            out.append(_read(f, True, s, q + 'm.' + f + '.b'))
            out.append('  %sm.%s = zext i1 %sm.%s.b to i32' % (q, f, q, f))
        else:
            out.append(_read(f, True, s, q + 'm.' + f))
        out.append(store('k.' + f, q + 'm.' + f))
    out += [store('ct', q + 'st'), store('cs', '1'), '  br label %s' % L('a'),
            '', lab('no') + ':', '  br label %s' % L('k'), '', lab('k') + ':',
            '  %s = phi i1 [ true, %s ], [ false, %s ]' % (pr.result, L('y'), L('no'))]
    edits = {pr.index: '\n'.join(out)}
    # The block's terminator now ends the last new block: its successors' phis
    # name that block.
    if blk.label:
        for sname in blk.successors():
            sb = fn.block(sname)
            for i in sb.instrs:
                if not i.is_phi:
                    break
                edits[i.index] = re.sub(re.escape(blk.label) + r'(?=\s*\])', L('k'), i.line)
    return _render(mod, edits)
