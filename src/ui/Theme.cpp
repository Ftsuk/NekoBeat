#include "ui/Theme.h"

#include <QApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QtMath>

#include <algorithm>

namespace
{
const QColor kAccent(0x0A, 0x84, 0xFF);
const QColor kBackground(0x1C, 0x1C, 0x1E);
const QColor kPanel(0x23, 0x23, 0x26);
const QColor kCard(0x2C, 0x2C, 0x2E);
const QColor kBorder(0x38, 0x38, 0x3A);
const QColor kText(0xF2, 0xF2, 0xF7);
const QColor kMuted(0x98, 0x98, 0x9D);
}

QColor Theme::accent() { return kAccent; }
QColor Theme::background() { return kBackground; }
QColor Theme::panel() { return kPanel; }
QColor Theme::card() { return kCard; }
QColor Theme::border() { return kBorder; }
QColor Theme::textPrimary() { return kText; }
QColor Theme::textMuted() { return kMuted; }

QColor Theme::axisColor(Track track)
{
    switch (track)
    {
    case Track::Stroke: return QColor(0x0A, 0x84, 0xFF);
    case Track::Surge: return QColor(0xFF, 0x9F, 0x0A);
    case Track::Sway: return QColor(0x30, 0xD1, 0x58);
    case Track::Twist: return QColor(0xFF, 0x37, 0x5F);
    case Track::Roll: return QColor(0xBF, 0x5A, 0xF2);
    case Track::Pitch: return QColor(0xFF, 0xD6, 0x0A);
    case Track::Vib: return QColor(0x40, 0xD0, 0xD0);
    case Track::Lube: return QColor(0xFF, 0x6B, 0x6B);
    case Track::Suck: return QColor(0xFF, 0xA8, 0x54);
    case Track::SuckPosition: return QColor(0x8E, 0xA9, 0xFF);
    case Track::None: break;
    }
    return kAccent;
}

namespace
{
QIcon glyphIcon(const QColor& color, int size, int kind)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);

    const qreal s = size;
    if (kind == 0) // play
    {
        QPainterPath path;
        path.moveTo(s * 0.24, s * 0.13);
        path.lineTo(s * 0.87, s * 0.50);
        path.lineTo(s * 0.24, s * 0.87);
        path.closeSubpath();
        painter.drawPath(path);
    }
    else if (kind == 1) // pause
    {
        const qreal width = s * 0.24;
        painter.drawRoundedRect(QRectF(s * 0.17, s * 0.11, width, s * 0.78), 2.2, 2.2);
        painter.drawRoundedRect(QRectF(s * 0.59, s * 0.11, width, s * 0.78), 2.2, 2.2);
    }
    else if (kind == 2) // stop
    {
        painter.drawRoundedRect(QRectF(s * 0.20, s * 0.20, s * 0.60, s * 0.60), 3.0, 3.0);
    }
    else if (kind == 3) // reset: circular arrows around a center dot
    {
        const QPointF center(s / 2.0, s / 2.0);
        const qreal radius = s * 0.40;
        const qreal stroke = std::max<qreal>(1.2, s * 0.075);
        const QRectF ring(center.x() - radius, center.y() - radius, radius * 2.0, radius * 2.0);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(color, stroke, Qt::SolidLine, Qt::RoundCap));
        QPainterPath upperArc;
        upperArc.arcMoveTo(ring, 200.0);
        upperArc.arcTo(ring, 200.0, -140.0);
        painter.drawPath(upperArc);
        QPainterPath lowerArc;
        lowerArc.arcMoveTo(ring, 160.0);
        lowerArc.arcTo(ring, 160.0, 140.0);
        painter.drawPath(lowerArc);

        // Arrow heads at both arc ends, pointing into the gap on the right. The
        // triangles sit fully in front of the arc so they stay crisp.
        const auto arrowAt = [&](qreal angleDeg, bool clockwise) {
            const qreal radians = qDegreesToRadians(angleDeg);
            const QPointF point(center.x() + radius * std::cos(radians),
                                center.y() - radius * std::sin(radians));
            const QPointF direction = clockwise
                                          ? QPointF(std::sin(radians), std::cos(radians))
                                          : QPointF(-std::sin(radians), -std::cos(radians));
            const QPointF normal(-direction.y(), direction.x());
            const qreal length = s * 0.24;
            const qreal halfWidth = s * 0.145;
            QPainterPath head;
            head.moveTo(point + direction * length);
            head.lineTo(point + normal * halfWidth);
            head.lineTo(point - normal * halfWidth);
            head.closeSubpath();
            painter.drawPath(head);
        };
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        arrowAt(60.0, true);
        arrowAt(300.0, false);

        // The dot sits in the gap between the two arrow heads so the whole mark
        // still reads as one circle.
        const QPointF dotCenter(center.x() + radius * 0.86, center.y());
        painter.drawEllipse(dotCenter, s * 0.11, s * 0.11);
    }
    return QIcon(pixmap);
}
}

