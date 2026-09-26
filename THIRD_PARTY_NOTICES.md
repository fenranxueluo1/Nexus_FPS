# 第三方声明 / Third-Party Notices

本项目（Nexus FPS）**不随仓库分发任何第三方库的源码**。构建时由 CMake 从本机依赖目录
（默认 `D:/library`，可用 `-DDEPS_DIR=<路径>` 覆盖）引入，Vulkan SDK 则通过
`find_package(Vulkan)` 使用。各第三方组件遵循其原有许可，声明如下。

---

## 1. 改写来源：Khronos Group Vulkan 课程示例

- **名称**：How to write a Vulkan application in 2026（SIGGRAPH 2026 Vulkan 课程示例）
- **来源**：https://github.com/KhronosGroup/Vulkan-Tutorial
- **版权**：Copyright (c) 2026, Khronos Group and contributors
- **许可**：MIT License
- **涉及范围**：`src/main.h`、`src/main.cpp`、`src/util.h`、`src/util.cpp`、`shaders/*.slang`

`src/` 与 `shaders/` 中的代码是在上述课程示例基础上改写而来（改为面向 Windows / MinGW-w64
与 Linux / GCC 构建，并加入了延迟导入场景、ImGui 场景面板、鼠标右键视角、中文本地化等改动）。
依据 MIT 许可，原始版权声明保留在各源文件头部，作者仅对本人新增与改写的部分主张著作权。

> 注意：本仓库**不包含**课程原始文档（该文档采用 CC-BY-SA 4.0，与代码的 MIT 是两套不同的授权）。

---

## 2. 第三方库（不随仓库分发）

| 库 | 用途 | 许可 | 来源 |
| --- | --- | --- | --- |
| [GLFW](https://www.glfw.org/) | 窗口与输入 | Zlib/libpng | https://github.com/glfw/glfw |
| [vk-bootstrap](https://github.com/charles-lunarg/vk-bootstrap) | Vulkan 实例/设备/交换链引导 | MIT | https://github.com/charles-lunarg/vk-bootstrap |
| [Dear ImGui](https://github.com/ocornut/imgui)（docking 分支） | 调试界面 | MIT | https://github.com/ocornut/imgui |
| [glm](https://github.com/g-truc/glm) | 数学库 | MIT | https://github.com/g-truc/glm |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON（Slang 反射解析） | MIT | https://github.com/nlohmann/json |
| [stb](https://github.com/nothings/stb) | 图像解码（stb_image） | MIT / 公有领域 | https://github.com/nothings/stb |
| [tinygltf](https://github.com/syoyo/tinygltf) | glTF 解析 | MIT | https://github.com/syoyo/tinygltf |
| [Vulkan SDK](https://www.lunarg.com/vulkan-sdk/) | Vulkan 头文件/加载器、slangc 编译器 | Apache-2.0 / MIT 等 | https://vulkan.lunarg.com/ |

界面字体使用 **Noto Sans SC**（SIL Open Font License 1.1），由操作系统提供，不随仓库分发。

---

## 3. 场景资源

| 资源 | 许可 | 说明 |
| --- | --- | --- |
| Sponza（`assets/external/sponza/`） | Cryengine Limited License Agreement | **不可再分发**，仅限本地开发使用 |
| Vulkan Logo、camera（`assets/SponzaExtras/`） | Khronos glTF Sample Assets 自带说明 | 见其目录下的 `ABOUT.md` |
| modular-demo（`assets/modular-demo/`） | 原素材自带授权 | 仅限本地开发使用 |

Vulkan 与 Vulkan 标志是 Khronos Group Inc. 的注册商标。
