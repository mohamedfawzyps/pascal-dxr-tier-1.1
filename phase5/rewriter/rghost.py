"""RayQuery in an APPLICATION's raygen (0.80.0, plan step 4).

A raygen can call TraceRay, so its queries are lowered exactly as a compute
shader's are. Rather than teach the lowering a second kind of input, the
raygen is presented to it AS a compute shader:

  raygen_as_cs(text, export)   the library with every other function, its
                               entry points and type annotations removed, the
                               raygen renamed @main with a compute entry of
                               numthreads(1, 1, 1), the shader model cs. The
                               compute lowering then turns it back into a
                               library: the raygen, lowered, under its own
                               export name, and the generated hit shaders.
  strip_function(text, export) the application's library without that raygen,
                               for the rest of its shaders.

The resources stay as the library had them (globals loaded and given to
createHandleForLib); the lowering leaves those alone and rebuilds them in the
hit shaders like any other value it can recompute.

Runs on NORMALISED text.
"""

import re

from dxil import Module, split_args
from rayquery import Unsupported

RT11_SHADER_FLAG = 0x2000000
_RQ_DECL = re.compile(r'^declare .*@dx\.op\.rayQuery_')


def exports(raygen):
    """The lowering's export names for a raygen's queries: the raygen keeps
    its own, the generated shaders are the shim's, named after it so two
    raygens of one pipeline do not clash."""
    pre = 'dxr11_rq_%s_' % raygen
    names = {'anyhit': 'AnyHit', 'closesthit': 'ClosestHit', 'miss': 'Miss',
             'intersection': 'Isect', 'closesthitproc': 'ClosestHitProc',
             'anyhitnull': 'AnyHitNull', 'isectnull': 'IsectNull',
             'anyhitproc': 'AnyHitProc'}
    ex = dict((k, pre + v) for k, v in names.items())
    ex['raygen'] = raygen
    return ex


def export_name(fn):
    """The name a state object uses for a library function: DXC mangles
    `\\01?RayGen@@YAXXZ` and exports it as RayGen."""
    m = re.match(r'^\\01\?([^@]+)@@', fn)
    return m.group(1) if m else fn


def _function(module, export):
    for f in module.functions:
        if export_name(f.name) == export:
            return f
    raise Unsupported('the library exports no function %s' % export)


def _md(lines):
    md = {}
    for n, l in enumerate(lines):
        m = re.match(r'^!(\d+) = (?:distinct )?!\{(.*)\}\s*$', l)
        if m:
            md[int(m.group(1))] = (n, m.group(2))
    return md


def _drop_unused_declarations(lines):
    """A declare nothing calls is itself a validation error."""
    body = '\n'.join(l for l in lines if not l.startswith('declare '))
    out = []
    for l in lines:
        m = re.match(r'^declare [^@]*@("[^"]+"|[\w.$-]+)\(', l)
        if m and ('@' + m.group(1) + '(') not in body:
            if out and out[-1].startswith('; Function Attrs:'):
                out.pop()
            continue
        out.append(l)
    return out


def _drop_unused_resources(lines):
    """A resource global no function loads any more is an unused internal
    declaration: the global goes, and its record, the others in its class
    renumbered from 0."""
    body = '\n'.join(l for l in lines if l.startswith('  '))
    used = lambda g: (g + ',') in body or (g + ' ') in body
    md = _md(lines)
    gone = set()
    for n, l in enumerate(lines):
        m = re.match(r'^(@"[^"]+"|@[\w.$-]+) = external constant ', l)
        if m and not used(m.group(1)):
            gone.add(m.group(1))
            lines[n] = None
    if not gone:
        return lines
    res = [l for l in lines if l and l.startswith('!dx.resources = ')]
    if res:
        top = int(re.search(r'!(\d+)', res[0]).group(1))
        groups = [g.strip() for g in md[top][1].split(',')]
        for g in groups:
            if g == 'null':
                continue
            n, body_ = md[int(g[1:])]
            ids = [x.strip() for x in body_.split(',')]
            keep = [x for x in ids
                    if not any(r + ',' in md[int(x[1:])][1] or r + ' to ' in md[int(x[1:])][1]
                               for r in gone)]
            for k, x in enumerate(keep):
                rn, rb = md[int(x[1:])]
                lines[rn] = '!%s = !{%s}' % (x[1:], re.sub(r'^i32 \d+', 'i32 %d' % k, rb))
            for x in ids:
                if x not in keep:
                    lines[md[int(x[1:])][0]] = None
            if keep:
                lines[n] = '!%s = !{%s}' % (g[1:], ', '.join(keep))
            else:
                lines[n] = None
                groups[groups.index(g)] = 'null'
        lines[md[top][0]] = '!%d = !{%s}' % (top, ', '.join(groups))
    return [l for l in lines if l is not None]


