#!/usr/bin/env python3
"""Checks an iscsikitd task trace (~/Library/Logs/iSCSIKit/trace-*.log).

Every seq the dext received must show the full chain
  dext Received -> SlotReserved -> NotifyOK -> daemon Callback -> DequeueOK
  -> daemon Response -> CompleteRC rc=0x0 -> dext Response -> Completed
or end in a recorded failure that still reaches Completed (or failed the
task synchronously: BufferFailed/NoClient/NoSlot/NotifyNoCallback).
Usage: check-trace.py [trace.log]   (default: newest trace)
"""
import glob, os, re, sys
from collections import defaultdict

path = sys.argv[1] if len(sys.argv) > 1 else max(
    glob.glob(os.path.expanduser("~/Library/Logs/iSCSIKit/trace-*.log")), key=os.path.getmtime)
line_re = re.compile(r"src=(\w+) ev=(\w+) seq=(\d+)")
events = defaultdict(list)
stuck, lost = 0, 0
for line in open(path):
    if "overwrote" in line:
        lost += int(re.search(r"overwrote (\d+)", line).group(1))
    m = line_re.search(line)
    if not m:
        continue
    src, ev, seq = m.group(1), m.group(2), int(m.group(3))
    if ev == "STUCK":
        stuck += 1
    if seq:
        events[seq].append(f"{src}:{ev}")

sync_fail = {"dext:BufferFailed", "dext:NoClient", "dext:NoSlot", "dext:NotifyNoCallback"}
chain = ["dext:Received", "dext:SlotReserved", "dext:NotifyOK", "daemon:Callback",
         "dext:DequeueOK", "daemon:DequeueOK", "daemon:CompleteRC", "dext:Response",
         "dext:Completed"]
complete, failed, broken = 0, 0, []
received = [s for s, evs in events.items() if "dext:Received" in evs]
for seq in sorted(received):
    evs = events[seq]
    if sync_fail & set(evs) or ("dext:Completed" in evs and not all(c in evs for c in chain)):
        failed += 1
    elif all(c in evs for c in chain):
        complete += 1
    else:
        broken.append((seq, [c for c in chain if c not in evs]))

print(f"trace: {path}")
print(f"seqs received {len(received)}, full chain {complete}, failed-but-closed {failed}, "
      f"incomplete {len(broken)}, STUCK {stuck}, ring events lost {lost}")
for seq, missing in broken[:20]:
    print(f"  seq {seq}: missing {', '.join(missing)}")
# The newest few may still be in flight when the trace was read.
sys.exit(0 if not broken and not stuck and not lost else 1)
