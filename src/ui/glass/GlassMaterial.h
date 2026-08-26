#pragma once

#include "ui/common/ThemeTokens.h"

#include <QColor>

namespace snapask::ui::glass {

enum class GlassBackdropMode {
    Native,
    Image,
    SolidFallback,
};

enum class GlassMaterialRole {
    Standard,
    Elevated,
    Control,
    ReadableContent,
};

struct GlassMaterial {
    QColor tint;
    QColor edgeBright;
    QColor edgeDim;
    QColor innerHighlight;
    QColor specular;
    QColor shadow;
    QColor hover;
    QColor pressed;

    qreal opacity{1.0};
    qreal radius{18.0};
    qreal highlightStrength{1.0};
    qreal shadowStrength{1.0};
    qreal materialDensity{1.0};
};

[[nodiscard]] inline GlassMaterial materialFor(
    const snapask::ui::ThemeTokenSet& tokens,
    const GlassMaterialRole role = GlassMaterialRole::Standard)
{
    GlassMaterial material{
        role == GlassMaterialRole::Elevated
            ? tokens.glassTintElevated
            : (role == GlassMaterialRole::Control
                   ? tokens.glassControlFill
                   : tokens.glassTint),
        tokens.glassEdgeBright,
        tokens.glassEdgeDim,
        tokens.glassInnerHighlight,
        tokens.glassSpecular,
        tokens.glassShadow,
        tokens.glassHover,
        tokens.glassPressed,
        1.0,
        static_cast<qreal>(role == GlassMaterialRole::Control
                               ? tokens.glassCapsuleRadius
                               : tokens.glassRadius),
        1.0,
        role == GlassMaterialRole::Control ? 0.24 : 1.0,
        1.0,
    };

    if (role == GlassMaterialRole::ReadableContent) {
        material.tint = tokens.elevatedSurface;
        material.opacity = tokens.dark ? 0.94 : 0.9;
        material.highlightStrength = 0.34;
        material.shadowStrength = 0.22;
        material.materialDensity = 1.15;
    }
    return material;
}

}  // namespace snapask::ui::glass
