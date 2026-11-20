# UI SDF 图集

`ui.sdf` 是 Noto Sans SC Regular 的栅格 SDF 图集，不是完整字体；与 `../../cpp/font_data.h` 一同生成。
来源为 Noto CJK 2.004，Copyright 2014–2021 Adobe，Reserved Font Name “Source”：
https://github.com/notofonts/noto-cjk/blob/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf

字体许可为 SIL Open Font License 1.1，完整副本见本目录 `OFL.txt`。本目录的字形图像衍生自该字体。

## 复现

需开发机提供 C 编译器、Python 3，实例化步骤额外需要 fontTools（正常 Android 构建不需要这些依赖）：

```sh
curl -fL -o /tmp/NotoSansSC-VF.ttf https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf
python3 -m fontTools.varLib.instancer /tmp/NotoSansSC-VF.ttf wght=400 -o /tmp/NotoSansSC-Regular.ttf
./scripts/generate-font.sh /tmp/NotoSansSC-Regular.ttf
```

- 下载的 VF 字体 SHA-256：`d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964`
- 图集：1024×1024 单通道 R8，em=40px，padding=5，边缘值 128，distance scale=24。
- 生成器提取 `ui.c` 的字符串字面量码点并加入 ASCII 32–126，按码点排序；修改文案后必须重跑，缺字/溢出会报错。
- SDF 仅支持 TrueType 二次曲线，不要传入 CFF OTF；C 编译器可通过 `CC` 指定。
