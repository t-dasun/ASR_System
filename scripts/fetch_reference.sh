#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
revision=7c6daf77a2421100f5fb066495372c00129d39ff
destination=third_party/qwen-reference
if [[ ! -e "$destination" ]]; then
  git clone --no-checkout https://github.com/QwenLM/Qwen3-ASR.git "$destination"
  git -C "$destination" checkout --detach "$revision"
fi
actual=$(git -C "$destination" rev-parse HEAD)
if [[ "$actual" != "$revision" ]]; then
  echo "Existing official reference is not the pinned revision; refusing to overwrite it." >&2
  exit 1
fi
git -C "$destination" diff --exit-code
git -C "$destination" diff --cached --exit-code
printf 'Official reference pinned at %s\n' "$revision"
