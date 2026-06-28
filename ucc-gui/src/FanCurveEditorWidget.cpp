#include "FanCurveEditorWidget.hpp"
#include <QPainter>
#include <QPalette>
#include <QMouseEvent>
#include <QMenu>
#include <algorithm>
#include <ranges>

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

void FanCurveEditorWidget::setCrosshair( double temp, double duty )
{
    m_crosshairVisible = true;
    m_crosshairTemp = temp;
    m_crosshairDuty = duty;
    update();
}

void FanCurveEditorWidget::clearCrosshair()
{
    m_crosshairVisible = false;
    update();
}

void FanCurveEditorWidget::sortPoints() {
    std::ranges::sort(m_points, {}, &Point::temp);
}

QPointF FanCurveEditorWidget::toWidget(const Point& pt) const {
    // use same margins as paintEvent for consistent mapping
    const int left = 80, right = 20, top = 28, bottom = 68;
    double plotW = width() - left - right;
    double plotH = height() - top - bottom;
    double x = left + (pt.temp - 20.0) / 80.0 * plotW;
    double y = top + (1.0 - pt.duty / 100.0) * plotH;
    return QPointF(x, y);
}

FanCurveEditorWidget::Point FanCurveEditorWidget::fromWidget(const QPointF& pos) const {
    const int left = 80, right = 20, top = 28, bottom = 68;
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
    // Margins for axes
    int left = 80, right = 20, top = 28, bottom = 68;

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

    // Draw grid and ticks/labels
    QFont tickFont = font();
    tickFont.setPointSize(9);
    tickFont.setWeight(QFont::Normal);
    p.setFont(tickFont);

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

    // X grid/ticks/labels (20-100 degC every 5 degC)
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
    int yLabelX = 14; // close to the left edge of the widget
    p.translate(yLabelX, plotRect.center().y());
    p.rotate(-90);
    p.setFont(yFont);
    QRectF yLabelRect(-plotRect.height()/2, -12, plotRect.height(), 24);
    p.drawText(yLabelRect, Qt::AlignCenter, "% Duty");
    p.restore();
    // X axis label
    p.setFont(axisFont);
    QRectF xLabelRect(plotRect.left(), plotRect.bottom() + 28, plotRect.width(), 20);
    p.drawText(xLabelRect, Qt::AlignHCenter|Qt::AlignTop, "Temperature (°C)");

    // Draw border around plot area (half-pixel aligned)
    p.setPen(QPen(labelColor, 1));
    QRectF borderRect(qRound(plotRect.left()) + 0.5, qRound(plotRect.top()) + 0.5,
                      qRound(plotRect.width()) - 1.0, qRound(plotRect.height()) - 1.0);
    p.drawRect(borderRect);

    // Draw curve
    p.setFont(tickFont);
    p.setPen(QPen(curveColor, 3));
    for (int i = 1; i < m_points.size(); ++i) {
        p.drawLine(toWidget(m_points[i-1]), toWidget(m_points[i]));
    }
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
    }

    // Draw live crosshair overlay
    if (m_crosshairVisible) {
        Point crossPt { m_crosshairTemp, m_crosshairDuty };
        QPointF cp = toWidget(crossPt);

        // Clamp to plot area
        cp.setX(std::clamp(cp.x(), (double)plotRect.left(), (double)plotRect.right()));
        cp.setY(std::clamp(cp.y(), (double)plotRect.top(), (double)plotRect.bottom()));

        // Dashed crosshair lines
        QPen crossPen(accentColor, 1.5, Qt::DashLine);
        p.setPen(crossPen);
        // Vertical line (temperature)
        p.drawLine(QPointF(cp.x(), plotRect.top()), QPointF(cp.x(), plotRect.bottom()));
        // Horizontal line (duty)
        p.drawLine(QPointF(plotRect.left(), cp.y()), QPointF(plotRect.right(), cp.y()));

        // Draw crosshair dot
        p.setBrush(accentColor);
        p.setPen(QPen(brightText, 1.5));
        p.drawEllipse(cp, 5.0, 5.0);

        // Draw labels
        QFont labelFont = font();
        labelFont.setPointSize(8);
        labelFont.setWeight(QFont::Bold);
        p.setFont(labelFont);

        // Temperature label (below X axis at crosshair X)
        QString tempLabel = QString::number(m_crosshairTemp, 'f', 0) + QChar(0x00B0) + "C";
        p.setPen(accentColor);
        QRectF tempLabelRect(cp.x() - 20, plotRect.bottom() + 1, 40, 14);
        p.fillRect(tempLabelRect, bgColor);
        p.drawText(tempLabelRect, Qt::AlignHCenter | Qt::AlignTop, tempLabel);

        // Duty label (left of Y axis at crosshair Y)
        QString dutyLabel = QString::number(m_crosshairDuty, 'f', 0) + "%";
        QRectF dutyLabelRect(plotRect.left() - 40, cp.y() - 7, 38, 14);
        p.fillRect(dutyLabelRect, bgColor);
        p.drawText(dutyLabelRect, Qt::AlignRight | Qt::AlignVCenter, dutyLabel);
    }

    // Draw rubber band selection rectangle
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
    std::ranges::sort(sorted);
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
    // Snap temperature to nearest 5 degC grid position
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
