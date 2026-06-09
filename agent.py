"""Python <-> xv6 LLM agent bridge.

Connects to the QEMU serial port exposed by `make qemu-agent` (TCP 4444)
and runs an autonomous *agent loop* on top of Upstage Solar:

    user request
      → the model reasons and either calls a tool or gives a final answer
      → a tool runs inside the xv6 sandbox (jailed agentd)
      → the tool's OUTPUT is fed back to the model as an observation
      → repeat until the model produces an answer

Before running that loop the bridge checks the kernel's F9 response cache
(CACHE_GET): a repeated or paraphrased *knowledge* question is answered
straight from the cache, skipping Solar entirely. Only answers that used
no tools are cached — tool / multi-step answers depend on live system
state (files, processes) and would go stale.

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

import codecs
import json
import os
import re
import select
import socket
import sys
import threading
import time

try:
    import termios            # POSIX raw-mode REPL input editor
except ImportError:           # non-POSIX host — the REPL falls back to input()
    termios = None

SOLAR_BASE_URL = "https://api.upstage.ai/v1"
DEFAULT_MODEL = "solar-pro2"      # override via UPSTAGE_MODEL
MAX_STEPS = 8                     # tool calls allowed per user request
MAX_HISTORY = 24                  # chat messages kept besides the system prompt
# Host-side confirm-escape read budget. The kernel denies a jailed dangerous
# syscall after CONFIRM_TIMEOUT_TICKS (150 ≈ 15 s, kernel/confirm.c); the host
# read MUST stay bounded by the same deadline so a buried / ignored prompt can
# never leave the confirm thread blocked on stdin past that point — a zombie
# reader would otherwise steal the user's next REPL line.
HOST_CONFIRM_TIMEOUT = 15.0       # seconds
# ── kernel wire buffer limits (mirror the fixed-size buffers in the kernel;
# the host MUST stay under them or the kernel silently truncates the line) ──
WIRE_MAX = 1200                   # meta REQ| line budget — kernel agent_q is
                                  # AGENT_LINE_MAX=1280 (agentcmd.c); 1200 leaves margin
AGENTD_WIRE_MAX = 255             # tool wires are re-copied into the jailed agentd's
                                  # 256-byte queue (AGENTQ_LEN, agentcmd.c) and
                                  # truncated past this — far tighter than WIRE_MAX
CACHE_VAL_MAX = 1024              # kernel CACHE_VAL (cache.c) — a longer value is
                                  # DROPped, not cached, so don't bother sending it
MAX_CAPTURE = 1 << 20            # cap on one tool's captured output (1 MiB) so a
                                  # runaway tool can't grow host memory without bound


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

# Raw-mode REPL input-editor state (all guarded by _LOCK). While _input_live
# is set, the REPL is collecting a line in raw mode; every async writer must
# lift that line off-screen (_wipe_input) and redraw it (_draw_input) so
# concurrent xv6 output can never strand characters or desync backspace.
_input_live = False
_input_buf = ""
_input_prompt = ""

# Confirm-escape coordination (guarded by _LOCK). While a jail confirm prompt is
# outstanding, (a) the reader thread must stop interleaving live xv6 output so
# the prompt isn't buried, and (b) the main REPL must not read stdin, so there is
# never more than one thread competing for the y/N answer.
_confirm_active = False
_confirm_deferred = []            # (line, terminated) xv6 output withheld during a confirm

_RST, _GREEN, _CYAN = "\033[0m", "\033[92m", "\033[96m"
_YELLOW, _RED, _BOLD, _DIM = "\033[93m", "\033[91m", "\033[1m", "\033[2m"


def _raw(s: str) -> None:
    try:
        sys.stdout.write(s)
        sys.stdout.flush()
    except (ValueError, OSError):
        pass  # stdout closed during shutdown


# ── live raw-mode input editor: width math + line lift/redraw ───────────
_CJK_RANGES = (
    (0x1100, 0x115F), (0x2E80, 0x303E), (0x3041, 0x33FF), (0x3400, 0x4DBF),
    (0x4E00, 0x9FFF), (0xA000, 0xA4CF), (0xAC00, 0xD7A3), (0xF900, 0xFAFF),
    (0xFE30, 0xFE4F), (0xFF00, 0xFF60), (0xFFE0, 0xFFE6),
)


def _disp_width(s: str) -> int:
    """On-screen column width of `s`: CJK / Hangul glyphs occupy two columns,
    control characters none."""
    w = 0
    for ch in s:
        o = ord(ch)
        if o < 0x20:
            continue
        if 0x20000 <= o <= 0x3FFFD or any(lo <= o <= hi
                                          for lo, hi in _CJK_RANGES):
            w += 2
        else:
            w += 1
    return w


def _vis_width(s: str) -> int:
    """Display width of `s`, skipping embedded ANSI escape sequences."""
    out, i, n = 0, 0, len(s)
    while i < n:
        if s[i] == "\033":
            i += 1
            if i < n and s[i] == "[":
                i += 1
                while i < n and not s[i].isalpha():
                    i += 1
            i += 1
            continue
        out += _disp_width(s[i])
        i += 1
    return out


def _term_cols() -> int:
    try:
        return os.get_terminal_size().columns or 80
    except OSError:
        return 80


def _wipe_input() -> None:
    """Erase the live input line (prompt + typed text), including any rows it
    wrapped onto, leaving the cursor where the line began. Call holding _LOCK
    while _input_live is set.

    Row count uses a +1 safety margin: for Hangul (CJK) inputs each character
    is *2 terminal cells*, and `_disp_width` is an approximation — boundary
    cases of `(width-1) // cols` can underestimate by exactly one row, which
    presented to the user as "backspace doesn't erase the first word"
    (the wrapped first row still showed the prompt + partial input). The
    extra `\033[1A\033[2K` is harmless when the input fits on one row
    (we just clear an empty line above the prompt before redrawing).
    """
    cols = _term_cols()
    width = _vis_width(_input_prompt) + _disp_width(_input_buf)
    rows_above = max(0, (width - 1) // cols) if width > 0 else 0
    _raw("\r\033[2K")
    for _ in range(rows_above + 1):   # +1 safety margin for CJK wrap boundary
        _raw("\033[1A\033[2K")
    _raw("\r")


def _draw_input() -> None:
    """Redraw the live input line. Call holding _LOCK while _input_live."""
    _raw(_input_prompt + _input_buf)


def _line(text: str, color: str) -> None:
    """Write one complete bridge-side line; always starts on a fresh line.
    While the raw-mode editor is live, lift the input line off-screen, print,
    then redraw it below so the user's typing is never disturbed."""
    global _cursor
    with _LOCK:
        if _input_live:
            _wipe_input()
            _raw(f"{color}{text}{_RST}\n")
            _draw_input()
            return
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


