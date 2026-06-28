#include "PumpCurveEditorWidget.hpp"
#include <QPainter>
#include <QPalette>
#include <QMouseEvent>
#include <algorithm>
#include <ranges>

PumpCurveEditorWidget::PumpCurveEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    // Default 3 threshold points: each one bumps pump level up by 1
    // Levels: Off -> V7 -> V8 -> V11
    // 12V intentionally omitted - it can be harmful to the pump
    m_points = {
        {40.0, 1},  // at 40 degC: pump goes to level 1 (V7)
        {55.0, 2},  // at 55 degC: pump goes to level 2 (V8)
        {70.0, 3},  // at 70 degC: pump goes to level 3 (V11)
    };
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QString PumpCurveEditorWidget::levelLabel(int level) {
    switch (level) {
        case 0: return QStringLiteral("Off");
        case 1: return QStringLiteral("7V");
        case 2: return QStringLiteral("8V");
        case 3: return QStringLiteral("11V");
        case 4: return QStringLiteral("12V");
        default: return QStringLiteral("?");
    }
}

void PumpCurveEditorWidget::setPoints(const QVector<Point>& pts) {
    m_points = pts;
    // Ensure exactly 3 points with levels 1..3 (12V/level 4 omitted)
    // If old profiles had 4 points, drop the highest
    if (m_points.size() > 3)
        m_points.resize(3);
    while (m_points.size() < 3) {
        int lvl = m_points.size() + 1;
        m_points.append({20.0 + lvl * 20.0, lvl});
    }
    // Fix levels to 1..3 in order
    for (int i = 0; i < 3; ++i)
        m_points[i].level = i + 1;
    sortPoints();
    m_selectedIndices.clear();
    update();
    emit pointsChanged(m_points);
}

void PumpCurveEditorWidget::sortPoints() {
    std::ranges::sort(m_points, {}, &Point::temp);
    // Re-assign levels 1..3 in temperature order
    for (int i = 0; i < m_points.size(); ++i)
        m_points[i].level = i + 1;
}

void PumpCurveEditorWidget::setCrosshair( double temp, int level )
{
    m_crosshairVisible = true;
    m_crosshairTemp = temp;
    m_crosshairLevel = std::clamp( level, 0, 3 );
    update();
}

void PumpCurveEditorWidget::clearCrosshair()
{
    m_crosshairVisible = false;
    update();
}

QPointF PumpCurveEditorWidget::toWidget(const Point& pt) const {
    const int left = 80, right = 20, top = 28, bottom = 68;
    double plotW = width() - left - right;
    double plotH = height() - top - bottom;
    double x = left + (pt.temp - 20.0) / 80.0 * plotW;
    // Map level 0..3 to y axis (bottom=0, top=3)
    double y = top + (1.0 - pt.level / 3.0) * plotH;
    return QPointF(x, y);
}

double PumpCurveEditorWidget::tempFromWidgetX(double x) const {
    const int left = 80, right = 20;
    double plotW = width() - left - right;
    double temp = (x - left) / plotW * 80.0 + 20.0;
    return std::clamp(temp, 20.0, 100.0);
}

QRectF PumpCurveEditorWidget::pointRect(const Point& pt) const {
    QPointF c = toWidget(pt);
    return QRectF(c.x() - 8.0, c.y() - 8.0, 16.0, 16.0);
}

void PumpCurveEditorWidget::enforceOrdering() {
    // Ensure temperatures are in ascending order with minimum 1 degC gap
    for (int i = 1; i < m_points.size(); ++i) {
        if (m_points[i].temp <= m_points[i-1].temp) {
            m_points[i].temp = std::min(m_points[i-1].temp + 1.0, 100.0);
        }
    }
    // Backward pass to fix overflows
    for (int i = m_points.size() - 2; i >= 0; --i) {
        if (m_points[i].temp >= m_points[i+1].temp) {
            m_points[i].temp = std::max(m_points[i+1].temp - 1.0, 20.0);
        }
    }
}

void PumpCurveEditorWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QPalette &pal = palette();
    const QColor bgColor       = pal.color(QPalette::Base);
    const QColor gridColor     = pal.color(QPalette::Mid);
    const QColor labelColor    = pal.color(QPalette::Text);
    const QColor brightText    = pal.color(QPalette::BrightText);
    const QColor disabledFill  = pal.color(QPalette::Disabled, QPalette::Mid);
    const QColor disabledBorder= pal.color(QPalette::Disabled, QPalette::Light);

    // Data-visualization colors: warm tones that won't collide with
    // typical blue-ish GUI highlight/link palette roles.
    const bool darkTheme = bgColor.lightnessF() < 0.5;
    const QColor curveColor    = darkTheme ? QColor( 0x3f, 0xa9, 0xf5 ) : QColor( 0x19, 0x76, 0xd2 );
    const QColor accentColor   = darkTheme ? QColor( 0xff, 0x57, 0x22 ) : QColor( 0xe6, 0x4a, 0x19 );
    const QColor selectedFill  = darkTheme ? QColor( 0xff, 0xa7, 0x26 ) : QColor( 0xfb, 0x8c, 0x00 );
    const QColor selectedBorder= darkTheme ? QColor( 0xff, 0x6f, 0x00 ) : QColor( 0xe6, 0x51, 0x00 );

    p.fillRect(rect(), bgColor);

    const int left = 80, right = 20, top = 28, bottom = 68;

    // Draw title at the top of the widget if set
    if (!m_title.isEmpty()) {
        QFont titleFont = font();
        titleFont.setPointSize(11);
        titleFont.setWeight(QFont::Bold);
        p.setFont(titleFont);
        p.setPen(labelColor);
        QRectF titleRect(left, 2, width() - left - right, top - 4);
        p.drawText(titleRect, Qt::AlignCenter, m_title);
    }

    QRectF plotRect(left, top, width() - left - right, height() - top - bottom);

    QFont tickFont = font();
    tickFont.setPointSize(9);
    tickFont.setWeight(QFont::Normal);
    p.setFont(tickFont);

    // Y grid/ticks/labels: pump levels Off, V7, V8, V11
    for (int lvl = 0; lvl <= 3; ++lvl) {
        double frac = lvl / 3.0;
        qreal y = plotRect.top() + (1.0 - frac) * plotRect.height();
        qreal yy = qRound(y) + 0.5;
        p.setPen(gridColor);
        p.drawLine(QPointF(qRound(plotRect.left()) + 0.5, yy),
                   QPointF(qRound(plotRect.right()) + 0.5, yy));
        p.setPen(labelColor);
        QRectF labelRect(0, yy - 12, left - 16, 24);
        p.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter, levelLabel(lvl));
    }

    // X grid/ticks/labels: 20-100 degC every 5 degC
    for (int i = 0; i <= 16; ++i) {
        double frac = i / 16.0;
        qreal x = plotRect.left() + frac * plotRect.width();
        qreal xx = qRound(x) + 0.5;
        p.setPen(gridColor);
        p.drawLine(QPointF(xx, qRound(plotRect.top()) + 0.5),
                   QPointF(xx, qRound(plotRect.bottom()) + 0.5));
        int temp = 20 + i * 5;
        p.setPen(labelColor);
        QRectF labelRect(xx - 20, plotRect.bottom() + 12, 40, 20);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop,
                   QString::number(temp) + QChar(0x00B0) + "C");
    }

    // Y axis label
    QFont yFont = font();
    yFont.setPointSize(10);
    p.setPen(labelColor);
    p.save();
    int yLabelX = 14; // close to the left edge of the widget
    p.translate(yLabelX, plotRect.center().y());
    p.rotate(-90);
    p.setFont(yFont);
    QRectF yLabelRect(-plotRect.height() / 2, -12, plotRect.height(), 24);
    p.drawText(yLabelRect, Qt::AlignCenter, "Pump Level");
    p.restore();

    // X axis label
    QFont axisFont = font();
    axisFont.setPointSize(11);
    p.setFont(axisFont);
    p.setPen(labelColor);
    QRectF xLabelRect(plotRect.left(), plotRect.bottom() + 28, plotRect.width(), 20);
    p.drawText(xLabelRect, Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8("Temperature (°C)"));

    // Border
    p.setPen(QPen(labelColor, 1));
    QRectF borderRect(qRound(plotRect.left()) + 0.5, qRound(plotRect.top()) + 0.5,
                      qRound(plotRect.width()) - 1.0, qRound(plotRect.height()) - 1.0);
    p.drawRect(borderRect);

    // Draw step curve: Off from left edge to first point, then steps
    p.setPen(QPen(curveColor, 3));
    p.setFont(tickFont);

    // Baseline: Off level from left edge to first threshold
    double offY = plotRect.top() + plotRect.height();  // level 0 = bottom
    QPointF firstWidgetPt = toWidget(m_points[0]);
    p.drawLine(QPointF(plotRect.left(), offY), QPointF(firstWidgetPt.x(), offY));

    // Vertical rise to first point level
    p.drawLine(QPointF(firstWidgetPt.x(), offY), firstWidgetPt);

    // Step-wise between thresholds
    for (int i = 0; i < m_points.size() - 1; ++i) {
        QPointF cur = toWidget(m_points[i]);
        QPointF next = toWidget(m_points[i + 1]);
        // Horizontal at current level to next threshold's x
        p.drawLine(cur, QPointF(next.x(), cur.y()));
        // Vertical rise to next level
        p.drawLine(QPointF(next.x(), cur.y()), next);
    }

    // Horizontal from last point to right edge at last level
    QPointF lastWidgetPt = toWidget(m_points.back());
    p.drawLine(lastWidgetPt, QPointF(plotRect.right(), lastWidgetPt.y()));

    // Draw points
    for (int i = 0; i < m_points.size(); ++i) {
        QRectF r = pointRect(m_points[i]);
        if (m_editable) {
            bool selected = m_selectedIndices.contains(i);
            if (selected) {
                p.setBrush(selectedFill);
                p.setPen(QPen(selectedBorder, 2));
            } else {
                p.setBrush(brightText);
                p.setPen(QPen(curveColor, 2));
            }
        } else {
            p.setBrush(disabledFill);
            p.setPen(QPen(disabledBorder, 2));
        }
        p.drawEllipse(r);

        // Draw temperature label to the left of the point
        p.setPen(labelColor);
        QPointF wp = toWidget(m_points[i]);
        QString tempLabel = QString::number(qRound(m_points[i].temp)) + QChar(0x00B0);
        QRectF tLabelRect(wp.x() - 50, wp.y() + 6, 42, 16);
        p.drawText(tLabelRect, Qt::AlignRight | Qt::AlignVCenter, tempLabel);
    }

    // Draw live crosshair overlay
    if (m_crosshairVisible) {
        Point crossPt { m_crosshairTemp, m_crosshairLevel };
        QPointF cp = toWidget(crossPt);

        // Clamp to plot area
        cp.setX(std::clamp(cp.x(), (double)plotRect.left(), (double)plotRect.right()));
        cp.setY(std::clamp(cp.y(), (double)plotRect.top(), (double)plotRect.bottom()));

        // Dashed crosshair lines
        QPen crossPen(accentColor, 1.5, Qt::DashLine);
        p.setPen(crossPen);
        p.drawLine(QPointF(cp.x(), plotRect.top()), QPointF(cp.x(), plotRect.bottom()));
        p.drawLine(QPointF(plotRect.left(), cp.y()), QPointF(plotRect.right(), cp.y()));

        // Crosshair dot
        p.setBrush(accentColor);
        p.setPen(QPen(brightText, 1.5));
        p.drawEllipse(cp, 5.0, 5.0);

        // Labels
        QFont labelFont = font();
        labelFont.setPointSize(8);
        labelFont.setWeight(QFont::Bold);
        p.setFont(labelFont);

        // Temperature label (below X axis)
        QString tempLabel = QString::number(m_crosshairTemp, 'f', 0) + QChar(0x00B0) + "C";
        p.setPen(accentColor);
        QRectF tempLabelRect(cp.x() - 20, plotRect.bottom() + 1, 40, 14);
        p.fillRect(tempLabelRect, bgColor);
        p.drawText(tempLabelRect, Qt::AlignHCenter | Qt::AlignTop, tempLabel);

        // Level label (left of Y axis)
        QString lvlLabel = levelLabel(m_crosshairLevel);
        QRectF lvlLabelRect(plotRect.left() - 55, cp.y() - 7, 53, 14);
        p.fillRect(lvlLabelRect, bgColor);
        p.drawText(lvlLabelRect, Qt::AlignRight | Qt::AlignVCenter, lvlLabel);
    }

    // Draw rubber band
    if (m_rubberBandActive && m_rubberBandRect.isValid()) {
        QColor bandFill = selectedFill;
        bandFill.setAlpha(40);
        QColor bandBorder = selectedFill;
        bandBorder.setAlpha(160);
        p.setBrush(bandFill);
        p.setPen(QPen(bandBorder, 1, Qt::DashLine));
        p.drawRect(m_rubberBandRect);
    }
}

