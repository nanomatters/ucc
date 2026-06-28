#pragma once
#include <QWidget>
#include <QPainter>
#include <QVector>
#include <QPointF>
#include <QSet>

class FanCurveEditorWidget : public QWidget {
    Q_OBJECT
public:
    explicit FanCurveEditorWidget(QWidget *parent = nullptr);
    QSize minimumSizeHint() const override { return QSize(400, 250); }
    QSize sizeHint() const override { return QSize(600, 350); }

    struct Point {
        double temp;
        double duty;
    };

    const QVector<Point>& points() const { return m_points; }
    void setPoints(const QVector<Point>& pts);
    void setEditable(bool editable) { m_editable = editable; }
    bool isEditable() const { return m_editable; }

    /** Set a title string drawn at the top of the widget. */
    void setTitle(const QString &title) { m_title = title; update(); }
    QString title() const { return m_title; }

    /** Set the live crosshair position (temperature in degC, duty in %). */
    void setCrosshair( double temp, double duty );
    /** Remove the crosshair from the display. */
    void clearCrosshair();

signals:
    void pointsChanged(const QVector<Point>&);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    QVector<Point> m_points;
    int m_draggedIndex = -1;
    bool m_editable = true;
    QString m_title;

    // Live crosshair state
    bool m_crosshairVisible = false;
    double m_crosshairTemp = 0.0;
    double m_crosshairDuty = 0.0;

    // Multi-select state
    QSet<int> m_selectedIndices;
    bool m_ctrlHeld = false;
    bool m_rubberBandActive = false;
    QPoint m_rubberBandOrigin;
    QRect m_rubberBandRect;
    // For multi-drag: store starting duty values for all selected points
    QVector<double> m_dragStartDuties;
    double m_dragStartY = 0.0;

    QRectF pointRect(const Point &pt) const;
    QPointF toWidget(const Point &pt) const;
    Point fromWidget(const QPointF &pos) const;
    void sortPoints();
    void addPoint(const Point &pt);
    void removePoint(int idx);
    void enforceMonotonicity(int modifiedIndex);
};
