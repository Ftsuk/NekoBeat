#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLWidget>

#include <atomic>

struct mpv_handle;
struct mpv_render_context;

class MpvVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT

public:
    explicit MpvVideoWidget(QWidget* parent = nullptr);
    ~MpvVideoWidget() override;

    void setMpvHandle(mpv_handle* handle);

    // Diagnostics only (see --playback-probe): how often the render call runs,
    // and how often mpv asked for a redraw.
    qint64 renderedFrameCount() const { return m_frameCount.load(); }
    qint64 renderRequestCount() const { return m_renderRequestCount.load(); }
    // Total time spent inside the render call, so a probe can report the average
    // cost of one frame on the GUI thread.
    qint64 renderNanos() const { return m_renderNanos.load(); }
    // True once the widget owns a render context bound to a live GL context.
    // mpv cannot configure its video output before that.
    bool hasRenderContext() const { return m_renderContext != nullptr; }
    // How often the render context has been created. A full screen switch that
    // makes Qt rebuild the GL context shows up here as a growing number.
    int renderContextGeneration() const { return m_renderContextGeneration; }

signals:
    void doubleClicked();
    void rightClicked();
    // The render context was created (or re-created after the GL context was
    // rebuilt). mpv needs one to open its video output, so this is also the
    // moment to check whether an already loaded file still has a working one.
    void renderContextReady();

private slots:
    // Runs on the GUI thread. mpv requires mpv_render_context_update() to be
    // called from the thread that renders, so the mpv-side callback only hops
    // over here.
    void handleRenderUpdate();

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    static void* getProcAddress(void* context, const char* name);
    static void onMpvRenderUpdate(void* context);

    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_renderContext = nullptr;
    // How often the render context has been created, so the log shows whether a
    // full screen switch made Qt tear the GL context down and rebuild it.
    int m_renderContextGeneration = 0;
    std::atomic<qint64> m_frameCount{0};
    std::atomic<qint64> m_renderRequestCount{0};
    std::atomic<qint64> m_renderNanos{0};
};
