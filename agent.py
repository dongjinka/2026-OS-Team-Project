"""Python <-> xv6 LLM agent bridge.

Connects to the QEMU serial port exposed by `make qemu-agent` (TCP 4444)
and runs an autonomous *agent loop* on top of Upstage Solar:

    user request
      → the model reasons and either calls a tool or gives a final answer
      → a tool runs inside the xv6 sandbox (jailed agentd)
      → the tool's OUTPUT is fed back to the model as an observation
      → repeat until the model produces an answer

The model keeps full conversation memory, so follow-up questions ("now
summarize them") build on earlier steps.

Modes:
  * Mock  (no UPSTAGE_API_KEY): a trivial rule-based stand-in — one tool
    call per request, no real planning. Lets you exercise the kernel path.
  * Solar (UPSTAGE_API_KEY set — read from a .env file next to this script
    or from the environment; needs `pip install openai`): the full loop.

Usage:
    cd xv6-riscv && make qemu-agent   # one terminal — xv6 listens on 4444
    python3 agent.py                  # another terminal
"""

import json
import os
import socket
import sys
import threading
import time

SOLAR_BASE_URL = "https://api.upstage.ai/v1"
DEFAULT_MODEL = "solar-pro2"      # override via UPSTAGE_MODEL
MAX_STEPS = 8                     # tool calls allowed per user request
MAX_HISTORY = 24                  # chat messages kept besides the system prompt


def load_dotenv():
    """Load KEY=VALUE pairs from a .env file next to this script, if present.
    Real environment variables take precedence over .env values."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env")
    try:
        with open(path) as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, val = line.split("=", 1)
                os.environ.setdefault(key.strip(),
                                      val.strip().strip('"').strip("'"))
    except FileNotFoundError:
        pass


# ── thread-safe coloured console output ────────────────────────────────
# The reader thread and the REPL thread both write to stdout. A shared
# lock serialises writes; `_cursor` tracks where the terminal cursor is
# ("start" / "xv6" mid-xv6-line / "other" mid-prompt) so every kind of
# line is started and prefixed exactly once.
_LOCK = threading.Lock()
_cursor = "start"

_RST, _GREEN, _CYAN = "\033[0m", "\033[92m", "\033[96m"
_YELLOW, _RED, _BOLD, _DIM = "\033[93m", "\033[91m", "\033[1m", "\033[2m"


def _raw(s: str) -> None:
    try:
        sys.stdout.write(s)
        sys.stdout.flush()
    except (ValueError, OSError):
        pass  # stdout closed during shutdown


def _line(text: str, color: str) -> None:
    """Write one complete bridge-side line; always starts on a fresh line."""
    global _cursor
    with _LOCK:
        if _cursor != "start":
            _raw(_RST + "\n")
        _raw(f"{color}{text}{_RST}\n")
        _cursor = "start"


def info(msg):  _line(msg, _CYAN)
def warn(msg):  _line(msg, _RED)
def think(msg): _line(f"   💭 {msg}", _DIM)
def step(n, tool, args):
    detail = "  ".join(f"{k}={v}" for k, v in args.items())
    _line(f"   ▶ step {n} · {tool}  {detail}".rstrip(), _YELLOW)


def answer(text):
    """Print the model's final answer as a clearly delimited block."""
    global _cursor
    with _LOCK:
        if _cursor != "start":
            _raw(_RST + "\n")
        _raw(f"{_BOLD}{_CYAN}╭─ answer ──────────────────────────────────────╮{_RST}\n")
        for ln in str(text).rstrip().split("\n"):
            _raw(f"{_BOLD}{_CYAN}│{_RST} {ln}\n")
        _raw(f"{_BOLD}{_CYAN}╰───────────────────────────────────────────────╯{_RST}\n")
        _cursor = "start"


# ── prompt + parsing ───────────────────────────────────────────────────

SYSTEM_PROMPT = """You are an autonomous agent running on a tiny operating system (xv6).
You help the user by reasoning step by step, calling tools, and observing
their results. You operate inside a sandbox: a single jailed directory.

TOOLS (each runs inside the sandbox):
  ls                       list the files in the sandbox (name and size)
  read   {"file":"<name>"} read a file's contents
  write  {"file":"<name>","text":"<content>"}  create/overwrite a file
  print  {"msg":"<text>"}  print a message on the xv6 console
  nice   {"pid":<int>,"priority":<int 0..20>}  change a process's priority
  ps                       list running processes (pid, state, priority, name)
  list   list the agent runtime's callable functions and their priorities
  help                     show every command and its argument format
(There is no "kill" / "exec" — the sandbox blocks them.)
To change a process's priority with `nice`, first call `ps` to find its pid.

PROTOCOL — on every step reply with EXACTLY ONE JSON object, nothing else:
  to call a tool:  {"thought":"<short reasoning>","tool":"<name>","args":{...}}
  to finish:       {"thought":"<short reasoning>","answer":"<reply to user>"}

After each tool call you receive an "OBSERVATION" with that tool's real
output. Use observations to decide the next step. Plan multi-step tasks:
e.g. to summarise every file, first call `ls`, then `read` each file, then
give the "answer". Keep going until you can answer; do not ask the user to
run commands themselves. Answer in the user's language.
"""

