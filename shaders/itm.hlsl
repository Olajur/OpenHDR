// Inverse tone mapping: game SDR back buffer -> scRGB (linear BT.709, 1.0 = 80 nits), written as is or,
// with FLAG_OUTPUT_PQ, encoded as HDR10 (PQ, BT.2020) for 10-bit swap chains.
//
// Pipeline per pixel:
//   debanding (SDR code values) -> dither -> decode with the contrast gamma -> saturation
//   -> highlight expansion above ShoulderStart up to the display peak -> scale by paper white.
// Contrast and saturation come from the -100..+100 sliders (see itm_params.cpp).

cbuffer Params : register(b0)
{
    float PaperWhite;          // scRGB units
    float PeakWhite;           // scRGB units
    float ShoulderStart;       // linear SDR luminance where expansion begins
    float ExpansionPower;      // >= 1, steepness of the expansion
    float Saturation;          // factor, 1.05 at the default slider position
    float DecodeGamma;         // contrast: 2.0 at the default slider position
    float HighlightSaturation; // 0 = per-channel expansion, 1 = luminance-only expansion
    float DitherAmount;        // in input code values (LSBs)
    uint  InputMode;           // INPUT_*
    uint  Flags;               // FLAG_*
    uint  FrameIndex;
    float SplitX;              // compare split in pixels
    float InputLsb;            // one code value of the game's format
    float OsdWhite;            // scRGB units for the settings panel's white
    float OsdOpacity;
    float DebandThreshold;     // in code values (0..1 range); 0 disables debanding
    float4 OsdRect;            // settings panel: x, y, width, height in pixels
    float2 InputOffset;        // input texel shown at output pixel (0, 0): capture mode skips the window frame
    float DebandRadius;        // in pixels
    float _pad;
};

#define INPUT_GAMMA22 0
#define INPUT_SRGB    1
#define INPUT_PQ      2

#define FLAG_PROCESS     1
#define FLAG_COMPARE     2
#define FLAG_SRV_IS_SRGB 4 // the view already decoded sRGB (game used an _SRGB back buffer)
#define FLAG_OSD         8
#define FLAG_OUTPUT_PQ   16 // HDR10 swap chain: encode the result as PQ in BT.2020

Texture2D<float4> SdrInput : register(t0);
Texture2D<float4> OsdPanel : register(t1);

static const float3 LUMA_709 = float3(0.2126, 0.7152, 0.0722);

void VSMain(uint id : SV_VertexID, out float4 position : SV_Position)
{
    // Full-screen triangle.
    float2 uv = float2((id << 1) & 2, id & 2);
    position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 SrgbToLinear(float3 v)
{
    return v <= 0.04045 ? v / 12.92 : pow(max((v + 0.055) / 1.055, 0.0), 2.4);
}

float3 LinearToSrgb(float3 v)
{
    return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

float3 PqToNits(float3 e)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(e), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0) / (c2 - c3 * p), 1.0 / m1);
}

static const float3x3 BT2020_TO_BT709 = {
     1.6604910, -0.5876411, -0.0728499,
    -0.1245505,  1.1328999, -0.0083494,
    -0.0181508, -0.1005789,  1.1187297,
};

static const float3x3 BT709_TO_BT2020 = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950,
};

