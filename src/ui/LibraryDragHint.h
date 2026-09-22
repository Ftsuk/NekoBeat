#pragma once

#include <QWidget>

class QPropertyAnimation;

// Floating feedback while the media library is being resized. It paints a
// gradient strip towards the window edge plus an arrow that points the way the
// release will move the panel, so "let go now to collapse / to pull it out" is
// visible during the drag itself.
class LibraryDragHint : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(qreal intensity READ intensity WRITE setIntensity)

public:
    enum class Mode
    {
        Collapse, // library is shrinking towards the left edge
        Expand    // library is being pulled out of the left edge
    };

    // The hint is mirrored for the right hand side panel: the solid edge sits
    // on the window border of that side and the arrow points the way the
    // release will move the panel.
    enum class Side
    {
        Left = 0,
        Right = 1
    };

    explicit LibraryDragHint(QWidget* parent = nullptr);

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }
    void setSide(Side side);
    Side side() const { return m_side; }
    // 0 = invisible, 1 = "release now" highlight. Values in between fade the
    // strip in as the pointer approaches the snap threshold.
    void setIntensity(qreal intensity);
    qreal intensity() const { return m_intensity; }

    // Fades the strip out over ~160 ms and hides it when it reaches zero. Used
    // the moment the mouse button is released.
    void fadeOut();
    // Cancels a running fade (the strip is being shown again).
    void stopFade();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    Mode m_mode = Mode::Collapse;
    Side m_side = Side::Left;
    qreal m_intensity = 0.0;
    QPropertyAnimation* m_fade = nullptr;
};
