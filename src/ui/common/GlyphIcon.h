#pragma once

#include <QColor>
#include <QIcon>

namespace snapask::ui {

enum class Glyph {
    Select,
    Rectangle,
    Arrow,
    Text,
    Mosaic,
    Color,
    Undo,
    Redo,
    Clear,
    Restore,
    Copy,
    Save,
    Pin,
    Ask,
    Send,
    Stop,
    Close,
    Capture,
    General,
    Service,
    Privacy,
    Sun,
    Moon,
    System,
};

// Small code-native line icons keep the floating screenshot UI sharp at every
// monitor scale without shipping a second raster asset pipeline.
[[nodiscard]] QIcon glyphIcon(
    Glyph glyph,
    QColor foreground = QColor(38, 38, 42));

}  // namespace snapask::ui
