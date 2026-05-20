"""Python <-> xv6 LLM bridge — Solar 프록시 + 역할 기반 액션 번역.

이 프로그램은 "OS for LLM" 설계 원칙에 따라 **의사결정을 전혀 하지 않는다**.
오케스트레이션(캐시 조회, LLM 호출 판단, 캐시 저장, 디스패치)은 모두 xv6 커널이
담당한다. 이 프로그램의 역할은 두 가지뿐이다:

  1. 사용자 입력 + 현재 역할(role)을 `REQ|agent:<role>|ASK|<프롬프트>` 로 중계
  2. 커널이 `LLM_REQ|<프롬프트>` 를 보내올 때만 Solar API를 호출하고,
     번역한 와이어 명령을 `REQ|LLM_RESP|<CMD>|<arg>` 로 돌려줌
     (xv6 커널은 인터넷이 없으므로 이 한 가지만 호스트가 대신한다)

역할(role) 은 클라이언트가 자유로이 토글하지만 **신뢰 경계는 LLM 출력**이지
사용자 본인이 아니다. ACL은 커널이 강제하므로 reader 가 KILL을 시도하면 커널이
`[deny]` 로 거절한다.

Modes:
  * Mock (UPSTAGE_API_KEY 미설정): 네트워크 호출 없이 규칙 기반 응답
  * Real: UPSTAGE_API_KEY 설정 시 Solar Pro 실제 호출 (openai SDK 필요)

Usage:
    cd /root/OS_Project/xv6-riscv && make qemu-agent   # 한 터미널
    python3 /root/OS_Project/agent.py                  # 다른 터미널
"""

import json
import os
import readline   # input() 의 line buffer 를 비동기 출력 후 redraw 하기 위해
import socket
import sys
import threading
import time

SOLAR_BASE_URL = "https://api.upstage.ai/v1"
DEFAULT_MODEL = "solar-pro2"

SYSTEM_PROMPT = """You translate a user's natural-language request into a single JSON action for an LLM-OS kernel.
Reply ONLY with one JSON object (no prose, no markdown fences). Schema:
  {"action":"print", "msg":"<text>"}                       # echo text to kernel log
  {"action":"chat",  "msg":"<reply>"}                      # natural-language reply
  {"action":"read",  "path":"<path>"}                      # read a file
  {"action":"write", "path":"<path>", "content":"<...>"}   # write/overwrite a file
  {"action":"ls",    "path":"<path>"}                      # list a directory
  {"action":"ps"}                                          # list processes
  {"action":"kill",  "pid": <int>}                         # kill a process
  {"action":"nice",  "pid": <int>, "priority": <int 0..20>}# change priority
If the user asks an open question, choose "chat". If they want a literal echo, "print".
Examples:
  user: hello world             -> {"action":"print","msg":"hello world"}
  user: kill pid 4              -> {"action":"kill","pid":4}
  user: lower pid 5 to nice 3   -> {"action":"nice","pid":5,"priority":3}
  user: write hi to /a.txt      -> {"action":"write","path":"/a.txt","content":"hi"}
  user: read /a.txt             -> {"action":"read","path":"/a.txt"}
  user: list root               -> {"action":"ls","path":"/"}
  user: process list            -> {"action":"ps"}
  user: how are you             -> {"action":"chat","msg":"I'm a kernel agent, all good."}
"""

# JSON action → 와이어 명령("CMD|arg"). REQ| 접두어와 role/LLM_RESP 래핑은 커널이 담당.
ACTION_TABLE = {
    "print": lambda a: f"PRINT|{a.get('msg','')}",
    "chat":  lambda a: f"CHAT|{a.get('msg','')}",
    "read":  lambda a: f"READ|{a['path']}",
    "write": lambda a: f"WRITE|{a['path']}|{a.get('content','')}",
    "ls":    lambda a: f"LS|{a.get('path','/')}",
    "ps":    lambda a: "PS",
    "kill":  lambda a: f"KILL|{int(a['pid'])}",
    "nice":  lambda a: f"NICE|{int(a['pid'])}:{int(a['priority'])}",
}

VALID_ROLES = ("reader", "writer", "admin")


