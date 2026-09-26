// 模块说明：通用工具实现。文件与图像读写、glTF 解析、Slang 反射解析以及数学辅助。
#include "util.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include <stb_image.h>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

namespace siggraph {
namespace util {
namespace {

// 把 Slang 反射的标量类型与分量数映射为 Vulkan 顶点格式。
[[nodiscard]] vk::Format vertexFormatFromSlangType(std::string_view scalarType, int elementCount)
{
    if (scalarType == "float32") {
        switch (elementCount) {
        case 1:
            return vk::Format::eR32Sfloat;
        case 2:
            return vk::Format::eR32G32Sfloat;
        case 3:
            return vk::Format::eR32G32B32Sfloat;
        case 4:
            return vk::Format::eR32G32B32A32Sfloat;
        default:
            break;
        }
    }

    if (scalarType == "uint32") {
        switch (elementCount) {
        case 1:
            return vk::Format::eR32Uint;
        case 2:
            return vk::Format::eR32G32Uint;
        case 3:
            return vk::Format::eR32G32B32Uint;
        case 4:
            return vk::Format::eR32G32B32A32Uint;
        default:
            break;
        }
    }

    throw std::runtime_error(std::format("不支持的 Slang 顶点输入类型：{}x{}", scalarType, elementCount));
}

// 把 Slang 反射的资源类型映射为描述符堆资源掩码。
[[nodiscard]] VkSpirvResourceTypeFlagsEXT resourceMaskFromSlangType(const nlohmann::json& type)
{
    const std::string kind = type.value("kind", std::string{});
    if (kind == "constantBuffer") {
        return VK_SPIRV_RESOURCE_TYPE_UNIFORM_BUFFER_BIT_EXT;
    }
    if (kind == "samplerState") {
        return VK_SPIRV_RESOURCE_TYPE_SAMPLER_BIT_EXT;
    }

    if (kind == "resource") {
        const std::string baseShape = type.value("baseShape", std::string{});
        if (baseShape == "structuredBuffer") {
            return VK_SPIRV_RESOURCE_TYPE_READ_ONLY_STORAGE_BUFFER_BIT_EXT;
        }
        if (baseShape == "texture2D") {
            if (type.value("combined", false)) {
                return VK_SPIRV_RESOURCE_TYPE_COMBINED_SAMPLED_IMAGE_BIT_EXT;
            }
            return VK_SPIRV_RESOURCE_TYPE_SAMPLED_IMAGE_BIT_EXT;
        }
    }

    throw std::runtime_error(std::format("不支持的 Slang 反射资源类型：{}", kind));
}

// sRGB 单通道转线性，用查表避免逐像素做幂运算。
[[nodiscard]] float sRgbChannelToLinear(std::uint8_t value)
{
    const float normalized = static_cast<float>(value) / 255.0F;
    if (normalized <= 0.04045F) {
        return normalized / 12.92F;
    }
    return std::pow((normalized + 0.055F) / 1.055F, 2.4F);
}

// 把 tinygltf 的 -1（缺省引用）归一化为无效 ID。
[[nodiscard]] std::uint32_t optionalGltfIndex(int index)
{
    return index < 0 ? gltf::invalidGltfId : safeCastToU32(index);
}

// 取必需索引；缺省时直接报错。
[[nodiscard]] std::uint32_t requiredGltfIndex(int index, std::string_view what)
{
    require(index >= 0, "glTF " + std::string(what) + " 索引缺失");
    return safeCastToU32(index);
}

// 取 accessor 并做边界检查。
[[nodiscard]] const tinygltf::Accessor& gltfAccessorAt(const tinygltf::Model& model, std::uint32_t index,
                                                       std::string_view what)
{
    require(index != gltf::invalidGltfId && index < model.accessors.size(),
            "glTF " + std::string(what) + " 访问器索引越界");
    return model.accessors[index];
}

// 取 bufferView 并做边界检查。
[[nodiscard]] const tinygltf::BufferView& gltfBufferViewAt(const tinygltf::Model& model, std::uint32_t index,
                                                           std::string_view what)
{
    require(index != gltf::invalidGltfId && index < model.bufferViews.size(),
            "glTF " + std::string(what) + " 缓冲区视图索引越界");
    return model.bufferViews[index];
}

// 取 buffer 并做边界检查。
[[nodiscard]] const tinygltf::Buffer& gltfBufferAt(const tinygltf::Model& model, std::uint32_t index,
                                                   std::string_view what)
{
    require(index != gltf::invalidGltfId && index < model.buffers.size(),
            "glTF " + std::string(what) + " 缓冲区索引越界");
    return model.buffers[index];
}

// 计算末元素之后的总字节数，含字节步长与溢出检查。
[[nodiscard]] std::size_t gltfAccessorByteSize(std::size_t elementCount, std::size_t byteStride,
                                               std::size_t elementSize, std::string_view what)
{
    if (elementCount == 0) {
        return 0;
    }

    const std::size_t lastElementIndex = elementCount - 1U;
    require(byteStride == 0 || lastElementIndex <= (std::numeric_limits<std::size_t>::max() - elementSize) / byteStride,
            "glTF " + std::string(what) + " 访问器的字节范围超出 size_t");
    return lastElementIndex * byteStride + elementSize;
}

// 校验访问范围完整落在缓冲区内。
// 条件检查失败时抛出异常，把静默错误变成明确报错。
void requireGltfAccessorByteRange(std::span<const std::byte> accessorBytes, std::size_t byteOffset,
                                  std::size_t byteSize, std::string_view what)
{
    require(byteOffset <= accessorBytes.size() && byteSize <= accessorBytes.size() - byteOffset,
            "glTF " + std::string(what) + " 访问器的字节范围读取越界");
}

// 计算 accessor 在缓冲中的字节范围，并校验步长与尺寸。
[[nodiscard]] std::span<const std::byte> gltfAccessorData(const tinygltf::Model& model,
                                                          const tinygltf::Accessor& accessor,
                                                          const tinygltf::BufferView& bufferView,
                                                          std::size_t elementSize, std::string_view what)
{
    require(!accessor.sparse.isSparse, "不支持 glTF 稀疏访问器");

    const std::uint32_t bufferIndex = requiredGltfIndex(bufferView.buffer, "buffer");
    const tinygltf::Buffer& buffer = gltfBufferAt(model, bufferIndex, what);

    const std::size_t byteOffset = bufferView.byteOffset + accessor.byteOffset;
    require(byteOffset <= buffer.data.size(), "glTF " + std::string(what) + " 访问器的起点超出缓冲区");

    const int stride = accessor.ByteStride(bufferView);
    require(stride > 0, "glTF " + std::string(what) + " 访问器的字节步长无效");

    require(static_cast<std::size_t>(stride) >= elementSize,
            "glTF " + std::string(what) + " 访问器的步长小于一个元素");

    const std::size_t byteSize =
        gltfAccessorByteSize(accessor.count, static_cast<std::size_t>(stride), elementSize, what);
    require(byteSize <= buffer.data.size() - byteOffset,
            "glTF " + std::string(what) + " 访问器读取超出缓冲区");

    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(buffer.data.data() + byteOffset), byteSize};
}

template <typename Value>
// 按分量类型把 accessor 数据展开成目标向量类型。
[[nodiscard]] std::vector<Value> copyGltfAccessorElements(const tinygltf::Accessor& accessor,
                                                          const tinygltf::BufferView& bufferView,
                                                          std::span<const std::byte> accessorBytes)
{
    const std::size_t byteStride = static_cast<std::size_t>(accessor.ByteStride(bufferView));
    require(byteStride > 0, "glTF 访问器的字节步长无效");
    require(byteStride >= sizeof(Value), "glTF 访问器的步长小于一个元素");

    std::vector<Value> values(accessor.count);
    const std::size_t requiredByteSize = gltfAccessorByteSize(values.size(), byteStride, sizeof(Value), "attribute");
    require(accessorBytes.size() >= requiredByteSize, "glTF 访问器的字节范围小于所需数据");
    if (values.empty()) {
        return values;
    }

    if (byteStride == sizeof(Value)) {
        requireGltfAccessorByteRange(accessorBytes, 0, requiredByteSize, "attribute");
        std::memcpy(values.data(), accessorBytes.data(), requiredByteSize);
        return values;
    }

    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t byteOffset = i * byteStride;
        requireGltfAccessorByteRange(accessorBytes, byteOffset, sizeof(Value), "attribute");
        std::memcpy(&values[i], accessorBytes.data() + byteOffset, sizeof(Value));
    }
    return values;
}

template <typename Value>
// 读取浮点向量 accessor，并校验分量类型与元素类型。
[[nodiscard]] std::vector<Value> readGltfFloatVectorAccessor(const tinygltf::Model& model, std::uint32_t accessorIndex,
                                                             int expectedAccessorType,
                                                             std::string_view expectedTypeName, std::string_view what)
{
    const tinygltf::Accessor& accessor = gltfAccessorAt(model, accessorIndex, what);
    require(accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT,
            "glTF " + std::string(what) + " 访问器必须包含浮点分量");
    require(accessor.type == expectedAccessorType,
            "glTF " + std::string(what) + " 访问器必须是 " + std::string(expectedTypeName));

    const std::uint32_t bufferViewIndex = requiredGltfIndex(accessor.bufferView, std::string(what) + " buffer view");
    const tinygltf::BufferView& bufferView = gltfBufferViewAt(model, bufferViewIndex, what);
    const std::span<const std::byte> accessorBytes = gltfAccessorData(model, accessor, bufferView, sizeof(Value), what);
    return copyGltfAccessorElements<Value>(accessor, bufferView, accessorBytes);
}

// 读取三维向量 accessor。
[[nodiscard]] std::vector<glm::vec3> readGltfVec3Accessor(const tinygltf::Model& model, std::uint32_t accessorIndex,
                                                          std::string_view what)
{
    return readGltfFloatVectorAccessor<glm::vec3>(model, accessorIndex, TINYGLTF_TYPE_VEC3, "VEC3", what);
}

// 读取四维向量 accessor。
[[nodiscard]] std::vector<glm::vec4> readGltfVec4Accessor(const tinygltf::Model& model, std::uint32_t accessorIndex,
                                                          std::string_view what)
{
    return readGltfFloatVectorAccessor<glm::vec4>(model, accessorIndex, TINYGLTF_TYPE_VEC4, "VEC4", what);
}

// 读取二维向量 accessor。
[[nodiscard]] std::vector<glm::vec2> readGltfVec2Accessor(const tinygltf::Model& model, std::uint32_t accessorIndex,
                                                          std::string_view what)
{
    return readGltfFloatVectorAccessor<glm::vec2>(model, accessorIndex, TINYGLTF_TYPE_VEC2, "VEC2", what);
}

// 返回索引分量的字节宽度。
[[nodiscard]] std::size_t gltfIndexElementSize(int componentType)
{
    switch (componentType) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return sizeof(std::uint8_t);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return sizeof(std::uint16_t);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return sizeof(std::uint32_t);
    default:
        throw std::runtime_error(std::format("glTF 索引访问器使用了不支持的分量类型 {}", componentType));
    }
}

