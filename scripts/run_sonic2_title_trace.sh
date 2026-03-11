#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
REPO_ROOT=${SCRIPT_DIR:h}
BUILD_DIR=${REPO_ROOT}/build

ROM_PATH="build/Sonic The Hedgehog 2 (World) (Rev A).md"
OUT_PATH="/tmp/sonic2_title_trace.log"
MAX_FRAMES=600
LOG_STATE_LOAD=1
LOG_BAD_SPLIT_TRACE=1
BAD_SPLIT_TRACE_LINE=119
LOG_SPLIT_GATES=1
LOG_SPLIT_STATE_WRITES=1
LOG_SPLIT_SETUP=0
LOG_SPLIT_WORK=0
LOG_VINT_WORK=0
LOG_HINT_HANDLER=0
LOG_M68K_DIVIDER=0
LOG_M68K_DIVIDER_REGS=0
LOG_HINT_TIMING=0
LOG_HELPER_STATE_WRITES=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --rom)
      ROM_PATH="$2"
      shift 2
      ;;
    --out)
      OUT_PATH="$2"
      shift 2
      ;;
    --frames)
      MAX_FRAMES="$2"
      shift 2
      ;;
    --log-state-load)
      LOG_STATE_LOAD="$2"
      shift 2
      ;;
    --log-bad-split)
      LOG_BAD_SPLIT_TRACE="$2"
      shift 2
      ;;
    --bad-line)
      BAD_SPLIT_TRACE_LINE="$2"
      shift 2
      ;;
    --log-split-gates)
      LOG_SPLIT_GATES="$2"
      shift 2
      ;;
    --log-split-state)
      LOG_SPLIT_STATE_WRITES="$2"
      shift 2
      ;;
    --log-split-setup)
      LOG_SPLIT_SETUP="$2"
      shift 2
      ;;
    --log-split-work)
      LOG_SPLIT_WORK="$2"
      shift 2
      ;;
    --log-vint-work)
      LOG_VINT_WORK="$2"
      shift 2
      ;;
    --log-hint-handler)
      LOG_HINT_HANDLER="$2"
      shift 2
      ;;
    --log-divider)
      LOG_M68K_DIVIDER="$2"
      shift 2
      ;;
    --log-divider-regs)
      LOG_M68K_DIVIDER_REGS="$2"
      shift 2
      ;;
    --log-hint)
      LOG_HINT_TIMING="$2"
      shift 2
      ;;
    --log-helper-state)
      LOG_HELPER_STATE_WRITES="$2"
      shift 2
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "$ROM_PATH" != /* ]]; then
  if [[ -e "${REPO_ROOT}/${ROM_PATH}" ]]; then
    ROM_PATH="${REPO_ROOT}/${ROM_PATH}"
  elif [[ -e "${BUILD_DIR}/${ROM_PATH}" ]]; then
    ROM_PATH="${BUILD_DIR}/${ROM_PATH}"
  fi
fi

cd "$BUILD_DIR"

GENESIS_LOG_STATE_LOAD="$LOG_STATE_LOAD" \
GENESIS_LOG_BAD_SPLIT_TRACE="$LOG_BAD_SPLIT_TRACE" \
GENESIS_LOG_BAD_SPLIT_TRACE_LINE="$BAD_SPLIT_TRACE_LINE" \
GENESIS_LOG_SPLIT_GATES="$LOG_SPLIT_GATES" \
GENESIS_LOG_SPLIT_STATE_WRITES="$LOG_SPLIT_STATE_WRITES" \
GENESIS_LOG_SPLIT_SETUP="$LOG_SPLIT_SETUP" \
GENESIS_LOG_SPLIT_WORK="$LOG_SPLIT_WORK" \
GENESIS_LOG_VINT_WORK="$LOG_VINT_WORK" \
GENESIS_LOG_HINT_HANDLER="$LOG_HINT_HANDLER" \
GENESIS_LOG_M68K_DIVIDER="$LOG_M68K_DIVIDER" \
GENESIS_LOG_M68K_DIVIDER_REGS="$LOG_M68K_DIVIDER_REGS" \
GENESIS_LOG_HINT_TIMING="$LOG_HINT_TIMING" \
GENESIS_LOG_HELPER_STATE_WRITES="$LOG_HELPER_STATE_WRITES" \
GENESIS_MAX_FRAMES="$MAX_FRAMES" \
./genesis "$ROM_PATH" > "$OUT_PATH" 2>&1

printf '%s\n' "$OUT_PATH"
