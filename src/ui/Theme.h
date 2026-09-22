#pragma once

#include "core/Track.h"

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;

namespace Theme
{
// Light, restrained palette in the spirit of Apple's marketing pages.
QString styleSheet();
void apply(QApplication& application);

QColor accent();
QColor background();
QColor panel();
QColor card();
QColor border();
QColor textPrimary();
QColor textMuted();
QColor axisColor(Track track);

QIcon playIcon(const QColor& color, int size = 16);
QIcon pauseIcon(const QColor& color, int size = 16);
QIcon stopIcon(const QColor& color, int size = 16);
QIcon resetIcon(const QColor& color, int size = 16);
QIcon chevronIcon(const QColor& color, int size = 16, bool pointingLeft = true);
QIcon searchIcon(const QColor& color, int size = 16);
QIcon volumeIcon(const QColor& color, int size = 16);
}
