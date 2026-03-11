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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstring>
#include <algorithm>
#include <functional>

namespace ucc
{

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
    setupUI();
    setFocusPolicy( Qt::StrongFocus );

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
    auto *hSplit = new QHBoxLayout();

    // Source selector
    auto *selectorBox = new QGroupBox( "Sources" );
    selectorBox->setMinimumWidth( 280 );
    selectorBox->setMaximumWidth( 360 );
    auto *selectorOuterLayout = new QVBoxLayout( selectorBox );

    auto *selectorScroll = new QScrollArea();
    selectorScroll->setWidgetResizable( true );
    selectorScroll->setFrameShape( QFrame::NoFrame );

    auto *selectorInner = new QWidget();
    m_selectorLayout = new QVBoxLayout( selectorInner );
    m_selectorLayout->setContentsMargins( 0, 0, 0, 0 );
    m_selectorLayout->addStretch();

    selectorScroll->setWidget( selectorInner );
    selectorOuterLayout->addWidget( selectorScroll, 1 );

    m_addSourceBtn = new QPushButton( "+ Add Source" );
    connect( m_addSourceBtn, &QPushButton::clicked, this, [this]() { addSourceRow(); } );
    selectorOuterLayout->addWidget( m_addSourceBtn );

    hSplit->addWidget( selectorBox );

    // Chart
    setupChart();
    hSplit->addWidget( m_chartView, 1 );

    mainLayout->addLayout( hSplit, 1 );

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
    m_yAxis = createYAxis( "", 0, 100 );
    m_chart->addAxis( m_xAxis, Qt::AlignBottom );
    m_chart->addAxis( m_yAxis, Qt::AlignLeft );

    m_chartView = createChartView( m_chart );
    m_chartView->viewport()->installEventFilter( this );

    // Crosshair line
    m_crosshairLine = new QGraphicsLineItem( m_chart );
    m_crosshairLine->setPen( QPen( QColor( 200, 200, 200, 100 ), 1, Qt::DashLine ) );
    m_crosshairLine->setZValue( 80 );
    m_crosshairLine->hide();

    // Hover callout
    installHoverCallout( m_chart );
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

    m_fpsRequireP0Check = new QCheckBox( "Require NVIDIA P0", controls );
    m_fpsRequireP0Check->setChecked( true );

    m_fpsCurrentAppLabel = new QLabel( "Current: -", controls );

    row->addWidget( sourceLabel );
    row->addWidget( m_fpsSourceCombo );
    row->addWidget( m_fpsRequireP0Check );
    row->addWidget( m_fpsCurrentAppLabel, 1 );

    connect( m_fpsSourceCombo, &QComboBox::currentIndexChanged, this, [this]( int ) {
        if ( m_syncingFpsControls || !m_client || !m_fpsSourceCombo )
            return;
        m_client->setFpsSourceApp( m_fpsSourceCombo->currentData().toString().toStdString() );
    } );

    connect( m_fpsRequireP0Check, &QCheckBox::toggled, this, [this]( bool checked ) {
        if ( m_syncingFpsControls || !m_client )
            return;
        m_client->setFpsRequireP0( checked );
    } );

    mainLayout->addWidget( controls );
    refreshFpsSourceControls();
}

