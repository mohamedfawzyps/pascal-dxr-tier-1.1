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

from dxil import Module
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
    # The function's pair in the annotation node: `void ()* @f, !n`.
    for n, l in enumerate(lines):
        if l is None or not re.match(r'^!\d+ = !\{i32 1, ', l):
            continue
        lines[n] = re.sub(r', void \(\)\* ' + re.escape(ref) + r', !\d+', '', l)
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
