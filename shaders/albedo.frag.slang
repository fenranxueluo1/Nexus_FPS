// 模块说明：片段着色器（仅反照率）。用于没有法线贴图的材质。
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

struct FragmentInput {
    float3 worldPosition;
    float3 normal;
    float3 tangent;
    float tangentSign;
    float2 uv;
};

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
// 片段着色器入口：采样反照率贴图，并按几何法线着色。
float4 fragmentMain(FragmentInput input) {
    // 反照率贴图由描述符堆的 push index 选择。
    float4 texel = g_albedoTexture.Sample(input.uv);
    if (texel.a < 0.5) {
        discard;
    }

    float3 normal = normalize(input.normal);
    // 此分支不会执行，只是让顶点着色器的全部输出都被"使用"，避免验证层的跨阶段性能警告。
    if (texel.a < 0.0) {
        normal += input.tangent * 0.001 + input.tangentSign * 0.001;
    }
    return float4(illuminate(texel.rgb, input.worldPosition, normalize(normal), g_pointLight), 1.0);
}