void PumpCurveEditorWidget::mousePressEvent(QMouseEvent* e) {
    if (!m_editable) return;
    m_ctrlHeld = (e->modifiers() & Qt::ControlModifier);

    int hitIndex = -1;
    for (int i = 0; i < m_points.size(); ++i) {
        if (pointRect(m_points[i]).contains(e->pos())) {
            hitIndex = i;
            break;
        }
    }

    if (hitIndex >= 0) {
        if (m_ctrlHeld) {
            if (m_selectedIndices.contains(hitIndex))
                m_selectedIndices.remove(hitIndex);
            else
                m_selectedIndices.insert(hitIndex);
        } else {
            if (!m_selectedIndices.contains(hitIndex)) {
                m_selectedIndices.clear();
                m_selectedIndices.insert(hitIndex);
            }
        }
        m_draggedIndex = hitIndex;
        m_dragStartX = e->pos().x();
        m_dragStartTemps.resize(m_points.size());
        for (int i = 0; i < m_points.size(); ++i)
            m_dragStartTemps[i] = m_points[i].temp;
        update();
        return;
    }

    // Rubber band
    if (!m_ctrlHeld)
        m_selectedIndices.clear();
    m_rubberBandActive = true;
    m_rubberBandOrigin = e->pos();
    m_rubberBandRect = QRect();
    update();
}

void PumpCurveEditorWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_editable) return;

    if (m_rubberBandActive) {
        m_rubberBandRect = QRect(m_rubberBandOrigin, e->pos()).normalized();
        if (!m_ctrlHeld)
            m_selectedIndices.clear();
        for (int i = 0; i < m_points.size(); ++i) {
            QPointF wp = toWidget(m_points[i]);
            if (m_rubberBandRect.contains(wp.toPoint()))
                m_selectedIndices.insert(i);
        }
        update();
        return;
    }

    if (m_draggedIndex < 0) return;

    // Drag horizontally (temperature only; levels are fixed 1..4)
    const int left = 110, right = 20;
    double plotW = width() - left - right;
    double deltaX = e->pos().x() - m_dragStartX;
    double deltaTemp = deltaX / plotW * 80.0;

    for (int idx : m_selectedIndices) {
        if (idx < 0 || idx >= m_points.size()) continue;
        double newTemp = m_dragStartTemps[idx] + deltaTemp;
        m_points[idx].temp = std::clamp(newTemp, 20.0, 100.0);
    }

    enforceOrdering();
    update();
    emit pointsChanged(m_points);
}

void PumpCurveEditorWidget::mouseReleaseEvent(QMouseEvent*) {
    if (m_rubberBandActive) {
        m_rubberBandActive = false;
        m_rubberBandRect = QRect();
        update();
    }
    m_draggedIndex = -1;
}
