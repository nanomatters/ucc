#pragma once
#include <QWidget>
#include <QPainter>
#include <QVector>
#include <QPointF>
#include <QSet>

/**
 * @brief Pump curve editor widget
 *
 * Provides a visual editor for pump voltage thresholds.
 * The pump has 4 discrete levels (Off, V7, V8, V11) corresponding to
 * integer values 0-3, with 3 temperature threshold points that can be
 * positioned freely between 20°C and 100°C. Below the first threshold the
 * pump is Off (level 0); each threshold raises the level by one.
 * 12V (level 4) is intentionally omitted — it can be harmful to the pump.
 *
 * This edits the "tablePump" field in FanProfile.
 */
class PumpCurveEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit PumpCurveEditorWidget(QWidget *parent = nullptr);
    QSize minimumSizeHint() const override { return QSize(400, 200); }
    QSize sizeHint() const override { return QSize(600, 250); }

    struct Point {
        double temp;   // temperature threshold in °C (20..100)
        int level;     // pump level 0=Off, 1=V7, 2=V8, 3=V11
    };

    const QVector<Point>& points() const { return m_points; }
    void setPoints(const QVector<Point>& pts);
    void setEditable(bool editable) { m_editable = editable; }
    bool isEditable() const { return m_editable; }

    /// Pump level labels
    static QString levelLabel(int level);

signals:
    void pointsChanged(const QVector<Point>&);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QVector<Point> m_points;   // exactly 3 threshold points
    int m_draggedIndex = -1;
    bool m_editable = true;

    // Multi-select
    QSet<int> m_selectedIndices;
    bool m_ctrlHeld = false;
    bool m_rubberBandActive = false;
    QPoint m_rubberBandOrigin;
    QRect m_rubberBandRect;
    QVector<double> m_dragStartTemps;
    double m_dragStartX = 0.0;

    QRectF pointRect(const Point &pt) const;
    QPointF toWidget(const Point &pt) const;
    double tempFromWidgetX(double x) const;
    void sortPoints();
    void enforceOrdering();
};