QIcon Theme::playIcon(const QColor& color, int size) { return glyphIcon(color, size, 0); }
QIcon Theme::pauseIcon(const QColor& color, int size) { return glyphIcon(color, size, 1); }
QIcon Theme::stopIcon(const QColor& color, int size) { return glyphIcon(color, size, 2); }
QIcon Theme::resetIcon(const QColor& color, int size) { return glyphIcon(color, size, 3); }

QIcon Theme::chevronIcon(const QColor& color, int size, bool pointingLeft)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, std::max<qreal>(1.6, size * 0.15), Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const qreal s = size;
    QPainterPath path;
    if (pointingLeft)
    {
        path.moveTo(s * 0.64, s * 0.20);
        path.lineTo(s * 0.34, s * 0.50);
        path.lineTo(s * 0.64, s * 0.80);
    }
    else
    {
        path.moveTo(s * 0.36, s * 0.20);
        path.lineTo(s * 0.66, s * 0.50);
        path.lineTo(s * 0.36, s * 0.80);
    }
    painter.drawPath(path);
    return QIcon(pixmap);
}

QIcon Theme::searchIcon(const QColor& color, int size)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, std::max<qreal>(1.4, size * 0.11), Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const qreal s = size;
    const qreal radius = s * 0.28;
    const QPointF center(s * 0.42, s * 0.42);
    painter.drawEllipse(center, radius, radius);
    painter.drawLine(QPointF(center.x() + radius * 0.72, center.y() + radius * 0.72),
                     QPointF(s * 0.86, s * 0.86));
    return QIcon(pixmap);
}

QIcon Theme::volumeIcon(const QColor& color, int size)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal s = size;
    QPainterPath speaker;
    speaker.moveTo(s * 0.14, s * 0.36);
    speaker.lineTo(s * 0.32, s * 0.36);
    speaker.lineTo(s * 0.52, s * 0.18);
    speaker.lineTo(s * 0.52, s * 0.82);
    speaker.lineTo(s * 0.32, s * 0.64);
    speaker.lineTo(s * 0.14, s * 0.64);
    speaker.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(speaker);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, std::max<qreal>(1.3, size * 0.09), Qt::SolidLine,
                        Qt::RoundCap));
    painter.drawArc(QRectF(s * 0.50, s * 0.32, s * 0.34, s * 0.36),
                    -70 * 16, 140 * 16);
    return QIcon(pixmap);
}

void Theme::apply(QApplication& application)
{
    QFont font(QStringLiteral("Segoe UI Variable Text"), 10);
    font.setStyleStrategy(QFont::PreferAntialias);
    application.setFont(font);

    // The style sheet only covers the widgets Qt draws itself. When the window
    // grows - entering full screen, for example - the newly exposed areas are
    // erased with the palette's Window colour before anything is painted, and the
    // system default for that is white. That is the white edge flashing by while
    // the picture expands. A dark palette makes every erase step invisible
    // against the dark UI.
    QPalette palette = application.palette();
    const QColor window(0x1C, 0x1C, 0x1E);
    const QColor base(0x15, 0x15, 0x17);
    const QColor alternate(0x2C, 0x2C, 0x2E);
    const QColor text(0xF2, 0xF2, 0xF7);
    const QColor disabled(0x8E, 0x8E, 0x93);
    const QColor highlight(0x0A, 0x84, 0xFF);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, alternate);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xFF, 0x45, 0x3A));
    palette.setColor(QPalette::ToolTipBase, alternate);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    application.setPalette(palette);

    application.setStyleSheet(styleSheet());
}

