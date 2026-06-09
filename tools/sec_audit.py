#!/usr/bin/env python3
"""
sec_audit.py — head-less red-team harness for the SECURITY_AUDIT findings.

Boots an isolated xv6/qemu instance (smp=1, dedicated TCP serial port, a
private fs.img copy — all via tools/qemu_harness.py) and runs reproducer
programs that exercise *existing* syscalls to demonstrate each finding. It
changes NO kernel code: each scenario just reports whether the kernel currently
allows the abuse.

    RESULT=VULNERABLE  -> the hole exists on this build (expected on main today)
    RESULT=SAFE        -> the corresponding fix is in place

The same harness therefore doubles as the regression that proves a fix once it
lands. smp=1 is deliberate: it avoids the unrelated, documented ASK cache-HIT
SMP panic so a crash here means a real problem.

Paths are derived relative to this file, so it runs from any clone. It uses an
isolated port (5557) and its own fs copy, so it can run alongside a 4444 agent
session or the ralph_* harnesses.

Usage:  python3 tools/sec_audit.py
Build first:  (cd xv6-riscv && make kernel/kernel fs.img)
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import QemuHarness

FS_COPY = "/tmp/fs_sec_audit.img"
LOG     = "/tmp/sec_audit_qemu.log"
PORT    = 5557

def info(msg): print(f"[sec] {msg}", flush=True)

# ---------- scenarios ----------
# Each scenario fn(h) drives the guest and returns
# ("VULNERABLE"|"SAFE"|"INCONCLUSIVE"|"ERROR", detail).
def run_secnice(h):
    """#3 — jailed agent reniceing a non-self process."""
    base = len(h.transcript())
    h.send("secnice")
    if not h.wait_for("SECNICE: RESULT=", timeout=25.0):
        return ("ERROR", "no RESULT marker (timeout)")
    tail = h.transcript()[base:]
    if "RESULT=VULNERABLE" in tail:
        # pull the rc/prio line for the report
        line = next((l for l in tail.splitlines() if "rc=" in l), "")
        return ("VULNERABLE", line.strip())
    if "RESULT=SAFE" in tail:
        return ("SAFE", "jailed setpriority on non-self denied")
    if "RESULT=INCONCLUSIVE" in tail:
        line = next((l for l in tail.splitlines() if "INCONCLUSIVE" in l), "")
        return ("INCONCLUSIVE", line.strip() or "victim not live at attack time")
    return ("ERROR", "marker present but unclassified")

def run_secconfirm(h):
    """#1 — jailed agent self-approving its own confirm-escape via dispatch."""
    base = len(h.transcript())
    h.send("secconfirm")
    if not h.wait_for("SECCONFIRM: RESULT=", timeout=30.0):
        return ("ERROR", "no RESULT marker (timeout)")
    tail = h.transcript()[base:]
    line = next((l for l in tail.splitlines()
                 if "SECCONFIRM:" in l and "RESULT=" in l), "")
    if "RESULT=VULNERABLE" in tail:
        return ("VULNERABLE", line.strip())
    if "RESULT=SAFE" in tail:
        return ("SAFE", line.strip() or "self-dispatch did not approve the blocked syscall")
    return ("ERROR", "marker present but unclassified")

SCENARIOS = [
    ("F3-nice", "jailed agent NICE on arbitrary pid (scheduling DoS)", run_secnice),
    ("F1-confirm", "jailed agent self-approves confirm-escape via dispatch", run_secconfirm),
]

# ---------- main ----------
def main():
    results = []
    with QemuHarness(port=PORT, fs_copy=FS_COPY, log=LOG, tag="sec") as h:
        h.start()
        if not h.wait_boot():
            sys.exit("[sec] shell never came up — see " + LOG)
        info("shell up")

        for sid, desc, fn in SCENARIOS:
            verdict, detail = fn(h)
            results.append((sid, desc, verdict, detail))
            info(f"{sid}: {verdict}  ({detail})")

        fatal = h.fatal_seen()
        print("\n==================== SEC AUDIT SUMMARY ====================", flush=True)
        for sid, desc, verdict, detail in results:
            print(f"  [{verdict:<12}] {sid:<10} {desc}", flush=True)
            if detail:
                print(f"               {detail}", flush=True)
        if fatal:
            print(f"  [FATAL] kernel marker(s) seen: {fatal}", flush=True)
        print("==========================================================", flush=True)
        print("VULNERABLE = hole present on this build (expected on main; flips to "
              "SAFE after the matching fix).", flush=True)

        # Exit non-zero only on harness/kernel failure (FATAL, ERROR, or an
        # INCONCLUSIVE race that needs a re-run) — never on a VULNERABLE verdict,
        # which is the expected, successful outcome of a red-team run.
        bad = any(v in ("ERROR", "INCONCLUSIVE") for _, _, v, _ in results)
        sys.exit(1 if (fatal or bad) else 0)

if __name__ == "__main__":
    main()
