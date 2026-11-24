"""A RayQuery compute shader beside groupshared memory, group barriers or wave
ops, split at its query (0.84.0, plan step 5).

A raygen has no thread group and lays its waves out its own way, so such a
shader cannot be hosted as one. It is cut in three compute shaders instead,
run one after another over the same groups:

  A  the shader up to the query. It stores every value the rest needs into
     a scratch buffer, one slot per thread, with a flag saying the thread got
     there (it may have returned first), and, after a barrier, every
     groupshared array part B reads, per group.
  Q  the query alone: an ordinary RayQuery compute shader, which the shim
     lowers as it lowers any. It loads what A left, and stores what B needs.
  B  the shader from where the query's control flow rejoins. Every thread
     copies the groupshared arrays back, a barrier, then a thread that did
     not reach the query returns, and the rest load what they need and go on.

What crosses a cut is one of three kinds. A handle, a system value (thread
and group ids) or a constant buffer read is REBUILT where it is needed: the
same instruction gives the same result in every part, since the groups are
the same and nothing writes a constant buffer during a dispatch. Any other
scalar is STORED and loaded. Anything else (a pointer, an aggregate from a
writable resource, a vector) is refused by name.

The scratch buffer is a RWByteAddressBuffer at u0 in GI_SPACE (shapes from
DXC, phase5/cases/reference/split_scratch_ref.hlsl). Its first three dwords,
which the shim writes, are the dispatch's group counts; then a slot of K
dwords per thread, (group linear index * threads per group + flattened id),
dword 0 the flag; then G dwords per group for the groupshared arrays.

First version: one query region (every query op after the first allocate,
which dominates them), not inside a loop, single entry and single exit: the
nearest block that post-dominates every query op, P, is where it ends, its
phis Q's and the rest B's. Nothing in the region may use groupshared memory,
a barrier or a wave op. Groupshared arrays saved are one-dimensional arrays
of 32-bit scalars, or such scalars.

Runs on NORMALISED text; returns (a, q, b, info) as text.
"""

import re

from dxil import Module, split_args
from lower import _resolve_attrs, _is_sm66, _operands, _type_names, HANDLE_TYPE
from rayquery import Unsupported

SPACE = 0x7FFF0000
RESRET = '%dx.types.ResRet.i32'
RAB = '%struct.RWByteAddressBuffer'
_QUERY_OPS = set(range(178, 216))
# Rebuilt where needed: handles, system values, constant buffer reads.
_REBUILD_OPS = {57, 59, 160, 216, 217, 218, 93, 94, 95, 96}
_BARRIER = 80
_HDR = 16          # bytes of header: the group counts


def _type_of(instr):
    """The type of the value an instruction defines, as written in the IR."""
    b = instr.body
    cut = b.find(';')
    if cut >= 0:
        b = b[:cut]
    b = b.strip()
    m = re.match(r'^call\s+(?:fast\s+)?(\S+)\s+@', b)
    if m:
        return m.group(1)
    m = re.match(r'^(?:phi|load)\s+(\S+?),?\s', b + ' ')
    if m:
        return m.group(1)
    m = re.match(r'^extractvalue\s+(%[\w.$-]+)\s', b)
    if m:
        suffix = m.group(1).rsplit('.', 1)[-1]
        return {'i32': 'i32', 'f32': 'float', 'f16': 'half', 'i16': 'i16', 'f64': 'double',
                'i64': 'i64', 'i1': 'i1'}.get(suffix)
    if re.match(r'^(?:icmp|fcmp)\s', b):
        return 'i1'
    m = re.match(r'^(?:zext|sext|trunc|uitofp|sitofp|fptoui|fptosi|fpext|fptrunc|bitcast)\s+\S+\s+\S+\s+to\s+(\S+)', b)
    if m:
        return m.group(1)
    m = re.match(r'^select\s+i1\s+[^,]+,\s*(\S+)\s', b)
    if m:
        return m.group(1)
    m = re.match(r'^(?:add|sub|mul|udiv|sdiv|urem|srem|shl|lshr|ashr|and|or|xor|fadd|fsub|fmul|fdiv|frem)'
                 r'(?:\s+(?:nuw|nsw|exact|fast|nnan|ninf|nsz|arcp))*\s+(\S+)\s', b)
    if m:
        return m.group(1)
    return None


