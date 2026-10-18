#pragma once
#include <string_view>

namespace WindowEffectsPolicy {
inline bool materialAllowed(bool enabled, bool reduceTransparency, bool highContrast)
{
    return enabled && !reduceTransparency && !highContrast;
}
inline int cornerRadius(bool maximized, bool fullscreen)
{
    return maximized || fullscreen ? 0 : 10;
}
enum class TitlebarAction { None, Minimize, Zoom };
inline TitlebarAction titlebarDoubleClickAction(std::string_view preference, bool fullscreen,
                                              bool attachedSheet, bool canMinimize, bool canZoom)
{
    if (fullscreen || attachedSheet || preference == "None") return TitlebarAction::None;
    if (preference == "Minimize") return canMinimize ? TitlebarAction::Minimize : TitlebarAction::None;
    // Maximize/Zoom and an unset preference use AppKit zoom, not Qt fullscreen/restore.
    return canZoom ? TitlebarAction::Zoom : TitlebarAction::None;
}
}