template <typename StoredIndex>
// 把不同宽度的索引统一转换成 uint32 索引流。
[[nodiscard]] std::vector<std::uint32_t> copyGltfIndexElements(const tinygltf::Accessor& accessor,
                                                               const tinygltf::BufferView& bufferView,
                                                               std::span<const std::byte> accessorBytes)
{
    const std::size_t byteStride = static_cast<std::size_t>(accessor.ByteStride(bufferView));
    require(byteStride > 0, "glTF 索引访问器的字节步长无效");
    require(byteStride >= sizeof(StoredIndex), "glTF 索引访问器的步长小于一个元素");

    std::vector<std::uint32_t> values(accessor.count);
    const std::size_t requiredByteSize = gltfAccessorByteSize(values.size(), byteStride, sizeof(StoredIndex), "index");
    require(accessorBytes.size() >= requiredByteSize, "glTF 索引访问器的字节范围小于索引数据");
    if (values.empty()) {
        return values;
    }

    if (byteStride == sizeof(StoredIndex)) {
        requireGltfAccessorByteRange(accessorBytes, 0, requiredByteSize, "index");
        if constexpr (std::is_same_v<StoredIndex, std::uint32_t>) {
            std::memcpy(values.data(), accessorBytes.data(), requiredByteSize);
        }
        else {
            for (std::size_t i = 0; i < values.size(); ++i) {
                StoredIndex value = 0;
                std::memcpy(&value, accessorBytes.data() + i * sizeof(StoredIndex), sizeof(value));
                values[i] = static_cast<std::uint32_t>(value);
            }
        }
        return values;
    }

    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t byteOffset = i * byteStride;
        requireGltfAccessorByteRange(accessorBytes, byteOffset, sizeof(StoredIndex), "index");
        StoredIndex value = 0;
        std::memcpy(&value, accessorBytes.data() + byteOffset, sizeof(value));
        values[i] = static_cast<std::uint32_t>(value);
    }
    return values;
}

