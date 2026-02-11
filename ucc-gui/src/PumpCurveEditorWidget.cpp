#include "PumpCurveEditorWidget.hpp"
#include <QPainter>
#include <QMouseEvent>
#include <algorithm>

PumpCurveEditorWidget::PumpCurveEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    // Default 3 threshold points: each one bumps pump level up by 1
    // Levels: Off -> V7 -> V8 -> V11
    // 12V intentionally omitted — it can be harmful to the pump
    m_points = {
        {40.0, 1},  // at 40°C: pump goes to level 1 (V7)
        {55.0, 2},  // at 55°C: pump goes to level 2 (V8)
        {70.0, 3},  // at 70°C: pump goes to level 3 (V11)
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
    std::sort(m_points.begin(), m_points.end(),
              [](const Point& a, const Point& b) { return a.temp < b.temp; });
    // Re-assign levels 1..3 in temperature order
    for (int i = 0; i < m_points.size(); ++i)
        m_points[i].level = i + 1;
}

QPointF PumpCurveEditorWidget::toWidget(const Point& pt) const {
    const int left = 110, right = 20, top = 28, bottom = 68;
    double plotW = width() - left - right;
    double plotH = height() - top - bottom;
    double x = left + (pt.temp - 20.0) / 80.0 * plotW;
    // Map level 0..3 to y axis (bottom=0, top=3)
    double y = top + (1.0 - pt.level / 3.0) * plotH;
    return QPointF(x, y);
}

double PumpCurveEditorWidget::tempFromWidgetX(double x) const {
    const int left = 110, right = 20;
    double plotW = width() - left - right;
    double temp = (x - left) / plotW * 80.0 + 20.0;
    return std::clamp(temp, 20.0, 100.0);
}

QRectF PumpCurveEditorWidget::pointRect(const Point& pt) const {
    QPointF c = toWidget(pt);
    return QRectF(c.x() - 8.0, c.y() - 8.0, 16.0, 16.0);
}

void PumpCurveEditorWidget::enforceOrdering() {
    // Ensure temperatures are in ascending order with minimum 1°C gap
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
    p.fillRect(rect(), QColor("#181e26"));

    const int left = 110, right = 20, top = 28, bottom = 68;
    QRectF plotRect(left, top, width() - left - right, height() - top - bottom);

    QFont tickFont = font();
    tickFont.setPointSize(9);
    tickFont.setWeight(QFont::Normal);
    p.setFont(tickFont);
    QColor gridColor(50,50,50);
    QColor labelColor("#bdbdbd");

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

    // X grid/ticks/labels: 20–100°C every 5°C
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
    int yLabelX = left / 2 - 6;
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
    QRectF xLabelRect(plotRect.left(), plotRect.bottom() + 22, plotRect.width(), 20);
    p.drawText(xLabelRect, Qt::AlignHCenter | Qt::AlignTop, QString::fromUtf8("Temperature (°C)"));

    // Border
    p.setPen(QPen(labelColor, 1));
    QRectF borderRect(qRound(plotRect.left()) + 0.5, qRound(plotRect.top()) + 0.5,
                      qRound(plotRect.width()) - 1.0, qRound(plotRect.height()) - 1.0);
    p.drawRect(borderRect);

    // Draw step curve: Off from left edge to first point, then steps
    QColor curveColor("#e040fb");  // purple for pump curve
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
                p.setBrush(QColor("#ffa726"));
                p.setPen(QPen(QColor("#ff6f00"), 2));
            } else {
                p.setBrush(Qt::white);
                p.setPen(QPen(curveColor, 2));
            }
        } else {
            p.setBrush(QColor("#666666"));
            p.setPen(QPen(QColor("#999999"), 2));
        }
        p.drawEllipse(r);

        // Draw temperature label below the point
        p.setPen(labelColor);
        QPointF wp = toWidget(m_points[i]);
        QString tempLabel = QString::number(qRound(m_points[i].temp)) + QChar(0x00B0);
        QRectF tLabelRect(wp.x() - 20, wp.y() - 22, 40, 16);
        p.drawText(tLabelRect, Qt::AlignHCenter | Qt::AlignBottom, tempLabel);
    }

    // Draw rubber band
    if (m_rubberBandActive && m_rubberBandRect.isValid()) {
        QColor bandFill(224, 64, 251, 40);
        QColor bandBorder(224, 64, 251, 160);
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
