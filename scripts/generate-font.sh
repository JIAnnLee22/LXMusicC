#!/usr/bin/env bash
# 开发期工具：传入 NotoSansSC-Regular.ttf（TrueType 曲线）；正常构建不依赖此工具。
set -euo pipefail
cd "$(dirname "$0")/.."
FONT="${1:?usage: scripts/generate-font.sh /path/to/NotoSansSC-Regular.ttf}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP/codepoints" <<'PY'
import pathlib, re, sys
source = pathlib.Path('app/src/main/cpp/ui.c').read_text()
text = ''.join(re.findall(r'"(?:\\.|[^"\\])*"', source))
chars = set(range(32, 127)) | {ord(c) for c in text if ord(c) > 127}
pathlib.Path(sys.argv[1]).write_text('\n'.join(map(str, sorted(chars))))
PY
"${CC:-cc}" -O2 scripts/font-atlas.c -lm -o "$TMP/font-atlas"
"$TMP/font-atlas" "$FONT" "$TMP/codepoints" app/src/main/cpp/font_data.h app/src/main/assets/fonts/ui.sdf