// 读取索引 accessor，并校验其必须是标量类型。
[[nodiscard]] std::vector<std::uint32_t> readGltfIndexAccessor(const tinygltf::Model& model,
                                                               std::uint32_t accessorIndex)
{
    const tinygltf::Accessor& accessor = gltfAccessorAt(model, accessorIndex, "index");
    require(accessor.type == TINYGLTF_TYPE_SCALAR, "glTF 索引访问器必须是 SCALAR");

    const std::size_t elementSize = gltfIndexElementSize(accessor.componentType);
    const std::uint32_t bufferViewIndex = requiredGltfIndex(accessor.bufferView, "index buffer view");
    const tinygltf::BufferView& bufferView = gltfBufferViewAt(model, bufferViewIndex, "index");
    const std::span<const std::byte> accessorBytes =
        gltfAccessorData(model, accessor, bufferView, elementSize, "index");

    switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return copyGltfIndexElements<std::uint8_t>(accessor, bufferView, accessorBytes);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return copyGltfIndexElements<std::uint16_t>(accessor, bufferView, accessorBytes);
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return copyGltfIndexElements<std::uint32_t>(accessor, bufferView, accessorBytes);
    default:
        throw std::runtime_error(
            std::format("glTF 索引访问器使用了不支持的分量类型 {}", accessor.componentType));
    }
}

// 由节点的矩阵或平移/旋转/缩放分量得到局部变换矩阵。
[[nodiscard]] glm::mat4 gltfNodeLocalTransform(const tinygltf::Node& node)
{
    if (!node.matrix.empty()) {
        require(node.matrix.size() == 16, "glTF 节点的 matrix 必须包含 16 个值");
        glm::mat4 transform{1.0F};
        for (glm::length_t column = 0; column < 4; ++column) {
            for (glm::length_t row = 0; row < 4; ++row) {
                const std::size_t matrixIndex = static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row);
                transform[column][row] = static_cast<float>(node.matrix[matrixIndex]);
            }
        }
        return transform;
    }

    glm::vec3 translation{0.0F};
    if (!node.translation.empty()) {
        require(node.translation.size() == 3, "glTF 节点的 translation 必须包含 3 个值");
        translation = glm::vec3{static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]),
                                static_cast<float>(node.translation[2])};
    }

    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    if (!node.rotation.empty()) {
        require(node.rotation.size() == 4, "glTF 节点的 rotation 必须包含 4 个值");
        rotation =
            glm::normalize(glm::quat{static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                                     static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2])});
    }

    glm::vec3 scale{1.0F};
    if (!node.scale.empty()) {
        require(node.scale.size() == 3, "glTF 节点的 scale 必须包含 3 个值");
        scale = glm::vec3{static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]),
                          static_cast<float>(node.scale[2])};
    }

    return glm::translate(glm::mat4{1.0F}, translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4{1.0F}, scale);
}

// 按属性 accessor 组合对几何去重时使用的键。
struct GeometryKey {
    std::uint32_t positionId = gltf::invalidGltfId;
    std::uint32_t indexId = gltf::invalidGltfId;
    std::uint32_t uvId = gltf::invalidGltfId;
    std::uint32_t normalId = gltf::invalidGltfId;
    std::uint32_t tangentId = gltf::invalidGltfId;

    [[nodiscard]] bool operator==(const GeometryKey& other) const = default;
};

// GeometryKey 的哈希函数。
struct GeometryKeyHash {
    [[nodiscard]] std::size_t operator()(const GeometryKey& key) const
    {
        return combineHash(key.positionId, key.indexId, key.uvId, key.normalId, key.tangentId);
    }
};

// 无法计算切线时，由法线生成一个正交的备用切线。
[[nodiscard]] glm::vec3 fallbackTangentForNormal(const glm::vec3& normal)
{
    const glm::vec3 helper = std::abs(normal.y) < 0.999F ? glm::vec3{0.0F, 1.0F, 0.0F} : glm::vec3{1.0F, 0.0F, 0.0F};
    return glm::normalize(glm::cross(helper, normal));
}

