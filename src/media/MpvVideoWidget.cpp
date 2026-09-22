#include "MpvVideoWidget.h"

#include "core/AppLogger.h"

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include <QOpenGLContext>
#include <QContextMenuEvent>
#include <QMouseEvent>

MpvVideoWidget::MpvVideoWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setMinimumSize(320, 180);
    setFocusPolicy(Qt::NoFocus);
}

MpvVideoWidget::~MpvVideoWidget()
{
    // Qt emits aboutToBeDestroyed from inside the widget's own destruction, at
    // which point makeCurrent()/doneCurrent() touch an already half-destroyed
    // widget. The cleanup below covers this case, so detach the handler first
    // instead of letting it run during teardown.
    if (QOpenGLContext* glContext = context())
        disconnect(glContext, nullptr, this, nullptr);

    if (!m_renderContext)
        return;

    makeCurrent();
    mpv_render_context_free(m_renderContext);
    m_renderContext = nullptr;
    doneCurrent();
}

void MpvVideoWidget::setMpvHandle(mpv_handle* handle)
{
    m_mpv = handle;
    if (isValid())
        update();
}

void MpvVideoWidget::initializeGL()
{
    initializeOpenGLFunctions();
    if (!m_mpv || m_renderContext)
        return;

    // Which GL implementation is actually in use decides how expensive a video
    // frame is: a software rasteriser (Qt falls back to opengl32sw when the
    // driver has no usable OpenGL) makes every frame costly and leaves the GUI
    // thread - and with it the device link - very little headroom.
    AppLogger::log(QStringLiteral("mpv"),
                   QStringLiteral("OpenGL: %1 | %2")
                       .arg(QString::fromLatin1(
                                reinterpret_cast<const char*>(glGetString(GL_RENDERER))),
                            QString::fromLatin1(
                                reinterpret_cast<const char*>(glGetString(GL_VERSION)))));

    // Qt destroys and recreates the widget's context when the window is
    // recreated (moving between screens, a display mode change, a full screen
    // transition on some drivers). The mpv render context belongs to exactly one
    // GL context, so it has to be dropped with it - otherwise the picture would
    // freeze (or the thread would stall while mpv renders into a dead context)
    // until the render context is recreated, which the next initializeGL() does.
    if (QOpenGLContext* glContext = context())
    {
        connect(glContext, &QOpenGLContext::aboutToBeDestroyed, this, [this]() {
            if (!m_renderContext)
                return;
            AppLogger::log(QStringLiteral("mpv"),
                           QStringLiteral("OpenGL 上下文即将销毁，释放 mpv 渲染上下文"));
            makeCurrent();
            mpv_render_context_free(m_renderContext);
            m_renderContext = nullptr;
            doneCurrent();
        });
    }

    mpv_opengl_init_params initParams{};
    initParams.get_proc_address = &MpvVideoWidget::getProcAddress;
    initParams.get_proc_address_ctx = this;

    int advancedControl = 1;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &initParams},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advancedControl},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };

    if (mpv_render_context_create(&m_renderContext, m_mpv, params) < 0)
    {
        m_renderContext = nullptr;
        AppLogger::log(QStringLiteral("mpv"),
                       QStringLiteral("mpv 渲染上下文创建失败，视频输出将无法初始化"));
    }
    else
    {
        ++m_renderContextGeneration;
        AppLogger::log(QStringLiteral("mpv"),
                       QStringLiteral("mpv 渲染上下文就绪（第 %1 次），窗口 %2x%3")
                           .arg(m_renderContextGeneration)
                           .arg(width())
                           .arg(height()));
        mpv_render_context_set_update_callback(m_renderContext, &MpvVideoWidget::onMpvRenderUpdate, this);
        emit renderContextReady();
    }
}

