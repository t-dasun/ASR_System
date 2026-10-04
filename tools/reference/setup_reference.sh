#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v uv >/dev/null 2>&1; then
  echo 'uv is required for the isolated official CPU reference environment.' >&2
  exit 1
fi
bash scripts/fetch_reference.sh
export UV_CACHE_DIR="$PWD/.cache/uv"
if [[ ! -e .venv-reference/bin/python ]]; then
  uv venv --python 3.12 .venv-reference
fi
uv pip install --python .venv-reference/bin/python --index https://download.pytorch.org/whl/cpu \
  --constraint tools/reference/constraints-linux-py312.txt \
  'torch==2.14.1+cpu' 'torchaudio==2.11.0+cpu' 'torchvision==0.29.1+cpu'
uv pip install --python .venv-reference/bin/python \
  --constraint tools/reference/constraints-linux-py312.txt \
  --editable third_party/qwen-reference \
  'jiwer==4.0.0' 'pyarrow==25.0.1' 'soundfile==0.14.0'
.venv-reference/bin/python -c 'import torch, qwen_asr, pyarrow, soundfile, jiwer; assert torch.version.cuda is None; print("Reference CPU environment ready:", torch.__version__)'