// 为缺少切线的网格，按法线与 UV 反算切线。
void generateMissingTangents(MeshGeometryData& geometry)
{
    constexpr float tangentEpsilon = 0.000001F;

    std::vector<glm::vec3> tangentSums(geometry.vertices.size(), glm::vec3{0.0F});
    std::vector<glm::vec3> bitangentSums(geometry.vertices.size(), glm::vec3{0.0F});

    for (std::size_t i = 0; i + 2 < geometry.indices.size(); i += 3) {
        const std::uint32_t i0 = geometry.indices[i + 0];
        const std::uint32_t i1 = geometry.indices[i + 1];
        const std::uint32_t i2 = geometry.indices[i + 2];

        const PackedVertex& v0 = geometry.vertices[i0];
        const PackedVertex& v1 = geometry.vertices[i1];
        const PackedVertex& v2 = geometry.vertices[i2];

        const glm::vec3 edge1 = v1.position - v0.position;
        const glm::vec3 edge2 = v2.position - v0.position;
        const glm::vec2 deltaUv1 = v1.uv - v0.uv;
        const glm::vec2 deltaUv2 = v2.uv - v0.uv;
        const float determinant = deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
        if (std::abs(determinant) <= tangentEpsilon) {
            continue;
        }

        const float inverseDeterminant = 1.0F / determinant;
        const glm::vec3 tangent = (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * inverseDeterminant;
        const glm::vec3 bitangent = (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * inverseDeterminant;

        tangentSums[i0] += tangent;
        tangentSums[i1] += tangent;
        tangentSums[i2] += tangent;
        bitangentSums[i0] += bitangent;
        bitangentSums[i1] += bitangent;
        bitangentSums[i2] += bitangent;
    }

    for (std::size_t i = 0; i < geometry.vertices.size(); ++i) {
        const glm::vec3 normal = glm::normalize(geometry.vertices[i].normal);
        glm::vec3 tangent = tangentSums[i] - normal * glm::dot(normal, tangentSums[i]);
        if (glm::dot(tangent, tangent) <= tangentEpsilon) {
            tangent = fallbackTangentForNormal(normal);
        }
        else {
            tangent = glm::normalize(tangent);
        }

        const float handedness = glm::dot(glm::cross(normal, tangent), bitangentSums[i]) < 0.0F ? -1.0F : 1.0F;
        geometry.vertices[i].tangent = glm::vec4{tangent, handedness};
    }
}

// 由世界变换构造一个解析后的场景实例。
[[nodiscard]] gltf::Node makeParsedNode(const glm::mat4& transform)
{
    gltf::Node parsedNode{};
    parsedNode.pos = glm::vec3{transform[3]};
    parsedNode.scale = glm::vec3{glm::length(glm::vec3{transform[0]}), glm::length(glm::vec3{transform[1]}),
                                 glm::length(glm::vec3{transform[2]})};

    glm::mat3 rotationMatrix{1.0F};
    for (glm::length_t column = 0; column < 3; ++column) {
        const float axisScale = parsedNode.scale[column];
        if (axisScale > 0.0F) {
            rotationMatrix[column] = glm::vec3{transform[column]} / axisScale;
        }
    }
    parsedNode.eulerAngles = glm::eulerAngles(glm::quat_cast(rotationMatrix));
    return parsedNode;
}

// 名称缺失时生成带序号的占位名称。
[[nodiscard]] std::string gltfNameOrFallback(const std::string& name, std::string_view fallbackPrefix,
                                             std::uint32_t index)
{
    if (!name.empty()) {
        return name;
    }
    return std::string{fallbackPrefix} + "[" + std::to_string(index) + "]";
}

// 生成网格图元的显示名称，用于日志与调试标签。
[[nodiscard]] std::string gltfMeshPrimitiveName(const tinygltf::Mesh& mesh, std::uint32_t meshIndex,
                                                std::uint32_t primitiveIndex, std::size_t primitiveCount)
{
    std::string name = gltfNameOrFallback(mesh.name, "Mesh", meshIndex);
    if (primitiveCount > 1) {
        name += "/Primitive[" + std::to_string(primitiveIndex) + "]";
    }
    return name;
}

// 生成贴图的显示名称，用于日志与调试标签。
[[nodiscard]] std::string gltfTextureName(const tinygltf::Texture& texture, const tinygltf::Image& image,
                                          std::uint32_t textureIndex)
{
    if (!texture.name.empty()) {
        return texture.name;
    }
    if (!image.name.empty()) {
        return image.name;
    }
    return gltfNameOrFallback(std::string{}, "Texture", textureIndex);
}

// 由当前数组长度推导新元素 ID，并防止与保留值冲突。
[[nodiscard]] std::uint32_t parsedDataIdFromSize(std::size_t size, std::string_view what)
{
    const std::uint32_t id = safeCastToU32(size);
    require(id != gltf::invalidGltfId, std::format("glTF {} 的数量使用了保留的无效 ID", what));
    return id;
}

[[nodiscard]] std::uint32_t
textureIdFromTextureIndex(const tinygltf::Model& model, const std::filesystem::path& gltfBaseDir,
                          std::uint32_t textureIndex, gltf::ParsedData& parsedData,
                          std::unordered_map<std::uint32_t, std::uint32_t>& imageToTextureId)
{
    if (textureIndex == gltf::invalidGltfId) {
        return gltf::invalidGltfId;
    }

    require(textureIndex < model.textures.size(), "glTF 贴图索引越界");

    const tinygltf::Texture& texture = model.textures[textureIndex];

    const std::uint32_t imageIndex = requiredGltfIndex(texture.source, "texture image");
    require(imageIndex < model.images.size(), "glTF 贴图图像索引越界");

    const auto existingIt = imageToTextureId.find(imageIndex);
    if (existingIt != imageToTextureId.end()) {
        return existingIt->second;
    }

    const tinygltf::Image& image = model.images[imageIndex];
    require(!image.uri.empty(), "glTF 贴图图像必须使用外部 URI");
    const std::filesystem::path texturePath =
        std::filesystem::path{image.uri}.is_absolute() ? std::filesystem::path{image.uri} : gltfBaseDir / image.uri;

    const std::uint32_t id = parsedDataIdFromSize(parsedData.textures.size(), "texture");
    parsedData.textures.push_back(gltf::GltfTexture{
        .name = gltfTextureName(texture, image, textureIndex),
        .filename = texturePath.lexically_normal().string(),
    });
    imageToTextureId.emplace(imageIndex, id);
    return id;
}

} // namespace

BinaryBuffer::BinaryBuffer(size_t size, size_t alignment) : size(size)
{
    storage.resize(alignedAllocationSize(size, alignment));

    const std::size_t storage_data = reinterpret_cast<std::size_t>(storage.data());
    offset = alignUp(storage_data, alignment) - storage_data;

    require(alignUp(storage_data, alignment) == reinterpret_cast<std::size_t>(data()),
            "生成的二进制缓冲区未对齐。");
    require((data() + this->size) <= (storage.data() + storage.size()), "生成的二进制缓冲区溢出");
};

// 读取 SPIR-V 文件并校验其按 32 位字对齐。
BinaryBuffer readSpirvFile(const std::filesystem::path& path)
{

    std::size_t spirvAlignment = 4;

    std::optional<BinaryBuffer> binBuffer = readBinaryFile(path, spirvAlignment);

    if (!binBuffer.has_value()) {
        throw std::runtime_error(std::format("读取 SPIR-V 文件失败：{}", path.string()));
    }

    if ((static_cast<std::uintmax_t>(binBuffer.value().size) % sizeof(std::uint32_t)) != 0U) {
        throw std::runtime_error(std::format("SPIR-V 文件大小未按字对齐：{}", path.string()));
    }

    return std::move(binBuffer.value());
}

// 读取二进制文件；文件缺失或为空时返回 nullopt，让缓存可以回退。
std::optional<BinaryBuffer> readBinaryFile(const std::filesystem::path& path, std::size_t alignment)
{
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        if (existsError) {
            throw std::runtime_error(std::format("查询二进制文件失败：{}", path.string()));
        }
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(std::format("打开二进制文件失败：{}", path.string()));
    }

    const auto fileSize = file.tellg();
    if (fileSize < 0) {
        throw std::runtime_error(std::format("查询二进制文件大小失败：{}", path.string()));
    }
    else if (fileSize == 0) {
        log_msg("文件 {} 为空", path.string());
        return std::nullopt;
    }
    BinaryBuffer binBuffer(fileSize, alignment);

    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(binBuffer.data()), binBuffer.size);

    if (!file) {
        throw std::runtime_error(std::format("读取二进制文件失败：{}", path.string()));
    }

    return binBuffer;
}

// 写入二进制文件，并自动创建父目录。
void writeBinaryFile(const std::filesystem::path& path, std::span<const std::uint8_t> data)
{
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error(std::format("以写入方式打开二进制文件失败：{}", path.string()));
    }

    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    if (!file) {
        throw std::runtime_error(std::format("写入二进制文件失败：{}", path.string()));
    }
}

// 把 8 位 sRGB 三通道转换为线性 RGB。
glm::vec3 sRgbToLinear(std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return glm::vec3{sRgbChannelToLinear(r), sRgbChannelToLinear(g), sRgbChannelToLinear(b)};
}

// 把一段字节混合进哈希值。
std::uint64_t combineHash(std::span<const std::uint8_t> data, std::uint64_t seed)
{
    constexpr std::uint64_t fnvPrime = 1099511628211ULL;

    const std::uint64_t dataSize = static_cast<std::uint64_t>(data.size());
    for (std::size_t i = 0; i < sizeof(dataSize); ++i) {
        const std::uint8_t value = static_cast<std::uint8_t>((dataSize >> (i * 8U)) & 0xFFU);
        seed ^= value;
        seed *= fnvPrime;
    }
    for (const std::uint8_t value : data) {
        seed ^= value;
        seed *= fnvPrime;
    }
    return seed;
}