_DWORDS = {'i1': 1, 'i16': 1, 'i32': 1, 'half': 1, 'float': 1, 'i64': 2, 'double': 2}


def _postdominators(fn):
    labels = [b.label for b in fn.blocks]
    succ = {b.label: b.successors() for b in fn.blocks}
    exits = [b.label for b in fn.blocks if not succ[b.label]]
    pdom = {l: set(labels) for l in labels}
    for e in exits:
        pdom[e] = {e}
    changed = True
    while changed:
        changed = False
        for l in reversed(labels):
            if l in exits:
                continue
            ss = [pdom[s] for s in succ[l] if s in pdom]
            new = (set.intersection(*ss) if ss else set()) | {l}
            if new != pdom[l]:
                pdom[l] = new
                changed = True
    return pdom


def _reach(fn, start, stop=None):
    """Labels reachable from `start` (included), not passing through `stop`."""
    succ = {b.label: b.successors() for b in fn.blocks}
    seen, stack = {start}, [start]
    while stack:
        cur = stack.pop()
        for s in succ.get(cur, []):
            if s == stop or s in seen:
                continue
            seen.add(s)
            stack.append(s)
    return seen


def _numthreads(text):
    m = re.search(r'^!dx\.entryPoints = !\{!(\d+)\}', text, re.M)
    md = dict((int(a), b) for a, b in re.findall(r'^!(\d+) = (?:distinct )?!\{(.*)\}\s*$', text, re.M))
    entry = split_args(md[int(m.group(1))])
    props = split_args(md.get(int(entry[4][1:]), '')) if entry[4] != 'null' else []
    for k in range(0, len(props) - 1, 2):
        if props[k] == 'i32 4':
            v = split_args(md[int(props[k + 1][1:])])
            return tuple(int(x.split()[1]) for x in v)
    raise Unsupported('the compute shader declares no numthreads')