QString Theme::styleSheet()
{
    return QStringLiteral(R"QSS(
QWidget {
    color: #F2F2F7;
    font-family: "Segoe UI Variable Text", "Segoe UI", "Microsoft YaHei UI", "PingFang SC", sans-serif;
    font-size: 13px;
}
QMainWindow, QDialog {
    background: #1C1C1E;
}
QToolTip {
    background: #2C2C2E;
    color: #F2F2F7;
    border: 1px solid #48484A;
    border-radius: 8px;
    padding: 6px 10px;
}

/* ---------- menu bar ---------- */
QMenuBar {
    background: #1C1C1E;
    border-bottom: 1px solid #38383A;
    padding: 3px 8px;
    font-size: 13px;
}
QMenuBar::item {
    background: transparent;
    padding: 5px 12px;
    border-radius: 7px;
    margin: 0px 1px;
}
QMenuBar::item:selected {
    background: #3A3A3C;
}
QMenuBar::item:pressed {
    background: #48484A;
}
QMenu {
    background: #2C2C2E;
    border: 1px solid #48484A;
    border-radius: 12px;
    padding: 6px;
}
QMenu::item {
    padding: 7px 26px 7px 14px;
    border-radius: 7px;
    color: #F2F2F7;
}
QMenu::item:selected {
    background: #0A84FF;
    color: #FFFFFF;
}
QMenu::item:disabled {
    color: #6E6E73;
}
QMenu::separator {
    height: 1px;
    background: #48484A;
    margin: 5px 10px;
}

/* ---------- status bar ---------- */
QStatusBar {
    background: #1C1C1E;
    border-top: 1px solid #38383A;
    color: #98989D;
    font-size: 12px;
}
QStatusBar::item {
    border: none;
}

/* ---------- buttons ---------- */
QToolButton, QPushButton {
    background: #3A3A3C;
    color: #F2F2F7;
    border: 1px solid #48484A;
    border-radius: 9px;
    padding: 6px 14px;
}
QToolButton:hover, QPushButton:hover {
    background: #48484A;
    border-color: #5A5A5E;
}
QToolButton:pressed, QPushButton:pressed {
    background: #2C2C2E;
}
QToolButton::menu-indicator {
    image: none;
}
QPushButton#Primary, QToolButton#Primary {
    background: #0A84FF;
    color: #FFFFFF;
    border: none;
    border-radius: 9px;
    padding: 7px 16px;
    font-weight: 600;
}
QPushButton#Primary:hover, QToolButton#Primary:hover {
    background: #2E95FF;
}
QPushButton#Primary:pressed, QToolButton#Primary:pressed {
    background: #0071E3;
}
QPushButton#Danger, QToolButton#Danger {
    background: #3A3A3C;
    color: #FF6961;
    border: 1px solid #5A3A3A;
}
QPushButton#Danger:hover, QToolButton#Danger:hover {
    background: #4A2F2F;
}
QToolButton#Compact {
    padding: 4px 10px;
    border-radius: 8px;
    font-size: 12px;
}
QToolButton#LibraryToggle {
    background: #4A4A4E;
    border: 1px solid #6C6C72;
    border-radius: 9px;
    color: #F2F2F7;
    font-size: 13px;
    padding: 0px;
}
QToolButton#LibraryToggle:hover {
    background: #0A84FF;
    border-color: #2E95FF;
}
QToolButton#LibraryToggle:pressed {
    background: #0071E3;
}
QToolButton:disabled, QPushButton:disabled {
    background: #262628;
    color: #6E6E73;
    border-color: #3A3A3C;
}
QToolButton#Compact:disabled {
    background: #262628;
    color: #6E6E73;
    border-color: #3A3A3C;
}
QToolButton#MediaButton:disabled {
    background: #262628;
    border-color: #3A3A3C;
}
QToolButton#Bare {
    background: transparent;
    border: none;
    color: #6BB4FF;
    padding: 4px 8px;
}
QToolButton#Bare:hover {
    background: #2C3A4D;
}
QToolButton#MediaButton {
    background: #3A3A3C;
    border: 1px solid #48484A;
    border-radius: 16px;
    padding: 0px;
    color: #F2F2F7;
    font-size: 12px;
}
QToolButton#MediaButton:hover {
    background: #48484A;
}
QToolButton#MediaButtonActive {
    background: #0A84FF;
    border: 1px solid #2E95FF;
    border-radius: 16px;
    padding: 0px;
    color: #FFFFFF;
    font-size: 12px;
    font-weight: 600;
}
QToolButton#MediaButtonActive:hover {
    background: #2E95FF;
}
QToolButton#MediaButtonAccent {
    background: #0A84FF;
    border: none;
    border-radius: 16px;
    padding: 0px;
}
QToolButton#MediaButtonAccent:hover {
    background: #2E95FF;
}

