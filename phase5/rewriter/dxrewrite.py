#!/usr/bin/env python3
"""Automated RayQuery to TraceRay lowering. Analysis front end.

    python phase5/rewriter/dxrewrite.py analyze <in.ll>
    python phase5/rewriter/dxrewrite.py lower   <in.ll> <out.ll>

Prints what the analysis found, so it can be checked against the hand
lowerings in phase5/hand before any transform is built on top of it. Nothing
here matches on instruction text; every site is found structurally.
"""

import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'hand'))

from dxil import Module
from llnorm import normalize
import rayquery
import lower
import nvapi


def cmd_analyze(path):
    text = normalize(io.open(path, encoding='utf-8').read())
    try:
        text = nvapi.fold(text)
    except rayquery.Unsupported as e:
        print('\n-- %s --\n   UNSUPPORTED            : %s' % (path, e))
        return 2
    mod = Module(text)
    print('\n-- %s --' % path)
    print('   functions              : %s'
          % ', '.join(f.name for f in mod.functions))

    try:
        q = rayquery.analyze(mod)
    except rayquery.Unsupported as e:
        print('   UNSUPPORTED            : %s' % e)
        return 2

    num, desc = q.pattern()
    print('   entry function         : %s' % q.fn.name)
    print('   query handle           : %s' % q.handle)
    print('   template flags         : %d (%s)'
          % (q.const_flags, rayquery.flag_names(q.const_flags)))
    print('   TraceRayInline flags   : %d (%s)'
          % (q.dyn_flags, rayquery.flag_names(q.dyn_flags)))
    print('   combined ray flags     : %d (%s)'
          % (q.ray_flags, rayquery.flag_names(q.ray_flags)))
    print('   acceleration structure : %s' % q.as_handle)
    ra = q.ray_args
    print('   ray                    : mask %s, tmin %s, tmax %s'
          % (ra['mask'], ra['tmin'], ra['tmax']))
    print('   Proceed calls          : %d in %s'
          % (len(q.proceeds), ', '.join(b.label or '<entry>' for b, _ in q.proceeds)))
    if q.loop:
        header, latch, body = q.loop
        print('   rotated Proceed loop   : header %s, latch %s' % (header, latch))
        print('   any-hit body blocks    : %s'
              % ', '.join(sorted(body - {latch})))
    else:
        print('   rotated Proceed loop   : none')
    print('   commits                : %d' % len(q.commits))
    print('   candidate accessors    : %s'
          % (', '.join(rayquery.KNOWN[i.dxop] for _, i in q.candidate_ops) or 'none'))
    print('   committed accessors    : %s'
          % (', '.join(rayquery.KNOWN[i.dxop] for _, i in q.committed_ops) or 'none'))
    print('   PATTERN                : %d, %s' % (num, desc))
    print('   any-hit shader needed  : %s' % ('yes' if q.needs_anyhit else 'no'))
    return 0


def cmd_lowerraygen(src, raygen, dst, stripped):
    """RayQuery in an application's raygen (0.80.0): see rghost.py."""
    import rghost
    text = normalize(io.open(src, encoding='utf-8').read())
    try:
        cs = rghost.raygen_as_cs(text, raygen)
        st = rghost.strip_function(text, raygen)
        mod = Module(nvapi.fold(cs))
        q = rayquery.analyze(mod)
        out = lower.lower(mod, q, rghost.exports(raygen))
    except (rayquery.Unsupported, lower.LowerError) as e:
        sys.stderr.write('REFUSED: %s\n' % e)
        return 2
    with io.open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(out)
    if st is not None:
        with io.open(stripped, 'w', encoding='utf-8', newline='\n') as f:
            f.write(st)
    print('lowered %s -> %s (%s)%s' % (raygen, dst, q.pattern_desc,
                                       '' if st is not None else ", the library's only shader"))
    return 0


def cmd_pointraygen(src, dst, queries, mult):
    """A lowered raygen library made ready for the application's pipeline
    (0.81.0): see point_queries in rghost.py. One scene slot."""
    import rghost
    text = normalize(io.open(src, encoding='utf-8').read())
    try:
        out = rghost.point_queries(text, queries, mult)
    except rayquery.Unsupported as e:
        sys.stderr.write('REFUSED: %s\n' % e)
        return 2
    with io.open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write(out)
    print('pointed %s -> %s' % (src, dst))
    return 0


def cmd_lower(src, dst):
    text = normalize(io.open(src, encoding='utf-8').read())
    try:
        mod = Module(nvapi.fold(text))
        q = rayquery.analyze(mod)
    except rayquery.Unsupported as e:
        sys.stderr.write('REFUSED: %s\n' % e)
        return 2
    try:
        out = lower.lower(mod, q)
    except (rayquery.Unsupported, lower.LowerError) as e:
        # A traceback is not "failing loudly" in any useful sense. Anything the
        # lowering cannot do has to come out as a refusal the caller can read.
        sys.stderr.write('REFUSED: %s\n' % e)
        return 2
    io.open(dst, 'w', encoding='utf-8', newline='\n').write(out)
    print('lowered %s -> %s (pattern %d, %s)'
          % (src, dst, q.pattern_num, q.pattern_desc))
    return 0


def main(argv):
    if len(argv) >= 3 and argv[1] == 'analyze':
        return cmd_analyze(argv[2])
    if len(argv) >= 4 and argv[1] == 'lower':
        return cmd_lower(argv[2], argv[3])
    if len(argv) >= 6 and argv[1] == 'lowerraygen':
        return cmd_lowerraygen(argv[2], argv[3], argv[4], argv[5])
    if len(argv) >= 6 and argv[1] == 'pointraygen':
        return cmd_pointraygen(argv[2], argv[3], int(argv[4]), int(argv[5]))
    print(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
