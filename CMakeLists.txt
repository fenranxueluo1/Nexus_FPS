# Nexus_FPS
#
# 用现代 Vulkan (2026 写法) 重写的渲染器: 动态渲染 + 着色器对象 + 描述符堆 +
# Slang 着色器 + glTF 场景加载, 带 ImGui 调试界面。场景不再在启动时加载,
# 而是在 GUI 里点击 "导入" 之后才加载。
#
# 参考实现: D:/vscode_Project/NeuxsRenderer (Khronos SIGGRAPH 2026 课程示例的移植)。
#
# 依赖:
#   * Vulkan SDK 由 find_package(Vulkan) 提供 (vulkan-hpp / vk-bootstrap 用的头 / slangc)
#   * 其余第三方库全部来自 DEPS_DIR (默认 D:/library), 不随本工程分发
#
# 构建 (Windows / MinGW-w64):
#       cmake -S . -B build -G "MinGW Makefiles"
#       cmake --build build --parallel
#       build\bin\Nexus_FPS.exe
#
# 构建 (Linux / GCC):
#       cmake -S . -B build
#       cmake --build build --parallel

cmake_minimum_required(VERSION 3.26)

project(Nexus_FPS LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 未指定时默认 Debug。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# 可执行文件统一输出到 build/bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# 第三方库根目录。换机器时用 -DDEPS_DIR=<路径> 覆盖即可。
set(DEPS_DIR "D:/library" CACHE PATH "第三方库根目录 (glfw / vk-bootstrap / imgui / glm / json / stb / tinygltf)")

# ---------------------------------------------------------------------------
# ImGui 界面字体
# ---------------------------------------------------------------------------
# 界面文案是中文, 所以字体必须有中文字形 (Open Sans 之类只有拉丁字形的字体不行)。
# 查找顺序: -DNEXUS_FPS_UI_FONT 显式指定 -> DEPS_DIR 下的 Noto Sans SC -> 系统已安装的
# Noto Sans SC。想固定用某一份, 传 -DNEXUS_FPS_UI_FONT=<路径> 即可。
set(NEXUS_FPS_UI_FONT "" CACHE FILEPATH "ImGui 界面字体文件 (留空则自动查找)")

if(NOT NEXUS_FPS_UI_FONT)
    foreach(_font_candidate
            "${DEPS_DIR}/Noto_Sans_SC/NotoSansSC-VF.ttf"
            "${DEPS_DIR}/Noto_Sans_SC/NotoSansSC-Regular.otf"
            "${DEPS_DIR}/NotoSansSC-VF.ttf"
            "C:/Windows/Fonts/NotoSansSC-VF.ttf")
        if(EXISTS "${_font_candidate}")
            set(NEXUS_FPS_UI_FONT "${_font_candidate}")
            break()
        endif()
    endforeach()
endif()

if(NOT EXISTS "${NEXUS_FPS_UI_FONT}")
    message(WARNING
        "找不到带中文字形的界面字体, 运行时会退回 ImGui 内置字体, 中文会显示成方框。\n"
        "请下载 Noto Sans SC 放到 ${DEPS_DIR}/Noto_Sans_SC/ 下, "
        "或用 -DNEXUS_FPS_UI_FONT=<路径> 指定字体文件。")
else()
    message(STATUS "界面字体: ${NEXUS_FPS_UI_FONT}")
endif()

# ---------------------------------------------------------------------------
# 第三方库路径检查: 缺库时给出明确提示, 而不是让 CMake 报一堆找不到文件的错。
# ---------------------------------------------------------------------------
foreach(required_dir
        "glfw-3.5.1"
        "vk-bootstrap-1.4.362"
        "imgui-docking"
        "glm"
        "json-3.12.0"
        "stb-master"
        "tinygltf-3.0.1")
    if(NOT EXISTS "${DEPS_DIR}/${required_dir}")
        message(FATAL_ERROR
            "缺少第三方库: ${DEPS_DIR}/${required_dir}\n"
            "请把它放到 ${DEPS_DIR} 下, 或用 -DDEPS_DIR=<路径> 指定第三方库根目录。")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Vulkan SDK (package 引入)
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)

