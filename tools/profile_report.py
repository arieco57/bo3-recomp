#!/usr/bin/env python3
"""Resolve bo3_profile.txt (runtime/src/profiler.cpp) into a flat profile.
usage: profile_report.py bo3_profile.txt build/bo3_zm [top]"""
import bisect, collections, subprocess, sys
prof, exe = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
lines = open(prof).read().split('\n')
n = int(lines[0].split()[1])
ips = [int(x, 16) for x in lines[1:1 + n]]
stacks = []
pos = n + 1
if lines[pos].startswith('stacks '):   # BO3_PROFILE_STACKS
    ns = int(lines[pos].split()[1])
    stacks = [[int(x, 16) for x in l.split()] for l in lines[pos + 1:pos + 1 + ns]]
    pos += 1 + ns
maps = []
for l in lines[pos + 1:]:
    p = l.split(None, 5)
    if len(p) >= 6 and 'x' in p[1]:
        a, b = (int(x, 16) for x in p[0].split('-'))
        maps.append((a, b, int(p[2], 16), p[5].strip()))
symcache = {}
def syms(path):
    if path in symcache: return symcache[path]
    out = subprocess.run(['nm', '-C', '--defined-only', '-n', path], capture_output=True, text=True).stdout
    if not out.strip():
        out = subprocess.run(['nm', '-C', '-D', '--defined-only', '-n', path], capture_output=True, text=True).stdout
    addrs, names = [], []
    for l in out.split('\n'):
        p = l.split(' ', 2)
        if len(p) == 3 and p[1].lower() in 'tw':
            addrs.append(int(p[0], 16)); names.append(p[2])
    symcache[path] = (addrs, names)
    return symcache[path]
count = collections.Counter()
for ip in ips:
    m = next((m for m in maps if m[0] <= ip < m[1]), None)
    if not m: count['?'] += 1; continue
    path = m[3]
    off = ip if path.endswith(exe.split('/')[-1]) else ip - m[0] + m[2]
    addrs, names = syms(path)
    i = bisect.bisect_right(addrs, off) - 1
    lib = path.split('/')[-1]
    count[(names[i][:110] if i >= 0 else hex(off)) + '  [' + lib + ']'] += 1
for k, v in count.most_common(top):
    print('%6.2f%%  %s' % (100.0 * v / len(ips), k))

def name_of(ip):
    m = next((m for m in maps if m[0] <= ip < m[1]), None)
    if not m: return None, '?'
    path = m[3]
    off = ip if path.endswith(exe.split('/')[-1]) else ip - m[0] + m[2]
    addrs, names = syms(path)
    i = bisect.bisect_right(addrs, off) - 1
    return path, (names[i] if i >= 0 else hex(off))

# BO3_CALLERS=<function substring> (needs BO3_PROFILE_STACKS): samples whose leaf is that
# function, grouped by the innermost caller in the executable (two levels).
import os
callee = os.environ.get('BO3_CALLERS')
if callee and stacks:
    c3 = collections.Counter()
    hits = 0
    for leaf, st in zip(ips, stacks):
        if callee not in name_of(leaf)[1]: continue
        hits += 1
        chain = []
        for a in st:
            path, nm = name_of(a - 1)
            if path and path.endswith(exe.split('/')[-1]) and 'OnProf' not in nm:
                chain.append(nm.replace('(anonymous namespace)::', '').split('(')[0][:60])
                if len(chain) == int(os.environ.get('BO3_DEPTH', '2')): break
        c3[' <- '.join(chain) or '?'] += 1
    print('--- callers of', callee, hits, 'samples (of', len(stacks), 'with stacks)')
    for k, v in c3.most_common(25): print('%6.2f%%  %s' % (100.0 * v / len(stacks), k))

# BO3_LINES=<function substring>: per-line breakdown of that function (needs -g)
import os
fn = os.environ.get('BO3_LINES')
if fn:
    sel = []
    for ip in ips:
        m = next((m for m in maps if m[0] <= ip < m[1]), None)
        if not m or not m[3].endswith(exe.split('/')[-1]): continue
        addrs, names = syms(m[3])
        i = bisect.bisect_right(addrs, ip) - 1
        if i >= 0 and fn in names[i]: sel.append(ip)
    out = subprocess.run(['addr2line', '-e', exe, '-i'] + ['%x' % a for a in sel[:20000]], capture_output=True, text=True).stdout.split('\n')
    # with -i several lines per address: take the first (innermost)
    res = subprocess.run(['addr2line', '-e', exe] + ['%x' % a for a in sel[:20000]], capture_output=True, text=True).stdout.split('\n')
    c2 = collections.Counter(l.split('/')[-1] for l in res if l)
    print('--- lines in', fn, len(sel))
    for k, v in c2.most_common(40): print('%6.2f%%  %s' % (100.0 * v / len(ips), k))

# BO3_CALLER_LINES=<leaf substring> (needs BO3_PROFILE_STACKS): source line of the innermost
# executable frame for samples whose leaf matches (which call site leads into a library).
cl = os.environ.get('BO3_CALLER_LINES')
if cl and stacks:
    addrs = []
    for leaf, st in zip(ips, stacks):
        if cl not in name_of(leaf)[1] and cl not in hex(leaf): 
            p0, nm = name_of(leaf)
            if not (p0 and cl in p0.split('/')[-1]): continue
        for a in st:
            path, nm = name_of(a - 1)
            if path and path.endswith(exe.split('/')[-1]) and 'OnProf' not in nm:
                addrs.append(a - 1); break
    res = subprocess.run(['addr2line', '-e', exe] + ['%x' % a for a in addrs[:30000]], capture_output=True, text=True).stdout.split('\n')
    c4 = collections.Counter(l.split('/')[-1] for l in res if l)
    print('--- call sites into', cl, len(addrs))
    for k, v in c4.most_common(25): print('%6.2f%%  %s' % (100.0 * v / len(stacks), k))
