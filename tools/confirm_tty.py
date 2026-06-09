#!/usr/bin/env python3
r"""
confirm_tty.py — pty unit test for the INTERACTIVE confirm read path.

This is the branch a real user hits: `agent._read_confirm_answer()` on a TTY,
which uses select()+os.read with a hard timeout (so a buried/ignored prompt can
never leave the reader blocked past the kernel's 15 s deny deadline, stealing the
user's next REPL line). The qemu / natlang harnesses drive agent.py over a PIPE
(isatty()==False) and therefore take the plain input() branch — they never
exercise this code. A pty slave reports isatty()==True, so it does.

    PASS = "y\n" typed → returns "y"; "n\n" → "n"; nothing typed within the
           timeout → "" (the zombie-prevention deadline fires); a partial line
           with no newline → "" (never returns a half-typed answer).

Run:  python3 tools/confirm_tty.py        # exit 0 = all pass
"""
import os, pty, sys, time, threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import agent


def read_with(stdin_obj, timeout):
    """Call _read_confirm_answer with sys.stdin swapped to stdin_obj. The method
    doesn't use `self`, so a dummy None is fine."""
    saved = sys.stdin
    sys.stdin = stdin_obj
    try:
        return agent.Agent._read_confirm_answer(None, timeout)
    finally:
        sys.stdin = saved


def case(label, to_write, write_delay, timeout, expected):
    master, slave = pty.openpty()
    f = os.fdopen(slave, "r", buffering=1)
    if to_write is not None:
        def w():
            if write_delay:
                time.sleep(write_delay)
            os.write(master, to_write)
        threading.Thread(target=w, daemon=True).start()
    try:
        got = read_with(f, timeout)
    finally:
        try: f.close()
        except Exception: pass
        try: os.close(master)
        except Exception: pass
    ok = (got == expected)
    print(f"  {'PASS' if ok else 'FAIL':4s}  {label:34s} -> {got!r}"
          + ("" if ok else f"  (expected {expected!r})"))
    return ok


def main():
    if not hasattr(agent, "Agent") or not hasattr(agent.Agent, "_read_confirm_answer"):
        print("FAIL: agent.Agent._read_confirm_answer is not defined")
        return 1
    ok = True
    ok &= case("allow: 'y' typed",          b"y\n",  0.0, 5.0, "y")
    ok &= case("deny: 'n' typed",           b"n\n",  0.0, 5.0, "n")
    ok &= case("delayed 'y' (0.3s)",        b"y\n",  0.3, 5.0, "y")
    ok &= case("timeout: nothing typed",    None,    0.0, 0.6, "")   # zombie guard
    ok &= case("partial: 'y' no newline",   b"y",    0.0, 0.6, "")   # never half-answer
    print("\n=== confirm_tty: ALL PASS ===" if ok
          else "\n=== confirm_tty: FAILURES ABOVE ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
