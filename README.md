# Nexus_FPS

一个用**现代 Vulkan** 写法实现的实时三维场景渲染程序。加载并显示 glTF 2.0 场景，带一个可停靠的 Dear ImGui 调试界面。

场景**不在启动时加载**：程序启动后视口为空，只有在界面里点击「导入」之后才真正读取并上传场景资源。

---

## 主要特性

- **三维场景渲染**：实时绘制 glTF 2.0 场景，支持多材质、法线贴图与 mipmap。
- **场景延迟导入**：启动时只建立渲染路径必需的占位资源；点击「导入」后才解析并上传场景。导入失败时当前场景不受影响，界面会给出中文原因。
- **预设场景**：内置若干预设，其中一个由多个 glTF 文件组合而成（场景模型 + 相机文件）。
- **自由相机**：按住鼠标右键拖动转视角，WASD 移动，QE 升降。
- **显示模式**：可在实体模式（填充分边形）与网格模式（线框）之间切换。
- **调试界面**：帧耗时与帧率、交换链尺寸、绘制次数、相机参数、场景统计与导入状态。
- **中文本地化**：界面文字与控制台日志均为中文。

### 使用的 Vulkan 特性

这个项目刻意跳过了很多老教程里的写法（着色器对象等扩展在启动时会作为设备筛选条件，缺哪个会明确提示）：

| 做什么 | 传统写法 | 本项目 |
| --- | --- | --- |
| 开始一次渲染 | `VkRenderPass` + `VkFramebuffer` | **动态渲染** `vkCmdBeginRendering`（Vulkan 1.3 核心） |
| 描述管线状态 | `VkGraphicsPipeline` | **着色器对象** `VK_EXT_shader_object` |
| 设置图形状态 | 管线内固定状态 | **扩展动态状态 3** + 顶点输入动态状态 |
| 给着色器传资源 | `VkDescriptorSet` + 布局 + 池 | **描述符堆** `VK_EXT_descriptor_heap` |
| 给着色器传小数据 | `vkCmdPushConstants` | **推送数据** `vkCmdPushDataEXT` |
| 着色器语言 | GLSL / HLSL | **Slang**（反射数据驱动顶点布局与描述符映射） |

同步统一使用 synchronization2（`vkCmdPipelineBarrier2` / `vkQueueSubmit2`）。实例、设备与交换链的创建由 vk-bootstrap 承担。

---

## 环境要求

### 硬件最低要求

| 项目 | 最低要求 |
| --- | --- |
| 处理器 | Intel® Core™ i3-4130 or Core™ i5-3470 or AMD FX™-6100 |
| 内存 | 8 GB RAM |
| 显卡 | NVIDIA® GeForce® GT 1030 (DDR4) or AMD Radeon™ RX 550 |
| 显存 | 4 GB RAM |
| 显示 | 支持 Vulkan 的显示输出，窗口默认 1280 × 720 |

> **显卡驱动必须支持 `VK_EXT_shader_object` 与 `VK_EXT_descriptor_heap`。** 这两项比较新，较老的显卡或驱动可能不支持——程序启动时会打印每张显卡缺什么。实例要求 Vulkan **1.4**。

### 软件环境

| 项目 | 要求 |
| --- | --- |
| 操作系统 | Windows 10 / 11（64 位）；也可在 Linux 上构建运行 |
| 编译工具链 | MinGW-w64 GCC（C++23）或 Linux GCC |
| 构建系统 | CMake 3.26 及以上 |
| Vulkan SDK | 1.4.33x 及以上，提供 Vulkan 头文件、加载器以及 Slang 编译器 `slangc` |
| 第三方库 | 见下方「第三方依赖」，不需要随仓库分发 |

---

## 构建

