#include "domain/capture/SelectionModel.h"

#include <QtTest>

using snapask::capture::SelectionHandle;
using snapask::capture::SelectionModel;

class M1SelectionModelTest final : public QObject {
    Q_OBJECT

private slots:
    void createNormalizesAndClampsToNegativeBounds();
    void cancelRestoresSelectionBeforeInteraction();
    void movePreservesSizeAndStaysInsideBounds();
    void resizeAllEightHandles();
    void resizeHonorsMinimumAndDesktopEdges();
    void hitTestFindsEightHandlesAndMoveArea();
};

void M1SelectionModelTest::createNormalizesAndClampsToNegativeBounds()
{
    SelectionModel model(QRect(-200, -100, 400, 300));
    QVERIFY(model.beginCreate(QPoint(100, 100)));
    QVERIFY(model.creatingSelection());
    QVERIFY(model.updateInteraction(QPoint(-300, -200)));
    QCOMPARE(model.selectionPx(), QRect(QPoint(-200, -100), QPoint(100, 100)));
    model.commitInteraction();
    QVERIFY(!model.interactionActive());
}

void M1SelectionModelTest::cancelRestoresSelectionBeforeInteraction()
{
    SelectionModel model(QRect(0, 0, 200, 100));
    model.setSelection(QRect(40, 20, 60, 40));
    QVERIFY(model.beginCreate(QPoint(5, 5)));
    QVERIFY(model.updateInteraction(QPoint(30, 30)));
    model.cancelInteraction();
    QCOMPARE(model.selectionPx(), QRect(40, 20, 60, 40));

    QVERIFY(model.beginTransform(SelectionHandle::Move, QPoint(50, 30)));
    QVERIFY(model.updateInteraction(QPoint(100, 60)));
    model.cancelInteraction();
    QCOMPARE(model.selectionPx(), QRect(40, 20, 60, 40));
}

void M1SelectionModelTest::movePreservesSizeAndStaysInsideBounds()
{
    SelectionModel model(QRect(-100, -50, 300, 200));
    model.setSelection(QRect(-20, 0, 60, 40));

    QVERIFY(model.beginTransform(SelectionHandle::Move, QPoint(0, 10)));
    QVERIFY(model.updateInteraction(QPoint(-500, -500)));
    QCOMPARE(model.selectionPx(), QRect(-100, -50, 60, 40));
    model.commitInteraction();

    QVERIFY(model.beginTransform(SelectionHandle::Move, QPoint(-90, -40)));
    QVERIFY(model.updateInteraction(QPoint(500, 500)));
    QCOMPARE(model.selectionPx(), QRect(140, 110, 60, 40));
    model.commitInteraction();
}

void M1SelectionModelTest::resizeAllEightHandles()
{
    struct Case final {
        SelectionHandle handle;
        QPoint press;
        QPoint target;
        QRect expected;
    };
    const QList<Case> cases{
        {SelectionHandle::NorthWest, {20, 20}, {10, 10}, {10, 10, 50, 40}},
        {SelectionHandle::North, {39, 20}, {39, 10}, {20, 10, 40, 40}},
        {SelectionHandle::NorthEast, {59, 20}, {70, 10}, {20, 10, 51, 40}},
        {SelectionHandle::East, {59, 34}, {70, 34}, {20, 20, 51, 30}},
        {SelectionHandle::SouthEast, {59, 49}, {70, 60}, {20, 20, 51, 41}},
        {SelectionHandle::South, {39, 49}, {39, 60}, {20, 20, 40, 41}},
        {SelectionHandle::SouthWest, {20, 49}, {10, 60}, {10, 20, 50, 41}},
        {SelectionHandle::West, {20, 34}, {10, 34}, {10, 20, 50, 30}},
    };

    for (const Case& test : cases) {
        SelectionModel model(QRect(0, 0, 100, 100));
        model.setSelection(QRect(20, 20, 40, 30));
        QVERIFY(model.beginTransform(test.handle, test.press));
        QVERIFY(model.updateInteraction(test.target));
        QCOMPARE(model.selectionPx(), test.expected);
        model.commitInteraction();
    }
}

void M1SelectionModelTest::resizeHonorsMinimumAndDesktopEdges()
{
    SelectionModel model(QRect(0, 0, 100, 80));
    model.setMinimumSize(QSize(8, 6));
    model.setSelection(QRect(20, 20, 40, 30));

    QVERIFY(model.beginTransform(SelectionHandle::East, QPoint(59, 34)));
    QVERIFY(model.updateInteraction(QPoint(0, 34)));
    QCOMPARE(model.selectionPx(), QRect(20, 20, 8, 30));
    model.commitInteraction();

    model.setSelection(QRect(20, 20, 40, 30));
    QVERIFY(model.beginTransform(SelectionHandle::NorthWest, QPoint(20, 20)));
    QVERIFY(model.updateInteraction(QPoint(-100, -100)));
    QCOMPARE(model.selectionPx(), QRect(0, 0, 60, 50));
    model.commitInteraction();

    model.setSelection(QRect(20, 20, 40, 30));
    QVERIFY(model.beginTransform(SelectionHandle::SouthEast, QPoint(59, 49)));
    QVERIFY(model.updateInteraction(QPoint(500, 500)));
    QCOMPARE(model.selectionPx(), QRect(20, 20, 80, 60));
}

void M1SelectionModelTest::hitTestFindsEightHandlesAndMoveArea()
{
    const QRect selection(20, 20, 40, 30);
    const auto centers = SelectionModel::handleCenters(selection);
    const std::array<SelectionHandle, 8> handles{
        SelectionHandle::NorthWest,
        SelectionHandle::North,
        SelectionHandle::NorthEast,
        SelectionHandle::East,
        SelectionHandle::SouthEast,
        SelectionHandle::South,
        SelectionHandle::SouthWest,
        SelectionHandle::West,
    };

    for (std::size_t index = 0; index < handles.size(); ++index) {
        QCOMPARE(SelectionModel::hitTest(selection, centers[index], 4), handles[index]);
    }
    QCOMPARE(SelectionModel::hitTest(selection, QPoint(40, 35), 4), SelectionHandle::Move);
    QCOMPARE(SelectionModel::hitTest(selection, QPoint(2, 2), 4), SelectionHandle::None);
}

QTEST_GUILESS_MAIN(M1SelectionModelTest)

#include "m1_selection_model_test.moc"