def split(text):
    m = Module(text)
    if len(m.functions) != 1:
        raise Unsupported('a compute shader with more than one function')
    fn = m.functions[0]
    types = _type_names(text)
    sm66 = _is_sm66(text)
    nt = _numthreads(text)
    NT = nt[0] * nt[1] * nt[2]

    # The region: from the first query op to P, the nearest block that
    # post-dominates every query op.
    qblocks, first = [], None
    for b in fn.blocks:
        for k, i in enumerate(b.instrs):
            if i.dxop in _QUERY_OPS:
                if first is None:
                    first = (b, k)
                if b.label not in qblocks:
                    qblocks.append(b.label)
    if first is None:
        raise Unsupported('no RayQuery in the shader')
    S, cut1 = first
    dom = fn.dominators()
    for l in qblocks:
        if S.label not in dom[l]:
            raise Unsupported('a query op the first query does not dominate')
    pdom = _postdominators(fn)
    cands = set.intersection(*[pdom[l] for l in qblocks]) - set(qblocks)
    P = None
    for c in cands:
        if all(o == c or o in pdom[c] for o in cands):
            P = c
    if P is None:
        raise Unsupported('the query region has no single exit')
    R = _reach(fn, S.label, P)
    if S.label in set(s for l in R for s in fn.block(l).successors()):
        raise Unsupported('a query inside a loop, beside groupshared memory, barriers or wave ops')
    for l in R:
        if S.label not in dom[l]:
            raise Unsupported('a block of the query region the query does not dominate')
    Bpart = _reach(fn, P)
    if Bpart & R or S.label in Bpart:
        raise Unsupported('the query region is reached again after its exit')
    Pb = fn.block(P)

    def part(b, k, i):
        if b.label in R:
            return 'A' if (b is S and k < cut1) else 'Q'
        if b.label == P:
            return 'Q' if i.is_phi else 'B'
        if b.label in Bpart:
            return 'B'
        return 'A'

    defs = {}
    where = {}
    for b in fn.blocks:
        for k, i in enumerate(b.instrs):
            where[id(i)] = part(b, k, i)
            if i.result:
                defs[i.result] = i
    for b in fn.blocks:
        for k, i in enumerate(b.instrs):
            if where[id(i)] != 'Q':
                continue
            if i.dxop == _BARRIER or (i.callee and re.search(r'dx\.op\.(wave|quad)', i.callee)):
                raise Unsupported('a barrier or wave op inside the query region')
            if 'addrspace(3)' in i.body:
                raise Unsupported('groupshared memory inside the query region')

    # What each part needs from before it: rebuilt, or stored and loaded.
    rebuild = {'Q': set(), 'B': set()}
    store = {'A': [], 'Q': []}
    loads = {'Q': [], 'B': []}

    def need(u, Y):
        d = defs.get(u)
        if d is None:
            raise Unsupported('a value with no definition crosses the split: %s' % u)
        X = where[id(d)]
        if X == Y:
            return
        if u in rebuild[Y] or u in loads[Y]:
            return
        # A groupshared address is rebuilt (B holds the same arrays, given
        # back from the group area when B reads one A wrote).
        if d.dxop in _REBUILD_OPS or (d.body.startswith('extractvalue ') and
                                      defs.get(_operands(d, types)[0], d).dxop in _REBUILD_OPS) or \
                (d.body.startswith('getelementptr ') and 'addrspace(3)' in d.body):
            rebuild[Y].add(u)
            for o in _operands(d, types):
                if o in defs:
                    need(o, Y)
            return
        t = _type_of(d)
        if t not in _DWORDS:
            raise Unsupported('a value of type %s crosses the split: %s' % (t, u))
        if u not in store[X]:
            store[X].append(u)
        loads[Y].append(u)

    for b in fn.blocks:
        for k, i in enumerate(b.instrs):
            Y = where[id(i)]
            if Y == 'A':
                continue
            for u in _operands(i, types):
                if u in defs and where[id(defs[u])] != Y:
                    need(u, Y)
    field, K = {}, 1
    for u in store['A'] + store['Q']:
        field[u] = K
        K += _DWORDS[_type_of(defs[u])]

    # Groupshared arrays part B reads that part A wrote: saved per group.
    gs = []
    for line in text.split('\n'):
        gm = re.match(r'^(@"[^"]+"|@[\w.$-]+) = (?:external )?addrspace\(3\) global (\[(\d+) x (i32|float)\]|i32|float)', line)
        if gm:
            gs.append((gm.group(1), gm.group(2), int(gm.group(3)) if gm.group(3) else 0, gm.group(4) or gm.group(2)))
        elif re.match(r'^@\S+ = .*addrspace\(3\) global', line):
            raise Unsupported('a groupshared variable of a shape the split does not save: %s' % line[:80])
    texts = {'A': [], 'B': []}
    for b in fn.blocks:
        for k, i in enumerate(b.instrs):
            if where[id(i)] in texts:
                texts[where[id(i)]].append(i.body)
            if i.result in rebuild['B']:
                texts['B'].append(i.body)
    saved, G = [], 0
    for g, ty, n, el in gs:
        if any(g in l for l in texts['A']) and any(g in l for l in texts['B']):
            saved.append((g, ty, n, el, G))
            G += max(n, 1)

    tmp = [0]

    def fresh(p):
        tmp[0] += 1
        return '%%sp.%s%d' % (p, tmp[0])

    def raw_load(o, off, name):
        r = fresh('r')
        o.append('  %s = call %s @dx.op.rawBufferLoad.i32(i32 139, %s %%sp.h, i32 %s, i32 undef, i8 1, i32 4)'
                 '  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)' % (r, RESRET, HANDLE_TYPE, off))
        o.append('  %s = extractvalue %s %s, 0' % (name, RESRET, r))

    def raw_store(o, off, v):
        o.append('  call void @dx.op.rawBufferStore.i32(i32 140, %s %%sp.h, i32 %s, i32 undef, i32 %s, '
                 'i32 undef, i32 undef, i32 undef, i8 1, i32 4)  ; RawBufferStore(uav,index,elementOffset,'
                 'value0,value1,value2,value3,mask,alignment)' % (HANDLE_TYPE, off, v))

    def addr(o, f):
        a = fresh('a')
        o.append('  %s = add i32 %%sp.tb, %d' % (a, 4 * f))
        return a

    def to_words(o, u, t):
        if t == 'i32':
            return [u]
        if t == 'float':
            w = fresh('w'); o.append('  %s = bitcast float %s to i32' % (w, u)); return [w]
        if t in ('i1', 'i16'):
            w = fresh('w'); o.append('  %s = zext %s %s to i32' % (w, t, u)); return [w]
        if t == 'half':
            f = fresh('w'); o.append('  %s = fpext half %s to float' % (f, u))
            w = fresh('w'); o.append('  %s = bitcast float %s to i32' % (w, f)); return [w]
        v = u
        if t == 'double':
            v = fresh('w'); o.append('  %s = bitcast double %s to i64' % (v, u))
        lo = fresh('w'); o.append('  %s = trunc i64 %s to i32' % (lo, v))
        sh = fresh('w'); o.append('  %s = lshr i64 %s, 32' % (sh, v))
        hi = fresh('w'); o.append('  %s = trunc i64 %s to i32' % (hi, sh))
        return [lo, hi]

    def from_words(o, u, t, ws):
        if t == 'i32':
            o.append('  %s = add i32 %s, 0' % (u, ws[0]))
        elif t == 'float':
            o.append('  %s = bitcast i32 %s to float' % (u, ws[0]))
        elif t == 'i1':
            o.append('  %s = icmp ne i32 %s, 0' % (u, ws[0]))
        elif t == 'i16':
            o.append('  %s = trunc i32 %s to i16' % (u, ws[0]))
        elif t == 'half':
            f = fresh('w'); o.append('  %s = bitcast i32 %s to float' % (f, ws[0]))
            o.append('  %s = fptrunc float %s to half' % (u, f))
        else:
            lo = fresh('w'); o.append('  %s = zext i32 %s to i64' % (lo, ws[0]))
            hi = fresh('w'); o.append('  %s = zext i32 %s to i64' % (hi, ws[1]))
            hs = fresh('w'); o.append('  %s = shl i64 %s, 32' % (hs, hi))
            if t == 'i64':
                o.append('  %s = or i64 %s, %s' % (u, lo, hs))
            else:
                v = fresh('w'); o.append('  %s = or i64 %s, %s' % (v, lo, hs))
                o.append('  %s = bitcast i64 %s to double' % (u, v))

    def prologue(o):
        if sm66:
            o.append('  %%sp.hb = call %s @dx.op.createHandleFromBinding(i32 217, %%dx.types.ResBind '
                     '{ i32 0, i32 0, i32 %d, i8 1 }, i32 0, i1 false)  ; CreateHandleFromBinding(bind,index,'
                     'nonUniformIndex)' % (HANDLE_TYPE, SPACE))
            o.append('  %%sp.h = call %s @dx.op.annotateHandle(i32 216, %s %%sp.hb, %%dx.types.ResourceProperties '
                     '{ i32 4107, i32 0 })  ; AnnotateHandle(res,props)  resource: RWByteAddressBuffer'
                     % (HANDLE_TYPE, HANDLE_TYPE))
        else:
            o.append('  %%sp.h = call %s @dx.op.createHandle(i32 57, i8 1, i32 @UAVID@, i32 0, i1 false)  '
                     '; CreateHandle(resourceClass,rangeId,index,nonUniformIndex)' % HANDLE_TYPE)
        raw_load(o, '0', '%sp.gx')
        raw_load(o, '4', '%sp.gy')
        raw_load(o, '8', '%sp.gz')
        for c in range(3):
            o.append('  %%sp.g%d = call i32 @dx.op.groupId.i32(i32 94, i32 %d)  ; GroupId(component)' % (c, c))
        o.append('  %sp.fi = call i32 @dx.op.flattenedThreadIdInGroup.i32(i32 96)  ; FlattenedThreadIdInGroup()')
        o += ['  %sp.l1 = mul i32 %sp.g2, %sp.gy', '  %sp.l2 = add i32 %sp.l1, %sp.g1',
              '  %sp.l3 = mul i32 %sp.l2, %sp.gx', '  %sp.gl = add i32 %sp.l3, %sp.g0',
              '  %%sp.s1 = mul i32 %%sp.gl, %d' % NT, '  %sp.sl = add i32 %sp.s1, %sp.fi',
              '  %%sp.s2 = mul i32 %%sp.sl, %d' % (4 * K), '  %%sp.tb = add i32 %%sp.s2, %d' % _HDR]
        if G:
            o += ['  %sp.n1 = mul i32 %sp.gx, %sp.gy', '  %sp.n2 = mul i32 %sp.n1, %sp.gz',
                  '  %%sp.n3 = mul i32 %%sp.n2, %d' % (4 * K * NT), '  %%sp.n4 = add i32 %%sp.n3, %d' % _HDR,
                  '  %%sp.n5 = mul i32 %%sp.gl, %d' % (4 * G), '  %sp.gb = add i32 %sp.n4, %sp.n5']

    def body_lines(b, keep):
        out = []
        for k, i in enumerate(b.instrs):
            if keep(k, i):
                out.append(i.line)
        return out

    def header(b, fallback):
        return b.header_line if b.header_line else fallback + ':'

    def rebuilt(o, Y):
        for b in fn.blocks:
            for i in b.instrs:
                if i.result in rebuild[Y]:
                    o.append(i.line)

    def load_all(o, Y):
        for u in loads[Y]:
            t = _type_of(defs[u])
            ws = []
            for d in range(_DWORDS[t]):
                w = fresh('l')
                raw_load(o, addr(o, field[u] + d), w)
                ws.append(w)
            from_words(o, u, t, ws)

    def gs_copy(o, out, pred):
        """Copies each saved array between groupshared memory and the scratch
        buffer: A every element (any thread may be the one left), B strided."""
        for g, ty, n, el, off in saved:
            lab = fresh('c')[1:]
            if not n:
                p = '%s' % g
                if out:
                    v = fresh('x'); o.append('  %s = load %s, %s addrspace(3)* %s, align 4' % (v, el, el, p))
                    w = to_words(o, v, el)[0]
                    a = fresh('a'); o.append('  %s = add i32 %%sp.gb, %d' % (a, 4 * off))
                    raw_store(o, a, w)
                else:
                    a = fresh('a'); o.append('  %s = add i32 %%sp.gb, %d' % (a, 4 * off))
                    w = fresh('l'); raw_load(o, a, w)
                    v = fresh('x'); from_words(o, v, el, [w])
                    o.append('  store %s %s, %s addrspace(3)* %s, align 4' % (el, v, el, p))
                continue
            start = '0' if out else '%sp.fi'
            step = 1 if out else NT
            if not out:
                c0 = fresh('k'); o.append('  %s = icmp ult i32 %%sp.fi, %d' % (c0, n))
                o.append('  br i1 %s, label %%%s, label %%%s.done' % (c0, lab, lab))
            else:
                o.append('  br label %%%s' % lab)
            o += ['', '%s:' % lab]
            i, inx = fresh('i'), fresh('i')
            o.append('  %s = phi i32 [ %s, %%%s ], [ %s, %%%s ]' % (i, start, pred, inx, lab))
            p = fresh('p')
            o.append('  %s = getelementptr %s, %s addrspace(3)* %s, i32 0, i32 %s' % (p, ty, ty, g, i))
            q = fresh('a'); o.append('  %s = add i32 %s, %d' % (q, i, off))
            q2 = fresh('a'); o.append('  %s = shl i32 %s, 2' % (q2, q))
            a = fresh('a'); o.append('  %s = add i32 %%sp.gb, %s' % (a, q2))
            if out:
                v = fresh('x'); o.append('  %s = load %s, %s addrspace(3)* %s, align 4' % (v, el, el, p))
                raw_store(o, a, to_words(o, v, el)[0])
            else:
                w = fresh('l'); raw_load(o, a, w)
                v = fresh('x'); from_words(o, v, el, [w])
                o.append('  store %s %s, %s addrspace(3)* %s, align 4' % (el, v, el, p))
            o.append('  %s = add i32 %s, %d' % (inx, i, step))
            c = fresh('k'); o.append('  %s = icmp ult i32 %s, %d' % (c, inx, n))
            o.append('  br i1 %s, label %%%s, label %%%s.done' % (c, lab, lab))
            o += ['', '%s.done:' % lab]
            pred = lab + '.done'
        return pred

    barrier = '  call void @dx.op.barrier(i32 80, i32 9)  ; Barrier(barrierMode)'

    # A path that leaves part A for a return without the query (an early
    # return) joins the query's path at sp.ajoin instead, so every thread of
    # the group takes A's barrier; it leaves the alive flag 0, so Q and B
    # return for it, as the original did.
    leave = set()
    for b in fn.blocks:
        if b.label in R or b.label == P or b.label in Bpart:
            continue
        for s in b.successors():
            if s == P or s in Bpart:
                t = fn.block(s)
                if len(t.instrs) != 1 or not t.instrs[0].body.startswith('ret void'):
                    raise Unsupported('a path around the query that rejoins after it')
                leave.add(s)

    def retarget(line):
        for s in leave:
            line = re.sub(r'label %s(?![\w.$-])' % re.escape(s), 'label %sp.ajoin', line)
        return line

    # --- A ---
    A = []
    for bi, b in enumerate(fn.blocks):
        if b.label in R and b is not S:
            continue
        if b.label == P or b.label in Bpart:
            continue
        if b.header_line:
            A.append(b.header_line)
        if bi == 0:
            prologue(A)
            raw_store(A, '%sp.tb', '0')
        if b is S:
            A += body_lines(b, lambda k, i: k < cut1)
            A.append('  br label %sp.atail')
            A += ['', 'sp.atail:']
            raw_store(A, '%sp.tb', '1')
            for u in store['A']:
                ws = to_words(A, u, _type_of(defs[u]))
                for d, w in enumerate(ws):
                    raw_store(A, addr(A, field[u] + d), w)
            A += ['  br label %sp.ajoin', '', 'sp.ajoin:']
            if saved:
                A.append(barrier)
                gs_copy(A, True, 'sp.ajoin')
            A.append('  ret void')
        else:
            A += [retarget(l) for l in body_lines(b, lambda k, i: True)]
        A.append('')

    # --- Q ---
    Q = ['sp.qentry:']
    prologue(Q)
    raw_load(Q, '%sp.tb', '%sp.al')
    Q += ['  %sp.alc = icmp ne i32 %sp.al, 0', '  br i1 %sp.alc, label %sp.qgo, label %sp.qret', '',
          'sp.qret:', '  ret void', '', 'sp.qgo:']
    load_all(Q, 'Q')
    rebuilt(Q, 'Q')
    sl = S.label[1:] if S.label else 'sp.s'
    Q += ['  br label %%%s' % sl, '', '%s:' % sl]
    Q += body_lines(S, lambda k, i: k >= cut1)
    Q.append('')
    for b in fn.blocks:
        if b.label in R and b is not S:
            Q.append(b.header_line)
            Q += body_lines(b, lambda k, i: True)
            Q.append('')
    Q.append(Pb.header_line)
    Q += [i.line for i in Pb.instrs if i.is_phi]
    for u in store['Q']:
        ws = to_words(Q, u, _type_of(defs[u]))
        for d, w in enumerate(ws):
            raw_store(Q, addr(Q, field[u] + d), w)
    Q += ['  ret void', '']

    # --- B ---
    Bt = ['sp.bentry:']
    prologue(Bt)
    pred = 'sp.bentry'
    if saved:
        pred = gs_copy(Bt, False, pred)
        Bt.append(barrier)
    raw_load(Bt, '%sp.tb', '%sp.al')
    Bt += ['  %sp.alc = icmp ne i32 %sp.al, 0', '  br i1 %sp.alc, label %sp.bgo, label %sp.bret', '',
           'sp.bret:', '  ret void', '', 'sp.bgo:']
    load_all(Bt, 'B')
    rebuilt(Bt, 'B')
    Bt += ['  br label %%%s' % P[1:], '', Pb.header_line]
    Bt += [i.line for i in Pb.instrs if not i.is_phi]
    Bt.append('')
    for b in fn.blocks:
        if b.label in Bpart and b.label != P:
            Bt.append(b.header_line)
            Bt += body_lines(b, lambda k, i: True)
            Bt.append('')

    info = {'K': K, 'G': G, 'numthreads': nt, 'stored': len(store['A']) + len(store['Q']),
            'saved': [g for g, _t, _n, _e, _o in saved]}
    return (_module(text, fn, A, sm66), _module(text, fn, Q, sm66), _module(text, fn, Bt, sm66), info)


