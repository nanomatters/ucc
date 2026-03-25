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

#include "MonitorTab.hpp"
#include "../libucc-dbus/UccdClient.hpp"
#include <QDateTime>
#include <QLabel>
#include <QScrollArea>
#include <QGridLayout>
#include <QToolTip>
#include <QDir>
#include <QSettings>
#include <QMainWindow>
#include <QStatusBar>
#include <QApplication>
#include <QSplitter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstring>
#include <algorithm>
#include <functional>

namespace ucc
{

// ---------------------------------------------------------------------------
// Clickable graphics rect — calls a callback on mouse press
// ---------------------------------------------------------------------------

class ClickableRectItem : public QGraphicsRectItem
{
public:
    explicit ClickableRectItem( QGraphicsItem *parent = nullptr )
        : QGraphicsRectItem( parent )
    {
        setAcceptedMouseButtons( Qt::LeftButton );
        setCursor( Qt::PointingHandCursor );
    }

    void setClickCallback( std::function< void() > cb ) { m_onClick = std::move( cb ); }

protected:
    void mousePressEvent( QGraphicsSceneMouseEvent *event ) override
    {
        if ( m_onClick )
            m_onClick();
        event->accept();
    }

private:
    std::function< void() > m_onClick;
};

// ---------------------------------------------------------------------------
// Colour palette — cycling, high-contrast on dark background
// ---------------------------------------------------------------------------

static const QColor kPalette[] = {
    QColor( 124, 179, 66 ),   // green
    QColor( 86, 182, 255 ),   // blue
    QColor( 255, 167, 38 ),   // orange
    QColor( 171, 71, 188 ),   // purple
    QColor( 0, 230, 118 ),    // emerald
    QColor( 255, 193, 7 ),    // amber
    QColor( 0, 188, 212 ),    // cyan
    QColor( 244, 67, 54 ),    // red
    QColor( 174, 234, 0 ),    // lime
    QColor( 46, 204, 113 ),   // sea green
    QColor( 255, 109, 0 ),    // deep orange
    QColor( 102, 187, 106 ),  // light green
    QColor( 255, 82, 82 ),    // red accent
    QColor( 156, 39, 176 ),   // deep purple
    QColor( 100, 181, 246 ),  // light blue
    QColor( 255, 241, 118 ),  // yellow
};
static constexpr int kPaletteSize = static_cast< int >( sizeof( kPalette ) / sizeof( kPalette[0] ) );

// ---------------------------------------------------------------------------
// Normalisation — map metric groups to 0–100 % unified scale
// ---------------------------------------------------------------------------

MetricGroup MonitorTab::groupForUnit( const std::string &unit )
{
    const QString u = QString::fromStdString( unit ).trimmed();
    const QString ul = u.toLower();

    if ( ul == QStringLiteral( "\xC2\xB0c" ) || ul == QStringLiteral( "°c" )
         || ul == QStringLiteral( "c" ) )
        return MetricGroup::Temp;
    if ( u == QStringLiteral( "%" ) )
        return MetricGroup::Duty;
    if ( ul == QStringLiteral( "w" ) )
        return MetricGroup::Power;
    if ( ul == QStringLiteral( "mhz" ) )
        return MetricGroup::Freq;
    if ( ul == QStringLiteral( "mv" ) )
        return MetricGroup::Volt;
    if ( ul == QStringLiteral( "fps" ) )
        return MetricGroup::Fps;
    if ( ul == QStringLiteral( "rpm" ) )
        return MetricGroup::Rpm;
    return MetricGroup::Unknown;
}

double MonitorTab::metricToNormalisedScale( MetricGroup g ) const
{
    switch ( g )
    {
        case MetricGroup::Temp:    return 100.0 / 105.0;
        case MetricGroup::Duty:    return 1.0;
        case MetricGroup::Power:   return 100.0 / m_maxPowerW;
        case MetricGroup::Freq:    return 100.0 / 6000.0;
        case MetricGroup::Volt:    return 100.0 / 1500.0;
        case MetricGroup::Fps:     return 100.0 / 300.0;
        case MetricGroup::Rpm:     return 100.0 / 5000.0;
        case MetricGroup::Unknown: return 1.0;
    }
    return 1.0;
}

double MonitorTab::metricFromNormalisedScale( double normalisedValue, MetricGroup g ) const
{
    switch ( g )
    {
        case MetricGroup::Temp:    return normalisedValue / ( 100.0 / 105.0 );
        case MetricGroup::Duty:    return normalisedValue;
        case MetricGroup::Power:   return normalisedValue / ( 100.0 / m_maxPowerW );
        case MetricGroup::Freq:    return normalisedValue / ( 100.0 / 6000.0 );
        case MetricGroup::Volt:    return normalisedValue / ( 100.0 / 1500.0 );
        case MetricGroup::Fps:     return normalisedValue / ( 100.0 / 300.0 );
        case MetricGroup::Rpm:     return normalisedValue / ( 100.0 / 5000.0 );
        case MetricGroup::Unknown: return normalisedValue;
    }
    return normalisedValue;
}

const char *MonitorTab::metricGroupUnit( MetricGroup g )
{
    switch ( g )
    {
        case MetricGroup::Temp:    return "°C";
        case MetricGroup::Duty:    return "%";
        case MetricGroup::Power:   return "W";
        case MetricGroup::Freq:    return "MHz";
        case MetricGroup::Volt:    return "mV";
        case MetricGroup::Fps:     return "fps";
        case MetricGroup::Rpm:     return "RPM";
        case MetricGroup::Unknown: return "";
    }
    return "";
}

void MonitorTab::initializeMaxPowerFromHardware()
{
    int maxGpuTgp = 0;
    int maxBoostTdp = 0;

    if ( auto gpuMax = m_client->getNVIDIAPowerCTRLMaxPowerLimit() )
        maxGpuTgp = *gpuMax;

    if ( auto tdpLimits = m_client->getODMPowerLimits() )
    {
        if ( tdpLimits->size() > 1 )
            maxBoostTdp = ( *tdpLimits )[1].toMap().value( "max" ).toInt();
    }

    if ( m_maxPowerW = std::max( maxGpuTgp, maxBoostTdp ); m_maxPowerW <= 0 )
        m_maxPowerW = 200;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static QChart *createChart()
{
    auto *chart = new QChart();
    chart->setAnimationOptions( QChart::NoAnimation );
    chart->legend()->setVisible( false );
    chart->setMargins( QMargins( 4, 4, 4, 4 ) );
    chart->setBackgroundBrush( QBrush( Qt::black ) );
    chart->setPlotAreaBackgroundBrush( QBrush( Qt::black ) );
    chart->setPlotAreaBackgroundVisible( true );
    return chart;
}

static void styleAxis( QAbstractAxis *axis )
{
    axis->setLabelsBrush( QBrush( Qt::white ) );
    axis->setTitleBrush( QBrush( Qt::white ) );
    QPen linePen( QColor( 100, 100, 100 ) );
    axis->setLinePen( linePen );
    axis->setGridLinePen( QPen( QColor( 45, 45, 45 ) ) );
    axis->setShadesBrush( QBrush( Qt::transparent ) );
}

static QDateTimeAxis *createXAxis()
{
    auto *axis = new QDateTimeAxis();
    axis->setFormat( "HH:mm:ss" );
    axis->setTickCount( 6 );
    styleAxis( axis );
    return axis;
}

static QValueAxis *createYAxis( const QString &title, double min, double max )
{
    auto *axis = new QValueAxis();
    axis->setTitleText( title );
    axis->setRange( min, max );
    axis->setLabelFormat( "%.0f" );
    styleAxis( axis );
    return axis;
}

static QChartView *createChartView( QChart *chart )
{
    auto *view = new QChartView( chart );
    view->setRenderHint( QPainter::Antialiasing );
    view->setMinimumHeight( 400 );
    return view;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

MonitorTab::MonitorTab( UccdClient *client, QWidget *parent )
    : QWidget( parent )
    , m_client( client )
{
    initializeMaxPowerFromHardware();
    setupUI();
    setFocusPolicy( Qt::StrongFocus );

    // Populate the source tree immediately if client is available
    if ( m_client )
        refreshAvailableSources();

    m_fetchTimer.setInterval( 1000 );
    connect( &m_fetchTimer, &QTimer::timeout, this, &MonitorTab::fetchData );
}

void MonitorTab::setMonitoringActive( bool active )
{
    if ( active )
    {
        // Clear all buffers
        for ( auto &[key, info] : m_seriesMap )
        {
            info.buffer.clear();
            info.series->clear();
        }

        refreshAvailableSources();

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_lastTimestamp = now - static_cast< qint64 >( m_windowSeconds ) * 1000;
        fetchData();
        m_fetchTimer.start();
        refreshFpsSourceControls();
        if ( m_graphTabs && m_graphTabs->currentWidget() )
            m_graphTabs->currentWidget()->setFocus();
        else
            m_chartView->setFocus();
    }
    else
    {
        m_fetchTimer.stop();
    }
}

// ---------------------------------------------------------------------------
// UI Setup
// ---------------------------------------------------------------------------

void MonitorTab::setupUI()
{
    auto *mainLayout = new QVBoxLayout( this );

    // ── Controls row (FPS source) ──
    setupControls();

    // ── Source selector panel (left) + Chart (right) ──
    m_monitorSplitter = new QSplitter( Qt::Horizontal, this );
    m_monitorSplitter->setChildrenCollapsible( true );

    // Source selector tree
    auto *selectorBox = new QGroupBox( "Sources" );
    selectorBox->setMinimumWidth( 0 );
    auto *selectorOuterLayout = new QVBoxLayout( selectorBox );

    m_sourceTree = new QTreeWidget();
    m_sourceTree->setMinimumWidth( 0 );
    m_sourceTree->setColumnCount( 1 );
    m_sourceTree->setHeaderHidden( true );
    m_sourceTree->setUniformRowHeights( true );
    m_sourceTree->setContextMenuPolicy( Qt::CustomContextMenu );
    m_sourceTree->setSelectionMode( QAbstractItemView::NoSelection );
    m_sourceTree->setAllColumnsShowFocus( false );

    connect( m_sourceTree, &QTreeWidget::itemChanged, this, &MonitorTab::onSourceTreeItemChanged );
    connect( m_sourceTree, &QTreeWidget::itemDoubleClicked, this, &MonitorTab::onSourceTreeDoubleClicked );
    connect( m_sourceTree, &QTreeWidget::customContextMenuRequested, this, &MonitorTab::onSourceTreeContextMenu );

    selectorOuterLayout->addWidget( m_sourceTree, 1 );

    auto *favoritesLabel = new QLabel( QStringLiteral( "Favorites" ), selectorBox );
    selectorOuterLayout->addWidget( favoritesLabel );

    m_favoritesList = new QListWidget( selectorBox );
    m_favoritesList->setMinimumWidth( 0 );
    m_favoritesList->setUniformItemSizes( true );
    m_favoritesList->setSelectionMode( QAbstractItemView::NoSelection );
    m_favoritesList->setAlternatingRowColors( false );
    m_favoritesList->setMinimumHeight( 180 );
    connect( m_favoritesList, &QListWidget::itemChanged,
             this, &MonitorTab::onFavoriteItemChanged );
    selectorOuterLayout->addWidget( m_favoritesList );

    m_monitorSplitter->addWidget( selectorBox );

    // Charts (subtabs)
    setupChart();
    setupGroupCharts();

    m_graphTabs = new QTabWidget();
    m_graphTabs->addTab( m_chartView, QStringLiteral( "Unified" ) );
    m_graphTabs->addTab( m_tempChartView, QStringLiteral( "Temperature" ) );
    m_graphTabs->addTab( m_fanChartView, QStringLiteral( "Fan" ) );
    m_graphTabs->addTab( m_powerChartView, QStringLiteral( "Power" ) );
    m_graphTabs->addTab( m_voltChartView, QStringLiteral( "Voltage" ) );
    m_graphTabs->addTab( m_freqChartView, QStringLiteral( "Frequency" ) );
    connect( m_graphTabs, &QTabWidget::currentChanged, this, [this]( int ) {
        m_cursorInPlot = false;
        hideCrosshair();
    } );

    m_monitorSplitter->addWidget( m_graphTabs );
    m_monitorSplitter->setCollapsible( 0, true );
    m_monitorSplitter->setCollapsible( 1, false );
    m_monitorSplitter->setStretchFactor( 0, 0 );
    m_monitorSplitter->setStretchFactor( 1, 1 );
    m_monitorSplitter->setSizes( { 260, 1020 } );
    connect( m_monitorSplitter, &QSplitter::splitterMoved,
             this, [this]( int, int ) { saveSourceSelection(); } );

    mainLayout->addWidget( m_monitorSplitter, 1 );

    // Pause label
    m_pauseLabel = new QLabel( "PAUSED" );
    m_pauseLabel->setStyleSheet( "QLabel { color: #FF6B6B; font-weight: bold; padding: 0 8px; }" );
    m_pauseLabel->hide();
    mainLayout->addWidget( m_pauseLabel );

    // Load saved source selection
    loadSourceSelection();
}

void MonitorTab::setupChart()
{
    m_chart = createChart();
    m_xAxis = createXAxis();
    m_yAxis = createYAxis( "%", 0, 100 );
    m_chart->addAxis( m_xAxis, Qt::AlignBottom );
    m_chart->addAxis( m_yAxis, Qt::AlignLeft );

    // Invisible anchor series so axes render labels even with no real series
    auto *anchor = new QLineSeries();
    anchor->setVisible( false );
    anchor->setPen( QPen( Qt::transparent ) );
    m_chart->addSeries( anchor );
    anchor->attachAxis( m_xAxis );
    anchor->attachAxis( m_yAxis );

    m_chartView = createChartView( m_chart );
    m_chartView->setMouseTracking( true );
    m_chartView->viewport()->setMouseTracking( true );
    m_chartView->viewport()->installEventFilter( this );
    m_chartView->setFocusPolicy( Qt::StrongFocus );

    // Hover callout
    installHoverCallout( m_chart );
}

void MonitorTab::setupGroupChart( QChart *&chart, QChartView *&view,
                                  QDateTimeAxis *&xAxis, QValueAxis *&yAxis,
                                  const QString &yTitle, double yMin, double yMax )
{
    chart = createChart();
    xAxis = createXAxis();
    yAxis = createYAxis( yTitle, yMin, yMax );
    chart->addAxis( xAxis, Qt::AlignBottom );
    chart->addAxis( yAxis, Qt::AlignLeft );

    auto *anchor = new QLineSeries();
    anchor->setVisible( false );
    anchor->setPen( QPen( Qt::transparent ) );
    chart->addSeries( anchor );
    anchor->attachAxis( xAxis );
    anchor->attachAxis( yAxis );

    view = createChartView( chart );
    view->setMouseTracking( true );
    view->viewport()->setMouseTracking( true );
    view->viewport()->installEventFilter( this );
    view->setFocusPolicy( Qt::StrongFocus );
    installHoverCallout( chart );
}

void MonitorTab::setupGroupCharts()
{
    setupGroupChart( m_tempChart, m_tempChartView, m_tempXAxis, m_tempYAxis,
                     QStringLiteral( "\xC2\xB0C" ), 0.0, 105.0 );
    setupGroupChart( m_fanChart, m_fanChartView, m_fanXAxis, m_fanYAxis,
                     QStringLiteral( "%" ), 0.0, 100.0 );
    setupGroupChart( m_powerChart, m_powerChartView, m_powerXAxis, m_powerYAxis,
                     QStringLiteral( "W" ), 0.0, static_cast< double >( m_maxPowerW ) );
    setupGroupChart( m_voltChart, m_voltChartView, m_voltXAxis, m_voltYAxis,
                     QStringLiteral( "mV" ), 0.0, 1500.0 );
    setupGroupChart( m_freqChart, m_freqChartView, m_freqXAxis, m_freqYAxis,
                     QStringLiteral( "MHz" ), 0.0, 6000.0 );
}

void MonitorTab::setupControls()
{
    auto *mainLayout = qobject_cast< QVBoxLayout * >( layout() );
    if ( !mainLayout )
        return;

    auto *controls = new QWidget( this );
    auto *row = new QHBoxLayout( controls );
    row->setContentsMargins( 0, 0, 0, 0 );

    auto *sourceLabel = new QLabel( "FPS Source:", controls );
    m_fpsSourceCombo = new QComboBox( controls );
    m_fpsSourceCombo->setMinimumWidth( 260 );
    m_fpsSourceCombo->addItem( "Auto", "auto" );

    m_fpsCurrentAppLabel = new QLabel( "Current: -", controls );

    row->addWidget( sourceLabel );
    row->addWidget( m_fpsSourceCombo );
    row->addWidget( m_fpsCurrentAppLabel, 1 );

    connect( m_fpsSourceCombo, &QComboBox::currentIndexChanged, this, [this]( int ) {
        if ( m_syncingFpsControls || !m_client || !m_fpsSourceCombo )
            return;
        m_client->setFpsSourceApp( m_fpsSourceCombo->currentData().toString().toStdString() );
    } );

    mainLayout->addWidget( controls );
    refreshFpsSourceControls();
}

void MonitorTab::refreshFpsSourceControls()
{
    if ( !m_client || !m_fpsSourceCombo || !m_fpsCurrentAppLabel )
        return;

    auto fpsOpt = m_client->getFpsSources();
    if ( !fpsOpt.has_value() )
        return;

    const QVariantMap &root = *fpsOpt;
    const QString selected = root.value( "selectedApp" ).toString();
    const QString currentApp = root.value( "currentApp" ).toString();
    const qint64 currentPid = root.value( "currentPid" ).toLongLong();

    // Keep the source chooser focused on live state.
    // Daemon `apps` is a historical set, so old app names (e.g. a closed benchmark)
    // can linger there and confuse users.
    QStringList apps;
    apps.append( "auto" );
    if ( !currentApp.isEmpty() && !apps.contains( currentApp, Qt::CaseInsensitive ) )
        apps.append( currentApp );
    if ( !selected.isEmpty() && !apps.contains( selected, Qt::CaseInsensitive ) )
        apps.append( selected );

    m_syncingFpsControls = true;

    m_fpsSourceCombo->clear();
    for ( const QString &a : apps )
    {
        const QString label = ( a.compare( "auto", Qt::CaseInsensitive ) == 0 ) ? "Auto" : a;
        m_fpsSourceCombo->addItem( label, a );
    }

    int idx = m_fpsSourceCombo->findData( selected );
    if ( idx < 0 )
        idx = m_fpsSourceCombo->findData( QStringLiteral( "auto" ) );
    if ( idx >= 0 )
        m_fpsSourceCombo->setCurrentIndex( idx );

    const QString currentText = currentApp.isEmpty()
        ? QStringLiteral( "Current: -" )
        : QStringLiteral( "Current: %1 (pid %2)" ).arg( currentApp ).arg( currentPid );
    m_fpsCurrentAppLabel->setText( currentText );

    m_syncingFpsControls = false;
}

// ---------------------------------------------------------------------------
// Available sources — fetch from daemon
// ---------------------------------------------------------------------------

void MonitorTab::refreshAvailableSources()
{
    if ( !m_client )
        return;

    loadSourceAliasesFromSettings();

    auto sourcesOpt = m_client->getMonitorSources();
    if ( !sourcesOpt.has_value() )
        return;

    m_availableSources.clear();
    for ( const auto &v : *sourcesOpt )
    {
        const QVariantMap obj = v.toMap();
        SourceDef sd;
        sd.key   = obj.value( "key" ).toString().toStdString();
        const std::string rawLabel = obj.value( "label" ).toString().toStdString();
        sd.label = displayLabelForSource( sd.key, rawLabel ).toStdString();
        sd.group = obj.value( "group" ).toString().toStdString();
        sd.unit  = obj.value( "unit" ).toString().toStdString();
        if ( !sd.key.empty() )
            m_availableSources.push_back( std::move( sd ) );
    }

    // Refresh labels of already-active series so callouts/marks use current aliases.
    for ( auto &[key, info] : m_seriesMap )
    {
        for ( const auto &sd : m_availableSources )
        {
            if ( sd.key != key )
                continue;

            info.label = QString::fromStdString( sd.label );
            if ( info.series )
                info.series->setName( info.label );
            if ( info.tempSeries )
                info.tempSeries->setName( info.label );
            if ( info.fanSeries )
                info.fanSeries->setName( info.label );
            if ( info.powerSeries )
                info.powerSeries->setName( info.label );
            if ( info.voltSeries )
                info.voltSeries->setName( info.label );
            if ( info.freqSeries )
                info.freqSeries->setName( info.label );
            break;
        }
    }

    // Keep only active keys that are still available.
    std::set< std::string > availableKeys;
    for ( const auto &sd : m_availableSources )
        availableKeys.insert( sd.key );

    for ( auto it = m_activeSources.begin(); it != m_activeSources.end(); )
    {
        if ( availableKeys.count( *it ) == 0 )
            it = m_activeSources.erase( it );
        else
            ++it;
    }

    for ( auto it = m_favoriteSources.begin(); it != m_favoriteSources.end(); )
    {
        if ( availableKeys.count( *it ) == 0 )
            it = m_favoriteSources.erase( it );
        else
            ++it;
    }

    // Ensure all active keys have a series.
    for ( const auto &key : m_activeSources )
        ensureSeries( key );

    // Drop any series that are no longer active.
    std::vector< std::string > staleSeries;
    for ( const auto &[key, _] : m_seriesMap )
    {
        if ( m_activeSources.count( key ) == 0 )
            staleSeries.push_back( key );
    }
    for ( const auto &key : staleSeries )
        removeSeries( key );

    // Rebuild the source selector tree
    rebuildSourceTree();
    rebuildFavoritesList();
}

void MonitorTab::loadSourceAliasesFromSettings()
{
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );

    m_sensorAliasById.clear();
    settings.beginGroup( QStringLiteral( "sensorAliases" ) );
    for ( const QString &id : settings.childKeys() )
    {
        const QString alias = settings.value( id ).toString().trimmed();
        if ( !alias.isEmpty() )
            m_sensorAliasById[id.toStdString()] = alias;
    }
    settings.endGroup();

    m_deviceAliasById.clear();
    settings.beginGroup( QStringLiteral( "deviceAliases" ) );
    for ( const QString &id : settings.childKeys() )
    {
        const QString alias = settings.value( id ).toString().trimmed();
        if ( !alias.isEmpty() )
            m_deviceAliasById[id.toStdString()] = alias;
    }
    settings.endGroup();
}

QString MonitorTab::displayLabelForSource( const std::string &key, const std::string &defaultLabel ) const
{
    const std::string::size_type sep = key.find( ':' );
    if ( sep == std::string::npos || sep + 1 >= key.size() )
        return QString::fromStdString( defaultLabel );

    const std::string prefix = key.substr( 0, sep );
    const std::string id = key.substr( sep + 1 );

    if ( prefix == "sensor" || prefix == "tsrc" )
    {
        auto it = m_sensorAliasById.find( id );
        if ( it != m_sensorAliasById.end() && !it->second.trimmed().isEmpty() )
            return it->second;
    }
    else if ( prefix == "fan" )
    {
        auto it = m_deviceAliasById.find( id );
        if ( it != m_deviceAliasById.end() && !it->second.trimmed().isEmpty() )
            return it->second;
    }

    return QString::fromStdString( defaultLabel );
}

// ---------------------------------------------------------------------------
// Source tree selector
// ---------------------------------------------------------------------------

QString MonitorTab::sourceDisplayText( const std::string &key ) const
{
    for ( const auto &sd : m_availableSources )
    {
        if ( sd.key != key )
            continue;

        if ( sd.unit.empty() )
            return QString::fromStdString( sd.label );

        return QStringLiteral( "%1 (%2)" )
            .arg( QString::fromStdString( sd.label ) )
            .arg( QString::fromStdString( sd.unit ) );
    }

    return QString::fromStdString( key );
}

void MonitorTab::rebuildSourceTree()
{
    if ( !m_sourceTree )
        return;

    // Remember which sources are currently checked
    std::set< std::string > checkedKeys = m_activeSources;

    m_sourceTree->blockSignals( true );
    m_sourceTree->clear();

    // Group sources by category
    std::map< std::string, std::vector< SourceDef > > groupedSources;
    for ( const auto &sd : m_availableSources )
    {
        groupedSources[sd.group].push_back( sd );
    }

    // Create tree structure: group (parent) -> sources (children)
    for ( const auto &[groupName, sources] : groupedSources )
    {
        // Create parent group item
        auto *groupItem = new QTreeWidgetItem();
        groupItem->setText( 0, QString::fromStdString( groupName ) );
        groupItem->setFlags( groupItem->flags() & ~Qt::ItemIsSelectable );
        m_sourceTree->addTopLevelItem( groupItem );

        // Add source children with checkboxes
        for ( const auto &sd : sources )
        {
            auto *sourceItem = new QTreeWidgetItem( groupItem );
            sourceItem->setText( 0, QString::fromStdString( sd.label ) );
            sourceItem->setData( 0, Qt::UserRole, QString::fromStdString( sd.key ) );
            sourceItem->setData( 0, Qt::UserRole + 1, QString::fromStdString( sd.unit ) );
            sourceItem->setFlags( sourceItem->flags() | Qt::ItemIsUserCheckable );
            sourceItem->setCheckState( 0, checkedKeys.count( sd.key ) ? Qt::Checked : Qt::Unchecked );

            // Tooltip with unit
            sourceItem->setToolTip( 0, QString::fromStdString( sd.unit ) );
        }

        groupItem->setExpanded( false );
    }

    m_sourceTree->blockSignals( false );
}

void MonitorTab::rebuildFavoritesList()
{
    if ( !m_favoritesList )
        return;

    m_syncingFavorites = true;
    m_favoritesList->blockSignals( true );
    m_favoritesList->clear();

    for ( const auto &key : m_favoriteSources )
    {
        auto *item = new QListWidgetItem( sourceDisplayText( key ), m_favoritesList );
        item->setData( Qt::UserRole, QString::fromStdString( key ) );
        item->setFlags( item->flags() | Qt::ItemIsUserCheckable | Qt::ItemNeverHasChildren );
        item->setCheckState( m_activeSources.count( key ) ? Qt::Checked : Qt::Unchecked );
        item->setToolTip( sourceDisplayText( key ) );
    }

    m_favoritesList->blockSignals( false );
    m_syncingFavorites = false;
}

void MonitorTab::recordSourceUsage( const std::string &key )
{
    if ( key.empty() )
        return;

    m_favoriteSources.erase(
        std::remove( m_favoriteSources.begin(), m_favoriteSources.end(), key ),
        m_favoriteSources.end() );
    m_favoriteSources.insert( m_favoriteSources.begin(), key );
    if ( m_favoriteSources.size() > 10 )
        m_favoriteSources.resize( 10 );
}

void MonitorTab::onSourceTreeItemChanged( QTreeWidgetItem *item, int column )
{
    if ( !item )
        return;

    Q_UNUSED( column );

    // Only process source items (children), not group items (parents)
    if ( !item->parent() )
        return;

    const QString key = item->data( 0, Qt::UserRole ).toString();
    if ( key.isEmpty() )
        return;

    const bool isChecked = item->checkState( 0 ) == Qt::Checked;
    const std::string stdKey = key.toStdString();

    if ( isChecked )
    {
        // Add this source
        if ( !m_activeSources.count( stdKey ) )
        {
            m_activeSources.insert( stdKey );
            recordSourceUsage( stdKey );
            ensureSeries( stdKey );
            backfillSeriesHistory( stdKey );
        }
    }
    else
    {
        // Remove this source
        if ( m_activeSources.count( stdKey ) )
        {
            m_activeSources.erase( stdKey );
            removeSeries( stdKey );
        }
    }

    rebuildFavoritesList();
    saveSourceSelection();
}

void MonitorTab::onFavoriteItemChanged( QListWidgetItem *item )
{
    if ( m_syncingFavorites || !item )
        return;

    const QString key = item->data( Qt::UserRole ).toString();
    if ( key.isEmpty() )
        return;

    const std::string stdKey = key.toStdString();
    const bool isChecked = item->checkState() == Qt::Checked;

    if ( isChecked )
    {
        if ( !m_activeSources.count( stdKey ) )
        {
            m_activeSources.insert( stdKey );
            recordSourceUsage( stdKey );
            ensureSeries( stdKey );
            backfillSeriesHistory( stdKey );
        }
    }
    else if ( m_activeSources.count( stdKey ) )
    {
        m_activeSources.erase( stdKey );
        removeSeries( stdKey );
    }

    rebuildSourceTree();
    rebuildFavoritesList();
    saveSourceSelection();
}

void MonitorTab::backfillSeriesHistory( const std::string &key )
{
    if ( !m_client || key.empty() )
        return;

    auto itSeries = m_seriesMap.find( key );
    if ( itSeries == m_seriesMap.end() )
        return;

    const qint64 since = QDateTime::currentMSecsSinceEpoch()
        - static_cast< qint64 >( m_windowSeconds ) * 1000;

    auto result = m_client->getMonitorDataSince( since );
    if ( !result.has_value() || result->isEmpty() )
        return;

    static constexpr size_t kPointSize = sizeof( int64_t ) + sizeof( double );

    const auto *p   = reinterpret_cast< const uint8_t * >( result->constData() );
    const auto *end = p + result->size();

    // Replace only this series buffer with fresh historical window data.
    itSeries->second.buffer.clear();

    while ( p < end )
    {
        if ( p + sizeof( uint16_t ) > end )
            break;
        uint16_t keyLen = 0;
        std::memcpy( &keyLen, p, sizeof( keyLen ) );
        p += sizeof( keyLen );

        if ( p + keyLen > end )
            break;
        std::string metricKey( reinterpret_cast< const char * >( p ), keyLen );
        p += keyLen;

        if ( p + sizeof( uint32_t ) > end )
            break;
        uint32_t count = 0;
        std::memcpy( &count, p, sizeof( count ) );
        p += sizeof( count );

        if ( p + static_cast< size_t >( count ) * kPointSize > end )
            break;

        if ( metricKey == key )
        {
            auto seriesIt = m_seriesMap.find( key );
            if ( seriesIt == m_seriesMap.end() )
                return;

            auto &buf = seriesIt->second.buffer;
            buf.reserve( buf.size() + static_cast< int >( count ) );

            for ( uint32_t j = 0; j < count; ++j )
            {
                int64_t ts = 0;
                double val = 0.0;
                std::memcpy( &ts, p, sizeof( ts ) );
                std::memcpy( &val, p + sizeof( ts ), sizeof( val ) );
                p += kPointSize;
                buf.append( QPointF( static_cast< qreal >( ts ), val ) );
            }
        }
        else
        {
            p += static_cast< size_t >( count ) * kPointSize;
        }
    }

    trimSeries();
    commitSeries();
    updateAxes();
    updateStickyMarkPositions();
    if ( m_chartView )
        m_chartView->viewport()->update();
}

void MonitorTab::onSourceTreeDoubleClicked( QTreeWidgetItem *item, int column )
{
    if ( !item || !item->parent() )
        return;

    Q_UNUSED( column );

    // Toggle checkbox on double-click
    const Qt::CheckState newState = item->checkState( 0 ) == Qt::Checked ? Qt::Unchecked : Qt::Checked;
    item->setCheckState( 0, newState );
}

void MonitorTab::onSourceTreeContextMenu( const QPoint &pos )
{
    QTreeWidgetItem *item = m_sourceTree->itemAt( pos );
    if ( !item || !item->parent() )
        return;

    QMenu menu;
    menu.addAction( "Remove from Monitor", [item]() {
        item->setCheckState( 0, Qt::Unchecked );
    } );

    menu.exec( m_sourceTree->mapToGlobal( pos ) );
}

// ---------------------------------------------------------------------------
// Series management
// ---------------------------------------------------------------------------

QColor MonitorTab::nextColor()
{
    const QColor c = kPalette[ m_colorIndex % kPaletteSize ];
    ++m_colorIndex;
    return c;
}

void MonitorTab::ensureSeries( const std::string &key )
{
    if ( key.empty() || m_seriesMap.count( key ) )
        return;

    // Find the SourceDef for label and unit
    QString label = QString::fromStdString( key );
    QString unit;
    MetricGroup mg = MetricGroup::Unknown;
    for ( const auto &sd : m_availableSources )
    {
        if ( sd.key == key )
        {
            label = QString::fromStdString( sd.label );
            unit = QString::fromStdString( sd.unit );
            mg = groupForUnit( sd.unit );
            break;
        }
    }

    // Fallback classification for sources with non-standard/empty unit strings.
    if ( mg == MetricGroup::Unknown )
    {
        if ( key.rfind( "sensor:", 0 ) == 0 || key.rfind( "tsrc:", 0 ) == 0
             || key == "cpuTemp" || key == "gpuTemp" )
        {
            mg = MetricGroup::Temp;
        }
        else if ( key.rfind( "fan:", 0 ) == 0 )
        {
            mg = MetricGroup::Rpm;
        }
        else if ( key == "cpuFanDuty" || key == "gpuFanDuty" )
        {
            mg = MetricGroup::Duty;
        }
        else if ( key == "cpuPower" || key == "gpuPower" )
        {
            mg = MetricGroup::Power;
        }
        else if ( key.rfind( "cpufreq:", 0 ) == 0
                  || key == "cpuFrequency" || key == "gpuFrequency" || key == "gpuVramFrequency" )
        {
            mg = MetricGroup::Freq;
        }
        else if ( key == "gpuCoreVoltage" || key.rfind( "voltage:", 0 ) == 0 )
        {
            mg = MetricGroup::Volt;
        }
        else if ( key == "fps" )
        {
            mg = MetricGroup::Fps;
        }
    }

    auto *series = new QLineSeries();
    series->setName( label );
    const QColor color = nextColor();
    QPen pen( color );
    pen.setWidth( 1 );
    series->setPen( pen );
    series->setProperty( "_unit", unit );
    series->setProperty( "_metricKey", QString::fromStdString( key ) );
    series->setProperty( "_normalised", true );

    m_chart->addSeries( series );
    series->attachAxis( m_xAxis );
    series->attachAxis( m_yAxis );

    // Install hover callout for this new series
    connect( series, &QLineSeries::hovered,
             this, [this, series]( const QPointF &point, bool state ) {
        showHoverCallout( series, m_chart, point, state );
    } );

    // Sticky mark — click to pin a data-point label
    connect( series, &QLineSeries::clicked,
             this, [this, series]( const QPointF &point ) {
        handleSeriesClick( series, point );
    } );

    SeriesInfo info;
    info.series = series;
    info.label = label;
    info.color = color;
    info.unit = unit;
    info.metricGroup = mg;

    auto createShadowSeries = [&]( QChart *chart, QDateTimeAxis *xAxis,
                                   QValueAxis *yAxis, bool normalised = false ) -> QLineSeries * {
        if ( !chart || !xAxis || !yAxis )
            return nullptr;

        auto *shadow = new QLineSeries();
        shadow->setName( label );
        QPen shadowPen( color );
        shadowPen.setWidth( 1 );
        shadow->setPen( shadowPen );
        shadow->setProperty( "_unit", unit );
        shadow->setProperty( "_metricKey", QString::fromStdString( key ) );
        shadow->setProperty( "_normalised", normalised );

        chart->addSeries( shadow );
        shadow->attachAxis( xAxis );
        shadow->attachAxis( yAxis );

        connect( shadow, &QLineSeries::hovered,
                 this, [this, shadow, chart]( const QPointF &point, bool state ) {
            showHoverCallout( shadow, chart, point, state );
        } );

        return shadow;
    };

    if ( mg == MetricGroup::Temp )
        info.tempSeries = createShadowSeries( m_tempChart, m_tempXAxis, m_tempYAxis );
    else if ( mg == MetricGroup::Duty || mg == MetricGroup::Rpm )
        info.fanSeries = createShadowSeries( m_fanChart, m_fanXAxis, m_fanYAxis,
                                             mg == MetricGroup::Rpm );
    else if ( mg == MetricGroup::Power )
        info.powerSeries = createShadowSeries( m_powerChart, m_powerXAxis, m_powerYAxis );
    else if ( mg == MetricGroup::Volt )
        info.voltSeries = createShadowSeries( m_voltChart, m_voltXAxis, m_voltYAxis );
    else if ( mg == MetricGroup::Freq )
        info.freqSeries = createShadowSeries( m_freqChart, m_freqXAxis, m_freqYAxis );

    m_seriesMap.emplace( key, std::move( info ) );
}

void MonitorTab::removeSeries( const std::string &key )
{
    auto it = m_seriesMap.find( key );
    if ( it == m_seriesMap.end() )
        return;

    m_chart->removeSeries( it->second.series );
    delete it->second.series;

    if ( it->second.tempSeries )
    {
        m_tempChart->removeSeries( it->second.tempSeries );
        delete it->second.tempSeries;
    }
    if ( it->second.fanSeries )
    {
        m_fanChart->removeSeries( it->second.fanSeries );
        delete it->second.fanSeries;
    }
    if ( it->second.powerSeries )
    {
        m_powerChart->removeSeries( it->second.powerSeries );
        delete it->second.powerSeries;
    }
    if ( it->second.voltSeries )
    {
        m_voltChart->removeSeries( it->second.voltSeries );
        delete it->second.voltSeries;
    }
    if ( it->second.freqSeries )
    {
        m_freqChart->removeSeries( it->second.freqSeries );
        delete it->second.freqSeries;
    }

    m_seriesMap.erase( it );
}

// ---------------------------------------------------------------------------
// Hover callout — shows denormalised real value under the pointer
// ---------------------------------------------------------------------------

void MonitorTab::installHoverCallout( QChart *chart )
{
    auto it = m_callouts.find( chart );
    if ( it != m_callouts.end() )
    {
        delete it->second.bg;
        delete it->second.text;
        m_callouts.erase( it );
    }

    auto *bg   = new QGraphicsRectItem( chart );
    auto *text = new QGraphicsSimpleTextItem( chart );
    bg->setBrush( QBrush( QColor( 30, 30, 30, 200 ) ) );
    bg->setPen( QPen( QColor( 200, 200, 200 ) ) );
    bg->setZValue( 100 );
    text->setBrush( Qt::white );
    text->setZValue( 101 );
    bg->hide();
    text->hide();

    m_callouts[ chart ] = { bg, text };
}

void MonitorTab::showHoverCallout( QLineSeries *ls, QChart *chart,
                                   const QPointF &point, bool state )
{
    auto calloutIt = m_callouts.find( chart );
    if ( calloutIt == m_callouts.end() )
        return;

    auto &co = calloutIt->second;
    if ( !state )
    {
        co.bg->hide();
        co.text->hide();
        return;
    }

    // Convert to real value only for normalised series.
    const QString metricKey = ls->property( "_metricKey" ).toString();
    const std::string keyStd = metricKey.toStdString();
    double realVal = point.y();
    QString unit = ls->property( "_unit" ).toString();
    const bool isNormalised = ls->property( "_normalised" ).toBool();

    auto seriesIt = m_seriesMap.find( keyStd );
    if ( isNormalised && seriesIt != m_seriesMap.end() )
        realVal = metricFromNormalisedScale( point.y(), seriesIt->second.metricGroup );

    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(
                            static_cast< qint64 >( point.x() ) );

    const QString label = unit.isEmpty()
        ? QStringLiteral( "%1\n%2: %3" )
            .arg( dt.toString( "HH:mm:ss" ), ls->name() )
            .arg( realVal, 0, 'f', 1 )
        : QStringLiteral( "%1\n%2: %3 %4" )
            .arg( dt.toString( "HH:mm:ss" ), ls->name() )
            .arg( realVal, 0, 'f', 1 )
            .arg( unit );

    co.text->setText( label );

    const QPointF scenePos = chart->mapToPosition( point );
    constexpr qreal pad = 4.0;
    const QRectF textRect = co.text->boundingRect();
    co.text->setPos( scenePos.x() + 10, scenePos.y() - textRect.height() - 6 );
    co.bg->setRect( co.text->pos().x() - pad,
                    co.text->pos().y() - pad,
                    textRect.width() + 2 * pad,
                    textRect.height() + 2 * pad );

    co.bg->show();
    co.text->show();
}

// ---------------------------------------------------------------------------
// Sticky marks — click to pin/unpin a data-point callout
// ---------------------------------------------------------------------------

void MonitorTab::handleSeriesClick( QLineSeries *ls, const QPointF &point )
{
    const QString keyStr = ls->property( "_metricKey" ).toString();
    if ( keyStr.isEmpty() )
        return;

    const std::string key = keyStr.toStdString();
    auto seriesIt = m_seriesMap.find( key );
    if ( seriesIt == m_seriesMap.end() )
        return;

    // Denormalise the plotted value back to raw
    const double rawValue = metricFromNormalisedScale( point.y(), seriesIt->second.metricGroup );
    const qint64 clickTs = static_cast< qint64 >( point.x() );

    if ( static_cast< int >( m_stickyMarks.size() ) >= MAX_STICKY_MARKS )
        return;

    // Snap to the nearest actual data point in the raw buffer
    auto &buf = seriesIt->second.buffer;
    if ( buf.isEmpty() )
        return;

    qint64 snapTs  = clickTs;
    double snapVal = rawValue;
    qint64 bestDist = static_cast< qint64 >( m_windowSeconds ) * 1000LL + 1;

    for ( const auto &pt : buf )
    {
        const qint64 d = std::abs( static_cast< qint64 >( pt.x() ) - clickTs );
        if ( d < bestDist )
        {
            bestDist = d;
            snapTs  = static_cast< qint64 >( pt.x() );
            snapVal = pt.y();
        }
    }

    addStickyMarkGroup( m_chart, snapTs, 0.5, { { key, snapVal } } );
}

void MonitorTab::addStickyMarkGroup( QChart *chart, qint64 ts, double clickDataY,
                                     const std::vector< StickyMetricEntry > &entries )
{
    if ( !chart )
        return;

    StickyMark mark;
    mark.timestamp  = ts;
    mark.clickDataY = clickDataY;
    mark.chart      = chart;
    mark.entries    = entries;

    const qint64 capturedTs = ts;
    QChart *capturedChart = chart;
    auto removeCb = [this, capturedTs, capturedChart]() {
        for ( auto it = m_stickyMarks.begin(); it != m_stickyMarks.end(); ++it )
        {
            if ( it->timestamp == capturedTs && it->chart == capturedChart )
            {
                removeStickyMark( it );
                return;
            }
        }
    };

    // Background rect (clickable)
    auto *bg = new ClickableRectItem( chart );
    bg->setBrush( QBrush( QColor( 30, 30, 30, 220 ) ) );
    bg->setPen( QPen( QColor( 200, 200, 200 ), 1 ) );
    bg->setZValue( 90 );
    bg->setClickCallback( removeCb );
    mark.bg = bg;

    // Timestamp header text
    auto *tsText = new QGraphicsSimpleTextItem( chart );
    tsText->setBrush( Qt::white );
    tsText->setZValue( 91 );
    tsText->setAcceptedMouseButtons( Qt::NoButton );
    mark.texts.push_back( tsText );

    // Per-metric texts
    for ( const auto &entry : entries )
    {
        auto seriesIt = m_seriesMap.find( entry.metricKey );
        const QColor col = ( seriesIt != m_seriesMap.end() ) ? seriesIt->second.color : Qt::white;
        auto *txt = new QGraphicsSimpleTextItem( chart );
        txt->setBrush( col );
        txt->setZValue( 91 );
        txt->setAcceptedMouseButtons( Qt::NoButton );
        mark.texts.push_back( txt );
    }

    // Vertical marker line
    auto *line = new QGraphicsLineItem( chart );
    line->setPen( QPen( QColor( 200, 200, 200, 150 ), 1, Qt::DashLine ) );
    line->setZValue( 89 );
    mark.line = line;

    m_stickyMarks.push_back( std::move( mark ) );
    updateStickyMarkPositions();
}

void MonitorTab::removeStickyMark( std::vector< StickyMark >::iterator it )
{
    for ( auto *txt : it->texts )
        delete txt;
    delete it->bg;
    delete it->line;

    m_stickyMarks.erase( it );
}

void MonitorTab::updateStickyMarkPositions()
{
    for ( auto &mark : m_stickyMarks )
    {
        if ( !mark.bg || !mark.chart )
            continue;

        // Check if any entry's series is visible
        bool anyVisible = false;
        for ( const auto &entry : mark.entries )
        {
            auto it = m_seriesMap.find( entry.metricKey );
            if ( it != m_seriesMap.end() )
            {
                QLineSeries *chartSeries = seriesForChart( it->second, mark.chart );
                if ( chartSeries && chartSeries->isVisible() )
                {
                    anyVisible = true;
                    break;
                }
            }
        }

        const QRectF plotArea = mark.chart->plotArea();
        const QPointF sceneX = mark.chart->mapToPosition(
            QPointF( static_cast< qreal >( mark.timestamp ), 0 ) );

        if ( !anyVisible || sceneX.x() < plotArea.left() || sceneX.x() > plotArea.right() )
        {
            mark.bg->hide();
            for ( auto *txt : mark.texts )
                txt->hide();
            if ( mark.line )
                mark.line->hide();
            continue;
        }

        // Set timestamp header text
        const QDateTime dt = QDateTime::fromMSecsSinceEpoch( mark.timestamp );
        mark.texts[0]->setText( dt.toString( "HH:mm:ss" ) );

        constexpr qreal pad = 4.0;
        constexpr qreal rowGap = 1.0;
        qreal totalH = 0;
        qreal maxW   = 0;

        // Measure timestamp row
        {
            const QRectF r = mark.texts[0]->boundingRect();
            totalH += r.height() + rowGap;
            maxW = std::max( maxW, r.width() );
        }

        // Measure + set metric rows
        size_t txtIdx = 1;
        for ( const auto &entry : mark.entries )
        {
            if ( txtIdx >= mark.texts.size() )
                break;

            auto seriesIt = m_seriesMap.find( entry.metricKey );
            bool vis = false;
            if ( seriesIt != m_seriesMap.end() )
            {
                QLineSeries *chartSeries = seriesForChart( seriesIt->second, mark.chart );
                vis = ( chartSeries && chartSeries->isVisible() );
            }

            if ( !vis )
            {
                mark.texts[ txtIdx ]->hide();
                ++txtIdx;
                continue;
            }

            const QString unit = seriesIt->second.unit;
            const QString rowText = QStringLiteral( "%1: %2 %3" )
                .arg( seriesIt->second.label )
                .arg( entry.rawValue, 0, 'f', 1 )
                .arg( unit );

            mark.texts[ txtIdx ]->setText( rowText );
            mark.texts[ txtIdx ]->show();

            const QRectF r = mark.texts[ txtIdx ]->boundingRect();
            totalH += r.height() + rowGap;
            maxW = std::max( maxW, r.width() );
            ++txtIdx;
        }

        // Position the label box
        const qreal boxW = maxW + 2 * pad;
        const qreal boxH = totalH + 2 * pad - rowGap;

        qreal bx = sceneX.x() + 8;
        if ( bx + boxW > plotArea.right() )
            bx = sceneX.x() - boxW - 8;

        qreal by = plotArea.top() + mark.clickDataY * plotArea.height() - boxH / 2.0;
        by = std::max( plotArea.top() + 2.0, by );
        by = std::min( plotArea.bottom() - boxH - 2.0, by );

        mark.bg->setRect( bx, by, boxW, boxH );
        mark.bg->show();

        // Position text rows inside the box
        qreal rowY = by + pad;
        for ( size_t t = 0; t < mark.texts.size(); ++t )
        {
            auto *txt = mark.texts[t];
            if ( !txt->isVisible() && t != 0 )
                continue;

            if ( t == 0 )
                txt->show();

            txt->setPos( bx + pad, rowY );
            rowY += txt->boundingRect().height() + rowGap;
        }

        // Vertical line from top to bottom of plot
        if ( mark.line )
        {
            mark.line->setLine( sceneX.x(), plotArea.top(), sceneX.x(), plotArea.bottom() );
            mark.line->show();
        }
    }
}

// ---------------------------------------------------------------------------
// Data fetching
// ---------------------------------------------------------------------------

void MonitorTab::fetchData()
{
    if ( !m_client || m_paused )
        return;

    ++m_fpsSourceRefreshTicks;
    if ( m_fpsSourceRefreshTicks % 2 == 0 )
        refreshFpsSourceControls();

    auto result = m_client->getMonitorDataSince( m_lastTimestamp );
    if ( !result.has_value() || result->isEmpty() )
        return;

    m_chartView->setUpdatesEnabled( false );

    applyBinaryData( *result );
    trimSeries();
    commitSeries();
    updateAxes();
    updateStickyMarkPositions();

    m_chartView->setUpdatesEnabled( true );

    if ( m_cursorInPlot && m_crosshairView )
        updateCrosshair( m_crosshairView, m_lastCrosshairPos, m_annotationsVisible );
}

void MonitorTab::applyBinaryData( const QByteArray &data )
{
    // Wire layout (native endian -- same-host IPC):
    //   per non-empty metric:
    //     uint16_t keyLen, char key[keyLen], uint32_t count,
    //     count x { int64_t timestampMs, double value }  (16 bytes each)

    static constexpr size_t kPointSize = sizeof( int64_t ) + sizeof( double );  // 16

    const auto *p   = reinterpret_cast< const uint8_t * >( data.constData() );
    const auto *end = p + data.size();
    qint64 maxTs = m_lastTimestamp;

    while ( p < end )
    {
        // Read key length
        if ( p + sizeof( uint16_t ) > end )
            break;
        uint16_t keyLen = 0;
        std::memcpy( &keyLen, p, sizeof( keyLen ) );
        p += sizeof( keyLen );

        // Read key string
        if ( p + keyLen > end )
            break;
        std::string key( reinterpret_cast< const char * >( p ), keyLen );
        p += keyLen;

        // Read count
        if ( p + sizeof( uint32_t ) > end )
            break;
        uint32_t count = 0;
        std::memcpy( &count, p, sizeof( count ) );
        p += sizeof( count );

        if ( p + static_cast< size_t >( count ) * kPointSize > end )
            break;

        // Only append to series we are actively tracking
        const bool tracked = ( m_seriesMap.count( key ) > 0 );

        for ( uint32_t j = 0; j < count; ++j )
        {
            int64_t ts  = 0;
            double  val = 0.0;
            std::memcpy( &ts,  p,              sizeof( ts ) );
            std::memcpy( &val, p + sizeof(ts), sizeof( val ) );
            p += kPointSize;

            if ( tracked )
            {
                m_seriesMap[ key ].buffer.append(
                    QPointF( static_cast< qreal >( ts ), val ) );
                if ( ts > maxTs )
                    maxTs = ts;
            }
        }
    }

    if ( maxTs > m_lastTimestamp )
        m_lastTimestamp = maxTs + 1;
}

void MonitorTab::trimSeries()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qreal cutoff = static_cast< qreal >( now - static_cast< qint64 >( m_windowSeconds ) * 1000 );

    for ( auto &[key, info] : m_seriesMap )
    {
        auto &buf = info.buffer;
        int stale = 0;
        while ( stale < buf.size() && buf[ stale ].x() < cutoff )
            ++stale;
        if ( stale > 0 )
            buf.remove( 0, stale );
    }
}

void MonitorTab::commitSeries()
{
    // Push in-memory buffers into QLineSeries, normalised to 0–100 % scale
    for ( auto &[key, info] : m_seriesMap )
    {
        const double scale = metricToNormalisedScale( info.metricGroup );
        if ( std::abs( scale - 1.0 ) < 1e-9 )
        {
            // No normalisation needed (Duty / Unknown) — direct replace
            info.series->replace( info.buffer );
        }
        else
        {
            QList< QPointF > scaled;
            scaled.reserve( info.buffer.size() );
            for ( const auto &pt : info.buffer )
                scaled.append( QPointF( pt.x(), pt.y() * scale ) );
            info.series->replace( scaled );
        }
    }

    commitGroupSeries();
}

void MonitorTab::commitGroupSeries()
{
    for ( auto &[key, info] : m_seriesMap )
    {
        if ( info.tempSeries )
            info.tempSeries->replace( info.buffer );

        if ( info.powerSeries )
            info.powerSeries->replace( info.buffer );

        if ( info.voltSeries )
            info.voltSeries->replace( info.buffer );

        if ( info.freqSeries )
            info.freqSeries->replace( info.buffer );

        if ( info.fanSeries )
        {
            if ( info.metricGroup == MetricGroup::Rpm )
            {
                const double scale = metricToNormalisedScale( MetricGroup::Rpm );
                QList< QPointF > scaled;
                scaled.reserve( info.buffer.size() );
                for ( const auto &pt : info.buffer )
                    scaled.append( QPointF( pt.x(), pt.y() * scale ) );
                info.fanSeries->replace( scaled );
            }
            else
            {
                info.fanSeries->replace( info.buffer );
            }
        }
    }
}

void MonitorTab::updateAxes()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime start = now.addSecs( -m_windowSeconds );
    if ( m_xAxis && m_zoomedCharts.count( m_chart ) == 0 )
        m_xAxis->setRange( start, now );
    if ( m_tempXAxis && m_zoomedCharts.count( m_tempChart ) == 0 )
        m_tempXAxis->setRange( start, now );
    if ( m_fanXAxis && m_zoomedCharts.count( m_fanChart ) == 0 )
        m_fanXAxis->setRange( start, now );
    if ( m_powerXAxis && m_zoomedCharts.count( m_powerChart ) == 0 )
        m_powerXAxis->setRange( start, now );
    if ( m_voltXAxis && m_zoomedCharts.count( m_voltChart ) == 0 )
        m_voltXAxis->setRange( start, now );
    if ( m_freqXAxis && m_zoomedCharts.count( m_freqChart ) == 0 )
        m_freqXAxis->setRange( start, now );
}

