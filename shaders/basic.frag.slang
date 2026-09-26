// 模块说明：片段着色器（反照率 + 法线贴图）。采样两张贴图并做 Blinn-Phong 着色。
struct CameraData {
    float4x4 viewProjection;
    float4 cameraPosition;
};

struct LightData {
    float3 pointPosition;
    float pointIntensity;
    float3 ambientColor;
    float ambientIntensity;
};

[[vk::binding(0, 0)]]
ConstantBuffer<CameraData> g_camera;

[[vk::binding(2, 0)]]
ConstantBuffer<LightData> g_pointLight;

[[vk::binding(3, 0)]]
Sampler2D g_albedoTexture;

[[vk::binding(4, 0)]]
Sampler2D g_normalTexture;

struct FragmentInput {
    float3 worldPosition;
    float3 normal;
    float3 tangent;
    float tangentSign;
    float2 uv;
};

// 切线空间法线贴图：用 TBN 基把贴图法线变换到世界空间。
float3 CalculateNormalMap(FragmentInput input, Sampler2D normalTexture)
{
    float3 normal = normalize(input.normal);
    float3 tangent = normalize(input.tangent - normal * dot(normal, input.tangent));
    float3 bitangent = normalize(cross(normal, tangent) * input.tangentSign);
    float3 tangentSpaceNormal = normalTexture.Sample(input.uv).xyz * 2.0 - 1.0;

    return normalize(tangent * tangentSpaceNormal.x + bitangent * tangentSpaceNormal.y + normal * tangentSpaceNormal.z);
}

// Blinn-Phong 光照：带距离衰减的点光加上环境光。
float3 illuminate(float3 albedo, float3 worldPosition, float3 normal, LightData pointLight)
{
    float3 lightDelta = pointLight.pointPosition - worldPosition;
    float lightDistance = length(lightDelta);
    float3 lightDirection = lightDelta / max(lightDistance, 0.001);
    float attenuation = 1.0 / (1.0 + 0.09 * lightDistance + 0.032 * lightDistance * lightDistance);

    float diffuseAmount = max(dot(normal, lightDirection), 0.0);
    float3 viewDirection = normalize(g_camera.cameraPosition.xyz - worldPosition);
    float3 halfVector = normalize(lightDirection + viewDirection);
    float specularAmount = pow(max(dot(normal, halfVector), 0.0), 32.0) * 0.2;

    float3 ambient = albedo * pointLight.ambientColor * pointLight.ambientIntensity;
    float3 direct =
        (albedo * diffuseAmount + specularAmount) * pointLight.ambientColor * pointLight.pointIntensity * attenuation;

    return ambient + direct;
}

[shader("fragment")]
// 片段着色器入口：采样反照率，丢弃半透明像素，再按法线贴图结果着色。
float4 fragmentMain(FragmentInput input) {
    // 两张贴图都由描述符堆的 push index 选择。
    float4 texel = g_albedoTexture.Sample(input.uv);
    if (texel.a < 0.5) {
        discard;
    }

    float3 normal = CalculateNormalMap(input, g_normalTexture);
    return float4(illuminate(texel.rgb, input.worldPosition, normal, g_pointLight), 1.0);
}
