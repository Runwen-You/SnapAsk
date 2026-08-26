#include "ui/onboarding/PrivacyNoticeDialog.h"
#include "ui/glass/GlassButton.h"
#include "ui/glass/GlassSurface.h"

#include <QDialog>
#include <QHostAddress>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTest>

using snapask::ui::onboarding::PrivacyNoticeDialog;

namespace {

template <typename Widget>
Widget* requiredChild(PrivacyNoticeDialog& dialog, const char* objectName)
{
    Widget* child = dialog.findChild<Widget*>(QString::fromLatin1(objectName));
    if (child == nullptr) {
        qFatal("Required PrivacyNoticeDialog child was not found: %s",
               objectName);
    }
    return child;
}

QString allNoticeText(const PrivacyNoticeDialog& dialog)
{
    QStringList text;
    const QList<QLabel*> labels = dialog.findChildren<QLabel*>();
    text.reserve(labels.size());
    for (const QLabel* label : labels) {
        text.append(label->text());
    }
    return text.join(QLatin1Char('\n'));
}

}  // namespace

class M6PrivacyNoticeTests final : public QObject {
    Q_OBJECT

private slots:
    void requiredPrivacyFactsAreVisibleAndEntirelyLocal();
    void acceptEmitsOneDecisionAndMakesNoConnection();
    void declineEmitsOneDecisionAndMakesNoConnection();
};

void M6PrivacyNoticeTests::requiredPrivacyFactsAreVisibleAndEntirelyLocal()
{
    PrivacyNoticeDialog dialog;
    const QString text = allNoticeText(dialog);

    QVERIFY(text.contains(QStringLiteral("只驻留在内存")));
    QVERIFY(text.contains(QStringLiteral("不会自动把它们写入磁盘")));
    QVERIFY(text.contains(QStringLiteral("点击发送或按 Ctrl+Enter")));
    QVERIFY(text.contains(QStringLiteral("已扁平化快照")));
    QVERIFY(text.contains(QStringLiteral("问题")));
    QVERIFY(text.contains(QStringLiteral("目标 AI 服务的域名")));
    QVERIFY(text.contains(QStringLiteral("自定义服务端点")));
    QVERIFY(text.contains(QStringLiteral("首次使用前")));
    QVERIFY(text.contains(QStringLiteral("Windows 凭据管理器")));
    QVERIFY(text.contains(QStringLiteral("并非强安全脱敏")));
    QVERIFY(text.contains(QStringLiteral("拒绝并退出")));

    QVERIFY(!text.contains(QStringLiteral("http://"), Qt::CaseInsensitive));
    QVERIFY(!text.contains(QStringLiteral("https://"), Qt::CaseInsensitive));
    const QList<QLabel*> labels = dialog.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        QVERIFY(!label->openExternalLinks());
    }

    auto* shell = requiredChild<snapask::ui::glass::GlassSurface>(
        dialog,
        "privacyGlassShell");
    auto* details = requiredChild<snapask::ui::glass::GlassSurface>(
        dialog,
        "privacyNoticeDetails");
    QCOMPARE(
        shell->materialRole(),
        snapask::ui::glass::GlassMaterialRole::Elevated);
    QCOMPARE(
        details->materialRole(),
        snapask::ui::glass::GlassMaterialRole::ReadableContent);
    auto* accept = qobject_cast<snapask::ui::glass::GlassButton*>(
        requiredChild<QPushButton>(dialog, "privacyAcceptButton"));
    QVERIFY(accept != nullptr);
    QVERIFY(accept->isAccent());
}

void M6PrivacyNoticeTests::acceptEmitsOneDecisionAndMakesNoConnection()
{
    QTcpServer networkSentinel;
    QVERIFY(networkSentinel.listen(QHostAddress::LocalHost, 0));
    QSignalSpy connectionSpy(&networkSentinel, &QTcpServer::newConnection);

    PrivacyNoticeDialog dialog;
    QSignalSpy acceptedSpy(&dialog, &PrivacyNoticeDialog::privacyAccepted);
    QSignalSpy declinedSpy(&dialog, &PrivacyNoticeDialog::privacyRejected);
    QSignalSpy dialogAcceptedSpy(&dialog, &QDialog::accepted);
    dialog.show();
    QTRY_VERIFY(dialog.isVisible());

    requiredChild<QPushButton>(dialog, "privacyAcceptButton")->click();

    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QCOMPARE(acceptedSpy.size(), 1);
    QCOMPARE(declinedSpy.size(), 0);
    QCOMPARE(dialogAcceptedSpy.size(), 1);
    dialog.accept();
    QCOMPARE(acceptedSpy.size(), 1);
    QTest::qWait(50);
    QCOMPARE(connectionSpy.size(), 0);
    QVERIFY(!networkSentinel.hasPendingConnections());
}

void M6PrivacyNoticeTests::declineEmitsOneDecisionAndMakesNoConnection()
{
    QTcpServer networkSentinel;
    QVERIFY(networkSentinel.listen(QHostAddress::LocalHost, 0));
    QSignalSpy connectionSpy(&networkSentinel, &QTcpServer::newConnection);

    PrivacyNoticeDialog dialog;
    QSignalSpy acceptedSpy(&dialog, &PrivacyNoticeDialog::privacyAccepted);
    QSignalSpy declinedSpy(&dialog, &PrivacyNoticeDialog::privacyRejected);
    QSignalSpy dialogRejectedSpy(&dialog, &QDialog::rejected);
    dialog.show();
    QTRY_VERIFY(dialog.isVisible());

    requiredChild<QPushButton>(dialog, "privacyDeclineButton")->click();

    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
    QCOMPARE(acceptedSpy.size(), 0);
    QCOMPARE(declinedSpy.size(), 1);
    QCOMPARE(dialogRejectedSpy.size(), 1);
    dialog.reject();
    QCOMPARE(declinedSpy.size(), 1);
    QTest::qWait(50);
    QCOMPARE(connectionSpy.size(), 0);
    QVERIFY(!networkSentinel.hasPendingConnections());
}

QTEST_MAIN(M6PrivacyNoticeTests)
#include "M6PrivacyNoticeTests.moc"
