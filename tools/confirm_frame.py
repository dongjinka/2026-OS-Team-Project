#!/usr/bin/env python3
r"""
confirm_frame.py — qemu reproducer for the confirm-escape wire-framing race.

The kernel echoes a "REQ|" command's prefix one byte per UART interrupt; agentd's
confirm_request() printf("CONFIRM_REQ|...\n") runs in the gap between two echo
interrupts, so the host sees `RCONFIRM_REQ|5|7|exec` / `EQ|` instead of a clean
line — and then never recognises the confirm. This stresses that path back-to-back
(SPAWN immediately followed by another REQ|, Hangul argv) and inspects the RAW
serial transcript, independent of any host-side recovery:

    clean      occurrence = a line that *starts* with  CONFIRM_REQ|<pid>|<call>|
    corrupted  occurrence = "CONFIRM_REQ" present but NOT at column 0 of its line

    VULNERABLE = >=1 corrupted occurrence (current/broken kernel)
    SAFE       = >=1 confirm seen and 0 corrupted (after the console echo fix)
    INCONCLUSIVE = no confirm lines seen at all (setup problem — re-run)

Boots its own qemu on an isolated port with a private fs.img copy, so it runs
alongside a live 4444 agent session.

Run:  cd xv6-riscv && make kernel/kernel fs.img && cd ..
      python3 tools/confirm_frame.py            # exit 0 unless harness/kernel error
"""
import os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from qemu_harness import QemuHarness

FS_COPY = "/tmp/fs_confirm_frame.img"
LOG     = "/tmp/confirm_frame_qemu.log"
PORT    = 5559
ITERS   = int(os.environ.get("CONFIRM_FRAME_ITERS", "30"))

CONFIRM = "CONFIRM_REQ|"
DONE    = "[agentd] SPAWN /echo done"

def info(msg): print(f"[frame] {msg}", flush=True)


def count_token(text, token):
    return text.count(token)


def latest_pid(text):
    """pid of the most recent CONFIRM_REQ| occurrence (corruption-tolerant)."""
    i = text.rfind(CONFIRM)
    if i == -1:
        return None
    tail = text[i + len(CONFIRM):]
    m = re.match(r"(\d+)\|", tail)
    return m.group(1) if m else None


def wait_token_count(h, token, target, timeout):
    """Wait until the transcript contains >= `target` occurrences of `token`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count_token(h.transcript(), token) >= target:
            return True
        time.sleep(0.1)
    return False


def stress(h):
    confirms_prior = 0
    done_prior = 0
    for k in range(ITERS):
        # SPAWN, then immediately a sibling REQ| whose prefix echo streams while
        # agentd execs — that's the byte that lands inside the confirm printf.
        h.send("REQ|SPAWN|/echo|echo|안녕", settle=0.0)
        h.send(f"REQ|PRINT|__OBS{k}__", settle=0.0)

        if not wait_token_count(h, CONFIRM, confirms_prior + 1, 16.0):
            info(f"iter {k}: no CONFIRM_REQ seen (timeout)")
            return "INCONCLUSIVE", f"no confirm at iter {k}"
        confirms_prior += 1

        pid = latest_pid(h.transcript())
        if pid is None:
            return "INCONCLUSIVE", f"could not parse pid at iter {k}"
        h.send(f"REQ|agent:host|CONFIRM_RES|{pid}|y", settle=0.0)

        # let the child exec + report before the next iteration
        wait_token_count(h, DONE, done_prior + 1, 16.0)
        done_prior = count_token(h.transcript(), DONE)
        if h.fatal_seen():
            return "ERROR", f"kernel fatal at iter {k}: {h.fatal_seen()}"

    # ---- inspect the raw transcript for framing corruption ----
    clean = corrupted = 0
    examples = []
    for ln in h.transcript().split("\n"):
        if "CONFIRM_REQ" not in ln:
            continue
        if ln.startswith(CONFIRM):
            clean += 1
        else:
            corrupted += 1
            if len(examples) < 5:
                examples.append(ln)
    info(f"confirm lines: clean={clean}  corrupted={corrupted}  (iters={ITERS})")
    for ex in examples:
        info(f"  corrupted example: {ex!r}")
    if clean == 0 and corrupted == 0:
        return "INCONCLUSIVE", "no CONFIRM_REQ lines in transcript"
    if corrupted > 0:
        return "VULNERABLE", f"{corrupted} corrupted / {clean+corrupted} confirm lines"
    return "SAFE", f"{clean} clean / 0 corrupted confirm lines"


def main():
    with QemuHarness(port=PORT, fs_copy=FS_COPY, log=LOG, tag="frame") as h:
        h.start()
        if not h.wait_boot():
            sys.exit("[frame] shell never came up — see " + LOG)
        info("shell up; stressing spawn -> confirm-escape framing")
        verdict, detail = stress(h)
        fatal = h.fatal_seen()
        print("\n==================== CONFIRM-FRAME SUMMARY ====================", flush=True)
        print(f"  [{verdict:<12}] {detail}", flush=True)
        if fatal:
            print(f"  [FATAL] kernel marker(s): {fatal}", flush=True)
        print("  VULNERABLE = console echo race corrupts the CONFIRM_REQ line "
              "(expected on the pre-fix kernel).", flush=True)
        print("==============================================================", flush=True)
        # Non-zero only on harness/kernel failure — VULNERABLE is an expected,
        # successful red verdict on the unpatched build.
        bad = verdict in ("ERROR", "INCONCLUSIVE")
        sys.exit(1 if (fatal or bad) else 0)


if __name__ == "__main__":
    main()
