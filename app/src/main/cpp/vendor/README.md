# vendor 目录

## stb_image.h

- 上游：https://github.com/nothings/stb（单文件 `stb_image.h`）
- 版本：v2.34（文件第 1 行版本标记）
- 来源：同项目的纯 C 界面参考仓库 `CUIDemo/app/src/main/cpp/third_party/stb_image.h`，原样复制，未做任何修改
- SHA-256：`dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020`
- 许可证：MIT 或 Public Domain（二选一），见同目录 `LICENSE`；本项目沿用其公开领域条款
- 用法：只由 `cover.c` 以 `STB_IMAGE_IMPLEMENTATION` 编译，并额外定义 `STBI_ONLY_JPEG` / `STBI_ONLY_PNG`，
  因此二进制里只有 JPEG/PNG 解码器，不含 BMP/TGA/GIF/HDR/PSD 等格式与 stdio 读写
- 上游单文件超过本项目自写代码 800 行约束，属于第三方原样文件，不拆分，便于核对来源和后续升级

## quickjs/

见 `quickjs/README.md` 与其 `LICENSE`。