void MonitorTab::refreshFpsSourceControls()
{
    if ( !m_client || !m_fpsSourceCombo || !m_fpsRequireP0Check || !m_fpsCurrentAppLabel )
        return;

    auto jsonOpt = m_client->getFpsSourcesJSON();
    if ( !jsonOpt.has_value() )
        return;

    const QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *jsonOpt ) );
    if ( !doc.isObject() )
        return;

    const QJsonObject root = doc.object();
    const QString selected = root.value( "selectedApp" ).toString( "auto" );
    const bool requireP0 = root.value( "requireP0" ).toBool( true );
    const QString currentApp = root.value( "currentApp" ).toString();
    const qint64 currentPid = root.value( "currentPid" ).toInteger( 0 );

    QStringList apps;
    apps.append( "auto" );
    const QJsonArray appArr = root.value( "apps" ).toArray();
    for ( const auto &v : appArr )
    {
        const QString a = v.toString().trimmed();
        if ( !a.isEmpty() && !apps.contains( a, Qt::CaseInsensitive ) )
            apps.append( a );
    }
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

    m_fpsRequireP0Check->setChecked( requireP0 );

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

    auto jsonOpt = m_client->getMonitorSourcesJSON();
    if ( !jsonOpt.has_value() )
        return;

    const QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *jsonOpt ) );
    if ( !doc.isArray() )
        return;

    m_availableSources.clear();
    for ( const auto &v : doc.array() )
    {
        const QJsonObject obj = v.toObject();
        SourceDef sd;
        sd.key   = obj.value( "key" ).toString().toStdString();
        sd.label = obj.value( "label" ).toString().toStdString();
        sd.group = obj.value( "group" ).toString().toStdString();
        sd.unit  = obj.value( "unit" ).toString().toStdString();
        if ( !sd.key.empty() )
            m_availableSources.push_back( std::move( sd ) );
    }

    // Re-populate existing combo boxes with the updated source list
    for ( auto &row : m_sourceRows )
    {
        if ( !row.combo )
            continue;

        const std::string savedKey = row.activeKey;
        row.combo->blockSignals( true );
        row.combo->clear();
        row.combo->addItem( QString::fromUtf8( "\xe2\x80\x94 Select source \xe2\x80\x94" ), QString() );

        // Group sources in the combo
        QString lastGroup;
        for ( const auto &sd : m_availableSources )
        {
            const QString group = QString::fromStdString( sd.group );
            if ( group != lastGroup )
            {
                row.combo->insertSeparator( row.combo->count() );
                lastGroup = group;
            }
            const QString display = QStringLiteral( "%1 (%2)" )
                .arg( QString::fromStdString( sd.label ) )
                .arg( QString::fromStdString( sd.unit ) );
            row.combo->addItem( display, QString::fromStdString( sd.key ) );
        }

        // Restore previous selection
        if ( !savedKey.empty() )
        {
            int idx = row.combo->findData( QString::fromStdString( savedKey ) );
            if ( idx >= 0 )
                row.combo->setCurrentIndex( idx );
        }
        row.combo->blockSignals( false );
    }
}

// ---------------------------------------------------------------------------
// Source rows — combo + checkbox + remove
// ---------------------------------------------------------------------------