void MpvVideoWidget::paintGL()
{
    // Clear first, unconditionally. When the window changes size Qt hands the
    // widget a freshly allocated frame buffer whose contents are undefined - on
    // this driver it reads as light grey, which is the white band that flashes
    // along the old window edges while the picture grows into full screen (and,
    // when the widget is repainted before mpv delivers a frame, a whole light
    // block). mpv repaints the entire frame right afterwards, so a black clear
    // is never visible on its own.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (!m_renderContext)
        return;

    QElapsedTimer renderClock;
    renderClock.start();

    const qreal dpr = devicePixelRatioF();
    mpv_opengl_fbo fbo{};
    fbo.fbo = static_cast<int>(defaultFramebufferObject());
    fbo.w = static_cast<int>(width() * dpr);
    fbo.h = static_cast<int>(height() * dpr);
    fbo.internal_format = 0;

    int flipY = 1;
    // BLOCK_FOR_TARGET_TIME is deliberately not passed: 0.6.4 passed 0 here to
    // stop the render call from blocking, but that also tells mpv the caller
    // drives the render loop itself, and it stopped announcing new frames. Back
    // to mpv's default scheduling, which is what 0.6.2 did.
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr}
    };
    mpv_render_context_render(m_renderContext, params);
    m_frameCount.fetch_add(1);
    m_renderNanos.fetch_add(renderClock.nsecsElapsed());

    // This call must stay in the millisecond range: it runs on the GUI thread,
    // which also drives the device. Anything slow here (a stalled decoder, a
    // driver hiccup) shows up as the machine pausing, so it is worth a log line.
    // 10 ms is already 60 % of a 60 fps frame budget and the GUI thread drives
    // the device too, so anything from there up is worth a line. Normal frames
    // cost well under a millisecond and never log.
    const qint64 elapsed = renderClock.elapsed();
    if (elapsed >= 10)
    {
        AppLogger::log(QStringLiteral("mpv"),
                       QStringLiteral("渲染单帧耗时 %1 ms（画面 %2x%3）")
                           .arg(elapsed)
                           .arg(width())
                           .arg(height()));
    }
}

void MpvVideoWidget::resizeGL(int width, int height)
{
    Q_UNUSED(width);
    Q_UNUSED(height);
}

void MpvVideoWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
        emit doubleClicked();
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void MpvVideoWidget::mousePressEvent(QMouseEvent* event)
{
    // Right click on the picture toggles pause/resume (works in full screen,
    // where this widget is the only visible child of the window).
    if (event->button() == Qt::RightButton)
    {
        emit rightClicked();
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void MpvVideoWidget::contextMenuEvent(QContextMenuEvent* event)
{
    // The right click is consumed as a pause/resume gesture; swallow the menu
    // request so it does not bubble up to the window.
    event->accept();
}

void* MpvVideoWidget::getProcAddress(void* context, const char* name)
{
    Q_UNUSED(context);
    QOpenGLContext* glContext = QOpenGLContext::currentContext();
    if (!glContext)
        return nullptr;
    return reinterpret_cast<void*>(glContext->getProcAddress(name));
}

void MpvVideoWidget::onMpvRenderUpdate(void* context)
{
    auto* widget = static_cast<MpvVideoWidget*>(context);
    if (!widget)
        return;

    // This callback comes from mpv's own thread; mpv_render_context_update()
    // must not be called from here, so only the hop is queued.
    QMetaObject::invokeMethod(widget, "handleRenderUpdate", Qt::QueuedConnection);
}

void MpvVideoWidget::handleRenderUpdate()
{
    if (!m_renderContext)
    {
        // The GL context was rebuilt between mpv's notification and this hop, so
        // the announcement is dropped - which under advanced control counts as
        // "not acknowledged". renderContextReady re-opens the file in that case;
        // this line only leaves a trace of how often it happens.
        AppLogger::log(QStringLiteral("mpv"),
                       QStringLiteral("渲染上下文已销毁，丢弃一次更新通知"));
        return;
    }

    m_renderRequestCount.fetch_add(1);

    // The render context was created with MPV_RENDER_PARAM_ADVANCED_CONTROL,
    // which makes the host responsible for acknowledging every update: mpv only
    // keeps announcing new frames while mpv_render_context_update() is called.
    // Skipping it (as 0.6.0-0.6.4 did) leaves mpv waiting after the first
    // notification, so the picture only refreshed whenever Qt repainted the
    // window for a reason of its own - a playing video with sound and a frozen
    // picture, most visible right after switching to full screen.
    const uint64_t flags = mpv_render_context_update(m_renderContext);
    if ((flags & MPV_RENDER_UPDATE_FRAME) == 0)
        return;

    update();
}
