#pragma once

#include "core/MediaItem.h"

#include <QDialog>
#include <QHash>
#include <QList>
#include <QString>

class QLabel;
class QPushButton;
class QTreeWidget;

// Shows every grey card next to the best guess of where its file went and lets
// the user confirm or correct the mapping. Matching is only a suggestion: a
// candidate that is merely "the same name" is never pre-selected, because that
// is exactly how two different videos end up sharing a cover.
class RelinkDialog : public QDialog
{
    Q_OBJECT

public:
    RelinkDialog(QWidget* parent, const QList<MediaItem>& missing,
                 const QList<MediaItem>& available);

    // Missing path -> chosen new path, for the rows the user kept ticked.
    QHash<QString, QString> mapping() const;

private:
    void updateSummary();

    QTreeWidget* m_tree = nullptr;
    QPushButton* m_applyButton = nullptr;
    QLabel* m_summary = nullptr;
    struct Row
    {
        QString missingPath;
        QWidget* chooser = nullptr;
    };
    QList<Row> m_rows;
};
