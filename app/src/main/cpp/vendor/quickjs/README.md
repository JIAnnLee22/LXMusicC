# QuickJS 2025-09-13

- 上游：https://bellard.org/quickjs/
- 原始归档：https://bellard.org/quickjs/quickjs-2025-09-13-2.tar.xz
- 归档 SHA-256：`996c6b5018fc955ad4d06426d0e9cb713685a00c825aa5c0418bd53f7df8b0b4`
- 许可证：MIT，见 `LICENSE`。
- 仅复制引擎、正则、Unicode、浮点转换及其头文件，源码未经修改；不编译 CLI、quickjs-libc、测试或模块加载器。
- 上游单文件/自动生成 Unicode 表保留原样，因此不适用本项目自写代码 ≤800 行约束；不拆分第三方文件，以便核对来源和后续升级。
- CMake 固定 `CONFIG_VERSION="2025-09-13"`，为两个 Android ABI 静态编译，无 Gradle 原生 SDK 依赖。