def _fn_ref(name):
    """How metadata and calls name a function."""
    return '@"%s"' % name if not re.match(r'^[\w.$-]+$', name) else '@' + name


# What the lowering names its own payload: an application struct of that name
# is renamed, so the two do not clash (0.80.0).
_PAYLOAD_NAME = re.compile(r'struct\.Payload(?![\w.])')


def raygen_as_cs(text, export):
    if re.search(r'^%struct\.Payload = type ', text, re.M):
        text = _PAYLOAD_NAME.sub('struct.Payload.app', text)
    m = Module(text)
    fn = _function(m, export)
    lines = text.split('\n')
    keep = [True] * len(lines)
    for f in m.functions:
        if f is fn:
            continue
        for n in range(f.index, f.end_index + 1):
            keep[n] = False
        if f.index > 0 and lines[f.index - 1].startswith('; Function Attrs:'):
            keep[f.index - 1] = False
    lines = [l for n, l in enumerate(lines) if keep[n]]
    old = _fn_ref(fn.name)
    lines = [l.replace(old + '(', '@main(') if l.startswith('define ') else l for l in lines]
    md = _md(lines)
    ep = [l for l in lines if l.startswith('!dx.entryPoints = ')]
    if not ep:
        raise Unsupported('the library has no entry points')
    ids = [int(x.strip()[1:]) for x in re.search(r'\{(.*)\}', ep[0]).group(1).split(',')]
    null = [i for i in ids if md[i][1].startswith('null')]
    if not null:
        raise Unsupported('the library declares no module entry')
    fields = [x.strip() for x in md[null[0]][1].split(',')]
    res, props = fields[3], fields[4]
    flags = 0
    if props != 'null':
        pm = re.search(r'i32 0, i64 (-?\d+)', md[int(props[1:])][1])
        if pm:
            flags = int(pm.group(1))
    top = max(md) + 1
    lines = [l for l in lines if not l.startswith('!dx.typeAnnotations = ')]
    # Every node naming a function is gone: the entry points and annotations
    # of the shaders this module no longer has.
    lines = [l for l in lines if not (re.match(r'^!\d+ = ', l) and re.search(r'\)\* @', l))]
    out = []
    for l in lines:
        if l.startswith('!dx.entryPoints = '):
            l = '!dx.entryPoints = !{!%d}' % (top + 2)
        sm = re.match(r'^(!\d+) = !\{!"lib", (i32 \d+, i32 \d+)\}\s*$', l)
        if sm:
            l = '%s = !{!"cs", %s}' % (sm.group(1), sm.group(2))
        out.append(l)
    out = _drop_unused_declarations(out)
    out = _drop_unused_resources(out)
    out = '\n'.join(out).rstrip('\n')
    out += ('\n!%d = !{i32 1, i32 1, i32 1}\n'
            '!%d = !{i32 0, i64 %d, i32 4, !%d}\n'
            '!%d = !{void ()* @main, !"main", null, %s, !%d}\n'
            % (top, top + 1, flags, top, top + 2, res, top + 1))
    return out


