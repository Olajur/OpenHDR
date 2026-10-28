// Scene analysis: reduces the game's SDR image to a small grid of statistics that the CPU reads back
// (a few frames late) to adapt the conversion to the scene. Drawn as a full-screen triangle into a
// kGridWidth x kGridHeight float target; each output texel summarises one cell of the image with a
// 4x4 grid of samples:
//   r = mean linear luminance, g = fraction above BrightThreshold, b = fraction near white, a = maximum.

cbuffer SceneParams : register(b0)
{
    float2 InputOffset;      // top-left of the analysed region in SdrInput (capture mode skips the window frame)
    float2 InputSize;        // size of the analysed region in pixels
    float2 GridSize;         // output grid, e.g. 64 x 36
    float DecodeGamma;       // same decode as the conversion (contrast)
    float BrightThreshold;   // linear luminance counted as "bright" (the expansion shoulder)
    uint Flags;              // FLAG_SRV_IS_SRGB as in itm.hlsl
    float3 _pad;
};

#define FLAG_SRV_IS_SRGB 4

Texture2D<float4> SdrInput : register(t0);

static const float3 LUMA_709 = float3(0.2126, 0.7152, 0.0722);
static const float NEAR_WHITE = 0.85;

void VSMain(uint id : SV_VertexID, out float4 position : SV_Position)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 LinearToSrgb(float3 v)
{
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    const float2 cellSize = InputSize / GridSize;
    const float2 cellOrigin = InputOffset + floor(position.xy) * cellSize;
    float sum = 0.0, bright = 0.0, white = 0.0, peak = 0.0;
    [unroll] for (int y = 0; y < 4; ++y)
    {
        [unroll] for (int x = 0; x < 4; ++x)
        {
            const int2 p = int2(cellOrigin + (float2(x, y) + 0.5) * 0.25 * cellSize);
            float3 v = SdrInput.Load(int3(p, 0)).rgb;
            if (Flags & FLAG_SRV_IS_SRGB) v = LinearToSrgb(saturate(v));
            const float lum = dot(pow(saturate(v), DecodeGamma), LUMA_709);
            sum += lum;
            bright += lum > BrightThreshold ? 1.0 : 0.0;
            white += lum > NEAR_WHITE ? 1.0 : 0.0;
            peak = max(peak, lum);
        }
    }
    return float4(sum, bright, white, 0.0) / 16.0 + float4(0, 0, 0, peak);
}
