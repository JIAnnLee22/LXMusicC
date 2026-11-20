#!/usr/bin/env bash
# 开发期工具：传入 NotoSansSC-Regular.ttf（TrueType 曲线）；正常构建不依赖此工具。
set -euo pipefail
cd "$(dirname "$0")/.."
FONT="${1:?usage: scripts/generate-font.sh /path/to/NotoSansSC-Regular.ttf}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
python3 - "$TMP/codepoints" <<'PY'
import pathlib, re, sys
source = '\n'.join(p.read_text() for p in pathlib.Path('app/src/main').rglob('*')
                   if p.suffix in ('.c', '.kt') and p.name != 'font_data.h')
text = ''.join(re.findall(r'"(?:\\.|[^"\\])*"', source))
chars = set(range(32, 127)) | {ord(c) for c in text if ord(c) > 127}
# GB2312 的 6763 个汉字覆盖常见音源名称/描述，罕见字仍回退为 ?。
for high in range(0xb0, 0xf8):
    for low in range(0xa1, 0xff):
        try:
            chars.add(ord(bytes((high, low)).decode('gb2312')))
        except UnicodeDecodeError:
            pass
pathlib.Path(sys.argv[1]).write_text('\n'.join(map(str, sorted(chars))))
PY
"${CC:-cc}" -O2 scripts/font-atlas.c -lm -o "$TMP/font-atlas"
"$TMP/font-atlas" "$FONT" "$TMP/codepoints" app/src/main/cpp/font_data.h app/src/main/assets/fonts/ui.sdf
