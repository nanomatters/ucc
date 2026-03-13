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
#include <QListWidget>
#include <QSplitter>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QMenu>
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
#include <QTabWidget>
#include <QSettings>
#include <QStatusBar>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QGraphicsSceneMouseEvent>
#include <QRubberBand>
#include <map>
#include <vector>
#include <set>

namespace ucc
{

class UccdClient;

/**
 * @brief Metric group categories for normalisation
 */
enum class MetricGroup
{
  Temp,   ///< Temperature (°C)       — normalised range 0–105
  Duty,   ///< Fan duty cycle (%)     — already 0–100
  Power,  ///< Power consumption (W)  — normalised range 0–maxPowerW
  Freq,   ///< Clock frequency (MHz)  — normalised range 0–6000
  Volt,   ///< Core voltage (mV)      — normalised range 0–1500
  Fps,    ///< Frames per second      — normalised range 0–300
  Rpm,    ///< Fan/pump RPM           — normalised range 0–5000
  Unknown ///< Anything else          — no normalisation (pass-through 0–100)
};

/**
 * @brief Monitoring tab with real-time hardware graphs.
 *
 * Dynamically discovers available monitoring sources (sensors, thermal
 * sources, fan/pump RPMs, legacy metrics) from the daemon and lets the
 * user choose which ones to plot via combo-box + checkbox selectors.
 *
 * The unified chart displays all metrics normalised to a 0–100 % Y axis.
 * Sticky marks (click-to-pin) allow annotating specific data points.
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
  void setupGroupCharts();
  void setupGroupChart( QChart *&chart, QChartView *&view,
                        QDateTimeAxis *&xAxis, QValueAxis *&yAxis,
                        const QString &yTitle, double yMin, double yMax );
  void setupControls();
  void refreshFpsSourceControls();

  /** Apply a new time window (clears series, re-fetches, updates label). */
  void setTimeWindow( int seconds );

  /** Fetch available sources from daemon and populate the combo box. */
  void refreshAvailableSources();

  /** Load user-defined sensor/device aliases from GUI settings. */
  void loadSourceAliasesFromSettings();

  /** Resolve a monitor source display label, preferring user alias when present. */
  QString displayLabelForSource( const std::string &key, const std::string &defaultLabel ) const;

  /** Rebuild the source tree from m_availableSources. */
  void rebuildSourceTree();

  /** Handle tree item checkbox state changes. */
  void onSourceTreeItemChanged( QTreeWidgetItem *item, int column );

  /** Handle tree item double-click to add/remove from monitor. */
  void onSourceTreeDoubleClicked( QTreeWidgetItem *item, int column );

  /** Handle tree item right-click context menu. */
  void onSourceTreeContextMenu( const QPoint &pos );

  /** Rebuild the favorites list from saved MRU keys. */
  void rebuildFavoritesList();

  /** Handle favorites checkbox state changes. */
  void onFavoriteItemChanged( QListWidgetItem *item );

  /** Record source usage into the favorites MRU list. */
  void recordSourceUsage( const std::string &key );

  /** Resolve a source label from the current available source list. */
  QString sourceDisplayText( const std::string &key ) const;

  /** Backfill one series with historical points for current time window. */
  void backfillSeriesHistory( const std::string &key );

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

  /** Update all per-group shadow series from raw buffers. */
  void commitGroupSeries();

  /** Pick a colour for a new series (cycling palette). */
  QColor nextColor();

  // --- Normalisation ---
  /** Determine the MetricGroup for a unit string. */
  static MetricGroup groupForUnit( const std::string &unit );

  /** Scale factor to normalise a metric group value to [0, 100]. */
  double metricToNormalisedScale( MetricGroup g ) const;

  /** Undo normalisation to restore real value. */
  double metricFromNormalisedScale( double normalisedValue, MetricGroup g ) const;

  /** Get the display unit for a metric group. */
  static const char *metricGroupUnit( MetricGroup g );

  /** Initialize m_maxPowerW from hardware TDP and GPU limits. */
  void initializeMaxPowerFromHardware();

  // --- Source metadata (from daemon) ---
  struct SourceDef
  {
    std::string key;     ///< Metric store key (e.g. "sensor:hwmon3_temp1", "fan:hwmon3_fan1")
    std::string label;   ///< Human-readable label
    std::string group;   ///< Category: "sensor", "thermal", "fan", "legacy"
    std::string unit;    ///< Display unit: "°C", "RPM", "%", "W", "MHz", "mV", "fps"
  };

  std::vector< SourceDef > m_availableSources;   ///< All sources from daemon
  std::map< std::string, QString > m_sensorAliasById;
  std::map< std::string, QString > m_deviceAliasById;

