#!/usr/bin/env bash
# Fetch pre-converted ggml HTDemucs weights (see sevagh/demucs.cpp README).
# Usage: download_model.sh [dest_dir] [--ft]
#   --ft  also fetch the fine-tuned 4-model ensemble (~320 MB, 4x inference
#         time, best quality; load any one of the ft files from the plugin)
set -euo pipefail
DEST="${1:-$HOME/tenganisha-models}"
mkdir -p "$DEST"
BASE="https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main"

echo "Downloading 4-source HTDemucs (recommended default)..."
curl -L -o "$DEST/ggml-model-htdemucs-4s-f16.bin" \
  "$BASE/ggml-model-htdemucs-4s-f16.bin"

if [[ "${2:-}" == "--ft" || "${1:-}" == "--ft" ]]; then
  for stem in drums bass other vocals; do
    echo "Downloading fine-tuned $stem model..."
    curl -L -o "$DEST/ggml-model-htdemucs_ft_${stem}-4s-f16.bin" \
      "$BASE/ggml-model-htdemucs_ft_${stem}-4s-f16.bin"
  done
fi

echo "Done. Load $DEST/ggml-model-htdemucs-4s-f16.bin from the plugin UI."
