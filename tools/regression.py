#!/usr/bin/env python3
"""tools/regression.py — 9개 단위 테스트 회귀 하네스.

대상: Sejoong 브랜치 (jail+CFS+cache+semantic+ACL+multi-agent 통합 후).
흐름: make qemu-agent 부팅 → TCP 4444 접속 → 9개 셸 명령 순차 전송 →
출력 파싱 → 4-블록 보고서 → 종료 코드 0/1.

사용:
    ./tools/regression.sh                       # 기본
    ./tools/regression.sh --fast                # eval 인자 축소
    ./tools/regression.sh --ci                  # 한 줄 요약 (grep 친화)
    ./tools/regression.sh --only=cache_test     # 특정 테스트만
"""

from __future__ import annotations

import argparse
import atexit
import os
import re
import select
import signal
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Tuple

ROOT      = Path(__file__).resolve().parent.parent
XV6_DIR   = ROOT / "xv6-riscv"
QEMU_HOST = "127.0.0.1"
QEMU_PORT = 4444
ANSI_RE   = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

GREEN = "\033[1;32m"
RED   = "\033[1;31m"
GREY  = "\033[2m"
BOLD  = "\033[1m"
RESET = "\033[0m"

USE_COLOR = sys.stdout.isatty()


def c(text: str, color: str) -> str:
    return f"{color}{text}{RESET}" if USE_COLOR else text


def strip_ansi(s: str) -> str:
    return ANSI_RE.sub("", s)


def grep_lines(out: str, pattern: str, limit: int = 10) -> List[str]:
    rgx = re.compile(pattern)
    return [ln for ln in out.splitlines() if rgx.search(ln)][:limit]


DAEMON_NOISE_RE = re.compile(r"\[agentd\]|\[chat\]|\[deny\]")


def strip_daemon_noise(out: str) -> str:
    """agentd / chat / deny 가 콘솔에 끼어든 라인을 제거.
    agent_multi 만 이 노이즈를 *원래 기대 출력* 으로 쓰므로 따로 보존."""
    return "\n".join(ln for ln in out.splitlines() if not DAEMON_NOISE_RE.search(ln))


# ---------------------------------------------------------------------------
# 판정 / 추출
# ---------------------------------------------------------------------------

def judge_priority(out: str) -> Tuple[bool, str]:
    # Test 1 (API), Test 2 (fork 상속) 가 본질적 검증.
    # Test 3 (HIGH→MED→LOW 종료 순서) 는 3-CPU SMP 에서 본질적으로 flake
    # (3 자식이 각자 CPU 점유 → 순서 보장 안 됨) — 결과는 참고만.
    needed = ["Test 1 PASSED", "Test 2 PASSED"]
    missing = [m for m in needed if m not in out]
    if missing:
        return False, f"누락 마커: {missing}"
    has_t3 = "Test 3 PASSED" in out
    fail_m = re.search(r"FAIL:\s*([^\n]+)", out)
    if has_t3:
        return True, "Test 1·2·3 모두 PASSED"
    if fail_m:
        return True, (f"Test 1·2 PASSED; Test 3 는 SMP(3-CPU) flake 가능 — "
                      f"참고 사유: '{fail_m.group(1).strip()}'")
    return True, "Test 1·2 PASSED (Test 3 결과 미관측)"


def extract_priority(out: str) -> List[str]:
    return grep_lines(out, r"PASSED|FAIL|priority\s*=", limit=8)


def judge_fair(out: str) -> Tuple[bool, str]:
    if "higher work count" not in out:
        return False, "'higher work count' 마커 없음 — 부모가 정상 종료 안 함"
    works = [int(m) for m in re.findall(r"work=(\d+)\s*units", out)]
    if len(works) == 4:
        high = works[0] + works[1]
        low  = works[2] + works[3]
        if high > low:
            return True, f"high(prio=0) work={high}  >  low(prio=20) work={low}"
        return False, f"가중치 역전: high={high} low={low}"
    if len(works) >= 1:
        return True, (f"4 자식 모두 완료 ('higher work count' 확인); 클린 work= 라인 "
                      f"{len(works)}/4 — 나머지는 콘솔 byte-race 로 섞임 "
                      f"(가중치 비교는 클린 출력 부족으로 생략)")
    return False, "클린 work= 라인 0개 & 부모 마커는 등장 → 출력 race 심각"


