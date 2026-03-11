/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QWidget>
#include <QTimer>
#include <QCheckBox>
#include <QPushButton>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QValueAxis>
#include <QByteArray>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsRectItem>
#include <QGraphicsLineItem>
#include <QLabel>
#include <QScrollArea>
#include <QSettings>
#include <QStatusBar>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QGraphicsSceneMouseEvent>
#include <QRubberBand>
#include <map>
#include <vector>
#include <functional>

namespace ucc
{

class UccdClient;

/**
 * @brief Monitoring tab with real-time hardware graphs.
 *
 * Dynamically discovers available monitoring sources (sensors, thermal
 * sources, fan/pump RPMs, legacy metrics) from the daemon and lets the
 * user choose which ones to plot via combo-box + checkbox selectors.
 */
class MonitorTab : public QWidget
{
  Q_OBJECT

public:
  explicit MonitorTab( UccdClient *client, QWidget *parent = nullptr );
  ~MonitorTab() override = default;

  /** Start / stop the incremental fetch timer. */
  void setMonitoringActive( bool active );

protected:
  void keyPressEvent( QKeyEvent *event ) override;
  void wheelEvent( QWheelEvent *event ) override;
  bool eventFilter( QObject *watched, QEvent *event ) override;

private slots:
  void fetchData();

private:
  // --- Setup helpers ---
  void setupUI();
  void setupChart();
  void setupControls();
  void refreshFpsSourceControls();

  /** Apply a new time window (clears series, re-fetches, updates label). */
  void setTimeWindow( int seconds );

  /** Fetch available sources from daemon and populate the combo box. */
  void refreshAvailableSources();

  /** Add a new source row (combo + checkbox) to the selector panel. */
  void addSourceRow( const std::string &key = {} );

  /** Remove a source row and its series from the chart. */
  void removeSourceRow( int row );

  /** Ensure a series exists for the given key; create if needed. */
  void ensureSeries( const std::string &key );

  /** Remove a series and its data for the given key. */
  void removeSeries( const std::string &key );

  /** Install hover callout on the chart. */
  void installHoverCallout( QChart *chart );

  /** Show/hide the hover callout for a data point on a series. */
  void showHoverCallout( QLineSeries *ls, QChart *chart,
                         const QPointF &point, bool state );

  /** Decode the binary payload returned by GetMonitorDataSince and append to buffers. */
  void applyBinaryData( const QByteArray &data );

  /** Push in-memory buffers into QLineSeries via replace() (single repaint per series). */
  void commitSeries();

  /** Save selected sources to ~/.config/uccrc. */
  void saveSourceSelection();

  /** Load selected sources from ~/.config/uccrc. */
  void loadSourceSelection();

  /** Trim series points that fall outside the visible time window. */
  void trimSeries();

  /** Update the X-axis range to [now - window, now]. */
  void updateAxes();

  /** Update the Y-axis range based on visible data. */
  void updateYRange();

  /** Pick a colour for a new series (cycling palette). */
  QColor nextColor();

  // --- Source metadata (from daemon) ---
  struct SourceDef
  {
    std::string key;     ///< Metric store key (e.g. "sensor:hwmon3_temp1", "fan:hwmon3_fan1")
    std::string label;   ///< Human-readable label
    std::string group;   ///< Category: "sensor", "thermal", "fan", "legacy"
    std::string unit;    ///< Display unit: "°C", "RPM", "%", "W", "MHz", "mV", "fps"
  };

  std::vector< SourceDef > m_availableSources;   ///< All sources from daemon

  // --- Active series ---
  struct SeriesInfo
  {
    QLineSeries        *series = nullptr;
    QString             label;
    QColor              color;
    QString             unit;
    QList< QPointF >    buffer;   ///< In-memory point buffer (source of truth)
  };

  std::map< std::string, SeriesInfo > m_seriesMap;

  // --- Source selector rows ---
  struct SourceRow
  {
    QComboBox  *combo    = nullptr;
    QCheckBox  *checkbox = nullptr;
    QPushButton *removeBtn = nullptr;
    std::string  activeKey;   ///< Currently selected key (empty = none)
  };

  std::vector< SourceRow > m_sourceRows;
  QVBoxLayout *m_selectorLayout = nullptr;  ///< Layout holding source rows
  QPushButton *m_addSourceBtn   = nullptr;  ///< "Add Source" button

  // --- Chart ---
  QChart        *m_chart     = nullptr;
  QChartView    *m_chartView = nullptr;
  QDateTimeAxis *m_xAxis     = nullptr;
  QValueAxis    *m_yAxis     = nullptr;

  // FPS source controls (daemon-side source selection/policy)
  QComboBox       *m_fpsSourceCombo   = nullptr;
  QCheckBox       *m_fpsRequireP0Check = nullptr;
  QLabel          *m_fpsCurrentAppLabel = nullptr;
  int              m_fpsSourceRefreshTicks = 0;
  bool             m_syncingFpsControls = false;

  // --- Hover callout ---
  struct Callout
  {
    QGraphicsRectItem       *bg   = nullptr;
    QGraphicsSimpleTextItem *text = nullptr;
  };
  std::map< QChart *, Callout > m_callouts;

  // --- Crosshair ---
  struct CrosshairLabel
  {
    QGraphicsRectItem       *bg   = nullptr;
    QGraphicsSimpleTextItem *text = nullptr;
  };

  QGraphicsLineItem *m_crosshairLine = nullptr;
  std::vector< CrosshairLabel > m_crosshairLabels;
  bool m_crosshairVisible = false;
  QPointF m_lastCrosshairPos;
  bool m_cursorInPlot = false;

  void updateCrosshair( const QPointF &widgetPos, bool ctrlHeld );
  void hideCrosshair();

  // --- Ctrl+LMB rubber-band zoom ---
  QRubberBand *m_zoomBand       = nullptr;
  QPoint       m_zoomOrigin;
  bool         m_zoomDragging   = false;
  bool         m_zoomed         = false;
  void applyZoomRect( const QRect &viewportRect );
  void resetZoom();

  // --- Controls ---
  QLabel    *m_pauseLabel      = nullptr;

  // --- State ---
  UccdClient *m_client = nullptr;
  QTimer      m_fetchTimer;
  qint64      m_lastTimestamp = 0;
  int         m_windowSeconds = 300;
  bool        m_paused = false;
  int         m_colorIndex = 0;   ///< Cycling index into colour palette
};

} // namespace ucc