def _wrap_display(s: str, width: int) -> list:
    """Wrap `s` into pieces each at most `width` display columns wide (CJK
    glyphs count as two). Breaks on spaces when possible, hard-breaks an
    over-long unbroken run otherwise. Always returns >= 1 piece."""
    pieces, cur = [], ""
    for word in s.split(" "):
        cand = word if not cur else cur + " " + word
        if _disp_width(cand) <= width:
            cur = cand
            continue
        if cur:
            pieces.append(cur)
            cur = ""
        # word alone exceeds the line — hard-break it into <=width chunks in a
        # single linear pass (no repeated full-width rescans of the remainder).
        start, used = 0, 0
        for i, ch in enumerate(word):
            w = _disp_width(ch)
            if used + w > width and i > start:  # i > start: never emit empty
                pieces.append(word[start:i])
                start, used = i, 0
            used += w
        cur = word[start:]                      # remainder (<=width) carries on
    if cur or not pieces:
        pieces.append(cur)
    return pieces


def answer(text):
    """Print the model's final answer as a clearly delimited block that adapts
    to the terminal width and wraps long / wide (CJK) lines, so the text never
    spills outside the box."""
    global _cursor
    with _LOCK:
        if _input_live:
            _wipe_input()
        elif _cursor != "start":
            _raw(_RST + "\n")

        maxw = max(20, min(_term_cols() - 4, 76))      # widest text area allowed
        rows = []
        for logical in str(text).rstrip().split("\n"):
            rows.extend(_wrap_display(logical, maxw))
        inner = min(maxw, max((_disp_width(r) for r in rows), default=0))
        inner = max(inner, _disp_width("─ answer "))    # title must still fit

        bar = f"{_BOLD}{_CYAN}"
        head = "─ answer "
        top = "╭" + head + "─" * (inner + 2 - _disp_width(head)) + "╮"
        bot = "╰" + "─" * (inner + 2) + "╯"
        _raw(f"{bar}{top}{_RST}\n")
        for r in rows:
            pad = " " * (inner - _disp_width(r))
            _raw(f"{bar}│{_RST} {r}{pad} {bar}│{_RST}\n")
        _raw(f"{bar}{bot}{_RST}\n")

        if _input_live:
            _draw_input()
        else:
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
  spawn  {"bin":"<path>","argv":["<a>","<b>",…]}  fork+exec a binary inside
                                                   the sandbox. The exec call
                                                   trips the confirm-escape
                                                   gate — the *host user* is
                                                   asked y/N before launch.
                                                   `bin` must be the absolute
                                                   path of a binary inside the
                                                   sandbox (e.g. "/echo").
                                                   argv[0] should repeat the
                                                   bin basename ("echo");
                                                   argv[1..] are the runtime
                                                   args.
  list   list the agent runtime's callable functions and their priorities
  help                     show every command and its argument format
(`spawn` is the only way to start another process. `kill` is still blocked
 outright by the sandbox.)
To change a process's priority with `nice`, first call `ps` to find its pid.

★ Token-boundary rule (applies to ALL tools — chat, write, print, spawn,
nice args, anywhere user text is echoed or quoted): preserve every byte of
non-ASCII tokens (Hangul, CJK, emoji) verbatim. Do NOT drop the first
character of "안녕"/"파일", do NOT let a leading quote merge with the first
multi-byte code point, and do NOT silently elide numbers inside Korean
sentences like "22 + 45는" — every digit and Hangul jamo must round-trip
exactly. If you are unsure, repeat the exact byte sequence the user typed
rather than paraphrasing.

PROTOCOL — on every step reply with EXACTLY ONE JSON object, nothing else:
  to call a tool:  {"thought":"<short reasoning>","tool":"<name>","args":{...}}
  to finish:       {"thought":"<short reasoning>","answer":"<reply to user>"}

After each tool call you receive an "OBSERVATION" with that tool's real
output. Use observations to decide the next step. Plan multi-step tasks:
e.g. to summarise every file, first call `ls`, then `read` each file, then
give the "answer". Keep going until you can answer; do not ask the user to
run commands themselves.