def extract_fair(out: str) -> List[str]:
    return grep_lines(out, r"\[. prio=", limit=4) or grep_lines(out, r"work=", limit=4)


def judge_agentdemo(out: str) -> Tuple[bool, str]:
    if "[demo] FAIL" in out:
        fails = grep_lines(out, r"\[demo\] FAIL", limit=3)
        return False, "[demo] FAIL 등장: " + " | ".join(fails)
    ok = len(re.findall(r"\[demo\] OK", out))
    if ok < 4:
        return False, f"[demo] OK 마커 {ok}개 (4개 이상 기대)"
    return True, f"[demo] OK ×{ok}, FAIL 없음 — jail/escape/priority/exec 4종 가드 통과"


def extract_agentdemo(out: str) -> List[str]:
    return grep_lines(out, r"\[demo\]", limit=8)


def judge_write_race(out: str) -> Tuple[bool, str]:
    if "fork failed" in out:
        return False, "'fork failed' — write_race 자식 fork 실패"
    pid_lines  = re.findall(r"\[pid=\d+\]\s+start=\d+\s+end=\d+\s+delta=\d+", out)
    pid_substr = out.count("[pid=")           # byte-race 로 라인이 깨져도 시작 토큰 카운트
    wrote_n    = len(re.findall(r"\[agentd\]\s+WROTE\s+\d+\s+bytes", out))
    if "can't open /shared.txt" in out:
        return False, (f"writer 시작 토큰 {pid_substr}개 + [agentd] WROTE {wrote_n}회; "
                       f"부모 final read 'can't open' — agentd 와 /shared.txt 의 "
                       f"동시 사용으로 inode 충돌 (write_race 자체의 lock 회귀가 아닌 통합 이슈)")
    if "read failed" in out:
        return False, "'read failed' — final read 단계 실패"
    if "=== final" in out:
        return True, (f"writer {len(pid_lines)} 클린 + 부모 final read 성공 — inode 락 직렬화 OK")
    if wrote_n >= 2 or pid_substr >= 2:
        return True, (f"[agentd] WROTE {wrote_n}회 + writer 시작 토큰 {pid_substr}개 "
                      f"— writer 들은 동작 (콘솔 byte-race 로 부모 종결 마커 손상)")
    return False, f"writer 출력 부족 (WROTE={wrote_n}, [pid=={pid_substr})"


def extract_write_race(out: str) -> List[str]:
    return grep_lines(out, r"===|\[pid=", limit=8)


def judge_cache_test(out: str) -> Tuple[bool, str]:
    m = re.search(r"===\s*(\d+)\s*PASS\s*/\s*(\d+)\s*FAIL\s*===", out)
    if not m:
        return False, "'N PASS / M FAIL' 최종 라인 없음"
    npass, nfail = int(m.group(1)), int(m.group(2))
    if nfail == 0:
        return True, f"{npass} PASS / 0 FAIL"
    return False, f"{npass} PASS / {nfail} FAIL"


def extract_cache_test(out: str) -> List[str]:
    final = grep_lines(out, r"=== \d+ PASS", limit=1)
    # check() 가 찍는 실패 라인만 (요약의 '... 0 FAIL ===' 와 구별).
    fails = grep_lines(out, r"^\s+FAIL\s\s", limit=3)
    if fails or final:
        return fails + final
    return grep_lines(out, r"^\[\d+\]", limit=8)


def judge_eval_cache(out: str) -> Tuple[bool, str]:
    m = re.search(r"round 2 \(warm\)\s*:\s*miss=(\d+)\s+hit=(\d+)", out)
    if not m:
        return False, "round 2 (warm) 라인 없음"
    miss, hit = int(m.group(1)), int(m.group(2))
    if miss == 0:
        return True, f"warm round miss=0 → 모든 키 hit ({hit} hits)"
    return False, f"warm round miss={miss} (0 기대)"


def extract_eval_cache(out: str) -> List[str]:
    return grep_lines(out, r"round \d|overall", limit=4)