def strip_function(text, export):
    """The library without `export`: its function, entry and annotation, the
    declarations only it used, and the RayQuery flag when nothing left uses
    RayQuery. None when it was the library's only shader."""
    m = Module(text)
    fn = _function(m, export)
    lines = text.split('\n')
    keep = [True] * len(lines)
    for n in range(fn.index, fn.end_index + 1):
        keep[n] = False
    if fn.index > 0 and lines[fn.index - 1].startswith('; Function Attrs:'):
        keep[fn.index - 1] = False
    lines = [l for n, l in enumerate(lines) if keep[n]]
    ref = _fn_ref(fn.name)
    md = _md(lines)
    ep = [n for n, l in enumerate(lines) if l.startswith('!dx.entryPoints = ')][0]
    ids = [x.strip() for x in re.search(r'\{(.*)\}', lines[ep]).group(1).split(',')]
    mine = [x for x in ids if ref in md[int(x[1:])][1]]
    rest = [x for x in ids if x not in mine]
    if not any(not md[int(x[1:])][1].startswith('null') for x in rest):
        return None
    lines[ep] = '!dx.entryPoints = !{%s}' % ', '.join(rest)
    for x in mine:
        lines[md[int(x[1:])][0]] = None
    # The function's pair in the annotation node: `void (...)* @f, !n`, its
    # parameters' types inside the brackets for a closest-hit or miss (0.82.0).
    for n, l in enumerate(lines):
        if l is None or not re.match(r'^!\d+ = !\{i32 1, ', l):
            continue
        lines[n] = re.sub(r', void \([^)]*\)\* ' + re.escape(ref) + r', !\d+', '', l)
    lines = [l for l in lines if l is not None]
    lines = _drop_unused_declarations(lines)
    lines = _drop_unused_resources(lines)
    if not any(_RQ_DECL.match(l) for l in lines):
        for n, l in enumerate(lines):
            fm = re.match(r'^(!\d+ = !\{i32 0, i64 )(-?\d+)(.*)$', l)
            if fm:
                lines[n] = fm.group(1) + str(int(fm.group(2)) & ~RT11_SHADER_FLAG) + fm.group(3)
    return '\n'.join(lines)


def fix_flags(text, said):
    """The module flags the validator computed, when it said the declared ones
    do not match usage ("Flags declared=D, actual=A"): what the shaders left
    use is the validator's to say. None when it said nothing of it."""
    m = re.search(r'Flags declared=(\d+), actual=(\d+)', said)
    if not m:
        return None
    lines = text.split('\n')
    for n, l in enumerate(lines):
        fm = re.match(r'^(!\d+ = !\{i32 0, i64 )%s(\}.*)$' % m.group(1), l)
        if fm:
            lines[n] = fm.group(1) + m.group(2) + fm.group(2)
            return '\n'.join(lines)
    return None


# The lowering's own TraceRay, by its payload overload (0.81.0).
SHIM_PAYLOAD = 'struct.Payload'
_SHIM_TRACE = re.compile(r'^(\s*)call void @dx\.op\.traceRay\.struct\.Payload\(i32 157, '
                         r'(%dx\.types\.Handle [^,]+, i32 [^,]+, i32 [^,]+, i32 [^,]+, '
                         r'i32 [^,]+), i32 ([^,]+), (.*)$')


def shim_miss(text):
    """The lowered query's TraceRay takes its miss index at run time (0.81.0):
    its miss is the shim's, after the application's misses, whose count only
    a dispatch knows (probed in 0.80.0, gitest --dynmiss). The index becomes
    a GeometryIndex() read, which geomidx.lower then turns into the constant
    at b0, space GI_SPACE, that the shim appends to the raygen's local root
    signature: the mechanism the GTX 1070 has run since 0.44.0."""
    out, n = [], 0
    for l in text.split('\n'):
        m = _SHIM_TRACE.match(l)
        if m:
            v = '%%rgq.ms%d' % n
            out.append('%s%s = call i32 @dx.op.geometryIndex.i32(i32 213)' % (m.group(1), v))
            l = ('%scall void @dx.op.traceRay.struct.Payload(i32 157, %s, i32 %s, %s'
                 % (m.group(1), m.group(2), v, m.group(4)))
            n += 1
        out.append(l)
    return '\n'.join(out)


_GI_RECORD = '!"dxr11.gi", i32 2147418112, i32 0, i32 1, i32 4, null'
_MS_READ = re.compile(r'^(\s*)%rgq\.ms(\d+) = extractvalue %dx\.types\.CBufRet\.i32 (%gi\.\d+\.r), 0$')
_SHIM_ARGS = re.compile(r', i32 (\d+), i32 (\d+), i32 %rgq\.ms(\d+), ')