// 字节视图重载，转发到 uint8 版本。
std::uint64_t combineHash(std::span<const std::byte> data, std::uint64_t seed)
{
    return combineHash(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(data.data()), data.size()},
                       seed);
}

// 把字符串按其字节内容混合进哈希值。
std::uint64_t combineHash(std::string_view value, std::uint64_t seed)
{
    const std::span bytes{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
    return combineHash(bytes, seed);
}

// Slang 反射解析：读取 slangc 产出的反射 JSON。
namespace slang {

std::unordered_map<std::string, ShaderResourceBinding>
// 读取反射 JSON，返回按名字索引的着色器资源绑定。
calculateReflectionShaderResourceBindings(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error(std::format("打开 Slang 反射文件失败：{}", path.string()));
    }

    const nlohmann::json reflection = nlohmann::json::parse(file, nullptr, true, true);

    std::unordered_map<std::string, ShaderResourceBinding> bindings;

    for (const nlohmann::json& parameter : reflection.value("parameters", nlohmann::json::array())) {
        const nlohmann::json& binding = parameter.value("binding", nlohmann::json::object());
        if (binding.value("kind", std::string{}) != "descriptorTableSlot") {
            continue;
        }

        bindings.emplace(parameter.at("name").get<std::string>(),
                         ShaderResourceBinding{
                             .set = binding.value("set", 0U),
                             .binding = binding.at("index").get<std::uint32_t>(),
                             .resourceMask = resourceMaskFromSlangType(parameter.at("type")),
                         });
    }

    if (bindings.empty()) {
        throw std::runtime_error(
            std::format("Slang 反射文件里没有找到着色器资源绑定：{}", path.string()));
    }

    return bindings;
}

std::unordered_map<std::string, ShaderResourceBinding>
// 合并多个反射文件的绑定，并校验同名资源的 set/binding 一致。
collectShaderResourceBindings(std::span<const std::filesystem::path> reflectionPaths)
{
    require(!reflectionPaths.empty(), "至少要提供一个着色器反射文件路径");

    std::unordered_map<std::string, ShaderResourceBinding> mergedBindings;
    for (const std::filesystem::path& reflectionPath : reflectionPaths) {
        for (const auto& [name, binding] : calculateReflectionShaderResourceBindings(reflectionPath)) {
            const auto [it, inserted] = mergedBindings.emplace(name, binding);
            if (!inserted) {
                require(it->second.set == binding.set && it->second.binding == binding.binding &&
                            it->second.resourceMask == binding.resourceMask,
                        "共享资源绑定在 Slang 反射里不一致：" + name);
            }
        }
    }

    return mergedBindings;
}

// 读取反射 JSON，返回按字段名索引的顶点输入。
std::unordered_map<std::string, VertexInput> calculateReflectionVertexInputs(const std::filesystem::path& path)
{
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error(std::format("打开 Slang 反射文件失败：{}", path.string()));
    }

    const nlohmann::json reflection = nlohmann::json::parse(file, nullptr, true, true);

    std::unordered_map<std::string, VertexInput> inputs;

    if (reflection.contains("entryPoints") && !reflection.at("entryPoints").empty()) {
        const nlohmann::json& entryPoint = reflection.at("entryPoints").at(0);
        if (entryPoint.contains("parameters") && !entryPoint.at("parameters").empty()) {
            const nlohmann::json& entryParameter = entryPoint.at("parameters").at(0);
            if (entryParameter.contains("type") && entryParameter.at("type").contains("fields")) {
                const nlohmann::json& fields = entryParameter.at("type").at("fields");
                for (const nlohmann::json& field : fields) {
                    const nlohmann::json& binding = field.value("binding", nlohmann::json::object());
                    if (binding.value("kind", std::string{}) != "varyingInput") {
                        continue;
                    }

                    const nlohmann::json& type = field.at("type");
                    const nlohmann::json& elementType =
                        type.value("kind", std::string{}) == "vector" ? type.at("elementType") : type;
                    int elementCount = type.value("elementCount", 1);
                    const std::string scalarType = elementType.at("scalarType").get<std::string>();

                    inputs.emplace(field.at("name").get<std::string>(),
                                   VertexInput{
                                       .location = binding.at("index").get<std::uint32_t>(),
                                       .format = vertexFormatFromSlangType(scalarType, elementCount),
                                   });
                }
            }
        }
    }

    if (inputs.empty()) {
        throw std::runtime_error(
            std::format("Slang 反射文件里没有找到顶点输入变量：{}", path.string()));
    }

    return inputs;
}

// 由反射数据构建动态顶点输入所需的绑定与属性描述。
PackedVertexInputLayout calculatePackedVertexInputLayout(const std::filesystem::path& reflectionPath)
{
    PackedVertexInputLayout layout{};
    layout.bindings = {vk::VertexInputBindingDescription2EXT{
        .binding = 0,
        .stride = sizeof(PackedVertex),
        .inputRate = vk::VertexInputRate::eVertex,
        .divisor = 1,
    }};

    const std::unordered_map<std::string, VertexInput> vertexInputs = calculateReflectionVertexInputs(reflectionPath);

    struct VertexField {
        std::string_view name;
        std::uint32_t offset;
    };
    const std::array vertexFields{
        VertexField{.name = "position", .offset = safeCastToU32(offsetof(PackedVertex, position))},
        VertexField{.name = "uv", .offset = safeCastToU32(offsetof(PackedVertex, uv))},
        VertexField{.name = "normal", .offset = safeCastToU32(offsetof(PackedVertex, normal))},
        VertexField{.name = "tangent", .offset = safeCastToU32(offsetof(PackedVertex, tangent))},
    };

    layout.attributes.reserve(vertexFields.size());
    for (const VertexField& vertexField : vertexFields) {
        const std::string fieldName{vertexField.name};
        const auto inputIt = vertexInputs.find(fieldName);
        require(inputIt != vertexInputs.end(), "Slang 反射里缺少顶点输入：" + fieldName);

        layout.attributes.emplace_back(vk::VertexInputAttributeDescription2EXT{
            .location = inputIt->second.location,
            .binding = 0,
            .format = inputIt->second.format,
            .offset = vertexField.offset,
        });
    }

    return layout;
}

} // namespace slang

// 用 stb_image 解码图像，并统一转换成紧凑的 RGBA8 像素。
ImageRgba8 readImageFileRgba8(const std::filesystem::path& path)
{
    int width = 0;
    int height = 0;
    int sourceChannels = 0;
    constexpr int requiredChannels = 4;
    stbi_uc* decodedPixels = stbi_load(path.string().c_str(), &width, &height, &sourceChannels, requiredChannels);
    if (decodedPixels == nullptr) {
        throw std::runtime_error(
            std::format("读取图像文件失败：{}（{}）", path.string(), stbi_failure_reason()));
    }

    ImageRgba8 image{};
    try {
        require(width > 0 && height > 0, "图像尺寸必须为正");
        image.m_width = safeCastToU32(width);
        image.m_height = safeCastToU32(height);

        const std::size_t byteCount =
            static_cast<std::size_t>(image.m_width) * static_cast<std::size_t>(image.m_height) * requiredChannels;
        image.m_pixels.resize(byteCount / sizeof(std::uint32_t));
        std::memcpy(image.m_pixels.data(), decodedPixels, byteCount);
    }
    catch (...) {
        stbi_image_free(decodedPixels);
        throw;
    }

    stbi_image_free(decodedPixels);
    return image;
}

