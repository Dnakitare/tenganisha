#!/usr/bin/env bash
# Fetch pre-converted ggml HTDemucs weights (see sevagh/demucs.cpp README).
set -euo pipefail
DEST="${1:-$HOME/tenganisha-models}"
mkdir -p "$DEST"
BASE="https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main"

echo "Downloading 4-source HTDemucs (recommended default)..."
curl -L -o "$DEST/ggml-model-htdemucs-4s-f16.bin" \
  "$BASE/ggml-model-htdemucs-4s-f16.bin"

echo "Done. Load $DEST/ggml-model-htdemucs-4s-f16.bin from the plugin UI."
