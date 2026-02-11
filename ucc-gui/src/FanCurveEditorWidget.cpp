#include "FanCurveEditorWidget.hpp"
#include <QPainter>
#include <QMouseEvent>
#include <QMenu>
#include <algorithm>

FanCurveEditorWidget::FanCurveEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    m_points.clear();
    // initialize 17 evenly spaced points from 20..100 with linear duty 0..100
    const int count = 17;
    for (int i = 0; i < count; ++i) {
        double t = 20.0 + (80.0 * i) / (count - 1); // 20..100
        double d = (100.0 * i) / (count - 1); // 0..100
        m_points.append({t, d});
    }
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void FanCurveEditorWidget::setPoints(const QVector<Point>& pts) {
    m_points = pts;
    sortPoints();
    // Enforce monotonicity on all points
    for (int i = 0; i < m_points.size(); ++i) {
        enforceMonotonicity(i);
    }
    m_selectedIndices.clear();
    update();
    emit pointsChanged(m_points);
}

void FanCurveEditorWidget::sortPoints() {
    std::sort(m_points.begin(), m_points.end(), [](const Point& a, const Point& b) { return a.temp < b.temp; });
}

QPointF FanCurveEditorWidget::toWidget(const Point& pt) const {
    // use same margins as paintEvent for consistent mapping
    const int left = 110, right = 20, top = 28, bottom = 68;
    double plotW = width() - left - right;
    double plotH = height() - top - bottom;
    double x = left + (pt.temp - 20.0) / 80.0 * plotW;
    double y = top + (1.0 - pt.duty / 100.0) * plotH;
    return QPointF(x, y);
}

FanCurveEditorWidget::Point FanCurveEditorWidget::fromWidget(const QPointF& pos) const {
    const int left = 110, right = 20, top = 28, bottom = 68;
    double plotW = width() - left - right;
    double plotH = height() - top - bottom;
    double temp = (pos.x() - left) / plotW * 80.0 + 20.0;
    double duty = (1.0 - (pos.y() - top) / plotH) * 100.0;
    return { std::clamp(temp, 20.0, 100.0), std::clamp(duty, 0.0, 100.0) };
}

QRectF FanCurveEditorWidget::pointRect(const Point& pt) const {
    QPointF c = toWidget(pt);
    return QRectF(c.x() - 7.0, c.y() - 7.0, 14.0, 14.0);
}

void FanCurveEditorWidget::enforceMonotonicity(int modifiedIndex) {
    if (modifiedIndex < 0 || modifiedIndex >= m_points.size()) return;
    
    double currentDuty = m_points[modifiedIndex].duty;
    
    // Check all higher temperature points (higher indices)
    for (int i = modifiedIndex + 1; i < m_points.size(); ++i) {
        if (m_points[i].duty < currentDuty) {
            m_points[i].duty = currentDuty;
        }
    }    
    // Check all lower temperature points (lower indices) - ensure they are <= current duty
    for (int i = modifiedIndex - 1; i >= 0; --i) {
        if (m_points[i].duty > currentDuty) {
            m_points[i].duty = currentDuty;
        }
    }
}

void FanCurveEditorWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor("#181e26"));
    // Margins for axes
    int left = 110, right = 20, top = 28, bottom = 68;
    QRectF plotRect(left, top, width() - left - right, height() - top - bottom);

    // Draw grid and ticks/labels
    QFont tickFont = font();
    tickFont.setPointSize(9);
    tickFont.setWeight(QFont::Normal);
    p.setFont(tickFont);
    QColor gridColor(50,50,50);
    QColor labelColor("#bdbdbd");

    // Y grid/ticks/labels (0-100% every 20%)
    for (int i = 0; i <= 5; ++i) {
        double frac = i / 5.0;
        qreal y = plotRect.top() + (1.0 - frac) * plotRect.height();
        qreal yy = qRound(y) + 0.5;
        // grid line
        p.setPen(gridColor);
        p.drawLine(QPointF(qRound(plotRect.left()) + 0.5, yy), QPointF(qRound(plotRect.right()) + 0.5, yy));
        // tick label
        int duty = i * 20;
        QString label = QString::number(duty) + "%";
        p.setPen(labelColor);
        QRectF labelRect(0, yy-12, left-16, 24);
        p.drawText(labelRect, Qt::AlignRight|Qt::AlignVCenter, label);
    }

    // X grid/ticks/labels (20-100°C every 5°C)
    for (int i = 0; i <= 16; ++i) {
        double frac = i / 16.0;
        qreal x = plotRect.left() + frac * plotRect.width();
        qreal xx = qRound(x) + 0.5;
        // grid line
        p.setPen(gridColor);
        p.drawLine(QPointF(xx, qRound(plotRect.top()) + 0.5), QPointF(xx, qRound(plotRect.bottom()) + 0.5));
        // tick label
        int temp = 20 + i * 5;
        QString label = QString::number(temp) + QChar(0x00B0) + "C";
        p.setPen(labelColor);
        QRectF labelRect(xx-20, plotRect.bottom()+12, 40, 20);
        p.drawText(labelRect, Qt::AlignHCenter|Qt::AlignTop, label);
    }

    // Axis labels
    QFont axisFont = font();
    axisFont.setPointSize(11);
    axisFont.setWeight(QFont::Normal);
    p.setPen(labelColor);
    // Y axis label rotated, outside tick labels
    QFont yFont = axisFont;
    yFont.setPointSize(10);
    p.save();
    int yLabelX = left/2 - 6; // put rotated label centered in the left margin, slight left offset
    p.translate(yLabelX, plotRect.center().y());
    p.rotate(-90);
    p.setFont(yFont);
    QRectF yLabelRect(-plotRect.height()/2, -12, plotRect.height(), 24);
    p.drawText(yLabelRect, Qt::AlignCenter, "% Duty");
    p.restore();
    // X axis label
    p.setFont(axisFont);
    QRectF xLabelRect(plotRect.left(), plotRect.bottom()+22, plotRect.width(), 20);
    p.drawText(xLabelRect, Qt::AlignHCenter|Qt::AlignTop, "Temperature (°C)");

    // Draw border around plot area (half-pixel aligned)
    p.setPen(QPen(labelColor, 1));
    QRectF borderRect(qRound(plotRect.left()) + 0.5, qRound(plotRect.top()) + 0.5,
                      qRound(plotRect.width()) - 1.0, qRound(plotRect.height()) - 1.0);
    p.drawRect(borderRect);

    // Draw curve
    p.setFont(tickFont);
    p.setPen(QPen(QColor("#3fa9f5"), 3));
    for (int i = 1; i < m_points.size(); ++i) {
        p.drawLine(toWidget(m_points[i-1]), toWidget(m_points[i]));
    }
    // Draw points
    for (int i = 0; i < m_points.size(); ++i) {
        QRectF r = pointRect(m_points[i]);
        if (m_editable) {
            bool selected = m_selectedIndices.contains(i);
            if (selected) {
                p.setBrush(QColor("#ffa726"));  // orange fill for selected
                p.setPen(QPen(QColor("#ff6f00"), 2));
            } else {
                p.setBrush(Qt::white);
                p.setPen(QPen(QColor("#3fa9f5"), 2));
            }
        } else {
            p.setBrush(QColor("#666666"));
            p.setPen(QPen(QColor("#999999"), 2));
        }
        p.drawEllipse(r);
    }

    // Draw rubber band selection rectangle
    if (m_rubberBandActive && m_rubberBandRect.isValid()) {
        QColor bandFill(63, 169, 245, 40);
        QColor bandBorder(63, 169, 245, 160);
        p.setBrush(bandFill);
        p.setPen(QPen(bandBorder, 1, Qt::DashLine));
        p.drawRect(m_rubberBandRect);
    }
}

