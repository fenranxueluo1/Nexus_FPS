// 模块说明：通用工具集。日志、二进制与图像读写、哈希、glTF 解析、Slang 反射与数学辅助。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <vulkan/vulkan.hpp>

namespace siggraph {
namespace util {

// 日志输出：把一行格式化文本写到标准输出。
inline void log_msg(std::format_string<> msg) { std::cout << msg.get() << '\n'; }

template <typename... T> inline void log_msg(std::format_string<T...> msg, T&&... args)
{
    std::cout << std::format(msg, std::forward<T>(args)...) << '\n';
}

// 带对齐偏移的二进制缓冲，用于存放 SPIR-V 等着色器二进制。
struct BinaryBuffer {
    std::vector<std::byte> storage;
    std::size_t offset = 0;
    std::size_t size = 0;

    BinaryBuffer(const BinaryBuffer&) = delete;
    BinaryBuffer(BinaryBuffer&&) = default;

    BinaryBuffer& operator=(const BinaryBuffer&) = delete;
    BinaryBuffer& operator=(BinaryBuffer&&) = delete;

    BinaryBuffer(std::size_t size, std::size_t alignment);

    [[nodiscard]] const std::byte* data() const { return storage.data() + offset; }

    [[nodiscard]] std::byte* data() { return storage.data() + offset; }

