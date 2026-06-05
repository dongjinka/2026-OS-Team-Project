#!/usr/bin/env python3
"""
sec_audit.py — head-less red-team harness for the SECURITY_AUDIT findings.

Boots an isolated xv6/qemu instance (smp=1, dedicated TCP serial port, a
private fs.img copy) and runs reproducer programs that exercise *existing*
syscalls to demonstrate each finding. It changes NO kernel code: each scenario
just reports whether the kernel currently allows the abuse.

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
import os, sys, socket, time, threading, subprocess, shutil

HERE     = os.path.dirname(os.path.abspath(__file__))
ROOT     = os.path.dirname(HERE)
XV6      = os.path.join(ROOT, "xv6-riscv")
KERNEL   = os.path.join(XV6, "kernel", "kernel")
FS_SRC   = os.path.join(XV6, "fs.img")
FS_COPY  = "/tmp/fs_sec_audit.img"
LOG      = "/tmp/sec_audit_qemu.log"
HOST     = "127.0.0.1"
PORT     = 5557

def info(msg): print(f"[sec] {msg}", flush=True)

# ---------- qemu lifecycle ----------
def stop_existing():
    subprocess.run(["pkill", "-f", f"tcp:{HOST}:{PORT}"], check=False)
    time.sleep(0.4)

def start_qemu():
    if not os.path.exists(KERNEL) or not os.path.exists(FS_SRC):
        sys.exit(f"[sec] build first: (cd xv6-riscv && make kernel/kernel fs.img)\n"
                 f"      missing {KERNEL if not os.path.exists(KERNEL) else FS_SRC}")
    shutil.copy(FS_SRC, FS_COPY)
    cmd = [
        "qemu-system-riscv64", "-machine", "virt", "-bios", "none",
        "-kernel", KERNEL, "-m", "128M", "-smp", "1",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={FS_COPY},if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-display", "none",
        "-serial", f"tcp:{HOST}:{PORT},server",
    ]
    logf = open(LOG, "w")
    p = subprocess.Popen(cmd, stdout=logf, stderr=logf, start_new_session=True)
    info(f"qemu pid={p.pid} (smp=1, port {PORT})")
    time.sleep(1.0)
    return p

# ---------- transcript ----------
buf = []
buf_lock = threading.Lock()
stop = {"q": False}

def reader(s):
    while not stop["q"]:
        try:
            d = s.recv(4096)
            if not d: break
            with buf_lock:
                buf.append(d.decode(errors="replace"))
        except socket.timeout:
            continue
        except Exception:
            break

def transcript():
    with buf_lock:
        return "".join(buf)

def wait_for(needle, timeout=20.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in transcript():
            return True
        time.sleep(0.1)
    return False

def send(line, settle=0.3):
    s_global.sendall((line + "\n").encode())
    time.sleep(settle)

FATAL = ["panic:", "kerneltrap", "scause="]

# ---------- scenarios ----------
# Each scenario: (id, description, shell_cmd, run_then_classify(transcript_tail))
# classify returns ("VULNERABLE"|"SAFE"|"ERROR", detail).
def run_secnice():
    """#3 — jailed agent reniceing a non-self process."""
    base = len(transcript())
    send("secnice")
    if not wait_for("SECNICE: RESULT=", timeout=25.0):
        return ("ERROR", "no RESULT marker (timeout)")
    tail = transcript()[base:]
    if "RESULT=VULNERABLE" in tail:
        # pull the rc/prio line for the report
        line = next((l for l in tail.splitlines() if "rc=" in l), "")
        return ("VULNERABLE", line.strip())
    if "RESULT=SAFE" in tail:
        return ("SAFE", "jailed setpriority on non-self denied")
    return ("ERROR", "marker present but unclassified")

def run_secconfirm():
    """#1 — jailed agent self-approving its own confirm-escape via dispatch."""
    base = len(transcript())
    send("secconfirm")
    if not wait_for("SECCONFIRM: RESULT=", timeout=30.0):
        return ("ERROR", "no RESULT marker (timeout)")
    tail = transcript()[base:]
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
results = []
qproc = None
try:
    stop_existing()
    qproc = start_qemu()
    s_global = socket.create_connection((HOST, PORT), timeout=20.0)
    s_global.settimeout(0.2)
    threading.Thread(target=reader, args=(s_global,), daemon=True).start()

    if not wait_for("init: starting sh", timeout=25.0) and not wait_for("$ ", timeout=5.0):
        sys.exit("[sec] shell never came up — see " + LOG)
    info("shell up")
    time.sleep(0.5)

    for sid, desc, fn in SCENARIOS:
        verdict, detail = fn()
        results.append((sid, desc, verdict, detail))
        info(f"{sid}: {verdict}  ({detail})")

    fatal = [m for m in FATAL if m in transcript()]
    print("\n==================== SEC AUDIT SUMMARY ====================", flush=True)
    for sid, desc, verdict, detail in results:
        print(f"  [{verdict:<10}] {sid:<10} {desc}", flush=True)
        if detail:
            print(f"               {detail}", flush=True)
    if fatal:
        print(f"  [FATAL] kernel marker(s) seen: {fatal}", flush=True)
    print("==========================================================", flush=True)
    print("VULNERABLE = hole present on this build (expected on main; flips to "
          "SAFE after the matching fix).", flush=True)

    # Exit non-zero only on harness/kernel failure, not on a VULNERABLE verdict
    # (a reproduced vuln is the expected, successful outcome of a red-team run).
    sys.exit(1 if (fatal or any(v == "ERROR" for _, _, v, _ in results)) else 0)
finally:
    stop["q"] = True
    if qproc:
        try:
            os.killpg(os.getpgid(qproc.pid), 9)
        except Exception:
            pass