class Agent:
    def __init__(self, host="127.0.0.1", port=4444):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._io_lock = threading.Lock()  # reader/worker 동시 출력 보호
        self.api_key = os.environ.get("UPSTAGE_API_KEY")
        self.mock = self.api_key is None
        self.client = None
        self.model = os.environ.get("UPSTAGE_MODEL", DEFAULT_MODEL)
        self.role = "reader"   # 안전한 기본 — 사용자가 :role 로 승격
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
        print(f"[agent] role = {self.role}  (use ':role <reader|writer|admin>' to switch)")

    # ---------- transport ----------

    def _emit_async(self, text: str) -> None:
        # 비동기 스레드(reader / LLM worker) 가 출력할 때 input() 의 prompt 와
        # 한 줄에 섞이지 않도록: 현재 prompt 줄을 \r\x1b[K 로 지우고, 본문을
        # 출력한 뒤 prompt + 현재 readline 입력버퍼를 다시 그린다.
        with self._io_lock:
            sys.stdout.write('\r\x1b[K')
            sys.stdout.write(text)
            if not text.endswith('\n'):
                sys.stdout.write('\n')
            try:
                buf = readline.get_line_buffer()
            except Exception:
                buf = ''
            sys.stdout.write(f'agent[{self.role}]> ' + buf)
            sys.stdout.flush()

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
        """라인 단위로 커널 출력을 처리한다. `LLM_REQ|`로 시작하면 Solar 호출
        워커를 띄우고, 그 외는 stdout에 초록색으로 출력."""
        buf = bytearray()
        while True:
            try:
                data = self.sock.recv(4096)
            except OSError:
                return
            if not data:
                return
            buf.extend(data)
            while True:
                idx = buf.find(b'\n')
                if idx == -1:
                    break
                line = bytes(buf[:idx]).decode('utf-8', 'ignore')
                del buf[:idx+1]
                if line.startswith('LLM_REQ|'):
                    prompt = line[len('LLM_REQ|'):]
                    threading.Thread(target=self._handle_llm_req,
                                     args=(prompt,), daemon=True).start()
                else:
                    self._emit_async(f'\033[92m{line}\033[0m')

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
        if head in ("ps", "프로세스"):
            return {"action": "ps"}
        if head == "ls":
            return {"action": "ls", "path": toks[1] if len(toks) >= 2 else "/"}
        if head == "read" and len(toks) >= 2:
            return {"action": "read", "path": toks[1]}
        if head == "write" and len(toks) >= 3:
            return {"action": "write", "path": toks[1], "content": " ".join(toks[2:])}
        if head == "chat":
            return {"action": "chat", "msg": " ".join(toks[1:]) or "(mock chat)"}
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
            self._emit_async(f"[agent] solar request failed: {e}")
            return None

        if text.startswith("```"):
            inner = text.strip("`")
            if "\n" in inner:
                inner = inner.split("\n", 1)[1].rsplit("\n", 1)[0]
            text = inner.strip()

        try:
            return json.loads(text)
        except json.JSONDecodeError as e:
            self._emit_async(f"[agent] malformed JSON from solar: {e}; raw={text!r}")
            return None

    def llm(self, prompt: str) -> dict | None:
        return self._mock_llm(prompt) if self.mock else self._solar_llm(prompt)

    # ---------- LLM_REQ 처리 (커널 요청에만 반응) ----------

    def _handle_llm_req(self, prompt: str) -> None:
        action = self.llm(prompt)
        if action is None:
            wire = "PRINT|(llm error)"
        else:
            fn = ACTION_TABLE.get(action.get("action"))
            try:
                wire = fn(action) if fn else f"PRINT|(unknown action {action})"
            except (KeyError, ValueError, TypeError) as e:
                wire = f"PRINT|(bad action: {e})"
        self.sock.sendall(f"REQ|LLM_RESP|{wire}\n".encode("utf-8"))
        self._emit_async(f"[agent] LLM_REQ '{prompt[:40]}' -> {wire}")

    # ---------- REPL ----------

    def repl(self) -> None:
        print("Type a request, or Ctrl-D / Ctrl-C to quit.")
        print("Commands: ':role <reader|writer|admin>' to switch role.")
        print("Examples: 'hello world', 'kill 7', 'write hi to /a.txt', 'list root'.")
        print("(캐시 조회·디스패치·ACL은 xv6 커널이 담당 — 이 프로그램은 중계만 한다)\n")
        while True:
            try:
                line = input(f"agent[{self.role}]> ")
            except (EOFError, KeyboardInterrupt):
                print()
                break
            line = line.strip()
            if not line:
                continue
            if line.startswith(":role"):
                parts = line.split()
                if len(parts) == 2 and parts[1] in VALID_ROLES:
                    self.role = parts[1]
                    print(f"[agent] role = {self.role}")
                else:
                    print(f"[agent] usage: :role <{ '|'.join(VALID_ROLES) }>")
                continue
            # 가공 없이 커널에 위임 — role prefix 부착, 커널이 캐시 조회 + ACL 검사
            self.sock.sendall(
                f"REQ|agent:{self.role}|ASK|{line}\n".encode("utf-8")
            )


def main():
    a = Agent()
    a.connect()
    time.sleep(2)
    try:
        a.repl()
    finally:
        a.close()


if __name__ == "__main__":
    main()
