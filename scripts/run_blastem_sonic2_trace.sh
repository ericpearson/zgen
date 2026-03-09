#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
REPO_ROOT=${SCRIPT_DIR:h}
BLASTEM_DIR="${REPO_ROOT}/third_party/blastem-src/blastem-884de5ef1263"

ROM_PATH="${REPO_ROOT}/build/Sonic The Hedgehog 2 (World) (Rev A).md"
OUT_PATH="/tmp/blastem_sonic2_trace.log"
FRAMES=4000

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
      FRAMES="$2"
      shift 2
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "$ROM_PATH" != /* ]]; then
  ROM_PATH="${REPO_ROOT}/${ROM_PATH}"
fi

cd "$BLASTEM_DIR"

BLASTEM_SONIC2_TRACE=1 \
./blastem -b "$FRAMES" "$ROM_PATH" > "$OUT_PATH" 2>&1

printf '%s\n' "$OUT_PATH"
