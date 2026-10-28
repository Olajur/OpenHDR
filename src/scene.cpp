#include "scene.h"

#include "g_ScenePS.h"
#include "g_SceneVS.h"

#include <algorithm>
#include <cmath>

namespace openhdr {
namespace {

float SmoothStep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Time constants of the temporal smoothing, in seconds.
constexpr float kBrightenSeconds = 0.25f;  // towards a brighter scene: react fast, avoid a glaring flash
constexpr float kDarkenSeconds = 1.0f;     // towards a darker scene: ease in, avoid pumping

float Approach(float current, float target, float dt) {
    const float tau = target > current ? kBrightenSeconds : kDarkenSeconds;
    return current + (target - current) * (1.0f - std::exp(-dt / tau));
}

}  // namespace

SceneStats ReduceSceneGrid(const float* rgba, size_t rowPitchFloats) {
    double mean = 0.0, bright = 0.0, white = 0.0;
    float peak = 0.0f;
    for (UINT y = 0; y < kSceneGridHeight; ++y) {
        const float* row = rgba + y * rowPitchFloats;
        for (UINT x = 0; x < kSceneGridWidth; ++x) {
            mean += row[x * 4 + 0];
            bright += row[x * 4 + 1];
            white += row[x * 4 + 2];
            peak = std::max(peak, row[x * 4 + 3]);
        }
    }
    const double cells = double(kSceneGridWidth) * kSceneGridHeight;
    return {float(mean / cells), float(bright / cells), float(white / cells), peak};
}

void SceneAdapter::Measured(const SceneStats& measured) {
    target_ = measured;
    if (!valid_) {
        smoothed_ = measured;  // first measurement: start there, no fade-in
        valid_ = true;
        QueryPerformanceCounter(&last_);
    }
}

void SceneAdapter::Advance() {
    if (!valid_) return;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float dt = std::clamp(float(now.QuadPart - last_.QuadPart) / float(frequency.QuadPart), 0.0f, 0.1f);
    last_ = now;
    smoothed_.meanLuminance = Approach(smoothed_.meanLuminance, target_.meanLuminance, dt);
    smoothed_.brightFraction = Approach(smoothed_.brightFraction, target_.brightFraction, dt);
    smoothed_.whiteFraction = Approach(smoothed_.whiteFraction, target_.whiteFraction, dt);
    smoothed_.maxLuminance = Approach(smoothed_.maxLuminance, target_.maxLuminance, dt);
}

SceneAdjustment AdaptToScene(const Config& cfg, const SceneStats& scene, float paperNits, float peakNits, float fullFrameNits) {
    SceneAdjustment a{peakNits, cfg.shoulderStart};
    const float k = cfg.sceneAdaptation;
    if (k <= 0.0f) return a;

    // How much of the frame is bright (a sky, snow, a white menu), and how dark the scene is overall
    // (mean linear luminance: a typical daylight frame is around 0.15-0.25, a night scene under 0.05).
    const float area = SmoothStep(0.10f, 0.50f, scene.brightFraction);
    const float dark = 1.0f - SmoothStep(0.02f, 0.10f, scene.meanLuminance);

    // Highlight headroom: up to half less when large areas are bright, so they don't glare...
    float peak = paperNits + std::max(peakNits - paperNits, 0.0f) * (1.0f - 0.5f * k * area);
    // ...and no more than the display can sustain over a large area, if it says so.
    if (fullFrameNits > paperNits && fullFrameNits < peakNits) {
        peak = std::min(peak, peakNits + (fullFrameNits - peakNits) * k * area);
    }
    a.peakNits = peak;

    // Shoulder: down to 60% of its base in dark scenes (lamps and fire get more pop), up halfway to
    // 0.5 in bright ones (midtones stay as in SDR).
    const float base = cfg.shoulderStart;
    a.shoulderStart = base * (1.0f - 0.4f * k * dark) + std::max(0.5f - base, 0.0f) * 0.5f * k * area;
    return a;
}

void SceneLog::Report(const Config& cfg, const SceneStats& scene, float peakNits, float shoulderStart) {
    const ULONGLONG now = GetTickCount64();
    if (!start_) start_ = now;
    const bool changed = std::abs(peakNits - lastPeak_) > 0.05f * std::max(lastPeak_, 1.0f) || std::abs(shoulderStart - lastShoulder_) > 0.02f;
    if (!cfg.sceneDebug && (!changed || now - lastTick_ < 250)) return;
    Log("Scene%s t=%llu ms: mean %.3f, bright %.3f, white %.3f, max %.2f -> peak %.0f nits, shoulder %.3f",
        cfg.sceneDebug ? "Dbg" : "", now - start_, scene.meanLuminance, scene.brightFraction, scene.whiteFraction, scene.maxLuminance,
        peakNits, shoulderStart);
    lastPeak_ = peakNits;
    lastShoulder_ = shoulderStart;
    lastTick_ = now;
}

HRESULT SceneAnalyzer11::Init(ID3D11Device* device) {
    device_ = device;
    HRESULT hr = device->CreateVertexShader(g_SceneVS, sizeof(g_SceneVS), nullptr, &vs_);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(g_ScenePS, sizeof(g_ScenePS), nullptr, &ps_);
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(SceneShaderParams);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(hr = device->CreateBuffer(&cb, nullptr, &params_))) return hr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kSceneGridWidth;
    td.Height = kSceneGridHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(hr = device->CreateTexture2D(&td, nullptr, &target_))) return hr;
    if (FAILED(hr = device->CreateRenderTargetView(target_.Get(), nullptr, &rtv_))) return hr;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& staging : staging_) {
        if (FAILED(hr = device->CreateTexture2D(&td, nullptr, &staging))) return hr;
    }
    return S_OK;
}

bool SceneAnalyzer11::Measure(ID3D11DeviceContext* context, ID3D11ShaderResourceView* input, const SceneShaderParams& params,
                              SceneStats& out) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(params_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &params, sizeof(params));
        context->Unmap(params_.Get(), 0);
    }
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, float(kSceneGridWidth), float(kSceneGridHeight), 0.0f, 1.0f};
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->PSSetShader(ps_.Get(), nullptr, 0);
    context->PSSetConstantBuffers(0, 1, params_.GetAddressOf());
    context->PSSetShaderResources(0, 1, &input);
    context->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);

    // Ring of staging copies: read the oldest one, which the GPU has long finished, without waiting.
    context->CopyResource(staging_[written_ % kSceneReadbackLatency].Get(), target_.Get());
    ++written_;
    if (written_ < kSceneReadbackLatency) return false;
    ID3D11Texture2D* oldest = staging_[written_ % kSceneReadbackLatency].Get();
    if (FAILED(context->Map(oldest, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return false;
    out = ReduceSceneGrid(static_cast<const float*>(mapped.pData), mapped.RowPitch / sizeof(float));
    context->Unmap(oldest, 0);
    return true;
}

}  // namespace openhdr
