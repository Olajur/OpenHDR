#pragma once

#include "common.h"
#include "config.h"

namespace openhdr {

// Scene adaptation: the conversion follows what is on screen. Each frame a small GPU pass (shaders/scene.hlsl)
// reduces the game image to a kSceneGridWidth x kSceneGridHeight grid, read back a few frames late;
// SceneAdapter smooths the result over time and AdaptToScene turns it into curve adjustments.

constexpr UINT kSceneGridWidth = 64;
constexpr UINT kSceneGridHeight = 36;
// Ring of read-back slots; results are this many frames old. Deep enough that the GPU (which runs a
// few frames ahead) has almost always finished the oldest slot, so reads rarely come back empty.
constexpr UINT kSceneReadbackLatency = 5;

// Constant buffer of shaders/scene.hlsl; layout must match.
struct alignas(16) SceneShaderParams {
    float inputOffset[2];
    float inputSize[2];
    float gridSize[2];
    float decodeGamma;
    float brightThreshold;
    uint32_t flags;
    float pad[3];
};
static_assert(sizeof(SceneShaderParams) == 48);

struct SceneStats {
    float meanLuminance = 0.0f;   // linear, 0..1 (SDR white = 1)
    float brightFraction = 0.0f;  // share of the frame above the expansion shoulder
    float whiteFraction = 0.0f;   // share near SDR white
    float maxLuminance = 0.0f;
};

// Averages a read-back grid (RGBA float rows as written by scene.hlsl).
SceneStats ReduceSceneGrid(const float* rgba, size_t rowPitchFloats);

// Temporal smoothing: each statistic moves towards the latest measurement exponentially, quickly when
// the scene gets brighter (no glaring flash on a bright cut) and slowly when it gets darker (no
// pumping). Measurements arrive irregularly (read-back), so smoothing advances every frame on its own.
class SceneAdapter {
public:
    void Measured(const SceneStats& measured);  // a new (late) measurement, whenever one arrives
    void Advance();                             // once per frame, before using Smoothed()
    bool Valid() const { return valid_; }
    const SceneStats& Smoothed() const { return smoothed_; }

private:
    SceneStats target_;
    SceneStats smoothed_;
    bool valid_ = false;
    LARGE_INTEGER last_{};
};

// What the scene changes in the conversion.
struct SceneAdjustment {
    float peakNits;       // effective peak: less headroom for large bright areas
    float shoulderStart;  // lower in dark scenes (more pop), higher in bright ones (midtones protected)
};

SceneAdjustment AdaptToScene(const Config& cfg, const SceneStats& scene, float paperNits, float peakNits, float fullFrameNits);

// Logs what scene adaptation does: on noticeable changes (at most 4 lines a second), or every frame
// with SceneDebug=1 (milliseconds since start, for measuring the smoothing).
class SceneLog {
public:
    void Report(const Config& cfg, const SceneStats& scene, float peakNits, float shoulderStart);

private:
    float lastPeak_ = -1.0f;
    float lastShoulder_ = -1.0f;
    ULONGLONG lastTick_ = 0;
    ULONGLONG start_ = 0;
};

// The D3D11 measurement pass (injected D3D11 mode and capture mode).
class SceneAnalyzer11 {
public:
    HRESULT Init(ID3D11Device* device);
    // Draws the analysis of `input` (bind state is set and left modified: call inside your own pipeline
    // state) and copies it for read-back. Returns true and fills `out` when an older result is ready.
    bool Measure(ID3D11DeviceContext* context, ID3D11ShaderResourceView* input, const SceneShaderParams& params, SceneStats& out);

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> params_;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> staging_[kSceneReadbackLatency];
    uint64_t written_ = 0;  // frames measured so far
};

}  // namespace openhdr