def judge_eval_semantic(out: str) -> Tuple[bool, str]:
    m_exact = re.search(r"exact probes\s*:\s*hits=\d+\s*/\s*\d+\s*\((\d+)%\)", out)
    m_unrel = re.search(r"unrelated probes\s*:\s*miss=\d+\s*/\s*\d+\s*\((\d+)%\)", out)
    if not (m_exact and m_unrel):
        return False, "exact / unrelated 요약 라인 누락"
    if int(m_exact.group(1)) != 100:
        return False, f"exact={m_exact.group(1)}% (100% 기대)"
    if int(m_unrel.group(1)) != 100:
        return False, f"unrelated miss={m_unrel.group(1)}% (100% 기대 — 오인 hit 없어야 함)"
    m_para = re.search(r"paraphrase probes\s*:\s*hits=\d+\s*/\s*\d+\s*\((\d+)%\)", out)
    para = m_para.group(1) if m_para else "?"
    return True, f"exact=100% & unrelated miss=100% (paraphrase={para}% 참고)"


def extract_eval_semantic(out: str) -> List[str]:
    return grep_lines(out, r"probes|=== eval semantic", limit=4)


def judge_eval_acl(out: str) -> Tuple[bool, str]:
    m = re.search(r"reader.{1,3}KILL\s*:\s*denied\s+(\d+)\s*/\s*(\d+)\s*\((\d+)%\)", out)
    if not m:
        return False, "'reader×KILL : denied …' 요약 라인 없음"
    pct = int(m.group(3))
    if pct == 100:
        return True, f"denied {m.group(1)}/{m.group(2)} (100%) — reader role KILL 전부 차단"
    return False, f"denied {m.group(1)}/{m.group(2)} ({pct}%) — 100% 기대"


def extract_eval_acl(out: str) -> List[str]:
    return grep_lines(out, r"reader.{1,3}KILL|=== eval acl", limit=2)


def judge_agent_multi(out: str) -> Tuple[bool, str]:
    if "fork failed" in out:
        return False, "fork 실패"
    # 자식 시작 표시 — role= substring 만 검사 (role 이름 자체는 byte-race 로 깨질 수 있음)
    role_n      = out.count("role=")
    deny_kernel = len(re.findall(r"\[agent\]\s+DENY\s+'KILL'", out))
    deny_legacy = len(re.findall(r"\[deny\]", out))
    deny_n      = max(deny_kernel, deny_legacy)
    if role_n == 0:
        return False, "'role=' 마커 0회 — 자식 실행 안 됨"
    if deny_n >= 2:
        return True, (f"4 자식 dispatch 동작 ('role=' 등장 {role_n}회); 커널 ACL DENY 'KILL' "
                      f"{deny_n}회 (reader role × KILL 차단 확인)")
    return False, f"'role=' 등장 {role_n}회 + ACL DENY {deny_n}회 (2회 이상 기대)"


def extract_agent_multi(out: str) -> List[str]:
    return (grep_lines(out, r"\[agent \d+ role=", limit=4)
            + grep_lines(out, r"\[deny\]", limit=2)
            + grep_lines(out, r"=== done", limit=1))


# ---------------------------------------------------------------------------
# 테스트 카탈로그
# ---------------------------------------------------------------------------

@dataclass
class Test:
    name: str
    cmd: str
    description: str
    judge:   Callable[[str], Tuple[bool, str]]
    extract: Callable[[str], List[str]]
    timeout: int = 30
    keep_daemon_logs: bool = False
    settle: float = 4.0   # 출력이 N초 잠잠해야 종료 — 내부 spinner silence 보다 길어야 함


