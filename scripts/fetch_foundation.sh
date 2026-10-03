#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
fetch() {
  local name="$1" url="$2" revision="$3"
  local destination="third_party/$name"
  if [[ ! -e "$destination" ]]; then
    git clone --no-checkout "$url" "$destination"
    git -C "$destination" checkout --detach "$revision"
  fi
  if [[ "$(git -C "$destination" rev-parse HEAD)" != "$revision" ]]; then
    echo "Existing $name is not at the pinned revision; refusing to overwrite." >&2
    exit 1
  fi
  git -C "$destination" diff --exit-code
  git -C "$destination" diff --cached --exit-code
}
fetch yaml-cpp https://github.com/jbeder/yaml-cpp.git f7320141120f720aecc4c32be25586e7da9eb978
fetch json https://github.com/nlohmann/json.git 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
