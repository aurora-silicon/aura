"""Words of the large DCP calls (swap_submit, color tables) from a dump_s2r_ring.py file.

  python3 bigdiff.py ring.txt [max_changes]

A DCP gives each large-call tag (A491 swap_submit on macOS 27.3, a color table method) its own slot. For
each DCP and slot print:
  first call: every payload word that was not zero in the first call seen, as +offset=value
  changes:    each payload offset that changed at most max_changes times (default 60), as
              #call@seconds:value (seconds = the ring's +secs column)
A brightness field is an offset that changes only when the brightness moves; one that changes on every
call (addresses, counters) has a big count and is left out.
Payload offsets count from the first byte of the input (packet byte 12).
"""
import collections, re, sys

path = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 60
call_rx = re.compile(r"\+([0-9.]+)s c\d+ PKT dcp(\d+) big-call w0 0x[0-9a-f]+ = 0x[0-9a-f]+ tag=(.*) in=0x([0-9a-f]+) slot=(\d+)")
word_rx = re.compile(r"\+([0-9.]+)s c\d+ PKT dcp(\d+) (big-word|big-volatile) s(\d+) w(\d+) 0x[0-9a-f]+ = 0x([0-9a-f]+)")
calls = collections.Counter()
info = {}
changes = collections.defaultdict(list)
volatile = collections.defaultdict(set)
for line in open(path, errors="replace"):
    m = call_rx.search(line)
    if m:
        key = (int(m[2]), int(m[5]))
        calls[key] += 1
        info[key] = (m[3], int(m[4], 16))
        continue
    m = word_rx.search(line)
    if not m:
        continue
    key, word, val = (int(m[2]), int(m[4])), int(m[5]), int(m[6], 16)
    if m[3] == "big-word":
        changes[(key, word * 8 - 12)].append((calls[key], float(m[1]), val))
    else:
        volatile[key].add(word * 8 - 12)
if not calls:
    sys.exit("no large DCP calls (big-call records) in " + path)
for key in sorted(calls):
    tag, size = info[key]
    print(f"dcp{key[0]} slot {key[1]} tag={tag} in=0x{size:x}: {calls[key]} calls, "
          f"{sum(1 for k in changes if k[0] == key)} offsets changed, "
          f"{len(volatile[key])} volatile (over the record limit)")
    first = [(off, seq[0][2]) for (k, off), seq in sorted(changes.items()) if k == key and seq[0][0] <= 1]
    print(f"  first call: {len(first)} words not zero")
    for i in range(0, len(first), 4):
        print("   ", "  ".join(f"+0x{o:x}=0x{v:x}" for o, v in first[i:i + 4]))
    for (k, off), seq in sorted(changes.items()):
        if k == key and len(seq) <= limit:
            vals = " ".join(f"#{n}@{t:.2f}:0x{v:x}" for n, t, v in seq[:12])
            print(f"  payload +0x{off:x}  changed {len(seq)}x  {vals}{' ...' if len(seq) > 12 else ''}")