# tool name -> wire payload (the kernel adds nothing; agentd executes it)
def wire_for(tool: str, args: dict):
    try:
        if tool == "ls":    return "LS|"
        if tool == "list":  return "LIST|"
        if tool == "ps":    return "PS|"
        if tool == "help":  return "HELP|"
        if tool == "read":  return f"READ|{args['file']}"
        if tool == "write": return f"WRITE|{args['file']}:{args.get('text','')}"
        if tool == "print": return f"PRINT|{args.get('msg','')}"
        if tool == "nice":  return f"NICE|{int(args['pid'])}:{int(args['priority'])}"
    except (KeyError, ValueError, TypeError):
        return None
    return None


def extract_json(text: str):
    """Pull one JSON object out of an LLM reply, tolerating fences/prose."""
    if not text:
        return None
    text = text.strip()
    if text.startswith("```"):
        text = text.strip("`")
        if "\n" in text:
            text = text.split("\n", 1)[1]
    i, j = text.find("{"), text.rfind("}")
    if i < 0 or j <= i:
        return None
    try:
        obj = json.loads(text[i:j + 1])
        return obj if isinstance(obj, dict) else None
    except json.JSONDecodeError:
        return None


def clean_observation(text: str) -> str:
    """Strip kernel/agentd line prefixes so the model sees tidy output."""
    out = []
    for ln in text.split("\n"):
        ln = ln.rstrip("\r")
        for p in ("[agentd] ", "[agent] "):
            if ln.startswith(p):
                ln = ln[len(p):]
                break
        out.append(ln)
    return "\n".join(out).strip()