LANGUAGE — important: write the final "answer" in the SAME language as the
user's latest message. If they ask in English, answer in English; if in
Korean, answer in Korean. Match the user's language exactly; do not default
to Korean for an English question.
"""

# Single-shot prompt used only by the kernel-mediated `:ask` cache path.
# The kernel asks the host (LLM_REQ) only on a cache miss; the host returns
# exactly ONE wire command, which the kernel caches under the user's prompt.
TRANSLATE_PROMPT = """You translate ONE user request into ONE command for a
sandboxed xv6 agent. Reply with EXACTLY ONE JSON object, nothing else:
  {"tool":"<name>","args":{...}}        or
  {"tool":"chat","args":{"msg":"<plain answer>"}}   for a conversational reply.
Tools: ls | read{"file"} | write{"file","text"} | print{"msg"} | ps |
       nice{"pid","priority"} | spawn{"bin","argv"} | chat{"msg"}.
No multi-step planning."""


# Payload escape — wire is one *line*, so any embedded newline would split the
# command on the kernel's console line buffer (and a multi-line WRITE would be
# half-stored, with the tail being mis-parsed as a shell command). Convention:
# every wire builder that takes user-supplied text routes it through
# `_wire_escape`, and agentd un-escapes before use.
#   \n  → \\n
#   \r  → dropped (Windows line endings normalised)
# Backslash itself is NOT doubled — this is a one-direction escape: only the
# `\n` two-character sequence has special meaning on un-escape. Using a literal
# backslash inside a wire payload is fine as long as it isn't immediately
# followed by 'n'; if you really need literal "\n" verbatim, write it twice.
def _wire_escape(s) -> str:
    return str(s).replace("\r", "").replace("\n", "\\n")


# tool name -> wire payload (the kernel adds nothing; agentd executes it)
def wire_for(tool: str, args: dict):
    try:
        if tool == "ls":    return "LS|"
        if tool == "list":  return "LIST|"
        if tool == "ps":    return "PS|"
        if tool == "help":  return "HELP|"
        if tool == "chat":  return f"CHAT|{_wire_escape(args.get('msg',''))}"
        if tool == "read":  return f"READ|{_wire_escape(args['file'])}"
        if tool == "write":
            wf = str(args['file'])
            if ':' in wf:       # ':' delimits <file>:<text> on the wire; a path
                return None     # containing ':' would mis-split the field in agentd
            return f"WRITE|{_wire_escape(wf)}:{_wire_escape(args.get('text',''))}"
        if tool == "print": return f"PRINT|{_wire_escape(args.get('msg',''))}"
        if tool == "nice":  return f"NICE|{int(args['pid'])}:{int(args['priority'])}"
        if tool == "spawn":
            # Wire: "SPAWN|<bin>|<arg0> <arg1> ..." — spaces separate argv.
            # bin must be a non-empty string; argv defaults to [bin] when omitted
            # (i.e. argv[0] == bin) so the spawned binary always has at least
            # one arg, matching the convention of exec().
            bin_ = str(args.get("bin", "")).strip()
            if not bin_:
                return None
            argv = args.get("argv")
            if not isinstance(argv, list) or not argv:
                argv = [bin_]
            # Escape bin/argv too: an embedded newline would otherwise split the
            # wire line and the tail would be mis-parsed as a bogus REQ.
            argv_str = " ".join(_wire_escape(str(a)) for a in argv)
            return f"SPAWN|{_wire_escape(bin_)}|{argv_str}"
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
        # The kernel echoes only the bare "REQ|" prefix of a sent wire line
        # (console.c suppresses the payload echo), leaving stray "REQ|" fragment
        # lines in the captured buffer — drop them as echo noise so they never
        # leak into the observation handed to the model.
        if ln.startswith("REQ|"):
            continue
        for p in ("[agentd] ", "[agent] "):
            if ln.startswith(p):
                ln = ln[len(p):]
                break
        out.append(ln)
    return "\n".join(out).strip()


def _drain_escape(fd: int) -> None:
    """Swallow the remaining bytes of an ESC sequence (arrow / navigation
    keys) already buffered in raw mode, so they are not inserted as text."""
    while select.select([fd], [], [], 0.0)[0]:
        if not os.read(fd, 32):
            break


def _recover_surrogates(s: str) -> str:
    """Repair input decoded with the ``surrogateescape`` handler.

    Under the C / C.UTF-8 locale Python opens stdin as utf-8 *with*
    errors="surrogateescape", so any byte the line reader can't fold into a
    clean UTF-8 unit (e.g. a CJK syllable the terminal/IME delivered split)
    survives as a lone surrogate (U+DC80–U+DCFF). Such surrogates pass through
    the REPL unharmed but blow up the first time we do ``.encode("utf-8")`` for
    the kernel wire — the default strict codec rejects them ("surrogates not
    allowed"), which is the crash behind a Korean prompt killing the agent.

    Re-encoding with surrogateescape recovers the original raw bytes; when they
    were in fact valid UTF-8 (the common case — a split syllable) decoding puts
    the real character back. Anything genuinely undecodable becomes U+FFFD, so
    the returned string is *always* safe to ``.encode("utf-8")`` downstream."""
    if not any("\ud800" <= c <= "\udfff" for c in s):
        return s            # fast path: already clean, no surrogate, no copy
    return s.encode("utf-8", "surrogateescape").decode("utf-8", "replace")


def _wire_fits(wire: str, limit: int) -> bool:
    """True if `wire` fits within `limit` bytes on the kernel wire.

    Measured with surrogateescape so the byte count matches exactly what
    `_send` transmits AND so a lone surrogate (from surrogateescape-decoded
    stdin) can never raise here — the strict default codec would, which was
    the crash behind a Korean prompt killing the agent. Callers that exceed
    the limit must refuse to send rather than let the kernel truncate the
    line (which corrupts the cache / desyncs the protocol)."""
    return len(wire.encode("utf-8", "surrogateescape")) <= limit


def _read_line(prompt: str) -> str:
    """Read one REPL line.

    Two modes:
      * cooked (default) — terminal's own line editor + IME stays visible.
        Async xv6 output may interleave with the prompt, but typed bytes
        ALWAYS reach Python; this matches the behavior most users expect.
      * raw (opt-in via AGENT_RAW_INPUT=1) — our own line editor with
        _wipe_input / _draw_input to keep the prompt pinned to one line.
        Faster visual updates but multi-byte (UTF-8 / IME) input only
        renders after the last byte arrives — which can feel like "the
        key didn't register, let me press it again."

    On a non-tty (piped input, tests) the function always uses input()."""
    global _cursor, _input_live, _input_buf, _input_prompt

    # Never read stdin while a jail-confirm prompt owns it — otherwise this
    # thread and the confirm handler both block on stdin and the user's line
    # goes to whichever wins the race. Yield until the confirm resolves.
    while True:
        with _LOCK:
            if not _confirm_active:
                break
        time.sleep(0.05)

    use_raw = (termios is not None
               and sys.stdin.isatty()
               and os.environ.get("AGENT_RAW_INPUT", "").strip() in ("1", "true", "yes"))

    if not use_raw:
        with _LOCK:
            if _cursor != "start":
                _raw(_RST + "\n")
            _raw("\n" + prompt)
            # Flush the prompt to the terminal *before* input() takes over —
            # without this, the prompt can sit in stdout buffer and the
            # terminal's notion of the line-start column drifts from what
            # the user sees, which propagates to incorrect backspace
            # behavior (especially noticeable with CJK input).
            sys.stdout.flush()
            _cursor = "other"
        try:
            return _recover_surrogates(input())
        except (EOFError, KeyboardInterrupt):
            raise

    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    mode = termios.tcgetattr(fd)
    # Raw INPUT: no canonical line editing, no echo, no signal / flow-control
    # keys. OUTPUT post-processing (mode[1]) is left ON so '\n' from the async
    # print helpers still yields a carriage return while raw mode is held.
    mode[0] &= ~(termios.BRKINT | termios.ICRNL | termios.INPCK |
                 termios.ISTRIP | termios.IXON)
    mode[3] &= ~(termios.ICANON | termios.ECHO | termios.IEXTEN |
                 termios.ISIG)
    mode[6][termios.VMIN] = 1
    mode[6][termios.VTIME] = 0

    decoder = codecs.getincrementaldecoder("utf-8")("ignore")
    with _LOCK:
        if _cursor != "start":
            _raw(_RST + "\n")
        _input_buf = ""
        _input_prompt = prompt
        _input_live = True
        _cursor = "other"
        _raw("\n")
        _draw_input()

    line = ""
    try:
        termios.tcsetattr(fd, termios.TCSADRAIN, mode)
        while True:
            ch = os.read(fd, 1)
            if not ch:                              # stdin closed
                raise EOFError
            b = ch[0]
            if b in (0x0a, 0x0d):                   # Enter
                break
            if b == 0x03:                           # Ctrl-C
                raise KeyboardInterrupt
            if b == 0x04:                           # Ctrl-D
                if not _input_buf:
                    raise EOFError
                continue                            # ignore mid-line
            if b == 0x1b:                           # ESC — drop arrow/nav keys
                _drain_escape(fd)
                continue
            if b in (0x08, 0x7f):                   # Backspace / Delete
                with _LOCK:
                    if _input_buf:
                        _wipe_input()
                        _input_buf = _input_buf[:-1]
                        _draw_input()
                continue
            if b == 0x15:                           # Ctrl-U — kill whole line
                with _LOCK:
                    if _input_buf:
                        _wipe_input()
                        _input_buf = ""
                        _draw_input()
                continue
            if b < 0x20:                            # other control bytes
                continue
            piece = decoder.decode(ch)              # buffers partial UTF-8
            if piece:
                with _LOCK:
                    _input_buf += piece
                    _raw(piece)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        with _LOCK:
            line = _input_buf
            _input_live = False
            _input_buf = ""
            _raw("\n")
            _cursor = "start"
    return _recover_surrogates(line)


def _is_korean(s: str) -> bool:
    """True if the text contains any Hangul syllable — used to localize
    host-side prompts (e.g. the confirm-escape gate) to the user's question
    language."""
    return any('가' <= c <= '힣' for c in (s or ""))


def _parse_confirm_req(line: str):
    """Parse a kernel `CONFIRM_REQ|<pid>|<call>|<summary>` control line into
    (pid, call, summary), or None if the line is not a confirm request.

    Tolerates a few stray bytes prepended by the console echo race (the kernel
    echoes a sibling `REQ|` command's prefix one byte per interrupt, which can
    land in front of the confirm printf — e.g. `RCONFIRM_REQ|5|7|exec`). The
    kernel-side fix removes that echo at the source; locating the `CONFIRM_REQ|`
    token here rather than matching the start of the line is belt-and-suspenders
    so a corrupted control line is still recognised and answered, never dropped."""
    i = line.find("CONFIRM_REQ|")
    if i == -1:
        return None
    parts = line[i:].rstrip("\r\n").split("|", 3)
    if len(parts) < 3:
        return None
    pid = parts[1]
    call = parts[2]
    summary = parts[3] if len(parts) >= 4 else ""
    return (pid, call, summary)


class Agent:
    def __init__(self, host=None, port=None):
        host = host or os.environ.get("XV6_HOST", "127.0.0.1")
        port = int(port or os.environ.get("XV6_PORT", "4444"))
        self.host, self.port = host, port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

        # serialises socket writes: the REPL thread and the LLM_REQ worker
        # thread can both call _send(), and socket.sendall() is not atomic
        # across threads.
        self._send_lock = threading.Lock()

        # output capture (the reader thread fills capture_buf while capturing)
        self.cap_lock = threading.Lock()
        self.capturing = False
        self.capture_buf = ""
        self._seq = 0
        # Mock-only multi-step chain flag — set by _mock_step's write branch
        # when the prompt also says "다시"/"읽어", cleared on the next
        # OBSERVATION step which returns a read tool call. Drives N14.
        self._mock_step_after_write = False

        # kernel F9 cache RPC — the reader thread hands each "RESP|" line
        # (a CACHE_GET / CACHE_SET reply) to whoever waits on _resp_event.
        self._resp_lock = threading.Lock()
        self._resp_event = threading.Event()
        self._resp_line = None

        # conversation memory — persists across REPL turns
        self.messages = [{"role": "system", "content": SYSTEM_PROMPT}]
        # text of the in-flight user question — localizes confirm-escape prompts
        self._cur_input = ""

        # role tag for the kernel-mediated `:ask` path (kernel ignores it today;
        # kept on the wire for diagnostics). Toggle with `:role <name>`.
        self.role = "user"

        self.api_key = os.environ.get("UPSTAGE_API_KEY")
        # Empty / whitespace-only key is treated the same as unset — both are
        # equally non-functional, and treating "" as mock lets test harnesses
        # force mock mode by setting UPSTAGE_API_KEY="" without having to
        # touch the on-disk .env file.
        self.mock = not (self.api_key and self.api_key.strip())
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
            with self._send_lock:
                # surrogateescape: never let a stray lone surrogate (from
                # surrogateescape-decoded stdin) raise at the wire boundary —
                # round-trip its raw byte to the kernel instead of crashing.
                self.sock.sendall((wire + "\n").encode("utf-8", "surrogateescape"))
        except OSError as e:
            warn(f"[bridge] send failed: {e}")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def _reader(self):
        # Live-print xv6 output and, while capturing, accumulate it raw.
        # The incremental decoder is mandatory: the kernel emits multi-byte
        # (e.g. Korean) output one byte per consputc(), so a single UTF-8
        # character arrives split across several recv() chunks. Decoding each
        # chunk on its own would drop the partial bytes and lose the char.
        self.sock.settimeout(0.3)
        decoder = codecs.getincrementaldecoder("utf-8")("ignore")
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
            text = decoder.decode(chunk).replace("\r", "")
            with self.cap_lock:
                if self.capturing and len(self.capture_buf) < MAX_CAPTURE:
                    # bound growth so a runaway tool can't exhaust host memory;
                    # if capped, the marker may be missed but wait_for_marker's
                    # timeout + one-shot retry already covers that.
                    self.capture_buf += text
            pending += text
            while "\n" in pending:
                ln, pending = pending.split("\n", 1)
                # kernel cache miss on a `:ask`: it asks the host to call the
                # LLM once. Handle off-thread so the reader keeps draining.
                if ln.startswith("LLM_REQ|"):
                    prompt = ln[len("LLM_REQ|"):]
                    threading.Thread(target=self._handle_llm_req,
                                     args=(prompt,), daemon=True).start()
                    continue
                # jail Confirm-Escape — 게스트의 위험 syscall 차단 시도가
                # `CONFIRM_REQ|<pid>|<call>|<summary>` 를 보냄. 사용자에게
                # 일회 허용/거부를 묻고 `CONFIRM_RES` 로 회신.
                if "CONFIRM_REQ|" in ln:
                    # `in`, not startswith: the console echo race can prepend a
                    # few stray bytes (RCONFIRM_REQ|...). _parse_confirm_req
                    # recovers the fields; never drop a confirm or it auto-denies.
                    threading.Thread(target=self._handle_confirm_req,
                                     args=(ln,), daemon=True).start()
                    continue
                # kernel F9 cache reply — hand it to the waiting _cache_rpc().
                if ln.startswith("RESP|"):
                    with self._resp_lock:
                        self._resp_line = ln
                    self._resp_event.set()
                    continue
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
            if _confirm_active:
                # a jail confirm prompt owns the screen; withhold this line and
                # replay it once the user has answered (markers are matched off
                # capture_buf, so deferring the live echo loses nothing).
                _confirm_deferred.append((ln, terminated))
                return
            if _input_live:
                # user is mid-prompt: lift the input line, print, redraw it
                _wipe_input()
                _raw(f"{_GREEN}   xv6 ┃ {ln}{_RST}\n")
                _draw_input()
                return
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

        # The whole "REQ|<wire>" line is re-copied into the jailed agentd's
        # 256-byte queue and silently truncated past AGENTD_WIRE_MAX. Refuse
        # rather than send a truncated command (a clipped WRITE corrupts the
        # file) — the model sees this observation and can shorten / split.
        full = "REQ|" + wire
        if not _wire_fits(full, AGENTD_WIRE_MAX):
            n = len(full.encode("utf-8", "surrogateescape"))
            return (f"ERROR: tool command {n} B exceeds the {AGENTD_WIRE_MAX} B "
                    f"agentd wire limit — shorten the file path/content or write "
                    f"fewer bytes per call")

        # A marker command runs right after the real one; agentd processes
        # the queue in order, so everything printed before "[agentd] <marker>"
        # is exactly this tool's output.
        marker = f"__OBS{self._seq}__"
        self._seq += 1
        marker_echo = "REQ|PRINT|" + marker        # marker request (only the bare "REQ|" is echoed back)
        marker_out = "[agentd] " + marker          # agentd prints this

        with self.cap_lock:
            self.capture_buf = ""
            self.capturing = True
        self._send(full)
        self._send(marker_echo)

        # Wait up to 24 s for the marker. agentd processes the queue
        # serially — if a heavy prior request (write_race-style dispatch,
        # a long file write, …) is still draining, our marker can sit in
        # the queue past the old 12 s budget. 24 s covers a generous tail.
        def wait_for_marker(seconds: float) -> bool:
            d = time.time() + seconds
            while time.time() < d:
                with self.cap_lock:
                    if marker_out in self.capture_buf:
                        return True
                time.sleep(0.05)
            return False

        found = wait_for_marker(24.0)
        if not found:
            # one-shot retry: resend just the marker — agentd may have
            # processed the real wire by now but the marker request itself
            # was dropped (line truncation, wire race, …). Another 12 s.
            self._send(marker_echo)
            found = wait_for_marker(12.0)

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
        # ── N14 multi-step chain — must come *before* the generic OBSERVATION
        #    answer branch below, otherwise write→OBSERVATION → immediate answer
        #    and the second tool call (read) is never issued.
        if self._mock_step_after_write and last.startswith("OBSERVATION"):
            self._mock_step_after_write = False
            return {"thought": "(mock) read after write",
                    "tool": "read", "args": {"file": "/mock_multi.txt"}}
        if last.startswith("OBSERVATION"):
            body = last.split("\n", 1)[1] if "\n" in last else ""
            return {"thought": "(mock) returning observation",
                    "answer": "(mock mode — no LLM)\n" + body}
        t = last.lower()
        # ── N16 kill — checked *before* spawn so "종료" doesn't fall through to
        #    the echo spawn. Triggers confirm-escape via /kill.
        if any(w in t for w in ("종료", "kill")):
            return {"thought": "(mock) kill via spawn",
                    "tool": "spawn",
                    "args": {"bin": "/kill", "argv": ["kill", "999"]}}
        # ── N15 nice ACL — a "priority"/"우선순위" prompt that mentions a
        #    negative number triggers a nice() with priority=-3 (always denied
        #    by the kernel's user-class guard).
        if ("priority" in t or "우선순위" in t) and re.search(r"-\s*\d", last):
            return {"thought": "(mock) nice negative — should be denied",
                    "tool": "nice", "args": {"pid": 1, "priority": -3}}
        # ── N11 greeting — a short "안녕..." prompt → chat (PURE) → cacheable.
        if t.strip().startswith("안녕") and len(t.strip()) < 12:
            return {"thought": "(mock) greeting",
                    "tool": "chat", "args": {"msg": "안녕하세요! (mock)"}}
        # ── N17 hangul argv — verbatim multi-byte preservation regression.
        if "안녕세상" in last:
            return {"thought": "(mock) spawn echo hangul argv",
                    "tool": "spawn",
                    "args": {"bin": "/echo", "argv": ["echo", "안녕세상"]}}
        # ── existing spawn intent
        if any(w in t for w in ("spawn", "프로세스", "실행", "process", "run ")):
            return {"thought": "(mock) spawn echo",
                    "tool": "spawn",
                    "args": {"bin": "/echo", "argv": ["echo", "(mock spawned)"]}}
        # multi-line WRITE 시연 — wire 의 newline-escape 동작 확인용. 본문은
        # 일부러 \n 을 포함해 escape ↔ unescape 라운드트립이 회귀에서 결정적.
        if any(w in t for w in ("작성", "쓰기", "write file", "wrote 1")):
            # If the prompt also says "읽어" / "다시", arm the chain flag so the
            # next OBSERVATION step auto-reads (N14).
            if any(w in t for w in ("다시", "읽어", "확인")):
                self._mock_step_after_write = True
            return {"thought": "(mock) multi-line write",
                    "tool": "write",
                    "args": {"file": "/mock_multi.txt",
                             "text": "line1\nline2\nline3"}}
        if any(w in t for w in ("읽어", "read ", "내용")):
            return {"thought": "(mock) read back",
                    "tool": "read", "args": {"file": "/mock_multi.txt"}}
        if any(w in t for w in ("file", "파일", "list", "목록", "ls", "wrote", "쓴")):
            return {"thought": "(mock) list files", "tool": "ls", "args": {}}
        return {"thought": "(mock) print it", "tool": "print",
                "args": {"msg": last[:120]}}

    # ---------- kernel-mediated `:ask` cache path ----------

    def _translate(self, prompt: str) -> str:
        """Turn one user prompt into ONE wire command for the kernel to cache."""
        if self.mock:
            return f"CHAT|(mock) {prompt[:100]}"
        try:
            resp = self.client.chat.completions.create(
                model=self.model, temperature=0,
                messages=[{"role": "system", "content": TRANSLATE_PROMPT},
                          {"role": "user", "content": prompt}])
            obj = extract_json(resp.choices[0].message.content)
        except Exception as e:                       # noqa: BLE001 - report any API error
            return f"CHAT|(llm error: {e})"
        if obj:
            w = wire_for(obj.get("tool", ""), obj.get("args") or {})
            if w:
                return w
        return "CHAT|(could not translate request)"

    def _handle_llm_req(self, prompt: str) -> None:
        info(f"[bridge] LLM_REQ ← cache miss; calling "
             f"{'mock' if self.mock else 'solar'}")
        # One wire command per line: collapse any newlines the LLM emitted so
        # the embedded text can't split the LLM_RESP line on the kernel side.
        wire = self._translate(prompt).replace("\n", " ").replace("\r", " ")
        # Never let an over-long translation truncate at the kernel intake (which
        # desyncs the line) — fall back to a short, well-formed response so the
        # kernel always gets a valid LLM_RESP and never blocks waiting on one.
        if not _wire_fits(f"REQ|LLM_RESP|{wire}", WIRE_MAX):
            warn("[bridge] LLM_RESP too long for the wire — sending short fallback")
            wire = "CHAT|(response too long to cache)"
        self._send(f"REQ|LLM_RESP|{wire}")

    def _handle_confirm_req(self, line: str) -> None:
        """게스트가 위험 syscall 차단 시도 시 보낸 CONFIRM_REQ 라인을 받아
        사용자에게 y/N 을 묻고 CONFIRM_RES 로 회신. 15초 안에 응답 없으면
        커널 측 timeout 으로 자동 거부됨."""
        global _confirm_active
        # `CONFIRM_REQ|<pid>|<call>|<summary>` — tolerant parse (recovers from a
        # console-echo-race byte prefix; see _parse_confirm_req).
        parsed = _parse_confirm_req(line)
        if parsed is None:
            return
        pid, call, summary = parsed
        info(f"[bridge] CONFIRM_REQ pid={pid} call={call} summary={summary!r}")

        # Claim the screen + stdin: _read_line yields and _show defers while this
        # is set, so the prompt is the only thing on screen and the only stdin
        # reader. Lift any in-flight raw-mode input line first.
        with _LOCK:
            if _input_live:
                _wipe_input()
            _confirm_active = True

        # stdin buffer drain — ReAct 가 도구 호출 시작한 후 사용자가 (무의식적
        # 으로) enter 한 번 친 빈 line 같은 *stale 입력* 이 tty line buffer 에
        # 남아 있을 수 있다. drain 안 하면 첫 read 가 그 빈 line 을 즉시 받아
        # "n" 으로 해석 — 사용자가 본 적도 없는 prompt 에 강제 deny.
        # TTY 일 때만 시도 — pipe stdin (회귀) 은 의도된 line 만 들어옴.
        try:
            if sys.stdin.isatty() and termios is not None:
                termios.tcflush(sys.stdin.fileno(), termios.TCIFLUSH)
        except Exception:
            pass

        # Localize the gate prompt to the language of the user's current question.
        # The kernel only ever sees the y/N reply; the regression harness matches the
        # language-neutral "[jail] pid=" prefix, which is kept in both variants.
        if _is_korean(getattr(self, "_cur_input", "")):
            prompt = (f"\n[jail] pid={pid} 가 위험 syscall '{summary or call}' 호출 요청 "
                      f"— 15초 내 허용? (y/N) ")
        else:
            prompt = (f"\n[jail] pid={pid} requests dangerous syscall '{summary or call}' "
                      f"— allow within 15s? (y/N) ")

        ans = ""
        try:
            with _LOCK:
                _raw(f"{_BOLD}{_YELLOW}{prompt}{_RST}")
            ans = self._read_confirm_answer(HOST_CONFIRM_TIMEOUT)
        finally:
            allow = "y" if ans.strip().lower().startswith("y") else "n"
            self._send(f"REQ|agent:host|CONFIRM_RES|{pid}|{allow}")
            info(f"[bridge] CONFIRM_RES → pid={pid} {allow} (answer={ans!r})")
            # Release the screen: replay the xv6 output withheld during the prompt,
            # then redraw any in-flight REPL input line.
            with _LOCK:
                _confirm_active = False
                deferred = list(_confirm_deferred)
                _confirm_deferred.clear()
            for dl, dterm in deferred:
                self._show(dl, dterm)
            with _LOCK:
                if _input_live:
                    _draw_input()

    def _read_confirm_answer(self, timeout: float) -> str:
        """Read one y/N answer line from stdin.

        On a real interactive TTY the read is bounded by `timeout` seconds via
        select(): a buried or ignored prompt must never leave this thread blocked
        on stdin past the kernel's deny deadline, or it would steal the user's
        next REPL line. Returns "" on timeout / EOF (caller treats it as deny).

        On a non-tty (piped input, tests) input is deterministic and line-oriented,
        so a plain blocking input() is used — no over-read, no behavior change."""
        if not (sys.stdin.isatty() and termios is not None):
            try:
                return input()
            except (EOFError, KeyboardInterrupt):
                return ""
        try:
            fd = sys.stdin.fileno()
        except (ValueError, OSError):
            return ""
        deadline = time.time() + timeout
        buf = b""
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                return ""                       # timeout → deny
            try:
                r, _, _ = select.select([fd], [], [], remaining)
            except (OSError, ValueError):
                return ""
            if not r:
                return ""                       # timeout → deny
            try:
                chunk = os.read(fd, 256)
            except OSError:
                return ""
            if not chunk:
                return ""                       # EOF → deny
            buf += chunk
            if b"\n" in buf or b"\r" in buf:
                first = buf.split(b"\n", 1)[0].split(b"\r", 1)[0]
                return first.decode("utf-8", "ignore")

    # ---------- option 2: whole-answer reuse via the kernel F9 cache ----------

    def _cache_rpc(self, wire: str, timeout: float = 4.0):
        """Send one CACHE_GET/CACHE_SET wire command; return its RESP| line."""
        with self._resp_lock:
            self._resp_line = None
            self._resp_event.clear()
        self._send(wire)
        if not self._resp_event.wait(timeout):
            return None
        with self._resp_lock:
            return self._resp_line

    def _cache_lookup(self, question: str):
        """Return a cached answer for `question`, or None on miss.

        The kernel matches exactly (FNV-1a) and then semantically (MinHash /
        Jaccard), so a paraphrase of an earlier question can still hit.

        Key normalization MUST match `_cache_store` byte-for-byte — same
        replace + strip — otherwise the FNV-1a hash diverges and a stored
        prompt is never found on lookup, even when the user types the
        exact same string. (This was the regression behind the user-reported
        "same prompt 3 times, no [cache HIT]" issue — fix is the .strip()
        below mirroring the store path.)"""
        key = question.replace("\n", " ").replace("\r", " ").strip()
        wire = "REQ|CACHE_GET|" + key
        if not _wire_fits(wire, WIRE_MAX):   # over the kernel line limit
            return None
        resp = self._cache_rpc(wire)
        if resp and resp.startswith("RESP|HIT|"):
            return resp[len("RESP|HIT|"):].replace("\\n", "\n")
        return None

    def _cache_store(self, question: str, answer_text: str) -> None:
        """Best-effort: store a no-impure-tool answer so the next ask hits.

        Skipped only for *very* short fragments (≤3 chars) — those are likely
        context-dependent follow-ups ("y", "더?") whose answer would mis-hit
        later. A short but complete question like "11+22?" is fine."""
        key = question.replace("\n", " ").replace("\r", " ").strip()
        if len(key) < 4:
            return
        # one wire line: collapse newlines so the value can't split the line.
        val = answer_text.replace("\r", "").replace("\n", "\\n")
        # a value longer than the kernel's CACHE_VAL slot is DROPped on store —
        # don't bother sending it (surrogateescape so this never raises either).
        if len(val.encode("utf-8", "surrogateescape")) > CACHE_VAL_MAX:
            return
        klen = len(key.encode("utf-8", "surrogateescape"))
        wire = f"REQ|CACHE_SET|{klen}:{key}{val}"
        # an over-long wire line would be truncated by the kernel intake buffer
        # and corrupt the cache — skip it (the answer just won't be cached).
        if not _wire_fits(wire, WIRE_MAX):
            return
        self._cache_rpc(wire)

    # ---------- agent loop ----------

    def handle(self, user_input: str) -> None:
        self._cur_input = user_input   # remember the question's language for confirm-escape
        # option 2 — kernel F9 cache pre-check. A repeated or paraphrased
        # question whose answer is cached skips Solar and the whole tool loop.
        cached = self._cache_lookup(user_input)
        if cached is not None:
            info("[cache HIT] answer reused from kernel F9 cache (Solar not called)")
            answer(cached)
            self.messages.append({"role": "user", "content": user_input})
            self.messages.append({"role": "assistant", "content": cached})
            self._trim_history()
            return

        self.messages.append({"role": "user", "content": user_input})
        tools_used = 0
        # chat/list/help 는 *시스템 상태와 무관* — wire 통로 또는 정적 문서 —
        # 거쳐도 답이 stale 되지 않으므로 캐시 안전. ls/read/write/ps/nice/print
        # 는 상태 의존 → 캐시 차단. impure 가 하나라도 섞였으면 캐시 X.
        PURE_TOOLS = {"chat", "list", "help"}
        used_impure = False
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
                # option 2 — cache the answer if no *impure* tool ran. Solar 가
                # 단순 산술/대화에도 chat 도구를 자주 부르는데, chat 은 wire
                # 통로일 뿐 시스템 상태와 무관하므로 캐시 안전. ls/read/write
                # 등 *상태 의존* 도구가 섞였을 때만 stale 위험 → 캐시 X.
                if not used_impure:
                    self._cache_store(user_input, reply["answer"])
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
            tools_used += 1
            if tool not in PURE_TOOLS:
                used_impure = True
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
        info("  Repeated knowledge questions return from the kernel F9 cache")
        info("  with no Solar call; tool-using questions always run fresh.")
        info("  e.g.  'what files have I created?'")
        info("        'summarise everything written to files so far'")
        info("        'make a file plan.txt with three TODO items'")
        info("  :ask <prompt>   one-shot via the kernel F9 cache (skips Solar on")
        info("                  a repeat/paraphrase hit). :role <name> tags it.")
        info("-" * 64)
        prompt = f"{_BOLD}you ▸{_RST} "
        while True:
            try:
                line = _read_line(prompt)
            except (EOFError, KeyboardInterrupt):
                with _LOCK:
                    _raw("\n")
                    _cursor = "start"
                break
            line = line.strip()
            if not line:
                continue
            if line.startswith(":role "):
                self.role = line[6:].strip() or self.role
                info(f"[bridge] role = {self.role}")
                continue
            if line.startswith(":ask "):
                prompt = line[5:].strip()
                if prompt:
                    ask_wire = f"REQ|agent:{self.role}|ASK|{prompt}"
                    if not _wire_fits(ask_wire, WIRE_MAX):
                        n = len(ask_wire.encode("utf-8", "surrogateescape"))
                        warn(f"[bridge] :ask prompt too long ({n} B > {WIRE_MAX} B) "
                             f"— not sent")
                    else:
                        # fire-and-forget: the kernel handles cache/LLM_REQ and the
                        # reader thread prints the result (or drives _handle_llm_req).
                        self._send(ask_wire)
                continue
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
