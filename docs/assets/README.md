# 데모 자산 (assets)

루트 [`README.md`](../../README.md)가 참조하는 데모 이미지가 여기 있다.

| 파일 | 쓰임 | 대체할 녹화본 |
| --- | --- | --- |
| `demo-cache.svg` | 자연어 → 캐시 히트 | `demo-cache.gif` |
| `demo-sandbox.svg` | confirm-escape + jail/nice 차단 | `demo-sandbox.gif` |

`.svg`는 **실제 실행 출력을 옮긴 정적 목업**이라 그대로 둬도 되지만, 발표/데모용으로는
아래 시나리오를 녹화해 `.gif`로 교체하면 더 생생하다. 교체 시 루트 README의
`docs/assets/demo-*.svg`를 `docs/assets/demo-*.gif`로 바꾸기만 하면 된다.

## 각 클립이 담을 내용 (실측 시나리오)

**demo-cache** — `make qemu-agent` + `python3 agent.py` 후:
```
11 + 22가 얼마인지 알려줘
11 + 22가 얼마인지 알려줘      ← 2번째에 [cache HIT] 표시 (Solar 호출 없음)
```

**demo-sandbox** — 같은 세션에서:
```
echo hello 출력하는 프로세스를 만들어줘    ← [jail] ... 허용? (y/N) 에 y
/etc/passwd 파일 내용을 읽어줘             ← READ ... not reachable inside jail
pid 1 프로세스 우선순위를 19로 낮춰줘      ← NICE: denied
```

## 녹화 → GIF

**터미널 녹화 (권장, 가벼움):** [asciinema](https://asciinema.org) + [agg](https://github.com/asciinema/agg)
```bash
asciinema rec demo.cast        # 위 시나리오 입력 후 exit
agg demo.cast docs/assets/demo-cache.gif --cols 90 --rows 22
```

**화면 녹화:** Peek(Linux) / ScreenToGif(Windows)로 터미널 영역만 캡처 → GIF 저장.

GIF는 가급적 **폭 ~760px, 10초 내외, 수 MB 이하**로 유지한다.
</content>
