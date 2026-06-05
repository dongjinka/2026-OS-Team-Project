#!/usr/bin/env python3
r"""
sec_wire.py — host-side reproducer for SECURITY_AUDIT finding #4
(wire command injection). Pure unit test against agent.py's wire_for():
no kernel, no qemu, NO existing-file edits.

agent.py escapes newlines for chat / write-text / print via _wire_escape(),
but NOT for the read filename, the write filename, or spawn bin/argv. Because
the kernel's consoleintr() frames the serial stream one "REQ|..." line per
'\n', a newline embedded in those unescaped fields (from LLM output, or user
text the model copies verbatim) injects a second, attacker-chosen command line.

    VULNERABLE = wire_for(...) output contains a raw newline -> splits into
                 >=2 lines -> the 2nd line is a forged command.
    SAFE       = newline escaped to "\n" -> single line, no injection.

The sibling cache path (_handle_llm_req) already does .replace("\n"," "), which
shows the omission on the run_tool path is an oversight, not by design.

Run:  python3 tools/sec_wire.py
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import agent  # import-safe: agent.py guards main() under `if __name__ == ...`

FORGED = "REQ|WRITE|/etc/pwned:owned"   # the second line an attacker injects
NL = "\n"

# (tool, args, label, expectation)  expectation is informational only.
CASES = [
    ("read",  {"file": "notes" + NL + FORGED},                          "read filename",        "unescaped"),
    ("write", {"file": "f" + NL + FORGED, "text": "hi"},                 "write filename",       "unescaped"),
    ("spawn", {"bin": "echo", "argv": ["echo", "x" + NL + FORGED]},      "spawn argv",           "unescaped"),
    ("chat",  {"msg": "x" + NL + FORGED},                                "chat msg (control)",   "escaped"),
    ("write", {"file": "f", "text": "x" + NL + FORGED},                  "write text (control)", "escaped"),
]

def classify(wire):
    if wire is None:
        return ("ERROR", "wire_for returned None")
    lines = wire.split("\n")
    if len(lines) >= 2 and "REQ|" in lines[1]:
        return ("VULNERABLE", f"injected 2nd line: {lines[1]!r}")
    return ("SAFE", "single line / newline escaped")

def main():
    print("============ SEC WIRE  (#4 wire command injection) ============")
    vuln = 0
    for tool, args, label, _exp in CASES:
        verdict, detail = classify(agent.wire_for(tool, args))
        if verdict == "VULNERABLE":
            vuln += 1
        print(f"  [{verdict:<10}] {label}")
        print(f"               {detail}")
    print("==============================================================")
    if vuln:
        print(f"{vuln} injectable field(s) on this build — fix #4 NOT applied "
              f"(this is the vulnerable state, e.g. main / pre-fix).")
    else:
        print("0 injectable field(s) — fix #4 is in place "
              "(read/write filename + spawn fields wrapped in _wire_escape).")
    return 0

if __name__ == "__main__":
    sys.exit(main())