/* ---------- inputs ---------- */
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: #2C2C2E;
    border: 1px solid #48484A;
    border-radius: 9px;
    padding: 5px 10px;
    min-height: 20px;
    color: #F2F2F7;
    selection-background-color: #0A84FF;
    selection-color: #FFFFFF;
}
QLineEdit:hover, QSpinBox:hover, QDoubleSpinBox:hover, QComboBox:hover {
    border-color: #5A5A5E;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
    border-color: #0A84FF;
}
QComboBox::drop-down {
    border: none;
    width: 22px;
}
QComboBox::down-arrow {
    image: none;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 5px solid #98989D;
    width: 0px;
    height: 0px;
    margin-right: 7px;
}
QComboBox QAbstractItemView {
    background: #2C2C2E;
    border: 1px solid #48484A;
    border-radius: 10px;
    outline: none;
    padding: 4px;
    selection-background-color: #0A84FF;
    selection-color: #FFFFFF;
}
QSpinBox::up-button, QSpinBox::down-button {
    background: transparent;
    border: none;
    width: 14px;
}
QSpinBox::up-arrow {
    image: none;
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-bottom: 4px solid #98989D;
}
QSpinBox::down-arrow {
    image: none;
    border-left: 3px solid transparent;
    border-right: 3px solid transparent;
    border-top: 4px solid #98989D;
}
QLabel#FieldLabel {
    color: #98989D;
    font-size: 11px;
}
QLabel#Muted {
    color: #98989D;
    font-size: 12px;
}
QLabel#StatusChip {
    color: #C7C7CC;
    font-size: 12px;
    /* No padding and no centring here, on purpose. A stylesheet padding is added
       around the text without always reaching QLabel::sizeHint(), so a padded chip
       can be laid out narrower than the text it draws and spill over its
       neighbour. Spacing between the chips comes from their layout instead
       (cornerLayout spacing), which the width calculation does account for. */
}
QLabel#SectionTitle {
    color: #F2F2F7;
    font-size: 14px;
    font-weight: 600;
}
QLabel#TimeLabel {
    color: #98989D;
    font-size: 12px;
    font-family: "SF Mono", "Cascadia Mono", "Consolas", monospace;
}

/* ---------- cards ---------- */
QFrame#Card {
    background: #2C2C2E;
    border: 1px solid #38383A;
    border-radius: 14px;
}
QFrame#PopupCard {
    background: #2C2C2E;
    border: 1px solid #48484A;
    border-radius: 16px;
}
QLabel#PopupTitle {
    color: #F2F2F7;
    font-size: 15px;
    font-weight: 600;
}
QLabel#PopupSubtitle {
    color: #98989D;
    font-size: 12px;
}