# ---------------------------------------------------------------------------
# 第三方库: GLFW
# ---------------------------------------------------------------------------
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
# DEPS_DIR 在工程目录之外, 所以要显式给出 binary dir。
add_subdirectory("${DEPS_DIR}/glfw-3.5.1" "${CMAKE_BINARY_DIR}/deps/glfw" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# 第三方库: vk-bootstrap
# ---------------------------------------------------------------------------
set(VK_BOOTSTRAP_TEST             OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_WERROR           OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL          OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_DISABLE_WARNINGS ON  CACHE BOOL "" FORCE)
add_subdirectory("${DEPS_DIR}/vk-bootstrap-1.4.362" "${CMAKE_BINARY_DIR}/deps/vk-bootstrap" EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# 第三方库: Dear ImGui (docking 分支, 1.93.0 WIP)
# ---------------------------------------------------------------------------
# 只保留核心源码 + 两个后端: GLFW 平台后端, Vulkan 渲染后端。
set(IMGUI_DIR "${DEPS_DIR}/imgui-docking" CACHE PATH "Dear ImGui (docking) 源码目录")
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp")
set_target_properties(imgui PROPERTIES CXX_STANDARD 17)
# SYSTEM: 不显示第三方库的头文件警告。
target_include_directories(imgui SYSTEM PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends")
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

# ---------------------------------------------------------------------------
# 第三方库: 纯头文件 (GLM / nlohmann json / stb / tinygltf)
# ---------------------------------------------------------------------------
# tinygltf 用的是 v2 (单头文件 tiny_gltf.h), 它在 tinygltf-3.0.1/attic 下。
# 通过 TINYGLTF_NO_INCLUDE_JSON 让它复用 nlohmann/json, 而不是自带的 json.hpp。
add_library(nexus_fps_headers INTERFACE)
target_include_directories(nexus_fps_headers SYSTEM INTERFACE
    "${DEPS_DIR}/glm"
    "${DEPS_DIR}/json-3.12.0/single_include"
    "${DEPS_DIR}/stb-master"
    "${DEPS_DIR}/tinygltf-3.0.1/attic")

# ---------------------------------------------------------------------------
# Slang 着色器 -> SPIR-V
# ---------------------------------------------------------------------------
find_program(SLANGC_EXECUTABLE
    NAMES slangc slangc.exe
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")
if(NOT SLANGC_EXECUTABLE)
    message(FATAL_ERROR
        "找不到 slangc。请安装 Vulkan SDK (1.4.33x+ 自带), "
        "或用 -DSLANGC_EXECUTABLE=<路径> 指定。")
endif()

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_OUT "${CMAKE_BINARY_DIR}/shaders")

# add_slang_shader(<target> <源文件> <入口函数> <stage> <输出 spv>)
function(add_slang_shader target source entry_point stage output)
    set(reflection "${output}.json")
    add_custom_command(
        OUTPUT "${output}" "${reflection}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUT}"
        COMMAND "${SLANGC_EXECUTABLE}" "${source}"
                -entry "${entry_point}"
                -stage "${stage}"
                -target spirv -profile spirv_1_5
                -matrix-layout-column-major
                -reflection-json "${reflection}"
                -o "${output}"
        DEPENDS "${source}"
        VERBATIM USES_TERMINAL
        COMMENT "Compiling shader ${source}")
    add_custom_target("${target}" DEPENDS "${output}" "${reflection}" SOURCES "${source}")
endfunction()

add_slang_shader(shader_basic_vert
    "${SHADER_DIR}/basic.vert.slang"       vertexMain   vertex   "${SHADER_OUT}/basic.vert.spv")
add_slang_shader(shader_basic_frag
    "${SHADER_DIR}/basic.frag.slang"       fragmentMain fragment "${SHADER_OUT}/basic.frag.spv")
add_slang_shader(shader_albedo_frag
    "${SHADER_DIR}/albedo.frag.slang"      fragmentMain fragment "${SHADER_OUT}/albedo.frag.spv")
add_slang_shader(shader_solid_color_frag
    "${SHADER_DIR}/solid_color.frag.slang" fragmentMain fragment "${SHADER_OUT}/solid_color.frag.spv")

set(SHADER_TARGETS shader_basic_vert shader_basic_frag shader_albedo_frag shader_solid_color_frag)

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/src/*.hpp")

add_executable(Nexus_FPS ${APP_SRC})

target_link_libraries(Nexus_FPS PRIVATE
    Vulkan::Vulkan
    glfw
    vk-bootstrap::vk-bootstrap
    imgui
    nexus_fps_headers)

target_compile_definitions(Nexus_FPS PRIVATE
    GLFW_INCLUDE_NONE
    # 默认资源目录: 里面放着可导入的 glTF 场景 (assets/modular-demo 等)。
    NEXUS_FPS_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
    # SPIR-V 与 Slang 反射 JSON 的输出目录。
    NEXUS_FPS_SHADER_DIR="${SHADER_OUT}"
    # ImGui 界面字体文件。
    NEXUS_FPS_UI_FONT="${NEXUS_FPS_UI_FONT}"
    # Vulkan-Hpp: 动态分发 + 指派初始化器。
    VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
    VULKAN_HPP_NO_STRUCT_CONSTRUCTORS
    # tinygltf 默认会 #include "json.hpp", 这里改用 nlohmann/json 3.12。
    TINYGLTF_NO_INCLUDE_JSON)

target_compile_options(Nexus_FPS PRIVATE
    -Wall -Wextra -Wpedantic
    # Vulkan C 结构体的指派初始化器会刷屏 -Wmissing-field-initializers。
    -Wno-missing-field-initializers)

if(MINGW)
    # main.cpp / util.cpp 是超大翻译单元, PE-COFF 的默认 section 数不够。
    target_compile_options(Nexus_FPS PRIVATE -Wa,-mbig-obj)
endif()

add_dependencies(Nexus_FPS ${SHADER_TARGETS})

# ---------------------------------------------------------------------------
# 场景资源
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/modular-demo/modular-demo.gltf")
    message(WARNING
        "缺少示例场景 assets/modular-demo/modular-demo.gltf, GUI 里将没有默认场景可导入。")
endif()