def point_queries(text, queries, mult, slot_of=None, slots=1, caps=None):
    """The lowered library made ready for the application's pipeline (0.81.0):
    the lowered TraceRay calls, and only they, pointed at the shim's copy of
    the scene, as the compute path's own layout does (0.61.0).

    What they take from the shim is read at run time from the constants it
    appends to the shader's local root signature, b0 in GI_SPACE: x the miss
    index (the shim's miss, after the application's), y the shader's first
    query slot in the shim's block, z the pipeline's query slots (0.83.0;
    until then y and z were literals, so a library could not be lowered before
    the pipeline linking it existed, as a collection is). Call k traces with
    ray contribution y + k and multiplier z."""
    import geomidx
    import shimtrace
    if re.search(r'@dx\.op\.geometryIndex\.', text):
        raise Unsupported('GeometryIndex() in a DXR shader that uses RayQuery: the shim reads its '
                          'own constants where GeometryIndex() would be read')
    try:
        text, _ = geomidx.lower(shim_miss(text))
        pairs = [(k, mult) for k in range(queries)]
        text, n = shimtrace.retrace(text, pairs, 0, slot_of, slots, caps, only=SHIM_PAYLOAD)
    except geomidx.Unsupported as e:
        raise Unsupported(str(e))
    if not n:
        raise Unsupported("the lowered raygen has no TraceRay of its own to point at the "
                          "shim's scene")
    # The constants are three dwords now; then each call's ray contribution
    # and multiplier from them.
    if text.count(_GI_RECORD) != 1:
        raise Unsupported("the shim's constant record is not where it was looked for")
    text = text.replace(_GI_RECORD, _GI_RECORD.replace('i32 4, null', 'i32 12, null'))
    lines = text.split('\n')
    lit = {}
    for n2, l in enumerate(lines):
        if 'call void @dx.op.traceRay.struct.Payload(' not in l:
            continue
        m = _SHIM_ARGS.search(l)
        if not m:
            continue
        k = m.group(3)
        if lit.setdefault(k, m.group(1)) != m.group(1):
            raise Unsupported('a lowered TraceRay traced with two query slots')
        lines[n2] = l[:m.start()] + ', i32 %%rgq.r%s, i32 %%rgq.tot%s, i32 %%rgq.ms%s, ' % (k, k, k) + l[m.end():]
    out = []
    for l in lines:
        out.append(l)
        m = _MS_READ.match(l)
        if m and m.group(2) in lit:
            ind, k, r = m.group(1), m.group(2), m.group(3)
            out += ['%s%%rgq.base%s = extractvalue %%dx.types.CBufRet.i32 %s, 1' % (ind, k, r),
                    '%s%%rgq.tot%s = extractvalue %%dx.types.CBufRet.i32 %s, 2' % (ind, k, r),
                    '%s%%rgq.r%s = add i32 %%rgq.base%s, %s' % (ind, k, k, lit[k])]
    return '\n'.join(out)



def restore_function(lowered, cs, orig, export):
    """A closest-hit or miss hosted as a compute shader comes back from the
    lowering as a raygen would: no parameters, a raygen's entry properties and
    annotation (0.82.0). Its own are put back: the parameters as `cs` (what
    raygen_as_cs made) declares them, the function type they make, and the
    entry properties (shader kind, payload and attribute sizes) and parameter
    annotations `orig`, the library, gives it. Those nodes name no function,
    so they are still in the lowered module under the same ids. A raygen comes
    back unchanged."""
    dm = re.search(r'^define void @main\((.*)\) #\d+ \{$', cs, re.M)
    if not dm or not dm.group(1).strip():
        return lowered
    params = dm.group(1)
    ftype = 'void (%s)*' % ', '.join(p.split()[0] for p in split_args(params))
    fn = _function(Module(orig), export)
    ref = _fn_ref(fn.name)
    props = ann = None
    for _n, body in _md(orig.split('\n')).values():
        f = split_args(body)
        if len(f) >= 5 and f[0].endswith(' ' + ref) and f[1].startswith('!"'):
            props = f[4]
        if f and f[0] == 'i32 1':
            for k in range(1, len(f) - 1, 2):
                if f[k].endswith(' ' + ref):
                    ann = f[k + 1]
    if props is None or ann is None:
        raise Unsupported('the entry or annotation of %s is not where it was looked for' % export)
    me = _fn_ref(export)
    out = []
    for l in lowered.split('\n'):
        if l.startswith('define void %s() ' % me):
            l = 'define void %s(%s) ' % (me, params) + l[len('define void %s() ' % me):]
        elif l.startswith('!'):
            l = l.replace('void ()* %s, !"' % me, '%s %s, !"' % (ftype, me))
            l = re.sub(r'(%s %s, !"[^"]*", null, null, )!\d+\}' % (re.escape(ftype), re.escape(me)),
                       lambda m: m.group(1) + props + '}', l)
            l = re.sub(r'void \(\)\* %s, !\d+' % re.escape(me),
                       lambda m: '%s %s, %s' % (ftype, me, ann), l)
        out.append(l)
    return '\n'.join(out)
