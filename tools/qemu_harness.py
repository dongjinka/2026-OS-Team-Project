#!/usr/bin/env python3
"""
qemu_harness.py — shared head-less xv6/qemu serial harness.

Boots an isolated xv6/qemu instance (single-core by default, a dedicated TCP
serial port, a private fs.img copy) and exposes a tiny transcript API
(send/wait_for/transcript) over the serial line. Both tools/sec_audit.py and
tools/bench_report.py drive the guest through this one implementation instead of
each re-pasting the boot + reader + wait_for + send scaffolding.

smp=1 is the default on purpose: it avoids the documented ASK cache-HIT SMP
panic, so a crash during a run means a real problem rather than that race.

Typical use:

    from qemu_harness import QemuHarness, FATAL_MARKERS

    h = QemuHarness(port=5557, fs_copy="/tmp/fs_sec.img", log="/tmp/sec_qemu.log")
    h.start()                 # pkill stale, copy fs.img, launch qemu, connect
    if not h.wait_boot():
        sys.exit("shell never came up — see " + h.log)
    h.send("secnice")
    h.wait_for("SECNICE: RESULT=", timeout=25.0)
    print(h.transcript())
    h.stop()                  # always call (or use `with QemuHarness(...) as h:`)
"""
import os, sys, socket, time, threading, subprocess, shutil

HERE   = os.path.dirname(os.path.abspath(__file__))
ROOT   = os.path.dirname(HERE)
XV6    = os.path.join(ROOT, "xv6-riscv")
KERNEL = os.path.join(XV6, "kernel", "kernel")
FS_SRC = os.path.join(XV6, "fs.img")

# Kernel markers that mean the guest crashed — a run that sees any of these has
# failed regardless of the scenario verdict.
FATAL_MARKERS = ["panic:", "kerneltrap", "scause="]


class QemuHarness:
    def __init__(self, port, fs_copy, log, host="127.0.0.1", smp=1, mem="128M",
                 tag="qemu"):
        self.port = port
        self.fs_copy = fs_copy
        self.log = log
        self.host = host
        self.smp = smp
        self.mem = mem
        self.tag = tag
        self._buf = []
        self._cache = ""            # joined transcript, rebuilt only on new data
        self._cache_n = 0           # number of chunks the cache reflects
        self._lock = threading.Lock()
        self._stop = False
        self._sock = None
        self._proc = None

    # ---------- logging ----------
    def info(self, msg):
        print(f"[{self.tag}] {msg}", file=sys.stderr, flush=True)

    # ---------- lifecycle ----------
    def _build_cmd(self):
        return [
            "qemu-system-riscv64", "-machine", "virt", "-bios", "none",
            "-kernel", KERNEL, "-m", self.mem, "-smp", str(self.smp),
            "-global", "virtio-mmio.force-legacy=false",
            "-drive", f"file={self.fs_copy},if=none,format=raw,id=x0",
            "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
            "-display", "none",
            "-serial", f"tcp:{self.host}:{self.port},server",
        ]

    def start(self, connect_timeout=20.0, settle=1.0):
        """pkill any stale server on this port, copy fs.img, launch qemu, and
        open the serial socket + reader thread. Returns self."""
        if not os.path.exists(KERNEL) or not os.path.exists(FS_SRC):
            missing = KERNEL if not os.path.exists(KERNEL) else FS_SRC
            sys.exit(f"[{self.tag}] build first: (cd xv6-riscv && "
                     f"make kernel/kernel fs.img)\n      missing {missing}")
        subprocess.run(["pkill", "-f", f"tcp:{self.host}:{self.port}"], check=False)
        time.sleep(0.4)
        shutil.copy(FS_SRC, self.fs_copy)
        logf = open(self.log, "w")
        self._proc = subprocess.Popen(self._build_cmd(), stdout=logf, stderr=logf,
                                      start_new_session=True)
        self.info(f"qemu pid={self._proc.pid} (smp={self.smp}, port {self.port})")
        time.sleep(settle)
        self._sock = socket.create_connection((self.host, self.port),
                                              timeout=connect_timeout)
        self._sock.settimeout(0.2)
        threading.Thread(target=self._reader, daemon=True).start()
        return self

    def _reader(self):
        while not self._stop:
            try:
                d = self._sock.recv(4096)
                if not d:
                    break
                with self._lock:
                    self._buf.append(d.decode(errors="replace"))
            except socket.timeout:
                continue
            except Exception:
                break

    # ---------- transcript ----------
    def transcript(self):
        # wait_for() polls every 0.1s; rebuild the joined string only when the
        # reader has appended new chunks, so a quiet wait stays O(1) instead of
        # re-joining a growing buffer on every poll.
        with self._lock:
            if self._cache_n != len(self._buf):
                self._cache = "".join(self._buf)
                self._cache_n = len(self._buf)
            return self._cache

    def wait_for(self, needle, timeout=20.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.transcript():
                return True
            time.sleep(0.1)
        return False

    def wait_boot(self, timeout=25.0):
        """True once the guest shell is up."""
        return (self.wait_for("init: starting sh", timeout)
                or self.wait_for("$ ", 5.0))

    def send(self, line, settle=0.3):
        self._sock.sendall((line + "\n").encode())
        time.sleep(settle)

    def fatal_seen(self):
        t = self.transcript()
        return [m for m in FATAL_MARKERS if m in t]

    # ---------- teardown ----------
    def stop(self):
        self._stop = True
        if self._proc:
            try:
                os.killpg(os.getpgid(self._proc.pid), 9)
            except Exception:
                pass
            self._proc = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()
        return False