def _module(text, fn, body, sm66):
    """The module with `body` as its function, the scratch buffer declared."""
    lines = text.split('\n')
    head = lines[:fn.index + 1]
    tail = lines[fn.end_index:]
    while body and body[-1] == '':
        body.pop()
    out = head + body + tail
    # Declarations, types and the scratch buffer's record.
    import rghost
    have = '\n'.join(out)
    decls = [
        ('@dx.op.rawBufferLoad.i32(', 'declare %s @dx.op.rawBufferLoad.i32(i32, %s, i32, i32, i8, i32) #RQRO'
         % (RESRET, HANDLE_TYPE)),
        ('@dx.op.rawBufferStore.i32(', 'declare void @dx.op.rawBufferStore.i32(i32, %s, i32, i32, i32, i32, '
         'i32, i32, i8, i32) #RQNW' % HANDLE_TYPE),
        ('@dx.op.groupId.i32(', 'declare i32 @dx.op.groupId.i32(i32, i32) #RQNONE'),
        ('@dx.op.flattenedThreadIdInGroup.i32(', 'declare i32 @dx.op.flattenedThreadIdInGroup.i32(i32) #RQNONE'),
        ('@dx.op.barrier(', 'declare void @dx.op.barrier(i32, i32) #SPBAR'),
    ]
    if sm66:
        decls += [('@dx.op.createHandleFromBinding(', 'declare %s @dx.op.createHandleFromBinding(i32, '
                   '%%dx.types.ResBind, i32, i1) #RQNONE' % HANDLE_TYPE),
                  ('@dx.op.annotateHandle(', 'declare %s @dx.op.annotateHandle(i32, %s, '
                   '%%dx.types.ResourceProperties) #RQNONE' % (HANDLE_TYPE, HANDLE_TYPE))]
    else:
        decls += [('@dx.op.createHandle(', 'declare %s @dx.op.createHandle(i32, i8, i32, i32, i1) #RQRO'
                   % HANDLE_TYPE)]
    adds = []
    for key, d in decls:
        declared = any(l.startswith('declare ') and key in l for l in out)
        if not declared and ('call ' in have and key in have):
            adds += ['', '; Function Attrs: ' + _ATTR_TEXT[d.rsplit(' ', 1)[1]], d]
    out = rghost._drop_unused_declarations(out)
    # A groupshared variable this part no longer touches is an unused
    # internal declaration.
    fbody = '\n'.join(body)
    out = [l for l in out
           if not (re.match(r'^(@"[^"]+"|@[\w.$-]+) = .*addrspace\(3\) global', l) and
                   re.match(r'^(@"[^"]+"|@[\w.$-]+)', l).group(1) not in fbody)]
    at = max(k for k, l in enumerate(out) if l.startswith('declare ') or l == '}')
    out[at + 1:at + 1] = adds
    types = []
    for t, d in [(RESRET, '%s = type { i32, i32, i32, i32, i32 }' % RESRET)] + (
            [('%dx.types.ResBind', '%dx.types.ResBind = type { i32, i32, i32, i8 }'),
             ('%dx.types.ResourceProperties', '%dx.types.ResourceProperties = type { i32, i32 }')]
            if sm66 else []) + [(RAB, '%s = type { i32 }' % RAB)]:
        if not any(l.startswith(t + ' = type') for l in out):
            types.append(d)
    last_type = max(k for k, l in enumerate(out) if re.match(r'^%[\w."<>, -]+ = type ', l))
    out[last_type + 1:last_type + 1] = types
    text2 = '\n'.join(out)
    text2 = _drop_unused_resources(text2, fbody, sm66)
    text2 = _scratch_record(text2)
    if '#SPBAR' in text2:
        bm = re.search(r'^declare void @dx\.op\.barrier\(i32, i32\) (#\d+)', text2, re.M)
        body = 'noduplicate nounwind'
        am = re.search(r'^attributes (#\d+) = \{ %s \}' % body, text2, re.M)
        if am:
            text2 = text2.replace('#SPBAR', am.group(1))
        else:
            ids = [int(x) for x in re.findall(r'^attributes #(\d+) = ', text2, re.M)]
            nid = max(ids) + 1 if ids else 0
            ls = text2.split('\n')
            last = max(k for k, l in enumerate(ls) if l.startswith('attributes #'))
            ls.insert(last + 1, 'attributes #%d = { %s }' % (nid, body))
            text2 = '\n'.join(ls).replace('#SPBAR', '#%d' % nid)
    return _resolve_attrs(text2)


