#!/bin/sh
# Recompile one binary with two revisions of recomp and say what changed.
#
#     tools/recomp-compare.sh <binary.rpx> [old-rev] [new-rev]
#
# old-rev defaults to a7b0bb3 (2026-09-20, the last revision before the
# recompiler changes of 2026-09-24..26); new-rev to HEAD. Both are built in
# git worktrees under $TMPDIR, run on the same binary, and compared function
# by function. Nothing is written into the repository.
#
# Prints recomp's own notes and warnings from each side, which functions
# appear or disappear, how many bodies differ, and the most common kinds of
# line added and removed, with addresses and numbers folded to N, and guest
# function names to F. That is enough to see a codegen change across
# thousands of functions without printing the recompiled game itself.
#
# Needs cmake, a C++ compiler, python3 and Capstone (CAPSTONE_PREFIX, default
# ~/devtools/capstone-install, the same default as CMakeLists.txt).
set -e
BIN="$1"; OLD="${2:-a7b0bb3}"; NEW="${3:-HEAD}"
[ -f "$BIN" ] || { echo "usage: $0 <binary.rpx> [old-rev] [new-rev]" >&2; exit 2; }
CQ="$(cd "$(dirname "$0")/.." && pwd)"
CAP="${CAPSTONE_PREFIX:-$HOME/devtools/capstone-install}"
W="${TMPDIR:-/tmp}/recomp-compare"
mkdir -p "$W"

build() {  # rev -> path of recomp
    rev="$1"; dir="$W/src-$(git -C "$CQ" rev-parse --short "$rev")"
    [ -d "$dir" ] || git -C "$CQ" worktree add -f --detach "$dir" "$rev" >/dev/null 2>&1
    cmake -S "$dir" -B "$dir/build" -DCMAKE_BUILD_TYPE=Release -DCAPSTONE_PREFIX="$CAP" >/dev/null
    cmake --build "$dir/build" -j"$(nproc)" --target recomp >/dev/null
    echo "$dir/build/recomp"
}

for side in old new; do
    rev=$OLD; [ $side = new ] && rev=$NEW
    echo "building recomp at $rev ($(git -C "$CQ" log -1 --format='%h %ad %s' --date=short "$rev"))"
    r="$(build "$rev")"
    set +e
    "$r" --entry-alias arkchemy_game_entry "$BIN" -o "$W/$side.c" 2>"$W/$side.err"
    echo "  exit $?"
    set -e
done

python3 - "$W" <<'PYEOF'
import collections, re, sys
w = sys.argv[1]
fdef = re.compile(r'^void (ppc_[A-Za-z0-9_]+)\(PpcContext \*ctx(?:, uint32_t addr)?\) \{$')

def funcs(path):
    out, name, body, depth = {}, None, [], 0
    for line in open(path, errors="replace"):
        if name is None:
            m = fdef.match(line.rstrip("\n"))
            if m:
                name, body, depth = m.group(1), [], line.count("{") - line.count("}")
            continue
        body.append(line.rstrip("\n"))
        depth += line.count("{") - line.count("}")
        if depth <= 0:
            out[name] = body
            name = None
    return out

def notes(path):
    c = collections.Counter()
    for line in open(path, errors="replace"):
        line = line.strip()
        if not line:
            continue
        c[re.sub(r"\b(0x[0-9a-fA-F]+|\d+)\b", "N", line)[:160]] += 1
    return c

print("\n== recomp's messages (count  text, numbers folded)")
o, n = notes(f"{w}/old.err"), notes(f"{w}/new.err")
for k in sorted(set(o) | set(n)):
    if o[k] != n[k]:
        print(f"  old {o[k]:>6}  new {n[k]:>6}  {k}")

old, new = funcs(f"{w}/old.c"), funcs(f"{w}/new.c")
names = set(old) | set(new)
print(f"\n== functions: old {len(old)}, new {len(new)}")
gone, added = sorted(set(old) - set(new)), sorted(set(new) - set(old))
print(f"  only in old: {len(gone)}  {' '.join(gone[:15])}")
print(f"  only in new: {len(added)}  {' '.join(added[:15])}")

guest = re.compile(r"\bppc_(" + "|".join(re.escape(x[4:]) for x in sorted(names, key=len, reverse=True)[:30000]) + r")\b") if names else None
def norm(l):
    l = re.sub(r"\bL_[0-9a-f]+\b", "L_N", l)
    l = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)u?\b", "N", l)
    return guest.sub("F", l) if guest else l

differ = 0
plus, minus = collections.Counter(), collections.Counter()
examples = {}
for k in sorted(set(old) & set(new)):
    if old[k] == new[k]:
        continue
    differ += 1
    a = collections.Counter(norm(x) for x in old[k])
    b = collections.Counter(norm(x) for x in new[k])
    for line, cnt in (b - a).items():
        plus[line] += cnt; examples.setdefault(("+", line), k)
    for line, cnt in (a - b).items():
        minus[line] += cnt; examples.setdefault(("-", line), k)
print(f"  bodies that differ: {differ}")
print("\n== most common lines ADDED (count  line  first function)")
for line, cnt in plus.most_common(30):
    print(f"  {cnt:>7}  {line.strip()[:110]}   [{examples[('+', line)]}]")
print("\n== most common lines REMOVED")
for line, cnt in minus.most_common(30):
    print(f"  {cnt:>7}  {line.strip()[:110]}   [{examples[('-', line)]}]")
PYEOF