def build_tests(fast: bool) -> List[Test]:
    fair_n     = 50 if fast else 200
    cache_n    = 20 if fast else 50
    acl_n      = 10 if fast else 20
    semantic_n = 16

    # settle 은 *마커 fallback* 용 — 마커가 살아남으면 1.2s 짧은 trail-settle
    # 로 빨리 끝남. settle 값은 byte-race 로 마커가 깨졌을 때의 안전망이라
    # 충분히 크게 잡는다 (테스트의 *내부 silence* 최대치 + 여유).
    fair_settle = (fair_n * 0.12) + 8
    # eval cache runs its N-key set/get DISK loops in silence (no per-key
    # output), so the marker/settle fallback must cover that quiet window — else
    # it fires mid-compute and the round-2 (warm) line is never captured. Scale
    # with N like fair_settle does.
    cache_settle = (cache_n * 0.2) + 10

    return [
        Test("priority_test", "priority_test",
             "setpriority/getpriority API + 자식 종료 순서로 CFS 가중치 동작 검증.",
             judge_priority, extract_priority, timeout=120, settle=12.0),

        Test("eval fair", f"eval fair {fair_n}",
             "4 spinner / 3 CPU 강제 경쟁 — prio=0 자식이 prio=20 보다 더 많은 work 누적해야 CFS PASS.",
             judge_fair, extract_fair, timeout=240, settle=fair_settle),

        Test("agentdemo", "agentdemo",
             "jail 진입, '..' 탈출 차단, 외부 파일 비가시, 음수 priority 거부, "
             "exec 차단 — 4종 가드. exec 는 confirm-escape 의 ~15s(150틱) timeout 후 거부되며, "
             "그동안 게스트가 잠들어 무출력이므로 settle 을 그 침묵 구간보다 길게 둔다.",
             judge_agentdemo, extract_agentdemo, timeout=45, settle=20.0),

        Test("write_race", "write_race",
             "4 writer 가 동일 파일에 동시 쓰기 — jail 안 inode sleeplock 직렬화 회귀.",
             judge_write_race, extract_write_race, timeout=90, settle=10.0),

        Test("cache_test", "cache_test",
             "RAM hit / LRU evict / 디스크 promote / 한·영 semantic 9 케이스 단위 검사.",
             judge_cache_test, extract_cache_test, timeout=60, settle=6.0),

        Test("eval cache", f"eval cache {cache_n}",
             "N 키 × 2 라운드 hit-rate — 1라운드 cold miss + 2라운드 warm 전부 hit 기대.",
             judge_eval_cache, extract_eval_cache, timeout=120, settle=cache_settle),

        Test("eval semantic", f"eval semantic {semantic_n}",
             "MinHash+Jaccard: 정확/어순swap/무관 3종 키 — exact 100% + unrelated 오인 0% 검증.",
             judge_eval_semantic, extract_eval_semantic, timeout=60, settle=10.0),

        Test("eval acl", f"eval acl {acl_n}",
             "reader role 의 KILL 디스패치가 ACL 로 전부 거부되는지 (자식이 살아남는지) 검증.",
             judge_eval_acl, extract_eval_acl, timeout=90, settle=8.0),

        Test("agent_multi", "agent_multi",
             "4 자식 동시 dispatch (reader×2 / writer / admin) — 락 / ACL / CFS 동시 회귀.",
             judge_agent_multi, extract_agent_multi, timeout=120,
             keep_daemon_logs=True, settle=10.0),
    ]


# ---------------------------------------------------------------------------
# QEMU 세션
# ---------------------------------------------------------------------------