# What the "; Function Attrs:" line before an added declaration says, which
# the lowering reads when it drops a declaration it no longer needs.
_ATTR_TEXT = {'#RQRO': 'nounwind readonly', '#RQNW': 'nounwind', '#RQNONE': 'nounwind readnone',
              '#SPBAR': 'noduplicate nounwind'}


def _drop_unused_resources(text, fbody, sm66):
    """A resource the part makes no handle for leaves its records, the rest
    renumbered (at 6.5 their handles' range ids with them): lowered into a
    library an unused one is an unused global, which the validator refuses."""
    md = dict((int(a), b) for a, b in re.findall(r'^!(\d+) = (?:distinct )?!\{(.*)\}\s*$', text, re.M))
    res = re.search(r'^!dx\.resources = !\{!(\d+)\}', text, re.M)
    if not res:
        return text
    rid = int(res.group(1))
    groups = split_args(md[rid])
    nodes = {}
    remap = {}
    for c, g in enumerate(groups):
        if g == 'null':
            continue
        recs = split_args(md[int(g[1:])])
        kept = []
        for r in recs:
            f = split_args(md[int(r[1:])])
            old = int(f[0].split()[1])
            if sm66:
                lo, space = f[4].split()[1], f[3].split()[1]
                used = re.search(r'%%dx\.types\.ResBind \{ i32 %s, i32 [^,]+, i32 %s, i8 %d \}' % (lo, space, c),
                                 fbody) or (lo == '0' and space == '0' and c == 0 and
                                            '%dx.types.ResBind zeroinitializer' in fbody)
            else:
                used = re.search(r'@dx\.op\.createHandle\(i32 57, i8 %d, i32 %d,' % (c, old), fbody)
            if not used:
                continue
            new = len(kept)
            kept.append(r)
            remap[(c, old)] = new
            if new != old:
                nodes[int(r[1:])] = ', '.join(['i32 %d' % new] + f[1:])
        if len(kept) != len(recs):
            if kept:
                nodes[int(g[1:])] = ', '.join(kept)
            else:
                groups[c] = 'null'
    nodes[rid] = ', '.join(groups)
    out = []
    for l in text.split('\n'):
        m = re.match(r'^!(\d+) = !\{', l)
        if m and int(m.group(1)) in nodes:
            l = '!%s = !{%s}' % (m.group(1), nodes[int(m.group(1))])
        out.append(l)
    text = '\n'.join(out)
    if not sm66:
        text = re.sub(r'(@dx\.op\.createHandle\(i32 57, i8 (\d), i32 )(\d+),',
                      lambda m: '%s%d,' % (m.group(1), remap.get((int(m.group(2)), int(m.group(3))),
                                                                 int(m.group(3)))),
                      text)
    return text


