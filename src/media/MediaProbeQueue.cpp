#include "MediaProbeQueue.h"

#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include <algorithm>

namespace
{
QString normalizedPath(const QString& path)
{
    return path.toLower();
}

class ProbeRunnable : public QRunnable
{
public:
    ProbeRunnable(const QPointer<MediaProbeQueue>& queue, QString jobKey, QString mediaPath,
                  QString thumbnailPath, QString temporaryRoot,
                  double seekRatio, qint64 positionMs, bool ignoreLocalCover,
                  std::shared_ptr<std::atomic<bool>> cancel)
        : m_queue(queue)
        , m_jobKey(std::move(jobKey))
        , m_mediaPath(std::move(mediaPath))
        , m_thumbnailPath(std::move(thumbnailPath))
        , m_temporaryRoot(std::move(temporaryRoot))
        , m_seekRatio(seekRatio)
        , m_positionMs(positionMs)
        , m_ignoreLocalCover(ignoreLocalCover)
        , m_cancel(std::move(cancel))
    {
    }

    void run() override
    {
        MediaProbeResult result;
        if (m_positionMs >= 0)
        {
            result = MediaProbe::captureFrameAt(m_mediaPath, m_positionMs, m_thumbnailPath,
                                                m_temporaryRoot, 8000, m_cancel);
        }
        else
        {
            MediaProbeOptions options;
            options.thumbnailPath = m_thumbnailPath;
            options.temporaryRoot = m_temporaryRoot;
            options.seekRatio = m_seekRatio;
            options.ignoreLocalCover = m_ignoreLocalCover;
            options.cancel = m_cancel;
            result = MediaProbe::probe(m_mediaPath, options);
        }

        MediaProbeQueue* queue = m_queue.data();
        if (!queue)
            return;

        const QString key = m_jobKey;
        const QString path = m_mediaPath;
        QMetaObject::invokeMethod(
            queue,
            [queue, key, path, result]() { queue->handleFinished(key, path, result); },
            Qt::QueuedConnection);
    }

private:
    QPointer<MediaProbeQueue> m_queue;
    QString m_jobKey;
    QString m_mediaPath;
    QString m_thumbnailPath;
    QString m_temporaryRoot;
    double m_seekRatio = -1.0;
    qint64 m_positionMs = -1;
    bool m_ignoreLocalCover = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};
}

MediaProbeQueue::MediaProbeQueue(QObject* parent)
    : QObject(parent)
    , m_cancel(std::make_shared<std::atomic<bool>>(false))
{
    qRegisterMetaType<MediaProbeResult>("MediaProbeResult");
    m_pool.setMaxThreadCount(2);
    m_pool.setExpiryTimeout(-1);
}

MediaProbeQueue::~MediaProbeQueue()
{
    if (m_cancel)
        m_cancel->store(true);
    m_queue.clear();
    m_queued.clear();
    m_pool.clear();
    m_pool.waitForDone(2000);
}

void MediaProbeQueue::setTemporaryRoot(const QString& root)
{
    m_temporaryRoot = root;
}

void MediaProbeQueue::setConcurrency(int count)
{
    m_pool.setMaxThreadCount(std::max(1, count));
}

void MediaProbeQueue::enqueue(const QString& mediaPath, const QString& thumbnailPath,
                              double seekRatio, bool force, bool ignoreLocalCover)
{
    Job job;
    job.mediaPath = mediaPath;
    job.thumbnailPath = thumbnailPath;
    job.seekRatio = seekRatio;
    job.ignoreLocalCover = ignoreLocalCover;
    enqueueJob(job, force);
}

void MediaProbeQueue::enqueueAt(const QString& mediaPath, qint64 positionMs,
                                const QString& thumbnailPath, bool force)
{
    Job job;
    job.mediaPath = mediaPath;
    job.thumbnailPath = thumbnailPath;
    job.positionMs = positionMs;
    job.ignoreLocalCover = true;
    enqueueJob(job, force);
}

void MediaProbeQueue::enqueueJob(const Job& job, bool force)
{
    if (job.mediaPath.isEmpty())
        return;

    // The key includes the output file: one video may be queued several times
    // with different loop clip thumbnails.
    Job prepared = job;
    prepared.key = normalizedPath(job.mediaPath) + QLatin1Char('|')
                   + normalizedPath(job.thumbnailPath);
    if (m_queued.contains(prepared.key))
    {
        if (!force)
            return;

        // A forced request (regenerate) must win over a queued/running job. If
        // the old copy is still waiting in the queue, replace it in place so
        // exactly one job exists for this key; if a worker is already running
        // it, re-run once that one finished (two tasks must never write the same
        // thumbnail file).
        for (int i = 0; i < m_queue.size(); ++i)
        {
            if (m_queue.at(i).key == prepared.key)
            {
                m_queue[i] = prepared;
                return;
            }
        }
        m_rerun.insert(prepared.key, prepared);
        return;
    }

    m_queued.insert(prepared.key);
    m_queue.enqueue(prepared);
    ++m_total;
    emit progressChanged(m_finished, m_total);
    startNext();
}

void MediaProbeQueue::clearPending()
{
    // Drop the de-duplication markers of the jobs that never ran, otherwise a
    // later enqueue of the same video would be rejected as "already queued"
    // and its thumbnail would stay pending until the next restart.
    for (const Job& job : m_queue)
        m_queued.remove(job.key);
    m_queue.clear();
    m_rerun.clear();
    m_total = m_finished;
    emit progressChanged(m_finished, m_total);
    if (isIdle())
        emit idle();
}

int MediaProbeQueue::pendingCount() const
{
    return m_queue.size();
}

void MediaProbeQueue::startNext()
{
    while (m_active < m_pool.maxThreadCount() && !m_queue.isEmpty())
    {
        const Job job = m_queue.dequeue();
        ++m_active;
        m_pool.start(new ProbeRunnable(this, job.key, job.mediaPath, job.thumbnailPath,
                                       m_temporaryRoot, job.seekRatio, job.positionMs,
                                       job.ignoreLocalCover, m_cancel));
    }
}

void MediaProbeQueue::handleFinished(const QString& jobKey, const QString& mediaPath,
                                     const MediaProbeResult& result)
{
    m_queued.remove(jobKey);
    --m_active;
    if (m_active < 0)
        m_active = 0;
    ++m_finished;

    emit probed(mediaPath, result);
    emit progressChanged(m_finished, m_total);

    // A forced regeneration arrived while this job was running: run it now with
    // the fresh parameters, now that the file is no longer being written.
    if (m_rerun.contains(jobKey))
    {
        const Job rerun = m_rerun.take(jobKey);
        m_queued.insert(rerun.key);
        m_queue.enqueue(rerun);
        ++m_total;
    }

    startNext();
    if (isIdle())
    {
        m_finished = 0;
        m_total = 0;
        m_queued.clear();
        emit idle();
    }
}
