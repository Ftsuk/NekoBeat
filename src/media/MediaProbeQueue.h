#pragma once

#include "media/MediaProbe.h"

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <memory>

Q_DECLARE_METATYPE(MediaProbeResult)

// Runs MediaProbe jobs on a small private thread pool so the UI thread never
// blocks on decoding. One or two workers keep CPU/GPU load predictable.
class MediaProbeQueue : public QObject
{
    Q_OBJECT

public:
    explicit MediaProbeQueue(QObject* parent = nullptr);
    ~MediaProbeQueue() override;

    void setTemporaryRoot(const QString& root);
    void setConcurrency(int count);

    void enqueue(const QString& mediaPath, const QString& thumbnailPath,
                 double seekRatio = -1.0, bool force = false,
                 bool ignoreLocalCover = false);
    // Grabs the frame at an exact position; used for loop clip thumbnails,
    // where the A point matters and the media library's "safe" ratios do not.
    void enqueueAt(const QString& mediaPath, qint64 positionMs,
                   const QString& thumbnailPath, bool force = false);
    void clearPending();

    int pendingCount() const;
    int finishedCount() const { return m_finished; }
    int totalCount() const { return m_total; }
    bool isIdle() const { return m_queue.isEmpty() && m_active == 0; }

    // Called from worker threads through a queued invocation.
    void handleFinished(const QString& jobKey, const QString& mediaPath,
                        const MediaProbeResult& result);

signals:
    void probed(const QString& mediaPath, const MediaProbeResult& result);
    void progressChanged(int finished, int total);
    void idle();

private:
    struct Job
    {
        QString key;
        QString mediaPath;
        QString thumbnailPath;
        double seekRatio = -1.0;
        qint64 positionMs = -1;
        bool ignoreLocalCover = false;
    };

    void startNext();
    void enqueueJob(const Job& job, bool force);

    QThreadPool m_pool;
    QQueue<Job> m_queue;
    QSet<QString> m_queued;
    // Forced jobs that arrived while the same key was already running: they are
    // re-run once the running copy finishes, so two tasks never write the same
    // thumbnail file at the same time.
    QHash<QString, Job> m_rerun;
    QString m_temporaryRoot;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    int m_active = 0;
    int m_finished = 0;
    int m_total = 0;
};
