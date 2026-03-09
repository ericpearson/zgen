#!/usr/bin/env bash
# Copyright (C) 2026 pagefault
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/cut_public_release.sh [options]

Create a sanitized one-commit export of the current repo and optionally push it
to the public mirror.

Options:
  --remote URL        Remote URL to push to.
                      Default: git@github.com:ericpearson/zgen.git
  --branch NAME       Remote branch name. Default: main
  --message TEXT      (required) Commit message for the public release commit.
  --output-dir DIR    Export into DIR instead of a temp directory.
                      DIR must not already contain files.
  --dry-run           Build the sanitized export but do not push it.
  --allow-dirty       Allow running from a dirty private repo checkout.
  -h, --help          Show this help.

Notes:
  - By default the script exports committed HEAD only. Uncommitted changes are
    rejected unless --allow-dirty is passed.
  - Paths removed from the public export are listed in .public-release.exclude.
EOF
}

REMOTE_URL="git@github.com:ericpearson/zgen.git"
BRANCH="main"
COMMIT_MESSAGE=""
OUTPUT_DIR=""
DRY_RUN=0
ALLOW_DIRTY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --remote)
            REMOTE_URL="${2:?missing value for --remote}"
            shift 2
            ;;
        --branch)
            BRANCH="${2:?missing value for --branch}"
            shift 2
            ;;
        --message)
            COMMIT_MESSAGE="${2:?missing value for --message}"
            shift 2
            ;;
        --output-dir)
            OUTPUT_DIR="${2:?missing value for --output-dir}"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --allow-dirty)
            ALLOW_DIRTY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
EXCLUDE_FILE="${REPO_ROOT}/.public-release.exclude"

cd "${REPO_ROOT}"

if [[ "${ALLOW_DIRTY}" -ne 1 ]]; then
    if ! git diff --quiet --ignore-submodules -- || ! git diff --cached --quiet --ignore-submodules --; then
        echo "Refusing to cut a public release from a dirty worktree." >&2
        echo "Commit or stash changes first, or rerun with --allow-dirty." >&2
        exit 1
    fi
fi

if [[ -z "${COMMIT_MESSAGE}" ]]; then
    echo "Error: --message is required." >&2
    usage >&2
    exit 1
fi

SOURCE_REV="$(git rev-parse --short HEAD)"

if [[ -z "${OUTPUT_DIR}" ]]; then
    OUTPUT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/zgen-public.XXXXXX")"
else
    mkdir -p "${OUTPUT_DIR}"
    if [[ -n "$(find "${OUTPUT_DIR}" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
        echo "Output directory is not empty: ${OUTPUT_DIR}" >&2
        exit 1
    fi
fi

git archive --format=tar HEAD | tar -xf - -C "${OUTPUT_DIR}"

if [[ -f "${EXCLUDE_FILE}" ]]; then
    while IFS= read -r path || [[ -n "${path}" ]]; do
        [[ -z "${path}" || "${path}" == \#* ]] && continue
        rm -rf "${OUTPUT_DIR}/${path}"
    done < "${EXCLUDE_FILE}"
fi

git -C "${OUTPUT_DIR}" init -b "${BRANCH}" >/dev/null

AUTHOR_NAME="$(git config --get user.name || true)"
AUTHOR_EMAIL="$(git config --get user.email || true)"
if [[ -n "${AUTHOR_NAME}" ]]; then
    git -C "${OUTPUT_DIR}" config user.name "${AUTHOR_NAME}"
fi
if [[ -n "${AUTHOR_EMAIL}" ]]; then
    git -C "${OUTPUT_DIR}" config user.email "${AUTHOR_EMAIL}"
fi

git -C "${OUTPUT_DIR}" add -A
git -C "${OUTPUT_DIR}" commit -m "${COMMIT_MESSAGE}" >/dev/null

echo "Created public release export:"
echo "  source repo: ${REPO_ROOT}"
echo "  source rev : ${SOURCE_REV}"
echo "  output dir : ${OUTPUT_DIR}"
echo "  branch     : ${BRANCH}"
echo "  remote     : ${REMOTE_URL}"

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "Dry run complete; nothing was pushed."
    exit 0
fi

git -C "${OUTPUT_DIR}" remote add origin "${REMOTE_URL}"
git -C "${OUTPUT_DIR}" push --force origin "${BRANCH}"

echo "Public release pushed successfully."
