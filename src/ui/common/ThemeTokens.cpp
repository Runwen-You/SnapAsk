#include "ui/common/ThemeTokens.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QStyleHints>

namespace snapask::ui {
namespace {

bool systemUsesDarkColors() {
    auto* guiApplication = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
    if (guiApplication == nullptr) {
        return false;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return guiApplication->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return guiApplication->palette().color(QPalette::Window).lightness() < 128;
#endif
}

QString rgba(const QColor& color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

}  // namespace

ThemeMode ThemeTokens::fromStorage(QStringView value) {
    const QString normalized = value.toString().trimmed().toLower();
    if (normalized == QStringLiteral("light")) {
        return ThemeMode::Light;
    }
    if (normalized == QStringLiteral("dark")) {
        return ThemeMode::Dark;
    }
    return ThemeMode::System;
}

QString ThemeTokens::toStorage(ThemeMode mode) {
    switch (mode) {
    case ThemeMode::Light:
        return QStringLiteral("light");
    case ThemeMode::Dark:
        return QStringLiteral("dark");
    case ThemeMode::System:
        return QStringLiteral("system");
    }
    return QStringLiteral("system");
}

ThemeTokenSet ThemeTokens::resolve(ThemeMode mode) {
    const bool useDark = mode == ThemeMode::Dark || (mode == ThemeMode::System && systemUsesDarkColors());
    if (useDark) {
        return {
            QColor(20, 20, 24, 184),
            QColor(35, 35, 42, 176),
            QColor(49, 49, 58, 210),
            QColor(250, 250, 250),
            QColor(174, 174, 178),
            QColor(255, 255, 255, 44),
            QColor(10, 132, 255),
            QColor(0, 99, 214),
            QColor(255, 69, 58),
            QColor(10, 132, 255, 96),
            QColor(38, 40, 48, 156),
            QColor(52, 54, 64, 196),
            QColor(255, 255, 255, 118),
            QColor(5, 8, 16, 116),
            QColor(255, 255, 255, 76),
            QColor(255, 255, 255, 82),
            QColor(0, 0, 0, 112),
            QColor(255, 255, 255, 30),
            QColor(0, 0, 0, 58),
            QColor(255, 255, 255, 34),
            18,
            10,
            20,
            24,
            140,
            200,
            280,
            180,
            true,
        };
    }

    return {
        QColor(245, 247, 252, 178),
        QColor(255, 255, 255, 174),
        QColor(255, 255, 255, 218),
        QColor(28, 28, 30),
        QColor(99, 99, 102),
        QColor(60, 60, 67, 38),
        QColor(0, 122, 255),
        QColor(0, 92, 204),
        QColor(255, 59, 48),
        QColor(0, 122, 255, 72),
        QColor(248, 251, 255, 132),
        QColor(255, 255, 255, 186),
        QColor(255, 255, 255, 218),
        QColor(72, 82, 101, 68),
        QColor(255, 255, 255, 178),
        QColor(255, 255, 255, 154),
        QColor(24, 31, 46, 76),
        QColor(255, 255, 255, 94),
        QColor(55, 66, 86, 34),
        QColor(255, 255, 255, 112),
        18,
        10,
        20,
        24,
        140,
        200,
        280,
        180,
        false,
    };
}

QString ThemeTokens::styleSheet(const ThemeTokenSet& tokens) {
    return QStringLiteral(R"(
        QWidget {
            color: %1;
            background: transparent;
            font-family: "Segoe UI Variable", "Segoe UI";
            font-size: 10pt;
        }
        QDialog, QMainWindow, QWidget#answerCardWindow {
            background-color: %3;
        }
        QFrame#SettingsCard, QFrame#GlassCard, QFrame#GlassSurface,
        QFrame#providerCard, QFrame#pendingSnapshotPanel,
        QFrame#answerCodeBlock {
            background-color: %2;
            border: 1px solid %5;
            border-radius: %6px;
        }
        QFrame#providerCard, QFrame#advancedOptionsPanel,
        QFrame#answerCodeBlock {
            background-color: %14;
        }
        QLabel, QCheckBox, QRadioButton {
            background: transparent;
        }
        QLabel#SecondaryLabel, QLabel#answerStatusLabel,
        QLabel#answerVersionLabel, QLabel#providerCardDetails,
        QLabel#answerTurnBindingLabel {
            color: %7;
            background: transparent;
        }
        QLineEdit, QPlainTextEdit, QTextEdit, QTextBrowser, QComboBox,
        QKeySequenceEdit, QSpinBox, QDoubleSpinBox {
            min-height: 30px;
            padding: 3px 10px;
            border: 1px solid %8;
            border-radius: %9px;
            background-color: %10;
            selection-background-color: %15;
        }
        QLineEdit:hover, QPlainTextEdit:hover, QTextEdit:hover,
        QTextBrowser:hover, QComboBox:hover, QKeySequenceEdit:hover,
        QSpinBox:hover, QDoubleSpinBox:hover {
            border-color: %11;
        }
        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
        QTextBrowser:focus, QComboBox:focus, QKeySequenceEdit:focus {
            border: 1px solid %11;
        }
        QPlainTextEdit#answerQuestionEdit,
        QPlainTextEdit#answerQuestionEdit:hover,
        QPlainTextEdit#answerQuestionEdit:focus {
            padding: 3px 5px;
            border: 0;
            border-radius: 0;
            background: transparent;
        }
        QPushButton {
            min-height: 30px;
            padding: 3px 12px;
            border: 1px solid %8;
            border-radius: %9px;
            background-color: %10;
        }
        QPushButton:hover, QToolButton:hover {
            border-color: %11;
            background-color: %15;
        }
        QPushButton:pressed, QToolButton:pressed, QToolButton:checked {
            background-color: %12;
        }
        QPushButton:disabled, QToolButton:disabled {
            color: %7;
            background-color: transparent;
        }
        QToolBar, QFrame#CaptureActionBar {
            spacing: 2px;
            padding: 5px;
            border: 1px solid %8;
            border-radius: %6px;
            background-color: %4;
        }
        QToolBar::separator {
            width: 1px;
            margin: 7px 4px;
            background-color: %8;
        }
        QToolButton {
            min-width: 30px;
            min-height: 30px;
            padding: 2px;
            border: 1px solid transparent;
            border-radius: %9px;
            background-color: transparent;
        }
        QComboBox QAbstractItemView {
            color: %13;
            background-color: %14;
            selection-background-color: %15;
            border: 1px solid %16;
            border-radius: %9px;
            padding: 4px;
        }
        QListWidget#settingsSidebar {
            border: 0;
            outline: 0;
            background: transparent;
            padding: 2px;
        }
        QListWidget#settingsSidebar::item {
            min-height: 38px;
            padding: 3px 9px;
            margin: 2px 0;
            border: 1px solid transparent;
            border-radius: %9px;
        }
        QListWidget#settingsSidebar::item:hover {
            background-color: %10;
            border-color: %8;
        }
        QListWidget#settingsSidebar::item:selected {
            color: %13;
            background-color: %17;
            border-color: %18;
        }
        QScrollArea, QScrollArea > QWidget > QWidget,
        QWidget#answerContent, QWidget#providerCardsContainer {
            border: 0;
            background: transparent;
        }
        QScrollBar:vertical {
            width: 9px;
            margin: 2px;
            border: 0;
            background: transparent;
        }
        QScrollBar::handle:vertical {
            min-height: 28px;
            border-radius: 3px;
            background-color: %16;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            height: 0;
            background: transparent;
        }
        QMenu {
            padding: 5px;
            border: 1px solid %18;
            border-radius: %9px;
            background-color: %19;
        }
        QMenu::item {
            min-height: 26px;
            padding: 4px 22px 4px 10px;
            border-radius: 7px;
        }
        QMenu::item:selected {
            background-color: %15;
        }
        QToolTip {
            color: %13;
            padding: 5px 8px;
            border: 1px solid %8;
            border-radius: 7px;
            background-color: %14;
        }
    )")
        .arg(rgba(tokens.textPrimary))
        .arg(rgba(tokens.surface))
        .arg(rgba(tokens.window))
        .arg(rgba(tokens.elevatedSurface))
        .arg(rgba(tokens.border))
        .arg(tokens.panelRadius)
        .arg(rgba(tokens.textSecondary))
        .arg(rgba(tokens.border))
        .arg(tokens.controlRadius)
        .arg(rgba(tokens.surface))
        .arg(rgba(tokens.accent))
        .arg(rgba(tokens.selection))
        .arg(rgba(tokens.textPrimary))
        .arg(rgba(tokens.elevatedSurface))
        .arg(rgba(tokens.selection))
        .arg(rgba(tokens.border))
        .arg(rgba(tokens.glassControlFill))
        .arg(rgba(tokens.glassEdgeBright))
        .arg(rgba(tokens.glassTintElevated));
}

void ThemeTokens::apply(QApplication& application, ThemeMode mode) {
    const ThemeTokenSet tokens = resolve(mode);
    QPalette palette;
    palette.setColor(QPalette::Window, tokens.window);
    palette.setColor(QPalette::WindowText, tokens.textPrimary);
    palette.setColor(QPalette::Base, tokens.surface);
    palette.setColor(QPalette::AlternateBase, tokens.elevatedSurface);
    palette.setColor(QPalette::Text, tokens.textPrimary);
    palette.setColor(QPalette::Button, tokens.surface);
    palette.setColor(QPalette::ButtonText, tokens.textPrimary);
    palette.setColor(QPalette::Highlight, tokens.accent);
    palette.setColor(QPalette::HighlightedText, QColor(Qt::white));
    palette.setColor(QPalette::PlaceholderText, tokens.textSecondary);
    application.setPalette(palette);
    application.setStyleSheet(styleSheet(tokens));
}

}  // namespace snapask::ui
