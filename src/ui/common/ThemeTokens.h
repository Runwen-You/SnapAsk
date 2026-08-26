#pragma once

#include <QColor>
#include <QPalette>
#include <QString>
#include <QStringView>

class QApplication;

namespace snapask::ui {

enum class ThemeMode {
    System,
    Light,
    Dark,
};

struct ThemeTokenSet {
    QColor window;
    QColor surface;
    QColor elevatedSurface;
    QColor textPrimary;
    QColor textSecondary;
    QColor border;
    QColor accent;
    QColor accentPressed;
    QColor danger;
    QColor selection;
    QColor glassTint;
    QColor glassTintElevated;
    QColor glassEdgeBright;
    QColor glassEdgeDim;
    QColor glassInnerHighlight;
    QColor glassSpecular;
    QColor glassShadow;
    QColor glassHover;
    QColor glassPressed;
    QColor glassControlFill;
    int panelRadius = 18;
    int controlRadius = 10;
    int glassRadius = 20;
    int glassCapsuleRadius = 24;
    int animationFastMs = 140;
    int animationNormalMs = 200;
    int animationSlowMs = 280;
    int animationDurationMs = 180;
    bool dark = false;
};

class ThemeTokens final {
public:
    ThemeTokens() = delete;

    [[nodiscard]] static ThemeMode fromStorage(QStringView value);
    [[nodiscard]] static QString toStorage(ThemeMode mode);
    [[nodiscard]] static ThemeTokenSet resolve(ThemeMode mode);
    [[nodiscard]] static QString styleSheet(const ThemeTokenSet& tokens);
    static void apply(QApplication& application, ThemeMode mode);
};

}  // namespace snapask::ui