/* ---------- media library ---------- */
QListView#MediaGrid {
    background: #232326;
    border: 1px solid #38383A;
    border-radius: 12px;
    outline: none;
    padding: 4px;
}
QScrollArea#MediaGridScroll {
    background: transparent;
    border: none;
}
QScrollArea#MediaGridScroll > QWidget > QWidget {
    background: transparent;
}
QScrollArea#FolderChecks {
    background: #232326;
    border: 1px solid #38383A;
    border-radius: 10px;
}
QScrollArea#FolderChecks > QWidget > QWidget {
    background: transparent;
}
QLabel#MediaDetails {
    background: #232326;
    border: 1px solid #38383A;
    border-radius: 10px;
    padding: 8px 10px;
}
QListWidget {
    background: #232326;
    border: 1px solid #38383A;
    border-radius: 12px;
    outline: none;
    padding: 4px;
}
QListWidget::item {
    border-radius: 8px;
    margin: 1px 2px;
    color: #F2F2F7;
}
QListWidget::item:hover {
    background: #2C2C2E;
}
QListWidget::item:selected {
    background: #0A84FF;
    color: #FFFFFF;
}
QLabel#MediaName {
    color: #F2F2F7;
    font-size: 13px;
}
QLabel#Badge {
    background: #2C2C2E;
    border: 1px solid #48484A;
    border-radius: 7px;
    color: #98989D;
    font-size: 11px;
    padding: 1px 7px;
}
QLabel#BadgeAccent {
    background: rgba(10, 132, 255, 0.18);
    border: 1px solid rgba(10, 132, 255, 0.45);
    border-radius: 7px;
    color: #6BB4FF;
    font-size: 11px;
    padding: 1px 7px;
}

/* ---------- player ---------- */
QFrame#VideoArea {
    background: #000000;
    border: 1px solid #38383A;
    border-radius: 14px;
}
QFrame#PlayerBar {
    background: #232326;
    border: 1px solid #38383A;
    border-radius: 14px;
}
#Heatmap {
    background: #1A1A1D;
    border: 1px solid #38383A;
    border-radius: 8px;
}
QSlider#Seek::groove:horizontal {
    height: 4px;
    background: #48484A;
    border-radius: 2px;
}
QSlider#Seek::sub-page:horizontal {
    background: #0A84FF;
    border-radius: 2px;
}
QSlider#Volume::groove:horizontal {
    height: 4px;
    background: #48484A;
    border-radius: 2px;
}
QSlider#Volume::sub-page:horizontal {
    background: #98989D;
    border-radius: 2px;
}
QSlider#Volume::handle:horizontal {
    background: #F2F2F7;
    width: 10px;
    margin: -5px 0;
    border-radius: 5px;
}
QSlider#Seek::add-page:horizontal {
    background: #48484A;
    border-radius: 2px;
}
QSlider#Seek::handle:horizontal {
    width: 13px;
    height: 13px;
    margin: -5px 0px;
    border-radius: 6px;
    background: #F2F2F7;
}
QSlider#Seek::handle:horizontal:hover {
    background: #FFFFFF;
}

/* ---------- media library splitter ---------- */
QSplitter::handle:horizontal {
    background: transparent;
}
QSplitter::handle:horizontal:hover {
    background: rgba(10, 132, 255, 110);
}

/* ---------- axis rows ---------- */
QLabel#AxisName {
    color: #F2F2F7;
    font-size: 13px;
}
QLabel#AxisCode {
    color: #98989D;
    font-size: 11px;
}
QLabel#AxisPosition {
    color: #98989D;
    font-size: 12px;
    font-family: "SF Mono", "Cascadia Mono", "Consolas", monospace;
}

/* ---------- scrollbars ---------- */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 2px;
}
QScrollBar::handle:vertical {
    background: #48484A;
    border-radius: 4px;
    min-height: 34px;
}
QScrollBar::handle:vertical:hover {
    background: #5A5A5E;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
    background: transparent;
    height: 0px;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 2px;
}
QScrollBar::handle:horizontal {
    background: #48484A;
    border-radius: 4px;
    min-width: 34px;
}
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal,
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal {
    background: transparent;
    width: 0px;
}

/* ---------- dialogs ---------- */
QGroupBox {
    border: 1px solid #38383A;
    border-radius: 12px;
    margin-top: 12px;
    padding: 12px 12px 10px 12px;
    background: #2C2C2E;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 12px;
    padding: 0 4px;
    color: #98989D;
}
QCheckBox {
    spacing: 7px;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border-radius: 5px;
    border: 1px solid #5A5A5E;
    background: #2C2C2E;
}
QCheckBox::indicator:checked {
    background: #0A84FF;
    border-color: #0A84FF;
}
QDialogButtonBox QPushButton {
    min-width: 74px;
}
)QSS");
}
