# X3FilmEngine

X3Lumina 内置的胶片引擎宿主 **`spektralab-host`** 的完整对应源码。

This repository is the source of the `spektralab-host` binary that ships inside
X3Lumina (`X3Lumina.app/Contents/Resources/SpektraLab/spektralab-host`), so that
the GPL-3.0-or-later obligation to provide the corresponding source is satisfied
by publishing it rather than by a written offer alone.

- 引擎的实现来自 **SpektraLab**（作者 Hanze Qiu 等），本仓库是它用于构建
  X3Lumina 随包二进制的那棵源码树；产品说明见
  [README.upstream.md](README.upstream.md) / [README.upstream.zh-CN.md](README.upstream.zh-CN.md)。
- `engine/` 的代码相对上游**未做修改**；本树额外带的是产品自有数据
  `engine/resources_product/`（CineStill 800T、四支黑白片、Ilford Multigrade
  IV RC 及其 paper grades / 滤色片表）与它的来源说明 `NOTICE.md`。

## 目录

| 路径 | 内容 |
|---|---|
| `engine/src/` | 引擎本体（C++20：光谱、胶片/相纸模型、颗粒、光晕、印放）与 `shaders/*.metal` |
| `engine/include/`、`engine/host/`、`engine/host/CMakeLists.txt` | 对外 C 接口与宿主进程（读图 → 渲染 → 回传，命令行协议见 `host/main.cpp`） |
| `engine/resources/profiles/`、`print_luts.json`、`neutral_print_filters.json` | spektrafilm 的片基/相纸配置与打印 LUT（CC BY-SA 4.0，未修改） |
| `engine/resources/spektrafilm_constants.bin` | 光谱/密度常量表 |
| `engine/resources_product/` | 产品自有片单与相纸数据（见其 `NOTICE.md`） |
| `engine/tests/`、`engine/tools/` | 对拍/校验脚本（`host_smoke.py`、`bw_checks.py` 等） |
| `license/LICENSE` | GPL-3.0 全文 |
| `license/SPEKTRAFILM_LICENSE.txt` | 胶片/相纸配置的 CC BY-SA 4.0 许可与归属要求 |

`build/`、`engine/build/` 是构建产物（依赖、目标文件、静态库、宿主二进制），
不入库。

## 构建

macOS（Metal）：

```bash
engine/setup-libraw.sh     # LibRaw 0.22.2，pinned，落 build/deps
engine/build.sh            # 引擎静态库 + spektrafilm.metallib + 测试驱动
```

`engine/build.sh` 里有两条不能去掉的编译参数（`-fmetal-math-mode=safe`、
`-fmetal-math-fp32-functions=precise`），原因见脚本头部注释。

宿主进程 `spektralab-host` 的源文件与链接的库列在 `engine/host/CMakeLists.txt`
（`main.cpp`/`image_io.cpp`/`output.cpp`/`platform.cpp`/`stb_impl.cpp`/
`tiff_ifd.cpp`/`white_balance.cpp`，链 `spektralab_engine`、`spektralab_core`、
`spk_image_io`、`spk_libraw`）。

Linux / Windows 的构建路径（Vulkan 后端）见 `engine/LINUX_WINDOWS.md`、
`engine/WINDOWS.md`，脚本为 `engine/build-linux.sh`、`engine/build-windows.ps1`、
`engine/build-windows-cross.sh`；这些路径与 macOS 的 `build.sh` 互不影响。

## 许可

- 根目录 [`COPYING`](COPYING) 是 GNU GPL v3 全文（仓库整体标注为
  GPL-3.0；`license/LICENSE` 是同一份）。仓库内不同部分许可不同，
  以本节为准。
- **引擎代码**：GNU GPL v3 或更新版本。
  以**独立子进程**被 X3Lumina 调用（通过命令行/管道交换像素与参数），
  X3Lumina 不与其链接。
- **`engine/resources/profiles/` 与打印 LUT**：CC BY-SA 4.0，作者
  Andrea Volpato（<https://github.com/andreavolpato/spektrafilm>）；
  本仓库分发的是未修改副本，归属要求见
  [`license/SPEKTRAFILM_LICENSE.txt`](license/SPEKTRAFILM_LICENSE.txt)。
- **`engine/resources_product/`**：产品自有数据，由厂商公开数据表拟合，
  说明见 `engine/resources_product/NOTICE.md`。
- **第三方库**：LibRaw 0.22.2（LGPL-2.1，静态链接）、stb（`engine/third_party/stb/LICENSE`，
  public domain 或 MIT，本树取 MIT）、volk（MIT，`engine/third_party/volk/LICENSE.md`）、
  metal-cpp（Apache-2.0）、Vulkan-Headers（Apache-2.0 OR MIT，仅 Linux/Windows 构建），
  汇总见 `engine/third_party/README.md`。