    [[nodiscard]] std::span<const std::byte> as_byte_span() const { return {data(), size}; }
};

// CPU 侧的 RGBA8 图像数据，上传前使用。
struct ImageRgba8 {
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::vector<std::uint32_t> m_pixels;
};

// 与着色器一致的顶点布局（位置/UV/法线/切线）。
struct PackedVertex {
    glm::vec3 position{};
    glm::vec2 uv{};
    glm::vec3 normal{};
    glm::vec4 tangent{1.0F, 0.0F, 0.0F, 1.0F};
};

// 展开后的网格几何数据：顶点数组与索引数组。
// 解析后的网格：按 ID 引用位置/索引/UV/法线/切线等 accessor。
struct MeshGeometryData {
    std::string name;
    std::vector<PackedVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// 读取编译好的 SPIR-V，并校验其按 32 位字对齐。
[[nodiscard]] BinaryBuffer readSpirvFile(const std::filesystem::path& path);

// 读取二进制文件；文件缺失或为空时返回 nullopt，便于缓存回退。
[[nodiscard]] std::optional<BinaryBuffer> readBinaryFile(const std::filesystem::path& path, std::size_t alignment);
// 写出二进制文件，用于保存着色器二进制缓存。
void writeBinaryFile(const std::filesystem::path& path, std::span<const std::uint8_t> data);

// 把 8 位 sRGB 显示色转换为线性 RGB，供着色器计算使用。
[[nodiscard]] glm::vec3 sRgbToLinear(std::uint8_t r, std::uint8_t g, std::uint8_t b);

// 把字节、字符串或整数值混合进哈希值，用于生成着色器缓存键。
[[nodiscard]] std::uint64_t combineHash(std::span<const std::uint8_t> data, std::uint64_t seed = 0);
[[nodiscard]] std::uint64_t combineHash(std::span<const std::byte> data, std::uint64_t seed = 0);
[[nodiscard]] std::uint64_t combineHash(std::string_view value, std::uint64_t seed = 0);

template <typename T> [[nodiscard]] inline std::uint64_t combineHash(T value, std::uint64_t seed = 0)
{
    if constexpr (std::is_enum_v<T>) {
        return combineHash(static_cast<std::underlying_type_t<T>>(value), seed);
    }
    else {
        static_assert(std::is_integral_v<T>, "combineHash 只支持整数值、枚举值、字符串和字节");
        using Unsigned = std::make_unsigned_t<T>;
        const Unsigned normalized = static_cast<Unsigned>(value);
        std::array<std::uint8_t, sizeof(Unsigned)> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>((normalized >> (i * 8U)) & static_cast<Unsigned>(0xFFU));
        }
        return combineHash(std::span<const std::uint8_t>{bytes.data(), bytes.size()}, seed);
    }
}
template <typename First, typename Second, typename Third, typename... Rest>
[[nodiscard]] inline std::uint64_t combineHash(First first, Second second, Third third, Rest... rest)
{
    std::uint64_t seed = combineHash(first);
    seed = combineHash(second, seed);
    seed = combineHash(third, seed);
    ((seed = combineHash(rest, seed)), ...);
    return seed;
}

// 解码图像并统一转换为紧凑的 RGBA8 像素。
[[nodiscard]] ImageRgba8 readImageFileRgba8(const std::filesystem::path& path);

// 把 VkResult 转换为可读名称，用于异常与日志。
[[nodiscard]] const char* vkResultName(VkResult result);

// Vulkan 调用返回失败时抛出带操作名的异常。
void checkVk(VkResult result, std::string_view operation);

// 断言式检查：条件不满足时立即抛出异常，把静默错误变成明确报错。
void require(bool condition, std::string_view message);

// 校验命令行参数并读取可选的 --frame-limit。
[[nodiscard]] std::uint32_t readFrameLimitCLI(int argc, char** argv);

// 先校验取值范围再做整数类型转换，避免溢出。
template <typename T, typename V> [[nodiscard]] inline T safeCastTo(V value)
{
    static_assert(std::is_integral_v<T> && std::is_integral_v<V>, "safeCastTo 只支持整数值");
    require(std::in_range<T>(value), "整数转换超出范围");
    return static_cast<T>(value);
}

template <typename V> [[nodiscard]] inline std::uint32_t safeCastToU32(V value)
{
    return safeCastTo<std::uint32_t>(value);
}

// 把 Vulkan 句柄统一转换成 uint64，便于设置调试名称。
template <typename RawHandle> [[nodiscard]] inline static std::uint64_t rawHandleToUint64(RawHandle handle)
{
    if constexpr (std::is_pointer_v<RawHandle>) {
        return reinterpret_cast<std::uint64_t>(handle);
    }
    else {
        return static_cast<std::uint64_t>(handle);
    }
}

// 把数值向上对齐到指定对齐值（描述符堆偏移计算使用）。
template <typename T> [[nodiscard]] inline static constexpr T alignUp(T value, T alignment)
{
    if (alignment == 0) {
        return value;
    }
    return (value + alignment - 1) / alignment * alignment;
}

// 计算从非对齐基址移动到对齐基址所需的字节偏移。
template <typename T, typename S> [[nodiscard]] inline static S alignedOffset(T value, S alignment)
{
    if (alignment == 0) {
        return 0;
    }

    const T alignedValue = alignUp(value, safeCastTo<T>(alignment));
    return safeCastTo<S>(alignedValue - value);
}

// 计算包含对齐子范围所需的分配尺寸。
template <typename T> [[nodiscard]] inline static constexpr T alignedAllocationSize(T rangeSize, T alignment)
{
    return rangeSize + ((alignment > 0) ? (alignment - 1) : 0);
}

} // namespace util

// 数学工具：欧拉角/模型矩阵、自由相机的前右方向与视图投影矩阵。
namespace util::math {

[[nodiscard]] glm::mat4 generateRotation(const glm::vec3& eulerAngles);

[[nodiscard]] glm::mat4 generateModel(const glm::vec3& pos, const glm::vec3& eulerAngles, const glm::vec3& scale);

[[nodiscard]] std::pair<float, float> calculateYawPitch(const glm::vec3& cameraPos, const glm::vec3& cameraLookAt);

[[nodiscard]] glm::vec3 calculateForward(float pitch, float yaw);

[[nodiscard]] glm::vec3 calculateRight(const glm::vec3& forward,
                                       const glm::vec3& worldUp = glm::vec3{0.0F, 1.0F, 0.0F});

[[nodiscard]] glm::mat4 calculateViewProjection(float cameraPitch, float cameraYaw, const glm::vec3& cameraPos,
                                                float aspectRatio, float verticalFieldOfView = 0.78539816339F,
                                                float nearPlane = 0.1F, float farPlane = 100.0F);

} // namespace util::math

// Slang 反射：从反射 JSON 提取描述符绑定与顶点输入。
namespace util::slang {

// 反射到的着色器资源：set、binding 与资源类型掩码。
struct ShaderResourceBinding {
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    VkSpirvResourceTypeFlagsEXT resourceMask = 0;
};

// 反射到的顶点输入：位置与对应的 Vulkan 格式。
struct VertexInput {
    std::uint32_t location = 0;
    vk::Format format = vk::Format::eUndefined;
};

// 动态顶点输入所需的绑定与属性描述表。
struct PackedVertexInputLayout {
    std::vector<vk::VertexInputBindingDescription2EXT> bindings;
    std::vector<vk::VertexInputAttributeDescription2EXT> attributes;
};

[[nodiscard]] std::unordered_map<std::string, ShaderResourceBinding>
calculateReflectionShaderResourceBindings(const std::filesystem::path& path);

[[nodiscard]] std::unordered_map<std::string, ShaderResourceBinding>
collectShaderResourceBindings(std::span<const std::filesystem::path> reflectionPaths);

[[nodiscard]] std::unordered_map<std::string, VertexInput>
calculateReflectionVertexInputs(const std::filesystem::path& path);

[[nodiscard]] PackedVertexInputLayout calculatePackedVertexInputLayout(const std::filesystem::path& reflectionPath);

} // namespace util::slang

// glTF 解析：把场景转换成便于逐步上传的紧凑数组。
namespace util::gltf {

inline constexpr std::uint32_t invalidGltfId = std::numeric_limits<std::uint32_t>::max();

struct Mesh {
    std::string name;
    std::uint32_t verticesPositionId = invalidGltfId;
    std::uint32_t indicesId = invalidGltfId;
    std::uint32_t uvId = invalidGltfId;
    std::uint32_t normalId = invalidGltfId;
    std::uint32_t tangentId = invalidGltfId;
};

// 解析后的场景实例：变换、网格 ID 与贴图 ID。
struct Node {
    std::string name;
    glm::vec3 pos{};
    glm::vec3 eulerAngles{};
    glm::vec3 scale{1.0F};
    std::uint32_t meshId = invalidGltfId;
    std::uint32_t albedoTextureId = invalidGltfId;
    std::uint32_t normalTextureId = invalidGltfId;
};

// 贴图元数据及其解析后的文件路径。
struct GltfTexture {
    std::string name;
    std::string filename;
};

// 解析结果：相机、实例、网格以及各类 accessor 数据。
struct ParsedData {
    glm::vec3 cameraPos{};
    glm::vec3 cameraLookAt{};
    bool hasCamera = false;
    std::vector<Node> nodes;
    std::vector<Mesh> meshes;
    std::vector<std::vector<glm::vec3>> verticesPositions;
    std::vector<std::vector<std::uint32_t>> indices;
    std::vector<std::vector<glm::vec2>> uvs;
    std::vector<std::vector<glm::vec3>> normals;
    std::vector<std::vector<glm::vec4>> tangents;
    std::vector<GltfTexture> textures;
};

// 解析一个 glTF 文件，并把结果并入已有的解析数据。
void appendGltfFile(const std::filesystem::path& path, ParsedData& parsedData);
// 解析一组 glTF 文件并合并为一个场景。
[[nodiscard]] ParsedData parseGltfFiles(std::span<const std::filesystem::path> paths);

// 把指定网格展开成可直接上传的顶点/索引数组。
[[nodiscard]] util::MeshGeometryData buildMeshGeometryData(std::uint32_t meshId, const ParsedData& gltfData);

} // namespace util::gltf
} // namespace siggraph
