#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
revision=924694251d9e0f18e5d86bbd06aa3ab5f870002d
destination=third_party/qwen-asr
if [[ ! -e "$destination" ]]; then
  git clone --no-checkout https://github.com/antirez/qwen-asr.git "$destination"
  git -C "$destination" checkout --detach "$revision"
fi
actual=$(git -C "$destination" rev-parse HEAD)
if [[ "$actual" != "$revision" ]]; then
  echo "Existing runtime is not the pinned revision; refusing to overwrite it." >&2
  exit 1
fi
git -C "$destination" diff --exit-code
git -C "$destination" diff --cached --exit-code
printf 'Native runtime pinned at %s\n' "$revision"
