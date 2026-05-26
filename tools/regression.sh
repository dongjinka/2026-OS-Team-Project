#!/usr/bin/env bash
# tools/regression.sh — xv6 기능 9종 단일 회귀 진입점.
#
# 사용:
#   ./tools/regression.sh                    # 기본 (~4분)
#   ./tools/regression.sh --fast             # eval 인자 축소 (~1분)
#   ./tools/regression.sh --ci               # 한 줄 요약 (grep 친화)
#   ./tools/regression.sh --only=cache_test  # 특정 테스트만
#
# 종료 코드: 0 = 9개 모두 PASS, 1 = 하나라도 FAIL/예외, 2 = 의존성 부족.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

for bin in python3 qemu-system-riscv64 make; do
  if ! command -v "$bin" >/dev/null 2>&1; then
    echo "ERROR: '$bin' 가 PATH 에 없음 — 회귀를 실행할 수 없습니다." >&2
    exit 2
  fi
done

cd "$PROJECT_ROOT"
exec python3 "$SCRIPT_DIR/regression.py" "$@"
