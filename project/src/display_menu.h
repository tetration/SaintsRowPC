// Display settings overlay: lists the monitor's display modes and applies
// them live with a 30-second confirm / auto-revert countdown.
//
// Uses the Win32 display settings API directly: the SDL window lives in
// rexruntime.dll's own statically-linked SDL instance, so exe-side SDL calls
// cannot see it. Window state changes still go through rex::ui::Window, and
// the resulting WM_SIZE / WM_DISPLAYCHANGE events reach SDL and the presenter
// through the normal event path.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

// Window modes offered by the display menu. kFullscreen changes the monitor's
// display mode; kBorderless is desktop-mode fullscreen.
enum class WindowMode { kWindowed = 0, kBorderless = 1, kFullscreen = 2 };

// Changes the desktop display mode to width/height for the monitor showing
// the window (temporary; reverted at process exit). Used to restore a saved
// mode at startup. No-op when the mode matches or is unsupported.
void ApplyFullscreenDisplayMode(rex::ui::Window* window, int width, int height);

class DisplaySettingsDialog : public rex::ui::ImGuiDialog {
public:
    // config_path: where confirmed settings are written (saintsrow.toml).
    DisplaySettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer, rex::ui::Window* window,
                          std::filesystem::path config_path);

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    struct Mode {
        int width = 0;
        int height = 0;
        int refresh = 0;
        std::string label;
    };

    // Saved state to return to when a pending change is reverted.
    struct Snapshot {
        WindowMode window_mode = WindowMode::kFullscreen;
        int width = 0;    // windowed window size
        int height = 0;
        int desktop_width = 0;   // desktop display mode at snapshot time
        int desktop_height = 0;
        int desktop_refresh = 0;
    };

    void EnumerateModes();
    void ApplySelection();
    void SetMode(int width, int height, int refresh, WindowMode mode);
    void RestoreDesktopModeForSnapshot(const Snapshot& snapshot);
    void RevertPending();
    void CommitPending();

    rex::ui::Window* window_;
    std::filesystem::path config_path_;
    std::string device_name_;  // Win32 display device (e.g. \\.\DISPLAY1)

    std::vector<Mode> modes_;
    int selected_ = -1;
    int window_mode_ = static_cast<int>(WindowMode::kFullscreen);
    std::string aspect_mode_ = "letterbox";
    std::string aspect_ratio_ = "auto";
    int render_scale_ = 1;  // 0-based index into 1x-4x
    bool desktop_mode_changed_ = false;  // we changed the desktop mode, so we may restore it

    bool pending_ = false;
    double deadline_ = 0.0;
    Snapshot snapshot_;
};
