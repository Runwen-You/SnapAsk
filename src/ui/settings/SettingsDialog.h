#pragma once

#include "ui/common/ThemeTokens.h"

#include <QDialog>
#include <QKeySequence>

class QComboBox;
class QLabel;
class QKeySequenceEdit;
class QListWidget;
class QStackedWidget;

namespace snapask::ui {

class ProviderSettingsWidget;

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    void setHotkeyStatus(bool registered, const QString& message = {});
    [[nodiscard]] ProviderSettingsWidget* providerSettingsWidget() const noexcept;

signals:
    void themeModeChanged(snapask::ui::ThemeMode mode);
    void captureHotkeyChanged(const QKeySequence& sequence);
    void captureNowRequested();

private:
    void loadSettings();
    void saveThemeMode(ThemeMode mode);
    void saveCaptureHotkey();

    QComboBox* themeModeCombo_ = nullptr;
    QKeySequenceEdit* captureHotkeyEdit_ = nullptr;
    QLabel* hotkeyStatusLabel_ = nullptr;
    QLabel* pageTitleLabel_ = nullptr;
    QListWidget* sidebar_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    ProviderSettingsWidget* providerSettingsWidget_ = nullptr;
};

}  // namespace snapask::ui