void FanCurveEditorWidget::mousePressEvent(QMouseEvent* e) {
    if (!m_editable) return;
    m_ctrlHeld = (e->modifiers() & Qt::ControlModifier);

    // Check if clicked on a point
    int hitIndex = -1;
    for (int i = 0; i < m_points.size(); ++i) {
        if (pointRect(m_points[i]).contains(e->pos())) {
            hitIndex = i;
            break;
        }
    }

    if (hitIndex >= 0) {
        if (m_ctrlHeld) {
            // Ctrl+click: toggle selection
            if (m_selectedIndices.contains(hitIndex))
                m_selectedIndices.remove(hitIndex);
            else
                m_selectedIndices.insert(hitIndex);
        } else {
            // Plain click on a point: if not already selected, replace selection
            if (!m_selectedIndices.contains(hitIndex)) {
                m_selectedIndices.clear();
                m_selectedIndices.insert(hitIndex);
            }
        }
        // Start dragging the entire selection
        m_draggedIndex = hitIndex;
        m_dragStartY = e->pos().y();
        m_dragStartDuties.resize(m_points.size());
        for (int i = 0; i < m_points.size(); ++i)
            m_dragStartDuties[i] = m_points[i].duty;
        update();
        return;
    }

    // Clicked on empty space - start rubber band
    if (!m_ctrlHeld)
        m_selectedIndices.clear();
    m_rubberBandActive = true;
    m_rubberBandOrigin = e->pos();
    m_rubberBandRect = QRect();
    update();
}

void FanCurveEditorWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!m_editable) return;

    // Rubber band selection
    if (m_rubberBandActive) {
        m_rubberBandRect = QRect(m_rubberBandOrigin, e->pos()).normalized();
        // Preview: select all points inside the rubber band
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

    // Dragging selected points
    if (m_draggedIndex < 0) return;

    const int top = 28, bottom = 68;
    double plotH = height() - top - bottom;
    double deltaY = e->pos().y() - m_dragStartY;
    double deltaDuty = -(deltaY / plotH) * 100.0;

    for (int idx : m_selectedIndices) {
        if (idx < 0 || idx >= m_points.size()) continue;
        double newDuty = m_dragStartDuties[idx] + deltaDuty;
        newDuty = std::clamp(newDuty, 0.0, 100.0);
        m_points[idx].duty = newDuty;
    }

    // Enforce monotonicity from each selected point
    // Process from lowest to highest index
    QList<int> sorted = m_selectedIndices.values();
    std::sort(sorted.begin(), sorted.end());
    for (int idx : sorted) {
        enforceMonotonicity(idx);
    }

    update();
    emit pointsChanged(m_points);
}

void FanCurveEditorWidget::mouseReleaseEvent(QMouseEvent*) {
    if (m_rubberBandActive) {
        m_rubberBandActive = false;
        m_rubberBandRect = QRect();
        update();
    }
    m_draggedIndex = -1;
}

void FanCurveEditorWidget::contextMenuEvent(QContextMenuEvent*) {
    // No context menu - points are fixed
}

void FanCurveEditorWidget::addPoint(const Point& pt) {
    // Snap temperature to nearest 5°C grid position
    double snappedTemp = std::round((pt.temp - 20.0) / 5.0) * 5.0 + 20.0;
    snappedTemp = std::clamp(snappedTemp, 20.0, 100.0);
    
    // Check if a point already exists at this temperature
    for (const auto& existingPt : m_points) {
        if (std::abs(existingPt.temp - snappedTemp) < 1.0) {
            return; // Don't add duplicate
        }
    }
    
    m_points.push_back({snappedTemp, pt.duty});
    sortPoints();
    
    // Find the index of the newly added point and enforce monotonicity
    int newIndex = -1;
    for (int i = 0; i < m_points.size(); ++i) {
        if (std::abs(m_points[i].temp - snappedTemp) < 1.0) {
            newIndex = i;
            break;
        }
    }
    if (newIndex >= 0) {
        enforceMonotonicity(newIndex);
    }
    
    update();
    emit pointsChanged(m_points);
}

void FanCurveEditorWidget::removePoint(int idx) {
    if (idx > 0 && idx < m_points.size()-1 && m_points.size() > 9) {
        m_points.remove(idx);
        update();
        emit pointsChanged(m_points);
    }
}
