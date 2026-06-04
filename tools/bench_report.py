#!/usr/bin/env python3
"""
bench_report.py — runs the evaluation benchmarks head-less and prints a
report-ready Markdown table (CPU share vs priority, and LLM-cache hit-rate).

It only RUNS existing/added benchmark programs (cfs_bench, eval); it changes no
kernel code. Boots an isolated xv6/qemu (smp=1 so a multi-core host does not
trivially absorb every runnable child, which is required for a meaningful CFS
share measurement), on a dedicated port + private fs.img copy.

    python3 tools/bench_report.py            # print Markdown to stdout
    python3 tools/bench_report.py > docs/BENCHMARKS.md

Build first:  (cd xv6-riscv && make kernel/kernel fs.img)
Needs ~150 MB free host RAM (qemu runs with -m 128M); on a tight machine,
close other memory hogs first or the guest may fail to finish booting.
"""
import os, sys, re, socket, time, threading, subprocess, shutil

HERE    = os.path.dirname(os.path.abspath(__file__))
ROOT    = os.path.dirname(HERE)
XV6     = os.path.join(ROOT, "xv6-riscv")
KERNEL  = os.path.join(XV6, "kernel", "kernel")
FS_SRC  = os.path.join(XV6, "fs.img")
FS_COPY = "/tmp/fs_bench.img"
LOG     = "/tmp/bench_qemu.log"
HOST    = "127.0.0.1"
PORT    = 5558

# Linux sched_prio_to_weight, as ported in kernel/proc.c cfs_weight[] (index =
# priority+20). Used only to show the *expected* share beside the measured one.
WEIGHT = {0: 1024, 4: 423, 8: 172, 12: 70, 16: 29, 19: 15}

def info(msg): print(f"[bench] {msg}", file=sys.stderr, flush=True)

def start_qemu():
    if not os.path.exists(KERNEL) or not os.path.exists(FS_SRC):
        sys.exit("[bench] build first: (cd xv6-riscv && make kernel/kernel fs.img)")
    subprocess.run(["pkill", "-f", f"tcp:{HOST}:{PORT}"], check=False)
    time.sleep(0.4)
    shutil.copy(FS_SRC, FS_COPY)
    cmd = ["qemu-system-riscv64", "-machine", "virt", "-bios", "none",
           "-kernel", KERNEL, "-m", "128M", "-smp", "1",
           "-global", "virtio-mmio.force-legacy=false",
           "-drive", f"file={FS_COPY},if=none,format=raw,id=x0",
           "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
           "-display", "none", "-serial", f"tcp:{HOST}:{PORT},server"]
    p = subprocess.Popen(cmd, stdout=open(LOG, "w"), stderr=subprocess.STDOUT,
                         start_new_session=True)
    info(f"qemu pid={p.pid} (smp=1, port {PORT})")
    time.sleep(1.0)
    return p

buf, buf_lock, stop = [], threading.Lock(), {"q": False}
def reader(s):
    while not stop["q"]:
        try:
            d = s.recv(4096)
            if not d: break
            with buf_lock: buf.append(d.decode(errors="replace"))
        except socket.timeout: continue
        except Exception: break
def transcript():
    with buf_lock: return "".join(buf)
def wait_for(needle, timeout=60.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in transcript(): return True
        time.sleep(0.1)
    return False
def send(line, settle=0.3):
    s_global.sendall((line + "\n").encode()); time.sleep(settle)

def run_cfs_bench():
    base = len(transcript())
    send("cfs_bench")
    if not wait_for("CFSBENCH DONE", timeout=60.0):
        info("cfs_bench timed out"); return []
    tail = transcript()[base:]
    rows = []
    for m in re.finditer(r"CFSBENCH prio=(-?\d+) count=(\d+)", tail):
        rows.append((int(m.group(1)), int(m.group(2))))
    return rows

def run_cache_hitrate(n=50):
    base = len(transcript())
    send(f"eval cache {n}")
    if not wait_for("overall", timeout=30.0):
        info("eval cache timed out"); return None
    tail = transcript()[base:]
    m = re.search(r"hits (\d+) / total (\d+) \((\d+)%\)", tail)
    return (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else None

def md_table(rows, cache):
    total = sum(c for _, c in rows) or 1
    out = []
    out.append("## CFS 우선순위 → CPU 점유율 (smp=1)\n")
    out.append("동일 wall-clock 동안 우선순위별 자식이 경쟁한 루프 카운트와 점유율. "
               "낮은 priority 값 = 높은 가중치 = 더 많은 CPU.\n")
    out.append("| priority | 가중치(weight) | loop count | 측정 share | 기대 share |")
    out.append("|---:|---:|---:|---:|---:|")
    rows = sorted(rows, key=lambda r: r[0])
    wsum = sum(WEIGHT.get(p, 0) for p, _ in rows) or 1
    for p, c in rows:
        meas = 100.0 * c / total
        exp = 100.0 * WEIGHT.get(p, 0) / wsum
        out.append(f"| {p} | {WEIGHT.get(p,'?')} | {c:,} | {meas:.1f}% | {exp:.1f}% |")
    out.append(f"\n_total = {total:,} iterations, {len(rows)} priority levels._\n")
    out.append("## LLM 응답 캐시 hit-rate\n")
    if cache:
        hits, tot, pct = cache
        out.append(f"`eval cache`: 2라운드(cold→warm) 동일 키 {tot//2}개 조회 → "
                   f"hits {hits} / total {tot} = **{pct}%**.\n")
    else:
        out.append("_측정 실패._\n")
    return "\n".join(out)

qproc = None
try:
    qproc = start_qemu()
    s_global = socket.create_connection((HOST, PORT), timeout=20.0)
    s_global.settimeout(0.2)
    threading.Thread(target=reader, args=(s_global,), daemon=True).start()
    if not wait_for("init: starting sh", timeout=25.0) and not wait_for("$ ", 5.0):
        sys.exit("[bench] shell never came up — see " + LOG)
    info("shell up; running benchmarks (cfs_bench ~15s)...")
    rows  = run_cfs_bench()
    cache = run_cache_hitrate(50)
    print("<!-- generated by tools/bench_report.py (smp=1) -->\n")
    print(md_table(rows, cache))
    sys.exit(0 if rows else 1)
finally:
    stop["q"] = True
    if qproc:
        try: os.killpg(os.getpgid(qproc.pid), 9)
        except Exception: pass
