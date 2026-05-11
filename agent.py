"""Python <-> xv6 LLM bridge.

Connects to the QEMU serial port exposed by `make qemu-agent` (TCP 4444),
asks Upstage Solar Pro 3 to translate a natural-language request into a
structured action JSON, then sends the action over the wire as
`REQ|<CMD>|<arg>\\n` for the in-kernel dispatcher (kernel/agentcmd.c).

Modes:
  * Mock (default when UPSTAGE_API_KEY is unset): no network call. A trivial
    rule-based "model" lets you exercise the kernel path without a key.
  * Real: set UPSTAGE_API_KEY (and optionally UPSTAGE_MODEL). Requires
    `pip install openai`. Solar is OpenAI-compatible.

Usage:
    cd /root/OS_Project/xv6-riscv && make qemu-agent   # in one terminal
    python3 /root/OS_Project/agent.py                  # in another
"""

import json
import os
import socket
import sys
import threading
import time

SOLAR_BASE_URL = "https://api.upstage.ai/v1"
DEFAULT_MODEL = "solar-pro2"  # override via UPSTAGE_MODEL if Pro 3 differs

SYSTEM_PROMPT = """You translate a user's natural-language request into a single JSON action for an LLM-OS kernel.
Reply ONLY with one JSON object (no prose, no markdown fences). Schema:
  {"action":"print", "msg": "<text>"}
  {"action":"kill",  "pid": <int>}
  {"action":"nice",  "pid": <int>, "priority": <int 0..20>}
The user might just type something to print. Default to "print" when uncertain.
Examples:
  user: hello world          ->  {"action":"print","msg":"hello world"}
  user: kill pid 4           ->  {"action":"kill","pid":4}
  user: lower pid 5 to nice 3->  {"action":"nice","pid":5,"priority":3}
"""

ACTION_TABLE = {
    "print": lambda a: f"PRINT|{a.get('msg','')}",
    "kill":  lambda a: f"KILL|{int(a['pid'])}",
    "nice":  lambda a: f"NICE|{int(a['pid'])}:{int(a['priority'])}",
}


class Agent:
    def __init__(self, host="127.0.0.1", port=4444):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.api_key = os.environ.get("UPSTAGE_API_KEY")
        self.mock = self.api_key is None
        self.client = None
        self.model = os.environ.get("UPSTAGE_MODEL", DEFAULT_MODEL)
        if not self.mock:
            try:
                from openai import OpenAI
            except ImportError:
                print("[agent] openai SDK missing; falling back to mock mode. "
                      "Install with: pip install openai")
                self.mock = True
            else:
                self.client = OpenAI(api_key=self.api_key, base_url=SOLAR_BASE_URL)

        mode = "mock" if self.mock else f"solar ({self.model})"
        print(f"[agent] mode = {mode}")

    # ---------- transport ----------

    def connect(self):
        print(f"[*] connecting to xv6 at {self.host}:{self.port} ...")
        while True:
            try:
                self.sock.connect((self.host, self.port))
                break
            except ConnectionRefusedError:
                time.sleep(1)
        print("[+] connected\n")
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            try:
                data = self.sock.recv(4096)
            except OSError:
                return
            if not data:
                return
            sys.stdout.write("\033[92m" + data.decode("utf-8", "ignore") + "\033[0m")
            sys.stdout.flush()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    # ---------- LLM ----------

    def _mock_llm(self, prompt: str) -> dict:
        toks = prompt.strip().split()
        if not toks:
            return {"action": "print", "msg": ""}
        head = toks[0].lower()
        if head == "kill" and len(toks) >= 2 and toks[1].isdigit():
            return {"action": "kill", "pid": int(toks[1])}
        if head == "nice" and len(toks) >= 3 and toks[1].isdigit() and toks[2].isdigit():
            return {"action": "nice", "pid": int(toks[1]), "priority": int(toks[2])}
        return {"action": "print", "msg": prompt[:200]}

    def _solar_llm(self, prompt: str) -> dict | None:
        try:
            resp = self.client.chat.completions.create(
                model=self.model,
                temperature=0,
                messages=[
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user",   "content": prompt},
                ],
            )
            text = resp.choices[0].message.content.strip()
        except Exception as e:
            print(f"[agent] solar request failed: {e}")
            return None

        # be lenient: strip ```json fences if present
        if text.startswith("```"):
            inner = text.strip("`")
            if "\n" in inner:
                inner = inner.split("\n", 1)[1].rsplit("\n", 1)[0]
            text = inner.strip()

        try:
            return json.loads(text)
        except json.JSONDecodeError as e:
            print(f"[agent] malformed JSON from solar: {e}; raw={text!r}")
            return None

    def llm(self, prompt: str) -> dict | None:
        return self._mock_llm(prompt) if self.mock else self._solar_llm(prompt)

    # ---------- dispatch ----------

    def dispatch(self, action: dict) -> None:
        kind = action.get("action")
        fn = ACTION_TABLE.get(kind)
        if fn is None:
            print(f"[agent] unknown action: {action}")
            return
        try:
            wire = fn(action) + "\n"
        except (KeyError, ValueError, TypeError) as e:
            print(f"[agent] bad action payload {action}: {e}")
            return
        self.sock.sendall(wire.encode("utf-8"))
        print(f"[agent->xv6] {wire.strip()}")

    # ---------- REPL ----------

    def repl(self) -> None:
        print("Type a request, or Ctrl-D / Ctrl-C to quit.")
        print("Examples: 'hello world', 'kill 7', 'nice 5 3'.\n")
        while True:
            try:
                line = input("agent> ")
            except (EOFError, KeyboardInterrupt):
                print()
                break
            line = line.strip()
            if not line:
                continue
            action = self.llm(line)
            if action:
                self.dispatch(action)


def main():
    a = Agent()
    a.connect()
    time.sleep(2)  # let xv6 finish booting before we send anything
    try:
        a.repl()
    finally:
        a.close()


if __name__ == "__main__":
    main()
