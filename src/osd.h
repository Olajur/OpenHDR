#pragma once

#include "common.h"
#include "config.h"

#include <vector>

namespace openhdr {

// In-game settings panel, toggled with its hotkey (Alt+1 by default). Drawn with GDI into a small CPU bitmap only when
// its content changes; the presenter uploads it and blends it over the HDR output.
// Arrow keys navigate while it is open (they also still reach the game).
class Osd {
public:
    // `store` holds the settings of the game the panel belongs to.
    explicit Osd(ConfigStore& store);

    // Keyboard handling; setting changes go to the per-game ini. Call once per frame. `togglePanel`:
    // the panel hotkey was pressed (see HotkeyWatcher).
    void Update(bool focused, bool togglePanel, float autoPeakNits, bool& compare);
    bool Visible() const { return visible_; }

    // Re-renders the panel if anything it shows changed; returns true when Pixels() changed.
    bool Rasterize(UINT targetHeight, const Config& cfg, float autoPeakNits, bool compare);
    const std::vector<uint32_t>& Pixels() const { return pixels_; }  // RGBA8, top row first
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }

private:
    // Fires on press, then repeats while held.
    struct RepeatKey {
        bool down = false;
        ULONGLONG next = 0;
        bool Poll(int vk);
    };

    void Adjust(int direction, float autoPeakNits, bool& compare);

    ConfigStore& store_;
    bool visible_ = false;
    int selected_ = 0;
    RepeatKey up_, down_, left_, right_;

    std::vector<uint32_t> pixels_;
    UINT width_ = 0;
    UINT height_ = 0;
    // What the current pixels show.
    uint32_t shownVersion_ = 0;
    int shownSelected_ = -1;
    bool shownCompare_ = false;
    UINT shownTargetHeight_ = 0;
};

// The panel's GPU copy. Upload after Rasterize; `changed` is what Rasterize returned.
struct OsdTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;

    void Upload(ID3D11Device* device, ID3D11DeviceContext* context, const Osd& osd, bool changed);
};

}  // namespace openhdr