float3 NitsToPq(float3 nits)
{
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float InterleavedGradientNoise(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// The game's code values at a texel, as display-encoded SDR (undoing an _SRGB view's decode).
float3 LoadCode(int2 p)
{
    float3 v = SdrInput.Load(int3(p, 0)).rgb;
    return (Flags & FLAG_SRV_IS_SRGB) ? LinearToSrgb(saturate(v)) : v;
}

// Debanding: SDR gradients (skies, fog) are stored in visible steps of one code value, and the
// highlight expansion makes those steps bigger. In a flat area, where four neighbours at a random
// distance all lie within the threshold, the pixel becomes their average, turning steps into a
// smooth ramp. Any real edge or texture exceeds the threshold, so it is left alone. The sample
// pattern is fixed per pixel (no per-frame noise), so it doesn't shimmer.
float3 Deband(int2 p, float3 v)
{
    uint w, h;
    SdrInput.GetDimensions(w, h);
    const float angle = InterleavedGradientNoise(float2(p)) * 6.2831853;
    const float radius = DebandRadius * (0.5 + 0.5 * InterleavedGradientNoise(float2(p) * 1.37 + 13.0));
    float3 sum = v;
    bool flat = true;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        const float a = angle + i * 1.5707963;
        const int2 q = clamp(p + int2(round(float2(cos(a), sin(a)) * radius)), int2(0, 0), int2(w, h) - 1);
        const float3 s = LoadCode(q);
        flat = flat && all(abs(s - v) <= DebandThreshold);  // any edge keeps the original
        sum += s;
    }
    return flat ? sum / 5.0 : v;
}

// Maps linear SDR luminance (1 = paper white) to output luminance relative to paper white.
// Identity below the shoulder; above it adds (peak/paper - 1) * x^power, so SDR white lands on the
// display peak and the curve joins the identity with matching slope (no visible kink).
float ExpandLuminance(float y)
{
    if (y <= ShoulderStart) return y;
    float x = saturate((y - ShoulderStart) / (1.0 - ShoulderStart));
    float headroom = max(PeakWhite / PaperWhite - 1.0, 0.0);
    return y + headroom * pow(x, ExpansionPower);
}

// An SDR pixel converted to scRGB.
float3 ConvertSdr(float4 position, int2 p)
{
    float3 v = LoadCode(p);
    if (DebandThreshold > 0) v = Deband(p, v);

    if (DitherAmount > 0)
    {
        // Triangular noise of +-1 code value: the expansion multiplies quantization steps in the
        // highlights, and this turns the resulting bands into invisible grain. Pure black and white stay exact.
        float2 offset = (FrameIndex & 63) * float2(5.588238, 3.137212);
        float n = InterleavedGradientNoise(position.xy + offset) + InterleavedGradientNoise(position.xy + offset + 117.3) - 1.0;
        float3 keep = (v > 0.0) * (v < 1.0);
        v = saturate(v + n * DitherAmount * InputLsb * keep);
    }

    v = saturate(v);
    // Reference: plain SDR at paper white (gamma 2.2), which is what an SDR monitor shows.
    const float3 sdr = InputMode == INPUT_SRGB ? SrgbToLinear(v) : pow(v, 2.2);
    if ((Flags & FLAG_COMPARE) && position.x < SplitX)
    {
        return position.x > SplitX - 2.0 ? PaperWhite.xxx : sdr * PaperWhite;
    }
    if (!(Flags & FLAG_PROCESS)) return sdr * PaperWhite;

    // Contrast is the decoding gamma: lower lifts shadows and midtones, higher deepens them.
    float3 c = InputMode == INPUT_SRGB ? pow(SrgbToLinear(v), DecodeGamma / 2.2) : pow(v, DecodeGamma);
    float y = dot(c, LUMA_709);
    c = max(y + (c - y) * Saturation, 0.0);
    y = dot(c, LUMA_709);

    // Luminance-only expansion keeps highlight colour; per-channel expansion pushes bright
    // saturated areas toward white, like real overexposure. Blend between the two.
    float3 byLuminance = c * (ExpandLuminance(y) / max(y, 1e-6));
    float3 byChannel = float3(ExpandLuminance(c.r), ExpandLuminance(c.g), ExpandLuminance(c.b));
    c = lerp(byChannel, byLuminance, HighlightSaturation);

    return min(c * PaperWhite, PeakWhite);
}

// The game's pixel converted to scRGB. (One return path: fxc misreads early returns around loops.)
float3 ConvertPixel(float4 position)
{
    const int2 p = int2(position.xy + InputOffset);
    float3 result;
    if (InputMode == INPUT_PQ)
    {
        // The game already renders HDR10: decode and pass through.
        result = mul(BT2020_TO_BT709, PqToNits(SdrInput.Load(int3(p, 0)).rgb)) / 80.0;
    }
    else
    {
        result = ConvertSdr(position, p);
    }
    return result;
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    float3 color = ConvertPixel(position);
    if (Flags & FLAG_OSD)
    {
        float2 local = position.xy - OsdRect.xy;
        if (all(local >= 0.0) && all(local < OsdRect.zw))
        {
            // The panel is drawn in sRGB-ish GDI colours; show its white at OsdWhite. The scene behind
            // is capped at that level first, so 1000-nit highlights can't wash out the text.
            float3 panel = pow(saturate(OsdPanel.Load(int3(local, 0)).rgb), 2.2) * OsdWhite;
            color = lerp(min(color, OsdWhite), panel, OsdOpacity);
        }
    }
    if (Flags & FLAG_OUTPUT_PQ) color = NitsToPq(max(mul(BT709_TO_BT2020, color), 0.0) * 80.0);
    return float4(color, 1);
}
