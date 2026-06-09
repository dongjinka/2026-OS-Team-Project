#!/usr/bin/env python3
r"""
confirm_wire.py — host-side reproducer + regression for the confirm-escape
wire-framing bug (the "RCONFIRM_REQ|5|7|exec" corruption the user hit).

Root cause (kernel): consoleintr() echoes a "REQ|" command's prefix bytes one
per UART interrupt; agentd's confirm_request() printf("CONFIRM_REQ|...\n") runs
in the *gap between* two echo interrupts and lands between 'R' and 'EQ|', so the
host reader sees `RCONFIRM_REQ|5|7|exec` / `EQ|` instead of a clean line. The
kernel-side fix stops echoing the REQ| prefix; this file is the deterministic,
qemu-free half: agent.py must still recognise + correctly parse a CONFIRM_REQ
control line even if a few stray bytes are prepended.

    PASS = agent._parse_confirm_req() recovers (pid, call, summary) from both a
           clean and a corruption-prefixed line, and returns None for non-confirm
           lines so ordinary tool output never false-triggers a host y/N reply.

Run:  python3 tools/confirm_wire.py      # exit 0 = all pass, 1 = a case failed
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import agent  # import-safe: agent.py guards main() under `if __name__ == ...`

# (input line, expected (pid, call, summary) or None, label)
CASES = [
    ("CONFIRM_REQ|5|7|exec",            ("5", "7", "exec"),  "clean line"),
    ("RCONFIRM_REQ|5|7|exec",           ("5", "7", "exec"),  "echo-prefixed (the reported bug)"),
    ("REQCONFIRM_REQ|12|7|exec",        ("12", "7", "exec"), "longer stray prefix"),
    ("CONFIRM_REQ|9|1|kill",            ("9", "1", "kill"),  "kill summary"),
    ("CONFIRM_REQ|5|7|",                ("5", "7", ""),      "empty summary"),
    ("CONFIRM_REQ|5|7",                 ("5", "7", ""),      "no summary field"),
    ("[agentd] SPAWN /echo done",       None,                "ordinary tool output -> ignore"),
    ("RESP|HIT|something",              None,                "other control line -> ignore"),
    ("a file mentioning REQ pipes",     None,                "prose -> ignore"),
]


def main():
    if not hasattr(agent, "_parse_confirm_req"):
        print("FAIL: agent._parse_confirm_req is not defined "
              "(host cannot recover a corrupted CONFIRM_REQ line)")
        return 1
    ok = True
    for line, expected, label in CASES:
        got = agent._parse_confirm_req(line)
        verdict = "PASS" if got == expected else "FAIL"
        if got != expected:
            ok = False
        print(f"  {verdict}  {label:38s}  {line!r:42s} -> {got!r}"
              + ("" if got == expected else f"  (expected {expected!r})"))
    print("\n=== confirm_wire: ALL PASS ===" if ok
          else "\n=== confirm_wire: FAILURES ABOVE ===")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