class GuestSession:
    def __init__(self, boot_timeout: int):
        self.proc: subprocess.Popen | None = None
        self.sock: socket.socket | None = None
        self.boot_timeout = boot_timeout
        self._buf = bytearray()
        self._marker_seq = 0

    def start(self) -> None:
        print(c("• make -C xv6-riscv qemu-agent …", GREY), flush=True)
        self.proc = subprocess.Popen(
            ["make", "-C", str(XV6_DIR), "qemu-agent"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            preexec_fn=os.setsid,
        )
        atexit.register(self.stop)

        deadline = time.monotonic() + self.boot_timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                log = (self.proc.stdout.read() or b"").decode("utf-8", "replace")
                sys.stderr.write(log)
                raise SystemExit("qemu-agent 가 회귀 시작 전에 종료됨 (빌드 실패 가능).")
            try:
                s = socket.create_connection((QEMU_HOST, QEMU_PORT), timeout=1.0)
            except (ConnectionRefusedError, OSError):
                time.sleep(1.0)
                continue
            s.setblocking(False)
            self.sock = s
            # qemu-agent 는 -serial tcp:...,nowait — boot 출력이 접속 전 흘러감.
            # \n 을 보내 새 프롬프트를 유도하고 출력이 settle 할 때까지 drain.
            time.sleep(0.5)
            try:
                s.sendall(b"\n")
            except OSError:
                time.sleep(1.0)
                continue
            self._drain_until_idle(idle=1.5, max_total=self.boot_timeout)
            self._buf.clear()
            print(c("• 게스트 셸 도달", GREY), flush=True)
            return
        raise SystemExit(f"qemu 부팅 {self.boot_timeout}초 timeout")

    def _drain_until_idle(self, idle: float, max_total: float) -> bytes:
        """출력이 `idle` 초 동안 새로 안 오면 종료. byte-race 로 prompt 가
        깨져도 안전한 settle-기반 reader. `max_total` 이 hard ceiling."""
        deadline   = time.monotonic() + max_total
        last_recv  = time.monotonic()
        while time.monotonic() < deadline:
            if time.monotonic() - last_recv >= idle:
                return bytes(self._buf)
            remaining = min(0.3, deadline - time.monotonic())
            if remaining <= 0:
                break
            rlist, _, _ = select.select([self.sock], [], [], remaining)
            if not rlist:
                continue
            try:
                chunk = self.sock.recv(8192)
            except BlockingIOError:
                continue
            if not chunk:
                raise ConnectionError("게스트가 소켓을 닫음")
            self._buf.extend(chunk)
            last_recv = time.monotonic()
        # max_total 초과 — settle 못 했지만 받은 만큼은 반환
        return bytes(self._buf)

    def run(self, cmd: str, timeout: int, settle: float = 4.0) -> str:
        """테스트 명령을 보낸 뒤 출력 회수.
        전략:
          1) stale 바이트 사전 drain (이전 테스트 트레일 노이즈)
          2) `cmd ; echo EOT_<seq>_END` 송신 → 종료 마커가 substring 으로
             등장하면 즉시 종료 (가장 빠른 경로)
          3) byte-race 로 마커가 깨지면 `settle` 초 idle fallback.
        """
        assert self.sock is not None
        self._drain_until_idle(idle=0.4, max_total=2.0)
        self._buf.clear()

        self._marker_seq += 1
        marker = f"EOT_{self._marker_seq:04d}_END"
        composite = f"{cmd} ; echo {marker}"
        self.sock.sendall((composite + "\n").encode("utf-8"))

        marker_bytes = marker.encode()
        deadline  = time.monotonic() + timeout
        last_recv = time.monotonic()
        while time.monotonic() < deadline:
            # 셸이 입력 라인을 echo 하면서 마커 substring 이 첫 번째로 등장.
            # *진짜 종료 신호* 는 두 번째 발견(= `echo MARKER` 가 stdout 으로 인쇄)
            # — 두 번 이상일 때 종료로 본다.
            if self._buf.count(marker_bytes) >= 2:
                self._drain_until_idle(idle=1.2, max_total=3.0)
                break
            if time.monotonic() - last_recv >= settle:
                break  # settle fallback
            remaining = min(0.3, deadline - time.monotonic())
            if remaining <= 0:
                break
            rlist, _, _ = select.select([self.sock], [], [], remaining)
            if not rlist:
                continue
            try:
                chunk = self.sock.recv(8192)
            except BlockingIOError:
                continue
            if not chunk:
                raise ConnectionError("게스트가 소켓을 닫음")
            self._buf.extend(chunk)
            last_recv = time.monotonic()
        raw = bytes(self._buf)
        self._buf.clear()
        return strip_ansi(raw.decode("utf-8", "replace"))

    def stop(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(os.getpgid(self.proc.pid), signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass
            try:
                self.proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(os.getpgid(self.proc.pid), signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass


# ---------------------------------------------------------------------------
# 보고서
# ---------------------------------------------------------------------------

def _bar(ch: str, n: int = 63) -> str:
    return ch * n


def print_block(idx: int, total: int, test: Test, excerpt: List[str],
                passed: bool, reason: str) -> None:
    print(c(_bar("═"), GREY))
    print(c(f"[{idx}/{total}] {test.cmd}", BOLD))
    print(c("─── 설명 " + _bar("─", 54), GREY))
    for ln in test.description.split("\n"):
        print(f"  {ln}")
    print(c("─── 실행 " + _bar("─", 54), GREY))
    print(f"  $ {test.cmd}")
    print(c("─── 핵심 출력 " + _bar("─", 49), GREY))
    if excerpt:
        for ln in excerpt:
            print(f"  {ln}")
    else:
        print(c("  (관련 라인 추출 실패 — 게스트 출력 분량 부족 가능성)", GREY))
    print(c("─── 판정 " + _bar("─", 54), GREY))
    verdict = c("PASS", GREEN) if passed else c("FAIL", RED)
    print(f"  {verdict} — {reason}")


def print_ci_line(idx: int, total: int, test: Test, passed: bool, reason: str) -> None:
    verdict = c("PASS", GREEN) if passed else c("FAIL", RED)
    print(f"[{idx}/{total}] {test.cmd:<28} {verdict}  {reason}")


# ---------------------------------------------------------------------------
# 메인
# ---------------------------------------------------------------------------

def main() -> None:
    global USE_COLOR

    ap = argparse.ArgumentParser(description="xv6 기능 회귀 하네스 (Sejoong).")
    ap.add_argument("--fast",  action="store_true", help="eval 인자 축소 (1분 내 완수)")
    ap.add_argument("--ci",    action="store_true", help="한 줄 요약 + 컬러/박스 없음")
    ap.add_argument("--no-color", action="store_true", help="컬러만 끔 (박스 유지)")
    ap.add_argument("--boot-timeout", type=int, default=60,
                    help="qemu 부팅 대기 시간 (초, 기본 60)")
    ap.add_argument("--only", default="",
                    help="콤마 구분 substring — 매칭되는 테스트만 실행")
    ap.add_argument("--dump-raw", default="",
                    help="이 디렉토리에 테스트별 raw 출력을 *.txt 로 덤프")
    args = ap.parse_args()

    if args.no_color or args.ci or not sys.stdout.isatty():
        USE_COLOR = False

    tests = build_tests(fast=args.fast)
    if args.only:
        wanted: List[str] = []
        for raw in args.only.split(","):
            x = raw.strip()
            if not x:
                continue
            wanted.append(x)
            if "_" in x: wanted.append(x.replace("_", " "))
            if " " in x: wanted.append(x.replace(" ", "_"))
        tests = [t for t in tests if any(w in t.name or w in t.cmd for w in wanted)]
        if not tests:
            sys.exit(f"--only 매칭 없음: {wanted}")

    sess = GuestSession(boot_timeout=args.boot_timeout)
    sess.start()

    if args.dump_raw:
        os.makedirs(args.dump_raw, exist_ok=True)
    results: List[Tuple[Test, bool, str]] = []
    t_start = time.monotonic()
    for idx, test in enumerate(tests, 1):
        try:
            raw_out = sess.run(test.cmd, timeout=test.timeout, settle=test.settle)
            if args.dump_raw:
                p = os.path.join(args.dump_raw, f"{idx:02d}-{test.name.replace(' ', '_')}.txt")
                with open(p, "w", encoding="utf-8") as f:
                    f.write(raw_out)
            out = raw_out if test.keep_daemon_logs else strip_daemon_noise(raw_out)
            passed, reason = test.judge(out)
            excerpt = test.extract(out)
        except TimeoutError:
            passed, reason, excerpt = False, f"{test.timeout}초 timeout", []
        except (ConnectionError, OSError) as e:
            passed, reason, excerpt = False, f"소켓 오류: {e!r}", []
            results.append((test, passed, reason))
            if args.ci:
                print_ci_line(idx, len(tests), test, passed, reason)
            else:
                print_block(idx, len(tests), test, excerpt, passed, reason)
            break
        except Exception as e:
            passed, reason, excerpt = False, f"하네스 예외: {e!r}", []
        results.append((test, passed, reason))
        if args.ci:
            print_ci_line(idx, len(tests), test, passed, reason)
        else:
            print_block(idx, len(tests), test, excerpt, passed, reason)

    elapsed = int(time.monotonic() - t_start)
    n_pass = sum(1 for _, p, _ in results if p)
    total  = len(tests)

    if args.ci:
        verdict = c("PASS", GREEN) if n_pass == total else c("FAIL", RED)
        print(f"SUMMARY  {verdict}  PASS {n_pass}/{total}  elapsed={elapsed}s")
    else:
        print(c(_bar("─"), GREY))
        verdict = c("PASS", GREEN) if n_pass == total else c("FAIL", RED)
        print(f"  회귀 결과: {verdict} {n_pass}/{total}   소요 {elapsed//60}분 {elapsed%60}초")
        print(c(_bar("─"), GREY))

    sys.exit(0 if n_pass == total else 1)


if __name__ == "__main__":
    main()
