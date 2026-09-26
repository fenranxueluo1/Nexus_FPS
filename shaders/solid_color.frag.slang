// 模块说明：片段着色器（纯色）。用于没有贴图的物体，例如 Vulkan 标志。
struct SolidColorData {
    float4 color;
};

[[vk::binding(2, 0)]]
ConstantBuffer<SolidColorData> g_solidColor;

struct FragmentInput {
    float3 worldPosition;
    float3 normal;
    float3 tangent;
    float tangentSign;
    float2 uv;
};

// 片段着色器入口：直接输出颜色缓冲中的固定颜色。
[shader("fragment")] float4 fragmentMain(FragmentInput input) : SV_Target
{
    float4 color = g_solidColor.color;

    // 此分支不会执行，只是让顶点着色器的全部输出都被"使用"，避免验证层的跨阶段性能警告。
    if (color.a < 0.0f) {
        color.xy += input.uv * 0.001;
        color.w += input.tangentSign * 0.001;
        color.xyz += input.tangent * 0.001;
        color.xyz += input.normal * 0.001;
        color.xyz += input.worldPosition * 0.001;
    }

    return color;
}