void MonitorTab::addSourceRow( const std::string &key )
{
    auto *rowWidget = new QWidget();
    auto *rowLayout = new QHBoxLayout( rowWidget );
    rowLayout->setContentsMargins( 0, 2, 0, 2 );

    auto *cb = new QCheckBox();
    cb->setChecked( true );

    auto *combo = new QComboBox();
    combo->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
    combo->addItem( QString::fromUtf8( "\xe2\x80\x94 Select source \xe2\x80\x94" ), QString() );

    // Populate from available sources
    QString lastGroup;
    for ( const auto &sd : m_availableSources )
    {
        const QString group = QString::fromStdString( sd.group );
        if ( group != lastGroup )
        {
            combo->insertSeparator( combo->count() );
            lastGroup = group;
        }
        const QString display = QStringLiteral( "%1 (%2)" )
            .arg( QString::fromStdString( sd.label ) )
            .arg( QString::fromStdString( sd.unit ) );
        combo->addItem( display, QString::fromStdString( sd.key ) );
    }

    auto *removeBtn = new QPushButton( QString::fromUtf8( "\xe2\x9c\x95" ) );
    removeBtn->setFixedWidth( 24 );
    removeBtn->setFixedHeight( 24 );

    rowLayout->addWidget( cb );
    rowLayout->addWidget( combo, 1 );
    rowLayout->addWidget( removeBtn );

    // Insert before the stretch at the end
    const int insertIdx = m_selectorLayout->count() - 1;
    m_selectorLayout->insertWidget( insertIdx, rowWidget );

    SourceRow row;
    row.combo = combo;
    row.checkbox = cb;
    row.removeBtn = removeBtn;
    row.activeKey = key;

    const int rowIndex = static_cast< int >( m_sourceRows.size() );
    m_sourceRows.push_back( row );

    // Select the key if provided
    if ( !key.empty() )
    {
        int idx = combo->findData( QString::fromStdString( key ) );
        if ( idx >= 0 )
            combo->setCurrentIndex( idx );
        ensureSeries( key );
    }

    // Connect combo change -> create/remove series
    connect( combo, &QComboBox::currentIndexChanged, this, [this, rowIndex]( int ) {
        if ( rowIndex >= static_cast< int >( m_sourceRows.size() ) )
            return;

        auto &r = m_sourceRows[static_cast< size_t >( rowIndex )];
        const std::string newKey = r.combo->currentData().toString().toStdString();

        // Remove old series if key changed
        if ( !r.activeKey.empty() && r.activeKey != newKey )
            removeSeries( r.activeKey );

        r.activeKey = newKey;

        if ( !newKey.empty() )
        {
            ensureSeries( newKey );
            // Apply checkbox state
            auto it = m_seriesMap.find( newKey );
            if ( it != m_seriesMap.end() )
                it->second.series->setVisible( r.checkbox->isChecked() );
        }
        saveSourceSelection();
    } );

    // Connect checkbox toggle -> show/hide series
    connect( cb, &QCheckBox::toggled, this, [this, rowIndex]( bool checked ) {
        if ( rowIndex >= static_cast< int >( m_sourceRows.size() ) )
            return;
        const auto &r = m_sourceRows[static_cast< size_t >( rowIndex )];
        if ( !r.activeKey.empty() )
        {
            auto it = m_seriesMap.find( r.activeKey );
            if ( it != m_seriesMap.end() )
                it->second.series->setVisible( checked );
        }
        saveSourceSelection();
    } );

    // Connect remove button
    connect( removeBtn, &QPushButton::clicked, this, [this, rowIndex]() {
        removeSourceRow( rowIndex );
    } );
}