### Windows（MinGW-w64）

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --parallel
build\bin\Nexus_FPS.exe
```

### Linux

```bash
cmake -S . -B build
cmake --build build --parallel
./build/bin/Nexus_FPS
```

着色器（`shaders/*.slang`）会在构建时由 `slangc` 自动编译成 SPIR-V 并输出反射 JSON，结果放在 `build/shaders/`；源码没改就不会重编。

### 构建选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `DEPS_DIR` | `D:/library` | 第三方库根目录，缺库时 CMake 会给出明确提示 |
| `NEXUS_FPS_UI_FONT` | 自动查找 | 界面字体文件；未指定时依次查找 `DEPS_DIR/Noto_Sans_SC/`、系统已安装的 Noto Sans SC |

例如：

```powershell
cmake -S . -B build -G "MinGW Makefiles" -DDEPS_DIR=E:/libs -DNEXUS_FPS_UI_FONT=E:/fonts/NotoSansSC-Regular.otf
```

> 界面文案是中文，因此字体**必须包含中文字形**（Open Sans 之类只有拉丁字形的字体不行，中文会显示成方框）。

---

## 运行

```
build\bin\Nexus_FPS.exe [--import <glTF>] [--frame-limit N]
```

| 参数 | 说明 |
| --- | --- |
| （无参数） | 正常启动，视口为空，等待在界面中导入场景 |
| `--import <glTF>` | 启动后立即导入指定场景；相对路径按 `assets/` 解析 |
| `--frame-limit N` | 渲染 N 帧后自动退出，`0` 表示不限制（用于自动化测试） |

### 操作

| 操作 | 效果 |
| --- | --- |
| 按住鼠标右键拖动 | 转动视角 |
| W / S | 前进 / 后退 |
| A / D | 左移 / 右移 |
| Q / E | 下降 / 上升 |
| Esc | 退出程序 |

在面板上拖动滑块或操作列表时不会同时转动视角——界面优先占用鼠标，只有在视口区域内按住右键才控制相机。

---

## 导入场景

启动后视口为空，在左侧「场景」面板中：

1. 直接点击任一**预设场景**按钮；或
2. 在「自定义单个文件」中填写 glTF 路径，或从下方扫描出的文件列表中选一个，再点击「导入」。

导入完成后状态区会显示网格 / 绘制 / 贴图数量。再次导入会自动释放旧场景。

### 预设场景

| 预设 | 组成文件 |
| --- | --- |
| `modular-demo` | `assets/modular-demo/modular-demo.gltf` |
| `Sponza（含相机）` | Sponza + `assets/SponzaExtras/camera.gltf` |
| `Sponza + Vulkan Logo` | Sponza + VulkanLogo + camera.gltf |
| `Vulkan Logo` | `assets/SponzaExtras/VulkanLogo/VulkanLogo.gltf` |

预设会把多个 glTF 文件合并成一个场景：Sponza 自身不含相机，由 `camera.gltf` 提供机位，因此不需要手动调整视角。

---

## 目录结构

```
src/            渲染器主体（main.h / main.cpp / util.h / util.cpp，共约 4400 行）
shaders/        Slang 着色器源码，构建时编译为 SPIR-V
assets/         场景资源
  modular-demo/   示例场景
  SponzaExtras/   Vulkan Logo 与相机文件
  external/       Sponza 场景（见下方许可说明）
软著材料/       软件著作权登记用的源程序清单与使用说明书
build/          构建产物（不纳入版本控制）
```

---

## 第三方依赖

**不随仓库分发**，构建时由 CMake 从 `DEPS_DIR`（默认 `D:/library`）引入；Vulkan SDK 通过 `find_package(Vulkan)` 使用。

| 库 | 用途 | 许可 |
| --- | --- | --- |
| GLFW 3.5.1 | 窗口与输入 | Zlib/libpng |
| vk-bootstrap 1.4.362 | Vulkan 实例/设备/交换链引导 | MIT |
| Dear ImGui 1.93.0 WIP（docking） | 调试界面 | MIT |
| glm 1.0.3 | 数学库 | MIT |
| nlohmann/json 3.12.0 | 解析 Slang 反射 JSON | MIT |
| stb（stb_image） | 图像解码 | MIT / 公有领域 |
| tinygltf 3.0.1 | glTF 解析 | MIT |

### 场景资源的许可

- `assets/SponzaExtras/` 来自 Khronos glTF Sample Assets，见其目录下的 `ABOUT.md`。
- `assets/modular-demo/` 为示例场景。
- `assets/external/sponza/`（约 50 MB）采用 **Cryengine Limited License Agreement，不可再分发**，仅限本地开发使用。

---

## 许可

代码以 **MIT** 许可发布，见 [LICENSE](LICENSE)。

本项目改写自 Khronos Group 的课程示例 *How to write a Vulkan application in 2026*（原始代码版权归 Khronos Group and contributors 所有，同样为 MIT），依据 MIT 的要求保留了原始声明。完整的第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

---

## 已知限制

- 窗口为固定尺寸（1280 × 720），未实现窗口缩放。
- 线框模式依赖设备的 `fillModeNonSolid` 特性；不支持时该选项在界面上置灰。
- 场景导入为单场景模式，再次导入会替换当前场景。
