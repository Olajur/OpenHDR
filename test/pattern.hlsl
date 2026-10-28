// SDR test pattern for sdr_test. Outputs display-encoded SDR values, like a game writing to an
// 8-bit back buffer: grey ramps, colour ramps, and a simple scene with a moving sun.

cbuffer Frame : register(b0)
{
    float2 Resolution;
    float Time;
    float Mode;  // 0 test pattern, 1 dark scene, 2 bright scene (scene adaptation tests)
};

// A night scene: dark ground and sky, one small bright lamp.
float3 DarkScene(float2 uv)
{
    float3 color = uv.y < 0.6 ? float3(0.03, 0.04, 0.08) : float3(0.05, 0.04, 0.03);
    const float d = length((uv - float2(0.5, 0.5)) * float2(Resolution.x / Resolution.y, 1));
    color = lerp(color, float3(1.0, 0.9, 0.7), saturate(exp(-d * 30.0)));
    if (d < 0.02) color = 1.0;
    return color;
}

// A snowfield under a bright sky: most of the frame is near white.
float3 BrightScene(float2 uv)
{
    return uv.y < 0.5 ? lerp(float3(0.85, 0.92, 1.0), float3(0.95, 0.97, 1.0), uv.y * 2.0) : float3(0.93, 0.94, 0.96);
}

void VSMain(uint id : SV_VertexID, out float4 position : SV_Position)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    float2 uv = position.xy / Resolution;
    if (Mode == 1.0) return float4(DarkScene(uv), 1);
    if (Mode == 2.0) return float4(BrightScene(uv), 1);

    if (uv.y < 0.08) return float4(uv.xxx, 1);                      // smooth grey ramp
    if (uv.y < 0.10) return float4((floor(uv.x * 16) / 15).xxx, 1);  // 16-step grey

    if (uv.y < 0.40)
    {
        static const float3 colors[6] = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1),
                                          float3(0, 1, 1), float3(1, 0, 1), float3(1, 1, 0) };
        int band = min(int((uv.y - 0.10) / 0.05), 5);
        return float4(colors[band] * uv.x, 1);
    }

    float t = (uv.y - 0.40) / 0.60;
    float3 color = t < 0.65 ? lerp(float3(0.20, 0.40, 0.80), float3(0.70, 0.85, 0.95), t / 0.65)
                            : lerp(float3(0.25, 0.35, 0.15), float3(0.08, 0.10, 0.05), (t - 0.65) / 0.35);

    float2 sun = float2(0.5 + 0.3 * sin(Time * 0.3), 0.55);
    float d = length((uv - sun) * float2(Resolution.x / Resolution.y, 1));
    color = lerp(color, 1.0.xxx, saturate(exp(-d * 18.0)));
    if (d < 0.03) color = 1.0;

    // Stand-in for a HUD element: near-white UI box.
    if (uv.x > 0.03 && uv.x < 0.25 && uv.y > 0.90 && uv.y < 0.96) color = 0.95;
    return float4(color, 1);
}
