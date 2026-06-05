# 데모 미디어 (assets)

루트 [`README.md`](../../README.md) §5.3 / [`README.ko.md`](../../README.ko.md) §5.3의
데모 자리표시가 가리키는 폴더다. 아래 파일명으로 캡처/GIF를 넣으면 README 링크가 렌더된다.

| 파일 | 무엇을 담나 | 어떻게 |
| --- | --- | --- |
| `agent-demo.gif` | 에이전트 REPL 멀티스텝 (계획 → 도구 → 관찰 → 답변) | `make qemu-agent` + `python3 agent.py` 세션 녹화 |
| `cache-hit.png` | 같은 질문 반복 시 `[cache HIT]` | 에이전트 모드에서 동일 질문 2회 |
| `priority-test.png` | `priority_test` 전부 PASSED | `make qemu` → `priority_test` |
| `cfs-bench.png` | `cfs_bench` 우선순위별 CPU 점유 표 | `make qemu CPUS=1` → `cfs_bench` |

## 녹화 → GIF

**터미널 녹화 (권장, 가벼움):** [asciinema](https://asciinema.org) + [agg](https://github.com/asciinema/agg)

```bash
asciinema rec demo.cast        # 위 시나리오 입력 후 exit
agg demo.cast docs/assets/agent-demo.gif --cols 90 --rows 22
```

**화면 녹화:** Peek(Linux) / ScreenToGif(Windows)로 터미널 영역만 캡처 → GIF/PNG 저장.

GIF는 가급적 **폭 ~760px, 10초 내외, 수 MB 이하**로 유지한다.
</content>
