#pragma once

#include <QDialog>

namespace snapask::ui::onboarding {

class PrivacyNoticeDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PrivacyNoticeDialog(QWidget* parent = nullptr);

signals:
    void privacyAccepted();
    void privacyRejected();

public slots:
    void accept() override;
    void reject() override;

private:
    bool decisionFinalized_{false};
};

}  // namespace snapask::ui::onboarding