// glTF 解析：把文件转换成上传所需的紧凑数组。
namespace gltf {

// 解析单个 glTF：读取各类 accessor，展开网格实例并收集贴图。
void appendGltfFile(const std::filesystem::path& path, ParsedData& parsedData)
{
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string error;
    std::string warning;
    const std::filesystem::path gltfBaseDir = path.parent_path();

    const std::string extension = path.extension().string();
    const bool loaded = extension == ".glb" ? loader.LoadBinaryFromFile(&model, &error, &warning, path.string())
                                            : loader.LoadASCIIFromFile(&model, &error, &warning, path.string());
    if (!loaded) {
        throw std::runtime_error(std::format("解析 glTF 文件失败：{} {}", path.string(), error));
    }

    const std::size_t nodeCountBefore = parsedData.nodes.size();
    const bool hadCameraBefore = parsedData.hasCamera;

    std::unordered_map<std::uint32_t, std::uint32_t> positionAccessorIds;
    std::unordered_map<std::uint32_t, std::uint32_t> uvAccessorIds;
    std::unordered_map<std::uint32_t, std::uint32_t> normalAccessorIds;
    std::unordered_map<std::uint32_t, std::uint32_t> tangentAccessorIds;
    std::unordered_map<std::uint32_t, std::uint32_t> indexAccessorIds;
    std::unordered_map<std::uint32_t, std::uint32_t> imageTextureIds;
    std::unordered_map<GeometryKey, std::uint32_t, GeometryKeyHash> meshIds;

    const auto getPositionId = [&model, &parsedData, &positionAccessorIds](std::uint32_t accessorIndex) {
        require(accessorIndex != gltf::invalidGltfId, "glTF 图元缺少 POSITION");
        auto [it, inserted] = positionAccessorIds.emplace(
            accessorIndex, parsedDataIdFromSize(parsedData.verticesPositions.size(), "POSITION accessor"));
        if (inserted) {
            parsedData.verticesPositions.push_back(readGltfVec3Accessor(model, accessorIndex, "POSITION"));
        }
        return it->second;
    };

    const auto getUvId = [&model, &parsedData, &uvAccessorIds](std::uint32_t accessorIndex) {
        if (accessorIndex == gltf::invalidGltfId) {
            return gltf::invalidGltfId;
        }
        auto [it, inserted] =
            uvAccessorIds.emplace(accessorIndex, parsedDataIdFromSize(parsedData.uvs.size(), "UV accessor"));
        if (inserted) {
            parsedData.uvs.push_back(readGltfVec2Accessor(model, accessorIndex, "TEXCOORD_0"));
        }
        return it->second;
    };

    const auto getNormalId = [&model, &parsedData, &normalAccessorIds](std::uint32_t accessorIndex) {
        if (accessorIndex == gltf::invalidGltfId) {
            return gltf::invalidGltfId;
        }
        auto [it, inserted] = normalAccessorIds.emplace(
            accessorIndex, parsedDataIdFromSize(parsedData.normals.size(), "NORMAL accessor"));
        if (inserted) {
            parsedData.normals.push_back(readGltfVec3Accessor(model, accessorIndex, "NORMAL"));
        }
        return it->second;
    };

    const auto getTangentId = [&model, &parsedData, &tangentAccessorIds](std::uint32_t accessorIndex) {
        if (accessorIndex == gltf::invalidGltfId) {
            return gltf::invalidGltfId;
        }
        auto [it, inserted] = tangentAccessorIds.emplace(
            accessorIndex, parsedDataIdFromSize(parsedData.tangents.size(), "TANGENT accessor"));
        if (inserted) {
            parsedData.tangents.push_back(readGltfVec4Accessor(model, accessorIndex, "TANGENT"));
        }
        return it->second;
    };

    const auto getIndexId = [&model, &parsedData, &indexAccessorIds](std::uint32_t accessorIndex) {
        require(accessorIndex != gltf::invalidGltfId, "glTF 图元必须有索引");
        auto [it, inserted] =
            indexAccessorIds.emplace(accessorIndex, parsedDataIdFromSize(parsedData.indices.size(), "index accessor"));
        if (inserted) {
            parsedData.indices.push_back(readGltfIndexAccessor(model, accessorIndex));
        }
        return it->second;
    };

    bool foundCamera = parsedData.hasCamera;
    const auto handleCamera = [&model, &parsedData, &foundCamera](const tinygltf::Node& node, const glm::mat4& world) {
        const std::uint32_t cameraIndex = optionalGltfIndex(node.camera);
        if (cameraIndex == gltf::invalidGltfId) {
            return;
        }
        if (foundCamera) {
            return;
        }
        require(cameraIndex < model.cameras.size(), "glTF 相机索引越界");
        parsedData.cameraPos = glm::vec3{world[3]};
        const glm::vec3 forward = glm::normalize(glm::vec3{world * glm::vec4{0.0F, 0.0F, -1.0F, 0.0F}});
        parsedData.cameraLookAt = parsedData.cameraPos + forward;
        parsedData.hasCamera = true;
        foundCamera = true;
    };

    const auto textureIdForMaterial = [&gltfBaseDir, &model, &parsedData, &imageTextureIds](int materialIndex,
                                                                                            bool normalTexture) {
        const std::uint32_t materialIndexU32 = optionalGltfIndex(materialIndex);
        if (materialIndexU32 == gltf::invalidGltfId) {
            return gltf::invalidGltfId;
        }
        require(materialIndexU32 < model.materials.size(), "glTF 材质索引越界");
        const tinygltf::Material& material = model.materials[materialIndexU32];
        const std::uint32_t textureIndex = optionalGltfIndex(
            normalTexture ? material.normalTexture.index : material.pbrMetallicRoughness.baseColorTexture.index);
        return textureIdFromTextureIndex(model, gltfBaseDir, textureIndex, parsedData, imageTextureIds);
    };

    std::function<void(std::uint32_t, const glm::mat4&)> traverseNode;
    traverseNode = [&getIndexId, &getNormalId, &getPositionId, &getTangentId, &getUvId, &handleCamera, &meshIds, &model,
                    &parsedData, &textureIdForMaterial,
                    &traverseNode](std::uint32_t nodeIndex, const glm::mat4& parentTransform) {
        require(nodeIndex != gltf::invalidGltfId && nodeIndex < model.nodes.size(),
                "glTF 场景节点索引越界");
        const tinygltf::Node& gltfNode = model.nodes[nodeIndex];

        const glm::mat4 worldTransform = parentTransform * gltfNodeLocalTransform(gltfNode);

        handleCamera(gltfNode, worldTransform);

        const std::uint32_t gltfMeshIndex = optionalGltfIndex(gltfNode.mesh);
        if (gltfMeshIndex != gltf::invalidGltfId) {
            require(gltfMeshIndex < model.meshes.size(), "glTF 网格索引越界");
            const tinygltf::Mesh& mesh = model.meshes[gltfMeshIndex];
            const std::uint32_t primitiveCount = safeCastToU32(mesh.primitives.size());

            for (std::uint32_t primitiveIndex = 0; primitiveIndex < primitiveCount; ++primitiveIndex) {

                const tinygltf::Primitive& primitive = mesh.primitives[primitiveIndex];

                require(primitive.mode == -1 || primitive.mode == TINYGLTF_MODE_TRIANGLES,
                        "只支持 glTF 三角形图元");

                const auto positionIt = primitive.attributes.find("POSITION");
                require(positionIt != primitive.attributes.end(), "glTF 图元缺少 POSITION");
                const auto uvIt = primitive.attributes.find("TEXCOORD_0");
                const auto normalIt = primitive.attributes.find("NORMAL");
                const auto tangentIt = primitive.attributes.find("TANGENT");

                const Mesh parsedMesh{
                    .name = gltfMeshPrimitiveName(mesh, gltfMeshIndex, primitiveIndex, mesh.primitives.size()),
                    .verticesPositionId = getPositionId(requiredGltfIndex(positionIt->second, "POSITION accessor")),
                    .indicesId = getIndexId(requiredGltfIndex(primitive.indices, "primitive indices accessor")),
                    .uvId = uvIt != primitive.attributes.end() ? getUvId(optionalGltfIndex(uvIt->second))
                                                               : gltf::invalidGltfId,
                    .normalId = normalIt != primitive.attributes.end()
                                    ? getNormalId(optionalGltfIndex(normalIt->second))
                                    : gltf::invalidGltfId,
                    .tangentId = tangentIt != primitive.attributes.end()
                                     ? getTangentId(optionalGltfIndex(tangentIt->second))
                                     : gltf::invalidGltfId,
                };
                const GeometryKey meshKey{
                    .positionId = parsedMesh.verticesPositionId,
                    .indexId = parsedMesh.indicesId,
                    .uvId = parsedMesh.uvId,
                    .normalId = parsedMesh.normalId,
                    .tangentId = parsedMesh.tangentId,
                };
                auto [meshIt, inserted] =
                    meshIds.emplace(meshKey, parsedDataIdFromSize(parsedData.meshes.size(), "mesh"));
                if (inserted) {
                    parsedData.meshes.push_back(parsedMesh);
                }

                Node parsedNode = makeParsedNode(worldTransform);
                parsedNode.name = gltfNameOrFallback(gltfNode.name, "Node", nodeIndex);
                parsedNode.meshId = meshIt->second;
                parsedNode.albedoTextureId = textureIdForMaterial(primitive.material, false);
                parsedNode.normalTextureId = textureIdForMaterial(primitive.material, true);
                parsedData.nodes.push_back(parsedNode);
            }
        }

        for (const int childIndex : gltfNode.children) {
            traverseNode(requiredGltfIndex(childIndex, "child node"), worldTransform);
        }
    };

    require(!model.scenes.empty(), "glTF 文件必须至少包含一个场景");
    const std::uint32_t sceneIndex = model.defaultScene >= 0 ? safeCastToU32(model.defaultScene) : std::uint32_t{0};
    require(sceneIndex < model.scenes.size(), "glTF 默认场景索引越界");
    for (const int nodeIndex : model.scenes[sceneIndex].nodes) {
        traverseNode(requiredGltfIndex(nodeIndex, "root node"), glm::mat4{1.0F});
    }

    require(parsedData.nodes.size() > nodeCountBefore || parsedData.hasCamera != hadCameraBefore,
            "glTF 场景里既没有相机，也没有可渲染的网格图元");
}

// 解析并合并一组 glTF 文件为一个完整场景。
ParsedData parseGltfFiles(std::span<const std::filesystem::path> paths)
{
    require(!paths.empty(), "至少要提供一个 glTF 文件路径");

    ParsedData mergedData{};
    for (const std::filesystem::path& path : paths) {
        appendGltfFile(path, mergedData);
    }

    require(!mergedData.nodes.empty(), "glTF 文件里没有可渲染的网格图元");
    return mergedData;
}

// 按网格 ID 展开几何数据，并为缺失的属性填默认值。
util::MeshGeometryData buildMeshGeometryData(std::uint32_t meshId, const ParsedData& gltfData)
{
    require(meshId < gltfData.meshes.size(), "glTF 节点引用了无效的网格 id");

    const Mesh& mesh = gltfData.meshes[meshId];

    require(mesh.verticesPositionId != gltf::invalidGltfId &&
                mesh.verticesPositionId < gltfData.verticesPositions.size(),
            "glTF 节点引用了无效的 POSITION id");
    require(mesh.indicesId != gltf::invalidGltfId && mesh.indicesId < gltfData.indices.size(),
            "glTF 节点引用了无效的索引 id");

    const std::vector<glm::vec3>& positions = gltfData.verticesPositions[mesh.verticesPositionId];
    const std::vector<std::uint32_t>& sourceIndices = gltfData.indices[mesh.indicesId];

    const std::vector<glm::vec2>* uvs = nullptr;
    const std::vector<glm::vec3>* normals = nullptr;
    const std::vector<glm::vec4>* tangents = nullptr;

    if (mesh.uvId != gltf::invalidGltfId) {
        require(mesh.uvId < gltfData.uvs.size(), "glTF 节点引用了无效的 UV id");
        uvs = &gltfData.uvs[mesh.uvId];
        require(uvs->size() == positions.size(), "glTF 的 UV 数量必须与 POSITION 数量一致");
    }
    if (mesh.normalId != gltf::invalidGltfId) {
        require(mesh.normalId < gltfData.normals.size(), "glTF 节点引用了无效的 NORMAL id");
        normals = &gltfData.normals[mesh.normalId];
        require(normals->size() == positions.size(), "glTF 的 NORMAL 数量必须与 POSITION 数量一致");
    }
    if (mesh.tangentId != gltf::invalidGltfId) {
        require(mesh.tangentId < gltfData.tangents.size(), "glTF 节点引用了无效的 TANGENT id");
        tangents = &gltfData.tangents[mesh.tangentId];
        require(tangents->size() == positions.size(), "glTF 的 TANGENT 数量必须与 POSITION 数量一致");
    }

    MeshGeometryData geometry{};
    geometry.name = gltfNameOrFallback(mesh.name, "Mesh", meshId);
    geometry.vertices.reserve(positions.size());

    for (std::size_t i = 0; i < positions.size(); ++i) {
        geometry.vertices.push_back(PackedVertex{
            .position = positions[i],
            .uv = uvs != nullptr ? (*uvs)[i] : glm::vec2{0.0F},
            .normal = normals != nullptr ? (*normals)[i] : glm::vec3{0.0F, 1.0F, 0.0F},
            .tangent = tangents != nullptr ? (*tangents)[i] : glm::vec4{1.0F, 0.0F, 0.0F, 1.0F},
        });
    }

    geometry.indices.reserve(sourceIndices.size());
    for (const std::uint32_t index : sourceIndices) {
        require(index < positions.size(), "glTF 索引引用了 POSITION 访问器之外的顶点");
        geometry.indices.push_back(index);
    }

    if (tangents == nullptr && uvs != nullptr && normals != nullptr) {
        generateMissingTangents(geometry);
    }
    return geometry;
}

} // namespace gltf

