#pragma once

#include "core/LoopClip.h"

#include <QDialog>
#include <QList>
#include <QPair>
#include <QSet>

class QCheckBox;
class LoopStore;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QVBoxLayout;
class QWidget;

// Information dialog for one loop clip: title, editable A/B timecodes, tags,
// loop folders and a note. The timecodes may be typed here even though the heat
// map overlay is read-only - editing them only changes stored data, it never
// seeks and never moves the device.
class LoopClipDialog : public QDialog
{
    Q_OBJECT

public:
    LoopClipDialog(QWidget* parent, LoopStore* store, const LoopClip& clip,
                   qint64 mediaDurationMs);

    // Filled with the edited values after the dialog was accepted.
    LoopClip clip() const { return m_clip; }

protected:
    void accept() override;
    // Enter never submits the dialog: in the tag field it adds a tag, anywhere
    // else it does nothing. Saving is an explicit click on「保存」.
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    // Rebuilds the folder check boxes; an empty set keeps whatever is ticked
    // right now (the clip's own folders on the first build).
    void refreshFolders(const QSet<QString>& checked = QSet<QString>());
    QSet<QString> checkedFolderIds() const;
    void addTag(const QString& text);
    void removeSelectedTag();
    void createFolder();
    bool commitTimecodes();

    LoopStore* m_store = nullptr;
    LoopClip m_clip;
    qint64 m_mediaDurationMs = 0;
    QLineEdit* m_titleEdit = nullptr;
    QLineEdit* m_startEdit = nullptr;
    QLineEdit* m_endEdit = nullptr;
    QLineEdit* m_tagEdit = nullptr;
    QListWidget* m_tagList = nullptr;
    QWidget* m_folderContainer = nullptr;
    QVBoxLayout* m_folderLayout = nullptr;
    QList<QPair<QString, QCheckBox*>> m_folderChecks;
    QPlainTextEdit* m_noteEdit = nullptr;
    QLabel* m_hint = nullptr;
};