void MonitorTab::removeSourceRow( int row )
{
    if ( row < 0 || row >= static_cast< int >( m_sourceRows.size() ) )
        return;

    auto &r = m_sourceRows[static_cast< size_t >( row )];

    // Remove the series
    if ( !r.activeKey.empty() )
        removeSeries( r.activeKey );

    // Remove the widget from the layout
    if ( r.combo )
    {
        QWidget *rowWidget = r.combo->parentWidget();
        m_selectorLayout->removeWidget( rowWidget );
        delete rowWidget;
    }

    m_sourceRows.erase( m_sourceRows.begin() + row );
    saveSourceSelection();
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
    for ( const auto &sd : m_availableSources )
    {
        if ( sd.key == key )
        {
            label = QString::fromStdString( sd.label );
            unit = QString::fromStdString( sd.unit );
            break;
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

    m_chart->addSeries( series );
    series->attachAxis( m_xAxis );
    series->attachAxis( m_yAxis );

    // Install hover callout for this new series
    connect( series, &QLineSeries::hovered,
             this, [this, series]( const QPointF &point, bool state ) {
        showHoverCallout( series, m_chart, point, state );
    } );

    m_seriesMap[ key ] = { series, label, color, unit, {} };
}

void MonitorTab::removeSeries( const std::string &key )
{
    auto it = m_seriesMap.find( key );
    if ( it == m_seriesMap.end() )
        return;

    m_chart->removeSeries( it->second.series );
    delete it->second.series;
    m_seriesMap.erase( it );
}

// ---------------------------------------------------------------------------
// Hover callout
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

    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(
                            static_cast< qint64 >( point.x() ) );
    const QString unit = ls->property( "_unit" ).toString();

    const QString label = unit.isEmpty()
        ? QStringLiteral( "%1\n%2: %3" )
            .arg( dt.toString( "HH:mm:ss" ), ls->name() )
            .arg( point.y(), 0, 'f', 1 )
        : QStringLiteral( "%1\n%2: %3 %4" )
            .arg( dt.toString( "HH:mm:ss" ), ls->name() )
            .arg( point.y(), 0, 'f', 1 )
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
// Data fetching
// ---------------------------------------------------------------------------

void MonitorTab::fetchData()
{
    if ( !m_client || m_paused )
        return;

    ++m_fpsSourceRefreshTicks;
    if ( m_fpsSourceRefreshTicks % 5 == 0 )
        refreshFpsSourceControls();

    auto result = m_client->getMonitorDataSince( m_lastTimestamp );
    if ( !result.has_value() || result->isEmpty() )
        return;

    m_chartView->setUpdatesEnabled( false );

    applyBinaryData( *result );
    trimSeries();
    commitSeries();
    updateAxes();
    updateYRange();

    m_chartView->setUpdatesEnabled( true );

    if ( m_cursorInPlot )
        updateCrosshair( m_lastCrosshairPos, true );
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
    for ( auto &[key, info] : m_seriesMap )
        info.series->replace( info.buffer );
}

void MonitorTab::updateAxes()
{
    if ( m_zoomed )
        return;

    const QDateTime now = QDateTime::currentDateTime();
    const QDateTime start = now.addSecs( -m_windowSeconds );
    m_xAxis->setRange( start, now );
}

void MonitorTab::updateYRange()
{
    if ( m_zoomed )
        return;

    // Compute Y range from all visible series
    double yMin = std::numeric_limits< double >::max();
    double yMax = std::numeric_limits< double >::lowest();
    bool anyData = false;

    for ( const auto &[key, info] : m_seriesMap )
    {
        if ( !info.series->isVisible() || info.buffer.isEmpty() )
            continue;

        for ( const auto &pt : info.buffer )
        {
            if ( pt.y() < yMin ) yMin = pt.y();
            if ( pt.y() > yMax ) yMax = pt.y();
            anyData = true;
        }
    }

    if ( !anyData )
    {
        m_yAxis->setRange( 0, 100 );
        return;
    }

    // Add 10% padding
    const double range = yMax - yMin;
    const double pad = ( range > 0 ) ? range * 0.1 : 10.0;
    m_yAxis->setRange( std::max( 0.0, yMin - pad ), yMax + pad );
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
        if ( !m_paused && m_zoomed )
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
    if ( watched != m_chartView->viewport() )
        return QWidget::eventFilter( watched, event );

    switch ( event->type() )
    {
        case QEvent::MouseMove:
        {
            auto *me = static_cast< QMouseEvent * >( event );
            m_lastCrosshairPos = me->pos();
            m_cursorInPlot = true;
            updateCrosshair( me->pos(), true );
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
                    m_zoomBand = new QRubberBand( QRubberBand::Rectangle, m_chartView->viewport() );
                m_zoomBand->setGeometry( QRect( m_zoomOrigin, QSize() ) );
                m_zoomBand->show();
                m_zoomDragging = true;
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
                    applyZoomRect( rect );
                return true;
            }
            break;
        }
        case QEvent::MouseButtonDblClick:
        {
            // Double-click to reset zoom
            if ( m_zoomed )
            {
                resetZoom();
                m_paused = false;
                if ( m_pauseLabel )
                    m_pauseLabel->hide();
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

// ---------------------------------------------------------------------------
// Crosshair
// ---------------------------------------------------------------------------

void MonitorTab::hideCrosshair()
{
    if ( m_crosshairLine )
        m_crosshairLine->hide();

    for ( auto &cl : m_crosshairLabels )
    {
        delete cl.bg;
        delete cl.text;
    }
    m_crosshairLabels.clear();
    m_crosshairVisible = false;
}

void MonitorTab::updateCrosshair( const QPointF &widgetPos, bool ctrlHeld )
{
    if ( !m_chart )
    {
        hideCrosshair();
        return;
    }

    const QPointF scenePos = m_chartView->mapToScene(
        static_cast< int >( widgetPos.x() ),
        static_cast< int >( widgetPos.y() ) );
    const QRectF plotArea = m_chart->plotArea();

    if ( !plotArea.contains( scenePos ) )
    {
        hideCrosshair();
        return;
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

    const QPointF dataPos = m_chart->mapToValue( scenePos );
    const qint64 cursorTs = static_cast< qint64 >( dataPos.x() );

    // Count visible metrics with data + 1 for the timestamp label
    int totalLabels = 1;
    for ( const auto &[key, info] : m_seriesMap )
    {
        if ( info.series->isVisible() && !info.buffer.isEmpty() )
            ++totalLabels;
    }

    int labelIndex = 0;

    for ( const auto &[key, info] : m_seriesMap )
    {
        if ( !info.series->isVisible() || info.buffer.isEmpty() )
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

        const double rawVal = buf[ bestIdx ].y();

        const QString lbl = info.unit.isEmpty()
            ? QStringLiteral( "%1: %2" ).arg( info.label ).arg( rawVal, 0, 'f', 1 )
            : QStringLiteral( "%1: %2 %3" ).arg( info.label ).arg( rawVal, 0, 'f', 1 ).arg( info.unit );

        auto *bg   = new QGraphicsRectItem( m_chart );
        auto *text = new QGraphicsSimpleTextItem( m_chart );

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
        auto *bg   = new QGraphicsRectItem( m_chart );
        auto *text = new QGraphicsSimpleTextItem( m_chart );

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

// ---------------------------------------------------------------------------
// Zoom
// ---------------------------------------------------------------------------

void MonitorTab::applyZoomRect( const QRect &viewportRect )
{
    if ( !m_chart || !m_xAxis || !m_yAxis )
        return;

    const QPointF topLeft = m_chartView->mapToScene( viewportRect.topLeft() );
    const QPointF bottomRight = m_chartView->mapToScene( viewportRect.bottomRight() );
    const QRectF plotArea = m_chart->plotArea();

    const QPointF clampedTL(
        std::max( topLeft.x(), plotArea.left() ),
        std::max( topLeft.y(), plotArea.top() ) );
    const QPointF clampedBR(
        std::min( bottomRight.x(), plotArea.right() ),
        std::min( bottomRight.y(), plotArea.bottom() ) );

    const QPointF dataMin = m_chart->mapToValue( clampedTL );
    const QPointF dataMax = m_chart->mapToValue( clampedBR );

    const qreal yLo = std::min( dataMin.y(), dataMax.y() );
    const qreal yHi = std::max( dataMin.y(), dataMax.y() );
    const qint64 tLo = static_cast< qint64 >( std::min( dataMin.x(), dataMax.x() ) );
    const qint64 tHi = static_cast< qint64 >( std::max( dataMin.x(), dataMax.x() ) );

    m_paused = true;
    if ( m_pauseLabel )
        m_pauseLabel->setVisible( true );

    m_xAxis->setRange(
        QDateTime::fromMSecsSinceEpoch( tLo ),
        QDateTime::fromMSecsSinceEpoch( tHi ) );
    m_yAxis->setRange( yLo, yHi );

    m_zoomed = true;
}

void MonitorTab::resetZoom()
{
    m_zoomed = false;
    updateYRange();
}

// ---------------------------------------------------------------------------
// Settings persistence
// ---------------------------------------------------------------------------

void MonitorTab::saveSourceSelection()
{
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    settings.beginGroup( "MonitorTab" );

    // Save as a list of key|checked pairs
    QStringList entries;
    for ( const auto &row : m_sourceRows )
    {
        if ( !row.activeKey.empty() )
        {
            const QString entry = QStringLiteral( "%1|%2" )
                .arg( QString::fromStdString( row.activeKey ) )
                .arg( row.checkbox && row.checkbox->isChecked() ? "1" : "0" );
            entries.append( entry );
        }
    }
    settings.setValue( "SelectedSources", entries );
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
        const auto parts = entry.split( '|' );
        if ( parts.size() >= 1 )
        {
            const std::string key = parts[0].toStdString();
            const bool checked = ( parts.size() >= 2 ) ? ( parts[1] == "1" ) : true;
            addSourceRow( key );
            if ( !m_sourceRows.empty() && m_sourceRows.back().checkbox )
                m_sourceRows.back().checkbox->setChecked( checked );
        }
    }

    settings.endGroup();
}

} // namespace ucc