// 数学辅助：欧拉角/模型矩阵、自由相机方向与视图投影矩阵。
namespace math {

// 由欧拉角生成旋转矩阵。
glm::mat4 generateRotation(const glm::vec3& eulerAngles)
{
    return glm::rotate(glm::mat4{1.0F}, eulerAngles.x, glm::vec3{1.0F, 0.0F, 0.0F}) *
           glm::rotate(glm::mat4{1.0F}, eulerAngles.y, glm::vec3{0.0F, 1.0F, 0.0F}) *
           glm::rotate(glm::mat4{1.0F}, eulerAngles.z, glm::vec3{0.0F, 0.0F, 1.0F});
}

// 由位置、欧拉角与缩放生成模型矩阵。
glm::mat4 generateModel(const glm::vec3& pos, const glm::vec3& eulerAngles, const glm::vec3& scale)
{
    return glm::translate(glm::mat4{1.0F}, pos) * generateRotation(eulerAngles) * glm::scale(glm::mat4{1.0F}, scale);
}

// 由相机位置与注视点反解偏航角与俯仰角。
std::pair<float, float> calculateYawPitch(const glm::vec3& cameraPos, const glm::vec3& cameraLookAt)
{
    const glm::vec3 forward = glm::normalize(cameraLookAt - cameraPos);
    return {
        std::atan2(forward.x, -forward.z),
        std::asin(std::clamp(forward.y, -1.0F, 1.0F)),
    };
}

// 由俯仰角与偏航角得到前方向量。
glm::vec3 calculateForward(float pitch, float yaw)
{
    return glm::normalize(glm::vec3{
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        -std::cos(pitch) * std::cos(yaw),
    });
}

// 由前方向与世界up向量叉乘得到右方向量。
glm::vec3 calculateRight(const glm::vec3& forward, const glm::vec3& worldUp)
{
    return glm::normalize(glm::cross(forward, worldUp));
}

// 生成视图投影矩阵，并翻转 Y 轴以适配 Vulkan 的裁剪空间。
glm::mat4 calculateViewProjection(float cameraPitch, float cameraYaw, const glm::vec3& cameraPos, float aspectRatio,
                                  float verticalFieldOfView, float nearPlane, float farPlane)
{
    require(aspectRatio > 0.0F, "视图投影矩阵的宽高比必须为正");
    require(verticalFieldOfView > 0.0F, "视图投影矩阵的垂直视场角必须为正");
    require(verticalFieldOfView < glm::pi<float>(), "视图投影矩阵的垂直视场角必须小于 pi");
    require(nearPlane > 0.0F, "视图投影矩阵的近裁剪面必须为正");
    require(farPlane > nearPlane, "视图投影矩阵的远裁剪面必须大于近裁剪面");

    const glm::vec3 forward = calculateForward(cameraPitch, cameraYaw);
    const glm::mat4 view = glm::lookAt(cameraPos, cameraPos + forward, glm::vec3{0.0F, 1.0F, 0.0F});
    glm::mat4 projection = glm::perspective(verticalFieldOfView, aspectRatio, nearPlane, farPlane);
    projection[1][1] *= -1.0F;
    return projection * view;
}

} // namespace math

