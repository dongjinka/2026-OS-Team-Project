# 데모 미디어 (assets)

루트 [`README.md`](../../README.md) §5 / [`README.ko.md`](../../README.ko.md) §5에서
인라인하는 데모 캡처 폴더다. **2026-06-08 캡처 8장이 들어 있고**, 모두
`mode: solar (solar-pro2)`, smp=1, agent mode 실제 세션 출력이다.

**전체 end-to-end 영상** (외부, Google Drive):
<https://drive.google.com/file/d/14ruIXM-Lg6nP3BLTjsIWOrV68E5yUNgK/view?usp=drive_link>
— 아래 8장 시연이 한 세션 안에 모두 등장. `priority_test` / `cfs_bench` 콘솔
캡처는 아직 안 들어왔다(아래 "추가 후보").

## 인덱스 (2026-06-08)

| 파일 | 무엇을 담나 | 매핑된 기능 |
| --- | --- | --- |
| `cache-hit.png` | 같은 산수 질문 2회 — 2번째에 `[cache HIT] Solar not called` | F9 캐시 hit ([Technical Report §5.4](../Technical_Report.md#54-f9--the-response-cache-implemented-on-main)) |
| `confirm-escape-allow.png` | "make a process that runs echo 'hello'" → `y` → `SPAWN /echo done (status=0)` | confirm-escape v2 + `spawn` allow ([§6.6](../Technical_Report.md#66-the-spawn-tool-verb-do_spawn--populate_jail)) |
| `confirm-escape-deny.png` | 같은 요청 → `N` → `denied (confirm-escape)` | confirm-escape v2 deny path ([§4.4](../Technical_Report.md#44-protection--isolation--a-chroot-jail--configurable-deny-list-f7)) |
| `nice-init-denied.png` | "change init priority to 5" → `[agentd] NICE: denied (pid=1 prio=5)` | F2 kernel-class 보호 ([§4.2](../Technical_Report.md#42-processes--priority-classes-f1f2)) |
| `jail-populate.png` | "list files in /agentbox" → `echo`/`cat`/`ls`/`grep`/`sh`/... | `populate_jail()` hard-link ([§6.6](../Technical_Report.md#66-the-spawn-tool-verb-do_spawn--populate_jail)) |
| `agent-write.png` | "write 'TODO 1\nTODO 2' into /plan.txt" → `WROTE 13 bytes` | WRITE 도구 + wire newline escape ([§5.2](../Technical_Report.md#52-wire-protocol--observation-capture)) |
| `agent-read-memory.png` | "re-read the file you just created and summarize it" → READ + 대화 메모리 | READ 도구 + `agent.py` conversation memory ([§5.3](../Technical_Report.md#53-conversation-memory)) |
| `agent-ps.png` | "show me the currently running processes" → `init [K] -5 / agentd [A] 8 / sh 10` | PS 자기관찰 + `[K]`/`[A]` 클래스 마커 ([§6.2](../Technical_Report.md#62-the-agentd-tool-table-whitelist--per-function-priority-f7f8)) |

## 추가 후보 (아직 없음)

End-to-end **비디오**는 Google Drive에 올라가 있다(위 링크). 아래 둘은
콘솔 캡처가 추가되면 좋은 항목:

| 파일 | 어떻게 |
| --- | --- |
| `priority-test.png` | `make qemu` → `priority_test` |
| `cfs-bench.png` | `make qemu CPUS=1` → `cfs_bench` |

## 녹화 → GIF

**터미널 녹화 (권장, 가벼움):** [asciinema](https://asciinema.org) + [agg](https://github.com/asciinema/agg)

```bash
asciinema rec demo.cast        # 위 시나리오 입력 후 exit
agg demo.cast docs/assets/agent-demo.gif --cols 90 --rows 22
```

**화면 녹화:** Peek(Linux) / ScreenToGif(Windows)로 터미널 영역만 캡처 → GIF/PNG 저장.

GIF는 가급적 **폭 ~760px, 10초 내외, 수 MB 이하**로 유지한다.