class Agent:
    def __init__(self, host="127.0.0.1", port=4444):
        self.host, self.port = host, port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

        # output capture (the reader thread fills capture_buf while capturing)
        self.cap_lock = threading.Lock()
        self.capturing = False
        self.capture_buf = ""
        self._seq = 0

        # conversation memory — persists across REPL turns
        self.messages = [{"role": "system", "content": SYSTEM_PROMPT}]

        self.api_key = os.environ.get("UPSTAGE_API_KEY")
        self.mock = self.api_key is None
        self.client = None
        self.model = os.environ.get("UPSTAGE_MODEL", DEFAULT_MODEL)
        if not self.mock:
            try:
                from openai import OpenAI
            except ImportError:
                warn("[bridge] openai SDK missing; falling back to mock mode. "
                     "Install with: pip install openai")
                self.mock = True
            else:
                self.client = OpenAI(api_key=self.api_key, base_url=SOLAR_BASE_URL)

        mode = "mock" if self.mock else f"solar ({self.model})"
        info("=" * 64)
        info(f"  xv6 LLM agent   ·   mode: {mode}   ·   max {MAX_STEPS} steps/request")
        info("=" * 64)

    # ---------- transport ----------

    def connect(self):
        info(f"[bridge] connecting to xv6 at {self.host}:{self.port} ...")
        while True:
            try:
                self.sock.connect((self.host, self.port))
                break
            except ConnectionRefusedError:
                time.sleep(1)
        info("[bridge] connected")
        threading.Thread(target=self._reader, daemon=True).start()

    def _send(self, wire: str) -> None:
        try:
            self.sock.sendall((wire + "\n").encode("utf-8"))
        except OSError as e:
            warn(f"[bridge] send failed: {e}")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _reader(self):
        # Live-print xv6 output and, while capturing, accumulate it raw.
        self.sock.settimeout(0.3)
        pending = ""
        while True:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                if pending:
                    self._show(pending, terminated=False)
                    pending = ""
                continue
            except OSError:
                return
            if not chunk:
                return
            text = chunk.decode("utf-8", "ignore").replace("\r", "")
            with self.cap_lock:
                if self.capturing:
                    self.capture_buf += text
            pending += text
            while "\n" in pending:
                ln, pending = pending.split("\n", 1)
                self._show(ln, terminated=True)

    def _show(self, ln: str, terminated: bool) -> None:
        """Live-print one line of xv6 output, prefixed and green."""
        global _cursor
        if terminated:
            if ln.startswith("REQ|"):
                return                       # xv6's echo of a command we sent
            if "__OBS" in ln:
                # internal end-of-output marker — but file data with no
                # trailing newline can be glued in front of it; show that.
                ln = ln.split("[agentd] __OBS")[0].rstrip()
                if not ln:
                    return
            if ln == "":
                return                       # blank line
        with _LOCK:
            if _cursor != "xv6":
                if _cursor == "other":
                    _raw("\n")
                _raw(f"{_GREEN}   xv6 ┃ ")
            _raw(ln)
            if terminated:
                _raw(f"{_RST}\n")
                _cursor = "start"
            else:
                _cursor = "xv6"

    # ---------- run one tool inside xv6, capture its output ----------

    def run_tool(self, tool: str, args: dict) -> str:
        wire = wire_for(tool, args)
        if wire is None:
            return f"ERROR: unknown tool '{tool}' or bad arguments {args}"

        # A marker command runs right after the real one; agentd processes
        # the queue in order, so everything printed before "[agentd] <marker>"
        # is exactly this tool's output.
        marker = f"__OBS{self._seq}__"
        self._seq += 1
        marker_echo = "REQ|PRINT|" + marker        # xv6 echoes this back
        marker_out = "[agentd] " + marker          # agentd prints this

        with self.cap_lock:
            self.capture_buf = ""
            self.capturing = True
        self._send("REQ|" + wire)
        self._send(marker_echo)

        deadline = time.time() + 12
        while time.time() < deadline:
            with self.cap_lock:
                if marker_out in self.capture_buf:
                    break
            time.sleep(0.05)
        with self.cap_lock:
            self.capturing = False
            buf = self.capture_buf

        # the observation sits between the echo of the marker command and
        # the marker's own output line
        s = buf.find(marker_echo)
        s = buf.find("\n", s) + 1 if s >= 0 else 0
        e = buf.find(marker_out)
        if e < 0:
            return "ERROR: timed out waiting for xv6 to respond"
        return clean_observation(buf[s:e]) or "(the tool produced no output)"

    # ---------- LLM ----------

    def _call_llm(self):
        if self.mock:
            return self._mock_step()
        try:
            resp = self.client.chat.completions.create(
                model=self.model, temperature=0, messages=self.messages)
            return extract_json(resp.choices[0].message.content)
        except Exception as e:
            warn(f"[bridge] solar request failed: {e}")
            return None

    def _mock_step(self):
        """A dumb stand-in for the LLM: one tool call, then an answer."""
        last = self.messages[-1]["content"]
        if last.startswith("OBSERVATION"):
            body = last.split("\n", 1)[1] if "\n" in last else ""
            return {"thought": "(mock) returning observation",
                    "answer": "(mock mode — no LLM)\n" + body}
        t = last.lower()
        if any(w in t for w in ("file", "파일", "list", "목록", "ls", "wrote", "쓴")):
            return {"thought": "(mock) list files", "tool": "ls", "args": {}}
        return {"thought": "(mock) print it", "tool": "print",
                "args": {"msg": last[:120]}}

    # ---------- agent loop ----------

    def handle(self, user_input: str) -> None:
        self.messages.append({"role": "user", "content": user_input})
        for n in range(1, MAX_STEPS + 1):
            self._trim_history()
            reply = self._call_llm()
            if reply is None:
                warn("[bridge] model gave no usable JSON; asking it to retry.")
                self.messages.append({"role": "user", "content":
                    "Your previous reply was not valid JSON. Reply with "
                    "exactly one JSON object as specified."})
                continue

            self.messages.append({"role": "assistant",
                                   "content": json.dumps(reply, ensure_ascii=False)})
            if reply.get("thought"):
                think(reply["thought"])

            if "answer" in reply:
                answer(reply["answer"])
                return

            tool = reply.get("tool")
            args = reply.get("args")
            if not isinstance(args, dict):
                args = {}
            if not tool:
                self.messages.append({"role": "user", "content":
                    "OBSERVATION: your reply had neither 'tool' nor 'answer'."})
                continue

            step(n, tool, args)
            obs = self.run_tool(tool, args)
            self.messages.append({"role": "user",
                                   "content": f"OBSERVATION ({tool}):\n{obs}"})

        warn(f"[bridge] stopped after {MAX_STEPS} steps without a final answer.")

    def _trim_history(self):
        if len(self.messages) > 1 + MAX_HISTORY:
            self.messages = [self.messages[0]] + self.messages[-MAX_HISTORY:]

    # ---------- REPL ----------

    def repl(self) -> None:
        global _cursor
        info("  Ask in plain language — I plan, run sandbox tools, observe,")
        info("  and remember the conversation.   Ctrl-D / Ctrl-C to quit.")
        info("  e.g.  'what files have I created?'")
        info("        'summarise everything written to files so far'")
        info("        'make a file plan.txt with three TODO items'")
        info("-" * 64)
        while True:
            with _LOCK:
                if _cursor != "start":
                    _raw(_RST + "\n")
                _raw(f"\n{_BOLD}you ▸{_RST} ")
                _cursor = "other"
            try:
                line = input()
            except (EOFError, KeyboardInterrupt):
                with _LOCK:
                    _raw("\n")
                    _cursor = "start"
                break
            line = line.strip()
            if line:
                self.handle(line)


def main():
    load_dotenv()  # pick up UPSTAGE_API_KEY / UPSTAGE_MODEL from .env
    a = Agent()
    a.connect()
    time.sleep(2)  # let xv6 finish booting before we send anything
    try:
        a.repl()
    finally:
        a.close()


if __name__ == "__main__":
    main()