  // --- Active series ---
  struct SeriesInfo
  {
    QLineSeries        *series = nullptr;
    QLineSeries        *tempSeries = nullptr;
    QLineSeries        *fanSeries = nullptr;
    QLineSeries        *powerSeries = nullptr;
    QLineSeries        *voltSeries = nullptr;
    QLineSeries        *freqSeries = nullptr;
    QString             label;
    QColor              color;
    QString             unit;
    MetricGroup         metricGroup = MetricGroup::Unknown;
    QList< QPointF >    buffer;   ///< In-memory point buffer (raw values)
  };

  std::map< std::string, SeriesInfo > m_seriesMap;

  // --- Source selector tree ---
  QSplitter          *m_monitorSplitter = nullptr;
  QTreeWidget        *m_sourceTree = nullptr;  ///< Hierarchical selector by group
  QListWidget        *m_favoritesList = nullptr;
  std::set< std::string > m_activeSources;     ///< Currently monitored source keys
  std::vector< std::string > m_favoriteSources;
  bool m_syncingFavorites = false;

  // --- Chart ---
  QChart        *m_chart     = nullptr;
  QChartView    *m_chartView = nullptr;
  QDateTimeAxis *m_xAxis     = nullptr;
  QValueAxis    *m_yAxis     = nullptr;
  QTabWidget    *m_graphTabs = nullptr;

  QChart        *m_tempChart = nullptr;
  QChartView    *m_tempChartView = nullptr;
  QDateTimeAxis *m_tempXAxis = nullptr;
  QValueAxis    *m_tempYAxis = nullptr;

  QChart        *m_fanChart = nullptr;
  QChartView    *m_fanChartView = nullptr;
  QDateTimeAxis *m_fanXAxis = nullptr;
  QValueAxis    *m_fanYAxis = nullptr;

  QChart        *m_powerChart = nullptr;
  QChartView    *m_powerChartView = nullptr;
  QDateTimeAxis *m_powerXAxis = nullptr;
  QValueAxis    *m_powerYAxis = nullptr;

  QChart        *m_voltChart = nullptr;
  QChartView    *m_voltChartView = nullptr;
  QDateTimeAxis *m_voltXAxis = nullptr;
  QValueAxis    *m_voltYAxis = nullptr;

  QChart        *m_freqChart = nullptr;
  QChartView    *m_freqChartView = nullptr;
  QDateTimeAxis *m_freqXAxis = nullptr;
  QValueAxis    *m_freqYAxis = nullptr;

  // FPS source controls (daemon-side source selection/policy)
  QComboBox       *m_fpsSourceCombo   = nullptr;
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

  // --- Sticky marks (click-to-pin) ---
  static constexpr int MAX_STICKY_MARKS = 10;

  struct MarkGfx
  {
    QGraphicsRectItem       *bg   = nullptr;
    QGraphicsSimpleTextItem *text = nullptr;
  };

  struct StickyMetricEntry
  {
    std::string metricKey;
    double      rawValue;
  };

  struct StickyMark
  {
    qint64                            timestamp  = 0;
    double                            clickDataY = 0.5;
    QChart                           *chart      = nullptr;
    std::vector< StickyMetricEntry >  entries;
    QGraphicsRectItem                *bg     = nullptr;
    std::vector< QGraphicsSimpleTextItem * > texts;
    QGraphicsLineItem                *line   = nullptr;
  };

  std::vector< StickyMark > m_stickyMarks;

  void handleSeriesClick( QLineSeries *ls, const QPointF &point );
  void addStickyMarkGroup( QChart *chart, qint64 ts, double clickDataY,
                           const std::vector< StickyMetricEntry > &entries );
  void removeStickyMark( std::vector< StickyMark >::iterator it );
  void updateStickyMarkPositions();
  void crosshairClick( QChartView *chartView, const QPointF &widgetPos );

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
  bool m_annotationsVisible = true;

  QChartView *chartViewForViewport( QObject *watched ) const;
  QDateTimeAxis *activeXAxis() const;
  QValueAxis *activeYAxis() const;
  QLineSeries *seriesForChart( const SeriesInfo &info, const QChart *chart ) const;

  void updateCrosshair( QChartView *chartView, const QPointF &widgetPos, bool ctrlHeld );
  void hideCrosshair();

  QChart *m_crosshairChart = nullptr;
  QChartView *m_crosshairView = nullptr;

  // --- Ctrl+LMB rubber-band zoom ---
  QRubberBand *m_zoomBand       = nullptr;
  QPoint       m_zoomOrigin;
  bool         m_zoomDragging   = false;
  std::set< QChart * > m_zoomedCharts;
  void applyZoomRect( QChartView *chartView, const QRect &viewportRect );
  void resetZoom( QChart *chart = nullptr );

  // --- Controls ---
  QLabel    *m_pauseLabel      = nullptr;

  // --- State ---
  UccdClient *m_client = nullptr;
  QTimer      m_fetchTimer;
  qint64      m_lastTimestamp = 0;
  int         m_windowSeconds = 300;
  bool        m_paused = false;
  int         m_colorIndex = 0;   ///< Cycling index into colour palette
  int         m_maxPowerW = 150;  ///< Platform max power (TDP)
};

} // namespace ucc