// 把 VkResult 映射为可读名称。
const char* vkResultName(VkResult result)
{
    switch (result) {
    case VK_SUCCESS:
        return "VK_SUCCESS";
    case VK_NOT_READY:
        return "VK_NOT_READY";
    case VK_TIMEOUT:
        return "VK_TIMEOUT";
    case VK_EVENT_SET:
        return "VK_EVENT_SET";
    case VK_EVENT_RESET:
        return "VK_EVENT_RESET";
    case VK_INCOMPLETE:
        return "VK_INCOMPLETE";
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED:
        return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST:
        return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED:
        return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_LAYER_NOT_PRESENT:
        return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_TOO_MANY_OBJECTS:
        return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_FRAGMENTED_POOL:
        return "VK_ERROR_FRAGMENTED_POOL";
    case VK_ERROR_UNKNOWN:
        return "VK_ERROR_UNKNOWN";
    default:
        return "VK_RESULT_UNRECOGNIZED";
    }
}

// Vulkan 调用失败时抛出带操作名的异常。
void checkVk(VkResult result, std::string_view operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::format("{} 失败，返回 {}", operation, vkResultName(result)));
    }
}

void require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

// 校验命令行参数并读取可选的 --frame-limit。
std::uint32_t readFrameLimitCLI(int argc, char** argv)
{
    constexpr std::string_view frameLimitArgument = "--frame-limit";

    const auto parseFrameLimit = [](std::string_view value) {
        if (value.empty()) {
            throw std::runtime_error("帧数上限不能为空");
        }

        std::uint64_t frameLimit = 0;
        const char* const begin = value.data();
        const char* const end = begin + value.size();
        const auto [parsedEnd, error] = std::from_chars(begin, end, frameLimit);
        if (error != std::errc{} || parsedEnd != end) {
            throw std::runtime_error("帧数上限必须是非负整数");
        }
        return safeCastToU32(frameLimit);
    };

    constexpr std::string_view importArgument = "--import";

    std::uint32_t frameLimit = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == frameLimitArgument) {
            if (i + 1 >= argc) {
                throw std::runtime_error("--frame-limit 需要一个数值");
            }
            frameLimit = parseFrameLimit(argv[++i]);
        }
        else if (argument == importArgument) {
            if (i + 1 >= argc) {
                throw std::runtime_error("--import 需要一个 glTF 路径");
            }
            ++i; // The path itself is read by main().
        }
        else {
            throw std::runtime_error(std::format("用法：{} [--frame-limit N] [--import <glTF>]", argv[0]));
        }
    }

    return frameLimit;
}

} // namespace util
} // namespace siggraph
