#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

class QWidget;
class FavoritesStore;
class LoopStore;

// The shared confirmation flow behind every "清理失效…" entry point.
//
// The plan is built before the question is asked, so the dialog can name exact
// numbers instead of "some entries", and both data files are copied aside
// before anything is removed. Both panels share it, so neither can grow its own
// half-complete clean-up again.
namespace MediaCleanupPrompt
{
// Returns true when a clean-up was actually performed.
bool run(QWidget* parent, FavoritesStore* favorites, LoopStore* loops,
         const QSet<QString>& staleKeys);

// Shown when a volume could not be reached: nothing may be removed until the
// user makes it available again.
void warnUnreachable(QWidget* parent, const QStringList& roots);
}