// ---------------------------------------------------------------------------
// Time window
// ---------------------------------------------------------------------------

void MonitorTab::setTimeWindow( int seconds )
{
    m_windowSeconds = std::clamp( seconds, 60, 1800 );

    if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    {
        const int mins = m_windowSeconds / 60;
        const int secs = m_windowSeconds % 60;
        QString text;
        if ( secs == 0 )
            text = QStringLiteral( "Time window: %1 min" ).arg( mins );
        else
            text = QStringLiteral( "Time window: %1:%2" )
                .arg( mins ).arg( secs, 2, 10, QLatin1Char( '0' ) );
        mw->statusBar()->showMessage( text, 3000 );
    }

    if ( m_paused )
    {
        updateAxes();
        updateStickyMarkPositions();
    }
    else
    {
        for ( auto &[key, info] : m_seriesMap )
        {
            info.buffer.clear();
            info.series->clear();
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_lastTimestamp = now - static_cast< qint64 >( m_windowSeconds ) * 1000;
        fetchData();
        updateAxes();
    }

    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    settings.beginGroup( "MonitorTab" );
    settings.setValue( "TimeWindowSeconds", m_windowSeconds );
    settings.endGroup();
    settings.sync();
}

// ---------------------------------------------------------------------------
// Keyboard and mouse events
// ---------------------------------------------------------------------------

void MonitorTab::keyPressEvent( QKeyEvent *event )
{
    if ( event->key() == Qt::Key_Space )
    {
        m_paused = !m_paused;
        if ( !m_paused && !m_zoomedCharts.empty() )
            resetZoom();
        if ( m_pauseLabel )
            m_pauseLabel->setVisible( m_paused );
        event->accept();
        return;
    }
    QWidget::keyPressEvent( event );
}

void MonitorTab::wheelEvent( QWheelEvent *event )
{
    if ( event->modifiers() & Qt::ControlModifier )
    {
        const int delta = event->angleDelta().y();
        if ( delta > 0 )
            setTimeWindow( m_windowSeconds - 30 );
        else if ( delta < 0 )
            setTimeWindow( m_windowSeconds + 30 );
        event->accept();
        return;
    }
    QWidget::wheelEvent( event );
}

bool MonitorTab::eventFilter( QObject *watched, QEvent *event )
{
    QChartView *activeView = chartViewForViewport( watched );
    if ( !activeView )
        return QWidget::eventFilter( watched, event );

    switch ( event->type() )
    {
        case QEvent::MouseMove:
        {
            auto *me = static_cast< QMouseEvent * >( event );

            if ( m_zoomDragging )
            {
                m_zoomBand->setGeometry( QRect( m_zoomOrigin, me->pos() ).normalized() );
                return true;
            }

            m_lastCrosshairPos = me->pos();
            m_cursorInPlot = true;
            updateCrosshair( activeView, me->pos(), m_annotationsVisible );
            break;
        }
        case QEvent::MouseButtonPress:
        {
            auto *me = static_cast< QMouseEvent * >( event );

            // Ctrl+LMB starts rubber-band zoom
            if ( me->button() == Qt::LeftButton
                 && ( me->modifiers() & Qt::ControlModifier ) )
            {
                m_zoomOrigin = me->pos();
                if ( !m_zoomBand )
                    m_zoomBand = new QRubberBand( QRubberBand::Rectangle, activeView->viewport() );
                else if ( m_zoomBand->parentWidget() != activeView->viewport() )
                    m_zoomBand->setParent( activeView->viewport() );
                m_zoomBand->setGeometry( QRect( m_zoomOrigin, QSize() ) );
                m_zoomBand->show();
                m_zoomDragging = true;
                return true;
            }

            // LMB on empty space creates a grouped sticky mark
            if ( me->button() == Qt::LeftButton && m_annotationsVisible )
            {
                const QPointF scenePos = activeView->mapToScene(
                    static_cast< int >( me->pos().x() ),
                    static_cast< int >( me->pos().y() ) );
                const auto items = activeView->chart()->scene()->items( scenePos );

                // If a clickable item (mark bg rect) is under the cursor, let it handle it
                for ( auto *item : items )
                {
                    if ( dynamic_cast< ClickableRectItem * >( item ) )
                        return false;
                }

                crosshairClick( activeView, me->pos() );
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease:
        {
            auto *me = static_cast< QMouseEvent * >( event );

            if ( me->button() == Qt::LeftButton && m_zoomDragging )
            {
                m_zoomDragging = false;
                if ( m_zoomBand )
                    m_zoomBand->hide();
                const QRect rect = QRect( m_zoomOrigin, me->pos() ).normalized();
                if ( rect.width() > 4 && rect.height() > 4 )
                    applyZoomRect( activeView, rect );
                return true;
            }

            // RMB toggles annotations
            if ( me->button() == Qt::RightButton )
            {
                m_annotationsVisible = !m_annotationsVisible;
                updateCrosshair( activeView, me->pos(), m_annotationsVisible );
                return true;
            }
            break;
        }
        case QEvent::MouseButtonDblClick:
        {
            // Double-click to reset zoom
            if ( activeView && m_zoomedCharts.count( activeView->chart() ) > 0 )
            {
                resetZoom( activeView->chart() );
                if ( m_zoomedCharts.empty() )
                {
                    m_paused = false;
                    if ( m_pauseLabel )
                        m_pauseLabel->hide();
                }
                return true;
            }
            break;
        }
        case QEvent::Leave:
            m_cursorInPlot = false;
            hideCrosshair();
            break;
        default:
            break;
    }

    return QWidget::eventFilter( watched, event );
}

QChartView *MonitorTab::chartViewForViewport( QObject *watched ) const
{
    if ( m_chartView && watched == m_chartView->viewport() )
        return m_chartView;
    if ( m_tempChartView && watched == m_tempChartView->viewport() )
        return m_tempChartView;
    if ( m_fanChartView && watched == m_fanChartView->viewport() )
        return m_fanChartView;
    if ( m_powerChartView && watched == m_powerChartView->viewport() )
        return m_powerChartView;
    if ( m_voltChartView && watched == m_voltChartView->viewport() )
        return m_voltChartView;
    if ( m_freqChartView && watched == m_freqChartView->viewport() )
        return m_freqChartView;
    return nullptr;
}

QDateTimeAxis *MonitorTab::activeXAxis() const
{
    if ( !m_graphTabs )
        return m_xAxis;

    switch ( m_graphTabs->currentIndex() )
    {
        case 1: return m_tempXAxis;
        case 2: return m_fanXAxis;
        case 3: return m_powerXAxis;
        case 4: return m_voltXAxis;
        case 5: return m_freqXAxis;
        default: return m_xAxis;
    }
}

QValueAxis *MonitorTab::activeYAxis() const
{
    if ( !m_graphTabs )
        return m_yAxis;

    switch ( m_graphTabs->currentIndex() )
    {
        case 1: return m_tempYAxis;
        case 2: return m_fanYAxis;
        case 3: return m_powerYAxis;
        case 4: return m_voltYAxis;
        case 5: return m_freqYAxis;
        default: return m_yAxis;
    }
}

QLineSeries *MonitorTab::seriesForChart( const SeriesInfo &info, const QChart *chart ) const
{
    if ( chart == m_chart )
        return info.series;
    if ( chart == m_tempChart )
        return info.tempSeries;
    if ( chart == m_fanChart )
        return info.fanSeries;
    if ( chart == m_powerChart )
        return info.powerSeries;
    if ( chart == m_voltChart )
        return info.voltSeries;
    if ( chart == m_freqChart )
        return info.freqSeries;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Crosshair — vertical line + per-series labels showing real values
// ---------------------------------------------------------------------------

void MonitorTab::hideCrosshair()
{
    if ( m_crosshairLine )
    {
        delete m_crosshairLine;
        m_crosshairLine = nullptr;
    }

    for ( auto &cl : m_crosshairLabels )
    {
        delete cl.bg;
        delete cl.text;
    }
    m_crosshairLabels.clear();
    m_crosshairChart = nullptr;
    m_crosshairView = nullptr;
    m_crosshairVisible = false;
}

void MonitorTab::updateCrosshair( QChartView *chartView, const QPointF &widgetPos, bool ctrlHeld )
{
    if ( !chartView || !chartView->chart() )
    {
        hideCrosshair();
        return;
    }

    QChart *chart = chartView->chart();

    const QPointF scenePos = chartView->mapToScene(
        static_cast< int >( widgetPos.x() ),
        static_cast< int >( widgetPos.y() ) );
    const QRectF plotArea = chart->plotArea();

    if ( !plotArea.contains( scenePos ) )
    {
        hideCrosshair();
        return;
    }

    if ( !m_crosshairLine || m_crosshairChart != chart )
    {
        hideCrosshair();
        m_crosshairLine = new QGraphicsLineItem( chart );
        m_crosshairLine->setPen( QPen( QColor( 200, 200, 200, 150 ), 1, Qt::DashLine ) );
        m_crosshairLine->setZValue( 80 );
        m_crosshairChart = chart;
        m_crosshairView = chartView;
    }

    m_crosshairLine->setLine( scenePos.x(), plotArea.top(),
                               scenePos.x(), plotArea.bottom() );
    m_crosshairLine->show();

    // Clean up old labels
    for ( auto &cl : m_crosshairLabels )
    {
        delete cl.bg;
        delete cl.text;
    }
    m_crosshairLabels.clear();

    if ( !ctrlHeld )
    {
        m_crosshairVisible = true;
        return;
    }

    const QPointF dataPos = chart->mapToValue( scenePos );
    const qint64 cursorTs = static_cast< qint64 >( dataPos.x() );

    // Count visible metrics with data + 1 for the timestamp label
    int totalLabels = 1;
    for ( const auto &[key, info] : m_seriesMap )
    {
        QLineSeries *series = seriesForChart( info, chart );
        if ( series && series->isVisible() && !info.buffer.isEmpty() )
            ++totalLabels;
    }

    int labelIndex = 0;

    for ( const auto &[key, info] : m_seriesMap )
    {
        QLineSeries *series = seriesForChart( info, chart );
        if ( !series || !series->isVisible() || info.buffer.isEmpty() )
            continue;

        // Binary search for nearest point
        const auto &buf = info.buffer;
        int lo = 0, hi = buf.size() - 1;
        while ( lo < hi )
        {
            const int mid = lo + ( hi - lo ) / 2;
            if ( static_cast< qint64 >( buf[ mid ].x() ) < cursorTs )
                lo = mid + 1;
            else
                hi = mid;
        }
        int bestIdx = lo;
        if ( lo > 0 )
        {
            const qint64 dLo = std::abs( static_cast< qint64 >( buf[ lo ].x() ) - cursorTs );
            const qint64 dPrev = std::abs( static_cast< qint64 >( buf[ lo - 1 ].x() ) - cursorTs );
            if ( dPrev < dLo )
                bestIdx = lo - 1;
        }

        // Raw value from buffer (not normalised)
        const double rawVal = buf[ bestIdx ].y();

        const QString lbl = info.unit.isEmpty()
            ? QStringLiteral( "%1: %2" ).arg( info.label ).arg( rawVal, 0, 'f', 1 )
            : QStringLiteral( "%1: %2 %3" ).arg( info.label ).arg( rawVal, 0, 'f', 1 ).arg( info.unit );

        auto *bg   = new QGraphicsRectItem( chart );
        auto *text = new QGraphicsSimpleTextItem( chart );

        bg->setBrush( QBrush( QColor( 30, 30, 30, 220 ) ) );
        bg->setPen( QPen( info.color, 1 ) );
        bg->setZValue( 95 );
        text->setBrush( info.color );
        text->setZValue( 96 );
        text->setAcceptedMouseButtons( Qt::NoButton );
        text->setText( lbl );

        constexpr qreal pad = 3.0;
        const QRectF textRect = text->boundingRect();
        const qreal rowH   = textRect.height() + 2 * pad + 2;
        const qreal startY = std::max( plotArea.top() + 2,
                               std::min( scenePos.y() - totalLabels * rowH / 2.0,
                                         plotArea.bottom() - totalLabels * rowH - 2 ) );
        const qreal baseY  = startY + labelIndex * rowH;
        qreal tx = scenePos.x() + 12;
        if ( tx + textRect.width() + 2 * pad > plotArea.right() )
            tx = scenePos.x() - textRect.width() - 2 * pad - 12;

        text->setPos( tx, baseY );
        bg->setRect( tx - pad, baseY - pad,
                     textRect.width() + 2 * pad,
                     textRect.height() + 2 * pad );
        bg->show();
        text->show();

        m_crosshairLabels.push_back( { bg, text } );
        ++labelIndex;
    }

    // Timestamp label at the bottom of the stack
    {
        constexpr qreal pad = 3.0;
        auto *bg   = new QGraphicsRectItem( chart );
        auto *text = new QGraphicsSimpleTextItem( chart );

        bg->setBrush( QBrush( QColor( 30, 30, 30, 220 ) ) );
        bg->setPen( QPen( QColor( 150, 150, 150 ), 1 ) );
        bg->setZValue( 95 );
        text->setBrush( Qt::white );
        text->setZValue( 96 );
        text->setAcceptedMouseButtons( Qt::NoButton );

        const QDateTime dt = QDateTime::fromMSecsSinceEpoch( cursorTs );
        text->setText( dt.toString( "HH:mm:ss" ) );

        const QRectF textRect = text->boundingRect();
        const qreal rowH   = textRect.height() + 2 * pad + 2;
        const qreal startY = std::max( plotArea.top() + 2,
                               std::min( scenePos.y() - totalLabels * rowH / 2.0,
                                         plotArea.bottom() - totalLabels * rowH - 2 ) );
        const qreal baseY  = startY + labelIndex * rowH;
        qreal tx = scenePos.x() + 12;
        if ( tx + textRect.width() + 2 * pad > plotArea.right() )
            tx = scenePos.x() - textRect.width() - 2 * pad - 12;

        text->setPos( tx, baseY );
        bg->setRect( tx - pad, baseY - pad,
                     textRect.width() + 2 * pad,
                     textRect.height() + 2 * pad );
        bg->show();
        text->show();

        m_crosshairLabels.push_back( { bg, text } );
    }

    m_crosshairVisible = true;
}

void MonitorTab::crosshairClick( QChartView *chartView, const QPointF &widgetPos )
{
    if ( !chartView || !chartView->chart() )
        return;

    QChart *chart = chartView->chart();

    const QPointF scenePos = chartView->mapToScene(
        static_cast< int >( widgetPos.x() ),
        static_cast< int >( widgetPos.y() ) );
    const QRectF plotArea = chart->plotArea();

    if ( !plotArea.contains( scenePos ) )
        return;

    const QPointF dataPos = chart->mapToValue( scenePos );
    const qint64 cursorTs = static_cast< qint64 >( dataPos.x() );

    if ( static_cast< int >( m_stickyMarks.size() ) >= MAX_STICKY_MARKS )
        return;

    // Collect all visible metrics at this timestamp
    std::vector< StickyMetricEntry > entries;
    qint64 snapTs = cursorTs;

    for ( const auto &[key, info] : m_seriesMap )
    {
        QLineSeries *chartSeries = seriesForChart( info, chart );
        if ( !chartSeries || !chartSeries->isVisible() )
            continue;

        const auto &buf = info.buffer;
        if ( buf.isEmpty() )
            continue;

        // Binary search for the nearest timestamp
        int lo = 0, hi = buf.size() - 1;
        while ( lo < hi )
        {
            const int mid = lo + ( hi - lo ) / 2;
            if ( static_cast< qint64 >( buf[ mid ].x() ) < cursorTs )
                lo = mid + 1;
            else
                hi = mid;
        }

        int bestIdx = lo;
        if ( lo > 0 )
        {
            const qint64 dLo = std::abs( static_cast< qint64 >( buf[ lo ].x() ) - cursorTs );
            const qint64 dPrev = std::abs( static_cast< qint64 >( buf[ lo - 1 ].x() ) - cursorTs );
            if ( dPrev < dLo )
                bestIdx = lo - 1;
        }

        // Use the first metric's snapped timestamp as the group timestamp
        if ( entries.empty() )
            snapTs = static_cast< qint64 >( buf[ bestIdx ].x() );

        entries.push_back( { key, buf[ bestIdx ].y() } );
    }

    if ( !entries.empty() )
    {
        const double plotFrac = ( plotArea.height() > 0 )
            ? ( scenePos.y() - plotArea.top() ) / plotArea.height()
            : 0.5;
        addStickyMarkGroup( chart, snapTs, plotFrac, entries );
    }
}

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------

void MonitorTab::applyZoomRect( QChartView *chartView, const QRect &viewportRect )
{
    if ( !chartView || !chartView->chart() )
        return;

    QChart *chart = chartView->chart();
    QDateTimeAxis *xAxis = nullptr;
    QValueAxis *yAxis = nullptr;

    if ( chart == m_chart )
    {
        xAxis = m_xAxis;
        yAxis = m_yAxis;
    }
    else if ( chart == m_tempChart )
    {
        xAxis = m_tempXAxis;
        yAxis = m_tempYAxis;
    }
    else if ( chart == m_fanChart )
    {
        xAxis = m_fanXAxis;
        yAxis = m_fanYAxis;
    }
    else if ( chart == m_powerChart )
    {
        xAxis = m_powerXAxis;
        yAxis = m_powerYAxis;
    }
    else if ( chart == m_voltChart )
    {
        xAxis = m_voltXAxis;
        yAxis = m_voltYAxis;
    }
    else if ( chart == m_freqChart )
    {
        xAxis = m_freqXAxis;
        yAxis = m_freqYAxis;
    }

    if ( !xAxis || !yAxis )
        return;

    const QPointF topLeft = chartView->mapToScene( viewportRect.topLeft() );
    const QPointF bottomRight = chartView->mapToScene( viewportRect.bottomRight() );
    const QRectF plotArea = chart->plotArea();

    const QPointF clampedTL(
        std::max( topLeft.x(), plotArea.left() ),
        std::max( topLeft.y(), plotArea.top() ) );
    const QPointF clampedBR(
        std::min( bottomRight.x(), plotArea.right() ),
        std::min( bottomRight.y(), plotArea.bottom() ) );

    const QPointF dataMin = chart->mapToValue( clampedTL );
    const QPointF dataMax = chart->mapToValue( clampedBR );

    const qreal yLo = std::min( dataMin.y(), dataMax.y() );
    const qreal yHi = std::max( dataMin.y(), dataMax.y() );
    const qint64 tLo = static_cast< qint64 >( std::min( dataMin.x(), dataMax.x() ) );
    const qint64 tHi = static_cast< qint64 >( std::max( dataMin.x(), dataMax.x() ) );

    m_paused = true;
    if ( m_pauseLabel )
        m_pauseLabel->setVisible( true );

    xAxis->setRange(
        QDateTime::fromMSecsSinceEpoch( tLo ),
        QDateTime::fromMSecsSinceEpoch( tHi ) );
    yAxis->setRange( yLo, yHi );

    m_zoomedCharts.insert( chart );
    if ( chart == m_chart )
        updateStickyMarkPositions();
}

void MonitorTab::resetZoom( QChart *chart )
{
    if ( chart == nullptr )
    {
        m_zoomedCharts.clear();
        if ( m_yAxis )
            m_yAxis->setRange( 0, 100 );
        if ( m_tempYAxis )
            m_tempYAxis->setRange( 0, 105 );
        if ( m_fanYAxis )
            m_fanYAxis->setRange( 0, 100 );
        if ( m_powerYAxis )
            m_powerYAxis->setRange( 0, static_cast< double >( m_maxPowerW ) );
        if ( m_voltYAxis )
            m_voltYAxis->setRange( 0, 1500 );
        if ( m_freqYAxis )
            m_freqYAxis->setRange( 0, 6000 );
        updateAxes();
        updateStickyMarkPositions();
        return;
    }

    if ( chart == m_chart && m_yAxis )
        m_yAxis->setRange( 0, 100 );
    else if ( chart == m_tempChart && m_tempYAxis )
        m_tempYAxis->setRange( 0, 105 );
    else if ( chart == m_fanChart && m_fanYAxis )
        m_fanYAxis->setRange( 0, 100 );
    else if ( chart == m_powerChart && m_powerYAxis )
        m_powerYAxis->setRange( 0, static_cast< double >( m_maxPowerW ) );
    else if ( chart == m_voltChart && m_voltYAxis )
        m_voltYAxis->setRange( 0, 1500 );
    else if ( chart == m_freqChart && m_freqYAxis )
        m_freqYAxis->setRange( 0, 6000 );

    m_zoomedCharts.erase( chart );
    updateAxes();
    if ( chart == m_chart )
        updateStickyMarkPositions();
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

void MonitorTab::saveSourceSelection()
{
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    settings.beginGroup( "MonitorTab" );

    QStringList entries;
    for ( const auto &key : m_activeSources )
        entries.append( QString::fromStdString( key ) );
    settings.setValue( "SelectedSources", entries );

    QStringList favoriteEntries;
    for ( const auto &key : m_favoriteSources )
        favoriteEntries.append( QString::fromStdString( key ) );
    settings.setValue( "FavoriteSources", favoriteEntries );

    if ( m_monitorSplitter )
        settings.setValue( "SplitterState", m_monitorSplitter->saveState() );

    settings.setValue( "TimeWindowSeconds", m_windowSeconds );
    settings.endGroup();
    settings.sync();
}

void MonitorTab::loadSourceSelection()
{
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    settings.beginGroup( "MonitorTab" );

    m_windowSeconds = std::clamp( settings.value( "TimeWindowSeconds", 300 ).toInt(), 60, 1800 );

    const QStringList entries = settings.value( "SelectedSources" ).toStringList();
    for ( const QString &entry : entries )
    {
        const std::string key = entry.toStdString();
        if ( !key.empty() )
            m_activeSources.insert( key );
    }

    const QStringList favoriteEntries = settings.value( "FavoriteSources" ).toStringList();
    std::set< std::string > seenFavorites;
    for ( const QString &entry : favoriteEntries )
    {
        const std::string key = entry.toStdString();
        if ( key.empty() || seenFavorites.count( key ) )
            continue;

        seenFavorites.insert( key );
        m_favoriteSources.push_back( key );
        if ( m_favoriteSources.size() >= 10 )
            break;
    }

    for ( const auto &key : m_activeSources )
    {
        if ( seenFavorites.count( key ) )
            continue;

        m_favoriteSources.push_back( key );
        seenFavorites.insert( key );
        if ( m_favoriteSources.size() >= 10 )
            break;
    }

    const QByteArray splitterState = settings.value( "SplitterState" ).toByteArray();

    settings.endGroup();

    if ( m_monitorSplitter && !splitterState.isEmpty() )
        m_monitorSplitter->restoreState( splitterState );
}

} // namespace ucc