def _scratch_record(text):
    """The scratch buffer's UAV record, after the module's own UAVs; the
    handle at 6.5 names its range id."""
    md = dict((int(a), b) for a, b in re.findall(r'^!(\d+) = (?:distinct )?!\{(.*)\}\s*$', text, re.M))
    nxt = [max(md) + 1]
    added = []

    def node(body):
        nid = nxt[0]
        nxt[0] += 1
        added.append('!%d = !{%s}' % (nid, body))
        return nid

    def rewrite(t, nid, body):
        return '\n'.join('!%d = !{%s}' % (nid, body) if re.match(r'^!%d = !\{' % nid, l) else l
                         for l in t.split('\n'))

    res = re.search(r'^!dx\.resources = !\{!(\d+)\}', text, re.M)
    if not res:
        raise Unsupported('a compute shader with no resources')
    rid = int(res.group(1))
    groups = split_args(md[rid])
    uavs = [] if groups[1] == 'null' else split_args(md[int(groups[1][1:])])
    rec = node('i32 %d, %s* undef, !"", i32 %d, i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null'
               % (len(uavs), RAB, SPACE))
    text = text.replace('@UAVID@', str(len(uavs)))
    lst = node(', '.join(uavs + ['!%d' % rec]))
    groups[1] = '!%d' % lst
    text = rewrite(text, rid, ', '.join(groups))
    return text.rstrip('\n') + '\n' + '\n'.join(added) + '\n'
