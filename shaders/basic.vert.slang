// 模块说明：顶点着色器。写入顶点位置，并把法线/切线/UV 等插值数据传给片段着色器。
struct CameraData {
    float4x4 viewProjection;
    float4 cameraPosition;
};

struct ObjectData {
    float4x4 model;
};
[[vk::binding(0, 0)]]
ConstantBuffer<CameraData> g_camera;

[[vk::binding(1, 0)]]
StructuredBuffer<ObjectData> g_objects;

// 顶点输入布局；各属性 location 与 vkCmdSetVertexInputEXT 设置的格式一一对应。
struct VertexInput {
    [[vk::location(0)]]
    float3 position;
    [[vk::location(1)]]
    float3 normal;
    [[vk::location(2)]]
    float4 tangent;
    [[vk::location(3)]]
    float2 uv;
};

struct VertexOutput {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
    float3 tangent;
    float tangentSign;
    float2 uv;
};

[shader("vertex")]
// 顶点着色器入口：按 SV_StartInstanceLocation 取出实例模型矩阵，再乘视图投影矩阵。
VertexOutput vertexMain(VertexInput input, uint instanceIndex : SV_InstanceID,
                                           uint baseInstance : SV_StartInstanceLocation) {
    // 实例数据来自存储缓冲，用 firstInstance 作为对象索引。
    ObjectData objectData = g_objects[baseInstance + instanceIndex];

    VertexOutput output;
    float4 worldPosition = mul(objectData.model, float4(input.position, 1.0));
    output.position = mul(g_camera.viewProjection, worldPosition);
    output.uv = input.uv;
    output.normal = normalize(mul((float3x3)objectData.model, input.normal));
    output.tangent = normalize(mul((float3x3)objectData.model, input.tangent.xyz));
    output.tangentSign = input.tangent.w;
    output.worldPosition = worldPosition.xyz;
    return output;
}
