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

#include "DashboardTab.hpp"
#include "SystemMonitor.hpp"
#include "ProfileManager.hpp"
#include <QDBusInterface>
#include <QDBusReply>
#include <QTimer>

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QFrame>
#include <QPalette>
#include <QColor>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <functional>
#include "CommonTypes.hpp"

namespace
{

// Fires a callback whenever the watched widget is resized, and (deferred) when
// it is shown — so caption overlays are repositioned even when a hidden panel
// becomes visible without a width change (e.g. the water-cooler panel on connect).
class ResizeFilter : public QObject
{
public:
  explicit ResizeFilter( QWidget *watched, std::function<void()> cb, QObject *parent = nullptr )
    : QObject( parent ), m_watched( watched ), m_cb( std::move( cb ) )
  {
    watched->installEventFilter( this );
  }
protected:
  bool eventFilter( QObject *, QEvent *ev ) override
  {
    if ( ev->type() == QEvent::Resize )
      m_cb();
    else if ( ev->type() == QEvent::Show )
      // Defer until the layout has settled; tie to m_watched so the call is
      // auto-cancelled if the widget is destroyed first.
      QTimer::singleShot( 0, m_watched, m_cb );
    return false;
  }
private:
  QWidget *m_watched;
  std::function<void()> m_cb;
};

QString formatFanSpeed( const QString &fanSpeed )
{
  QString display = "---";

  if ( fanSpeed.endsWith( " %" ) || fanSpeed.endsWith( "%" ) )
  {
    QString num = fanSpeed;
    if ( fanSpeed.endsWith( " %" ) )
      num = fanSpeed.left( fanSpeed.size() - 2 ).trimmed();
    else
      num = fanSpeed.left( fanSpeed.size() - 1 ).trimmed();

    bool ok = false;
    int pct = num.toInt( &ok );
    if ( ok && pct >= 0 )
      display = QString::number( pct );
  }
  else if ( fanSpeed.endsWith( " RPM" ) )
  {
    QString rpmStr = fanSpeed.left( fanSpeed.size() - 4 ).trimmed();
    bool ok = false;
    int rpm = rpmStr.toInt( &ok );

    if ( ok && rpm > 0 )
    {
      int pct = rpm / 60;
      display = QString::number( pct );
    }
  }

  return display;
}

}


namespace ucc
{

DashboardTab::DashboardTab( SystemMonitor *systemMonitor, ProfileManager *profileManager, bool waterCoolerSupported,
                            const QString &laptopModel, const QString &cpuModel,
                            const QString &dGpuModel, const QString &iGpuModel,
                            const QString &ramSummary, const QString &ramModules,
                            QWidget *parent )
  : QWidget( parent )
  , m_systemMonitor( systemMonitor )
  , m_profileManager( profileManager )
  , m_waterCoolerSupported( waterCoolerSupported )
  , m_laptopModel( laptopModel )
  , m_cpuModel( cpuModel )
  , m_dGpuModel( dGpuModel )
  , m_iGpuModel( iGpuModel )
  , m_ramSummary( ramSummary )
  , m_ramModules( ramModules )
{
  setupUI();
  connectSignals();

  // m_activeProfileLabel is created but hidden; it's only for internal use

  // Initialize water cooler status polling only if supported
  if ( m_waterCoolerSupported )
  {
    m_waterCoolerDbus = new QDBusInterface(QStringLiteral("com.uniwill.uccd"), QStringLiteral("/com/uniwill/uccd"), QStringLiteral("com.uniwill.uccd"), QDBusConnection::systemBus(), this);
    m_waterCoolerPollTimer = new QTimer(this);
    connect(m_waterCoolerPollTimer, &QTimer::timeout, this, &DashboardTab::updateWaterCoolerStatus);
    m_waterCoolerPollTimer->start(1000);
    updateWaterCoolerStatus();
  }
}

void DashboardTab::setupUI()
{
  // Do not force a static background or text color here; allow the application's
  // palette/theme to control colors so the UI remains readable in all themes.
  QVBoxLayout *layout = new QVBoxLayout( this );
  layout->setContentsMargins( 20, 20, 20, 20 );
  layout->setSpacing( 20 );

  // Resolve palette colors once and reuse as explicit hex values so the
  // applied styles have consistent contrast in both light and dark themes.
  QPalette pal = this->palette();
  const QString textHex = pal.color(QPalette::WindowText).name();
  const QString midHex = pal.color(QPalette::Mid).name();
  const QString highlightHex = pal.color(QPalette::Highlight).name();
  const QColor windowBg = pal.color(QPalette::Window);
  // Choose a high-contrast inner text color based on window background
  const QString innerTextHex = (windowBg.value() < 128) ? QString("#ffffff") : QString("#000000");
  m_ringColorHex = QString("#d32f2f");  // Red for disconnected state and other alerts

  // Title — use laptop model from daemon if available
  // Use a grid so the title is centered over the full row width while
  // the checkbox floats to the right edge, both occupying the same cell.
  QGridLayout *titleLayout = new QGridLayout();
  const QString titleText = m_laptopModel.isEmpty() ? QStringLiteral( "System Monitor" ) : m_laptopModel;
  QLabel *titleLabel = new QLabel( titleText );
  titleLabel->setStyleSheet( QString("font-size: 22px; font-weight: bold;") );

  // Water Cooler Enable toggle button (synced with FanControlTab)
  m_waterCoolerEnableCheckBox = new QPushButton( "Water Cooler" );
  m_waterCoolerEnableCheckBox->setCheckable( true );
  m_waterCoolerEnableCheckBox->setChecked( ucc::WATER_COOLER_INITIAL_STATE );
  m_waterCoolerEnableCheckBox->setToolTip( tr( "When enabled the daemon will scan for water cooler devices" ) );
  m_waterCoolerEnableCheckBox->setFixedHeight( 24 );

  {
    const QString enabledColorHex = QStringLiteral("#4caf50"); // green
    const QString disabledColorHex = QStringLiteral("#d32f2f"); // red
    m_waterCoolerEnableCheckBox->setStyleSheet(
      QString("QPushButton { font-size: 11px; padding: 2px 16px; border: 1px solid %1; border-radius: 4px; background-color: %2; }"
              "QPushButton:checked { background-color: %3; font-weight: bold; padding: 2px 12px; }")
        .arg(midHex, disabledColorHex, enabledColorHex) );
  }

  // Hide water cooler checkbox if water cooler not supported
  if ( !m_waterCoolerSupported )
    m_waterCoolerEnableCheckBox->setVisible( false );

  // Both widgets share the same cell — title centered, checkbox right-aligned
  titleLayout->addWidget( titleLabel,                  0, 0, Qt::AlignCenter );
  titleLayout->addWidget( m_waterCoolerEnableCheckBox, 0, 0, Qt::AlignRight | Qt::AlignVCenter );
  layout->addLayout( titleLayout );

  // Active Profile label (created but not shown; only used in status bar)
  m_activeProfileLabel = new QLabel( "Loading..." );
  m_activeProfileLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(textHex) );
  m_activeProfileLabel->setVisible( false );

  // Water Cooler Status label (created but not shown; only used in status bar)
  m_waterCoolerStatusLabel = new QLabel( "Disconnected" );
  m_waterCoolerStatusLabel->setStyleSheet( QString("font-weight: bold; color: %1;").arg(m_ringColorHex) );
  m_waterCoolerStatusLabel->setVisible( false );

  // makeCard: compact value-only cell — units are shown in caption badges
  auto makeCard = [&]( QLabel *&valueLabel ) -> QWidget * {
    QWidget *card = new QWidget();
    QVBoxLayout *vl = new QVBoxLayout( card );
    vl->setContentsMargins( 16, 6, 16, 6 );
    vl->setSpacing( 0 );
    vl->setAlignment( Qt::AlignCenter );

    valueLabel = new QLabel( "--" );
    valueLabel->setStyleSheet( QString("font-size: 26px; font-weight: bold; color: %1; background: transparent; border: none;").arg(innerTextHex) );
    valueLabel->setAlignment( Qt::AlignCenter );

    vl->addWidget( valueLabel, 0, Qt::AlignCenter );
    return card;
  };

  const int capOverlap = 9;
  const QString panelBorderStyle = QString("QFrame#metricPanel { border: 2px solid %1; border-radius: 6px; background: transparent; }"
                                           "QWidget { background: transparent; }").arg(m_ringColorHex);

  // makeCaptionBadge: small label with black background, sits ON the panel border
  auto makeCaptionBadge = [&]( const QString &text ) -> QLabel * {
    QLabel *label = new QLabel( text );
    label->setStyleSheet( QString("font-size: 11px; font-weight: 600; color: #f2f2f2; "
                                  "background-color: #000000; border: none; border-radius: 4px; "
                                  "padding: 1px 6px;") );
    label->setAlignment( Qt::AlignCenter );
    return label;
  };

  // makeCaptionRow: horizontal row of caption badges aligned to match card columns
  auto makeCaptionRow = [&]( std::initializer_list<QLabel *> captions ) -> QWidget * {
    QWidget *row = new QWidget();
    row->setAttribute( Qt::WA_TransparentForMouseEvents );
    QHBoxLayout *hl = new QHBoxLayout( row );
    hl->setContentsMargins( 2, 0, 2, 0 );
    hl->setSpacing( 0 );
    bool first = true;
    for ( QLabel *cap : captions )
    {
      if ( !first )
      {
        auto *spacer = new QWidget();
        spacer->setFixedWidth( 1 );
        hl->addWidget( spacer );
      }
      auto *cell = new QWidget();
      auto *ch = new QHBoxLayout( cell );
      ch->setContentsMargins( 0, 0, 0, 0 );
      ch->setAlignment( Qt::AlignCenter );
      ch->addWidget( cap );
      hl->addWidget( cell, 1 );
      first = false;
    }
    return row;
  };

  // makePanelWithCaps: wraps content in a bordered QFrame with caption overlays as siblings.
  // topCaps sits ON the top border, bottomCaps sits ON the bottom border.
  auto makePanelWithCaps = [&]( QWidget *content, QWidget *topCaps, QWidget *bottomCaps = nullptr ) -> QWidget * {
    QWidget *container = new QWidget();
    QVBoxLayout *containerLayout = new QVBoxLayout( container );
    containerLayout->setContentsMargins( 0, topCaps ? capOverlap : 0, 0, bottomCaps ? capOverlap : 0 );
    containerLayout->setSpacing( 0 );

    QFrame *panel = new QFrame();
    panel->setObjectName( "metricPanel" );
    panel->setStyleSheet( panelBorderStyle );
    QVBoxLayout *panelLayout = new QVBoxLayout( panel );
    panelLayout->setContentsMargins( 0, 0, 0, 0 );
    panelLayout->setSpacing( 0 );
    panelLayout->addWidget( content );
    containerLayout->addWidget( panel );

    if ( topCaps )
    {
      topCaps->setParent( container );
      topCaps->raise();
    }
    if ( bottomCaps )
    {
      bottomCaps->setParent( container );
      bottomCaps->raise();
    }

    new ResizeFilter( container, [container, topCaps, bottomCaps]() {
      if ( topCaps )
      {
        topCaps->setFixedWidth( container->width() );
        topCaps->adjustSize();
        int capH = topCaps->sizeHint().height();
        topCaps->move( 0, capOverlap - capH / 2 );
        topCaps->raise();
      }
      if ( bottomCaps )
      {
        bottomCaps->setFixedWidth( container->width() );
        bottomCaps->adjustSize();
        int capH = bottomCaps->sizeHint().height();
        bottomCaps->move( 0, container->height() - capOverlap - capH / 2 );
        bottomCaps->raise();
      }
    }, container );

    return container;
  };

  // makeCardRow: horizontal row of cards with dashed vertical dividers, no outer border
  auto makeCardRow = [&]( std::initializer_list<QWidget *> cards ) -> QWidget * {
    QWidget *row = new QWidget();
    QHBoxLayout *hl = new QHBoxLayout( row );
    hl->setContentsMargins( 0, 0, 0, 0 );
    hl->setSpacing( 0 );
    bool first = true;
    for ( QWidget *card : cards )
    {
      if ( !first )
      {
        QFrame *sep = new QFrame();
        sep->setFrameShape( QFrame::VLine );
        sep->setFixedWidth( 1 );
        sep->setStyleSheet( QString("QFrame { border: none; border-left: 2px dashed %1; background: transparent; }").arg(m_ringColorHex) );
        hl->addWidget( sep );
      }
      hl->addWidget( card, 1 );
      first = false;
    }
    return row;
  };

  // CPU section
  const QString cpuHeaderText = m_cpuModel.isEmpty() ? QStringLiteral( "Main Processor Monitor" ) : m_cpuModel;
  QLabel *cpuHeader = new QLabel( cpuHeaderText );
  cpuHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  cpuHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( cpuHeader );

  // CPU panel with a DRAM details subsection under a dashed separator
  {
    QWidget *cpuContent = new QWidget();
    QVBoxLayout *cpuContentLayout = new QVBoxLayout( cpuContent );
    cpuContentLayout->setContentsMargins( 0, 0, 0, 0 );
    cpuContentLayout->setSpacing( 0 );

    cpuContentLayout->addWidget( makeCardRow({
      makeCard( m_cpuTempLabel ),
      makeCard( m_fanSpeedLabel ),
      makeCard( m_cpuFrequencyLabel ),
      makeCard( m_cpuPowerLabel )
    }) );

    QFrame *ramSep = new QFrame();
    ramSep->setFrameShape( QFrame::HLine );
    ramSep->setFixedHeight( 2 );
    ramSep->setStyleSheet( QString("QFrame { border: none; border-top: 2px dashed %1; background: transparent; margin: 0px 8px; }").arg(m_ringColorHex) );
    cpuContentLayout->addWidget( ramSep );

    QWidget *ramInfo = new QWidget();
    QVBoxLayout *ramInfoLayout = new QVBoxLayout( ramInfo );
    ramInfoLayout->setContentsMargins( 12, 8, 12, 10 );
    ramInfoLayout->setSpacing( 6 );

    m_ramSummaryLabel = new QLabel( m_ramSummary.isEmpty() ? QStringLiteral( "DRAM summary unavailable" ) : m_ramSummary );
    m_ramSummaryLabel->setStyleSheet( QString( "font-size: 14px; font-weight: bold; color: %1;" ).arg( textHex ) );
    m_ramSummaryLabel->setAlignment( Qt::AlignCenter );
    m_ramSummaryLabel->setWordWrap( true );
    ramInfoLayout->addWidget( m_ramSummaryLabel );

    m_ramModulesLabel = new QLabel( m_ramModules );
    m_ramModulesLabel->setStyleSheet( QString( "font-size: 13px; color: %1;" ).arg( textHex ) );
    m_ramModulesLabel->setAlignment( Qt::AlignCenter );
    m_ramModulesLabel->setWordWrap( true );
    m_ramModulesLabel->setTextInteractionFlags( Qt::TextSelectableByMouse );
    m_ramModulesLabel->setVisible( !m_ramModules.isEmpty() );
    ramInfoLayout->addWidget( m_ramModulesLabel );

    cpuContentLayout->addWidget( ramInfo );

    layout->addWidget( makePanelWithCaps(
      cpuContent,
      makeCaptionRow({ makeCaptionBadge("Temperature (°C)"), makeCaptionBadge("Fan (%)"), makeCaptionBadge("Frequency (GHz)"), makeCaptionBadge("Power (W)") }),
      makeCaptionRow({ makeCaptionBadge("Memory") })
    ) );
  }

  // GPU section — single section with toggle between dGPU and iGPU
  // Initial GPU header text: prefer dGPU model, fall back to iGPU model
  const QString gpuHeaderText = !m_dGpuModel.isEmpty() ? m_dGpuModel
                               : !m_iGpuModel.isEmpty() ? m_iGpuModel
                               : QStringLiteral( "Graphics Card Monitor" );
  m_gpuHeaderLabel = new QLabel( gpuHeaderText );
  m_gpuHeaderLabel->setStyleSheet( "font-size: 14px; font-weight: bold;" );

  m_gpuToggleButton = new QPushButton( "Show iGPU" );
  m_gpuToggleButton->setFixedHeight( 24 );
  m_gpuToggleButton->setStyleSheet(
    QString("QPushButton { font-size: 11px; padding: 2px 12px; border: 1px solid %1; border-radius: 4px; }"
            "QPushButton:hover { background-color: %2; }").arg(midHex, highlightHex) );
  m_gpuToggleButton->setVisible( false );  // Hidden until both GPUs detected

  // Same grid trick as title row: both share cell (0,0) — label centered, button right-aligned
  QGridLayout *gpuHeaderLayout = new QGridLayout();
  gpuHeaderLayout->setContentsMargins( 0, 0, 0, 0 );
  gpuHeaderLayout->addWidget( m_gpuHeaderLabel,  0, 0, Qt::AlignCenter );
  gpuHeaderLayout->addWidget( m_gpuToggleButton, 0, 0, Qt::AlignRight | Qt::AlignVCenter );
  layout->addLayout( gpuHeaderLayout );

  // dGPU panel (default view) — two-row box: main metrics + NVIDIA extended row (hidden until data)
  m_dGpuGaugeContainer = new QWidget();
  {
    QVBoxLayout *outerVL = new QVBoxLayout( m_dGpuGaugeContainer );
    outerVL->setContentsMargins( 0, capOverlap, 0, capOverlap );
    outerVL->setSpacing( 0 );

    QFrame *panel = new QFrame();
    panel->setObjectName( "metricPanel" );
    panel->setStyleSheet( panelBorderStyle );

    QVBoxLayout *panelVL = new QVBoxLayout( panel );
    panelVL->setContentsMargins( 0, 0, 0, 0 );
    panelVL->setSpacing( 0 );

    // Row 1: primary dGPU metrics
    panelVL->addWidget( makeCardRow({
      makeCard( m_gpuTempLabel ),
      makeCard( m_gpuFanSpeedLabel ),
      makeCard( m_gpuFrequencyLabel ),
      makeCard( m_gpuPowerLabel )
    }) );

    // Horizontal dashed separator (only visible when row 2 is shown)
    m_dGpuExtraHSep = new QFrame();
    m_dGpuExtraHSep->setFrameShape( QFrame::HLine );
    m_dGpuExtraHSep->setFixedHeight( 2 );
    m_dGpuExtraHSep->setStyleSheet( QString("QFrame { border: none; border-top: 2px dashed %1; background: transparent; margin: 0px 8px; }").arg(m_ringColorHex) );
    m_dGpuExtraHSep->setVisible( false );
    panelVL->addWidget( m_dGpuExtraHSep );

    // Row 2: NVIDIA extended metrics — hidden until data arrives
    m_dGpuExtraRow = makeCardRow({
      makeCard( m_gpuComputeUtilLabel ),
      makeCard( m_gpuMemoryUtilLabel ),
      makeCard( m_gpuPstateLabel ),
      makeCard( m_gpuClockOffsetLabel )
    });
    m_gpuClockOffsetLabel->setStyleSheet( QString("font-size: 18px; font-weight: bold; color: %1; background: transparent; border: none;").arg(innerTextHex) );
    m_dGpuExtraRow->setVisible( false );
    panelVL->addWidget( m_dGpuExtraRow );

    outerVL->addWidget( panel );

    // Top caption overlay — ON top border (labels for row 1)
    QWidget *dGpuTopCaps = makeCaptionRow({
      makeCaptionBadge( "Temperature (°C)" ),
      makeCaptionBadge( "Fan (%)" ),
      makeCaptionBadge( "Core Frequency (GHz)" ),
      makeCaptionBadge( "Power (W)" )
    });
    dGpuTopCaps->setParent( m_dGpuGaugeContainer );
    dGpuTopCaps->raise();

    // Bottom caption overlay — ON bottom border (labels for row 2, hidden until row 2 visible)
    m_dGpuBottomCaps = makeCaptionRow({
      makeCaptionBadge( "GPU Load (%)" ),
      makeCaptionBadge( "VRAM Bus Load (%)" ),
      makeCaptionBadge( "P-State" ),
      makeCaptionBadge( "Clock Offset (MHz)" )
    });
    m_dGpuBottomCaps->setParent( m_dGpuGaugeContainer );
    m_dGpuBottomCaps->setVisible( false );

    new ResizeFilter( m_dGpuGaugeContainer, [this, dGpuTopCaps]() {
      auto *c = m_dGpuGaugeContainer;
      dGpuTopCaps->setFixedWidth( c->width() );
      dGpuTopCaps->adjustSize();
      int capH = dGpuTopCaps->sizeHint().height();
      dGpuTopCaps->move( 0, capOverlap - capH / 2 );
      dGpuTopCaps->raise();

      if ( m_dGpuBottomCaps && m_dGpuBottomCaps->isVisible() )
      {
        m_dGpuBottomCaps->setFixedWidth( c->width() );
        m_dGpuBottomCaps->adjustSize();
        int bCapH = m_dGpuBottomCaps->sizeHint().height();
        m_dGpuBottomCaps->move( 0, c->height() - capOverlap - bCapH / 2 );
        m_dGpuBottomCaps->raise();
      }
    }, m_dGpuGaugeContainer );
  }
  layout->addWidget( m_dGpuGaugeContainer );

  // iGPU panel (hidden by default)
  m_iGpuGaugeContainer = new QWidget();
  {
    QVBoxLayout *vl = new QVBoxLayout( m_iGpuGaugeContainer );
    vl->setContentsMargins( 0, 0, 0, 0 );
    vl->setSpacing( 0 );
    vl->addWidget( makePanelWithCaps(
      makeCardRow({
        makeCard( m_iGpuTempLabel ),
        makeCard( m_iGpuFanSpeedLabel ),
        makeCard( m_iGpuFrequencyLabel ),
        makeCard( m_iGpuPowerLabel )
      }),
      makeCaptionRow({ makeCaptionBadge("Temperature (°C)"), makeCaptionBadge("Fan (%)"), makeCaptionBadge("Frequency (GHz)"), makeCaptionBadge("Power (W)") })
    ) );
  }
  m_iGpuGaugeContainer->setVisible( false );
  layout->addWidget( m_iGpuGaugeContainer );

  // Water cooler section (restyled to match the metric panels)
  m_waterCoolerHeader = new QLabel( "Water Cooler Monitor" );
  m_waterCoolerHeader->setStyleSheet( "font-size: 14px; font-weight: bold;" );
  m_waterCoolerHeader->setAlignment( Qt::AlignCenter );
  layout->addWidget( m_waterCoolerHeader );

  m_waterCoolerGrid = new QGridLayout();
  m_waterCoolerGrid->setContentsMargins( 0, 0, 0, 0 );
  m_waterCoolerGrid->addWidget( makePanelWithCaps(
    makeCardRow({
      makeCard( m_waterCoolerFanSpeedLabel ),
      makeCard( m_waterCoolerPumpLabel )
    }),
    makeCaptionRow({ makeCaptionBadge("Fan (%)"), makeCaptionBadge("Pump (Level)") })
  ), 0, 0 );
  layout->addLayout( m_waterCoolerGrid );

  // Hide water cooler monitor section if water cooler not supported
  if ( !m_waterCoolerSupported )
  {
    m_waterCoolerHeader->setVisible( false );
    for ( int i = 0; i < m_waterCoolerGrid->count(); ++i )
    {
      if ( auto *w = m_waterCoolerGrid->itemAt( i )->widget() )
        w->setVisible( false );
    }
  }

  layout->addStretch();
}

void DashboardTab::connectSignals()
{
  connect( m_systemMonitor, &SystemMonitor::cpuTempChanged,
           this, &DashboardTab::onCpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::cpuFrequencyChanged,
           this, &DashboardTab::onCpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::cpuPowerChanged,
           this, &DashboardTab::onCpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::ramUsageChanged,
           this, &DashboardTab::onRamUsageChanged );
  connect( m_systemMonitor, &SystemMonitor::dramTemperaturesChanged,
           this, &DashboardTab::onDramTemperaturesChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuTempChanged,
           this, &DashboardTab::onGpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuFrequencyChanged,
           this, &DashboardTab::onGpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuPowerChanged,
           this, &DashboardTab::onGpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::iGpuFrequencyChanged,
           this, &DashboardTab::onIGpuFrequencyChanged );
  connect( m_systemMonitor, &SystemMonitor::iGpuPowerChanged,
           this, &DashboardTab::onIGpuPowerChanged );
  connect( m_systemMonitor, &SystemMonitor::iGpuTempChanged,
           this, &DashboardTab::onIGpuTempChanged );
  connect( m_systemMonitor, &SystemMonitor::fanSpeedChanged,
           this, &DashboardTab::onFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::gpuFanSpeedChanged,
           this, &DashboardTab::onGpuFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuComputeUtilChanged,
           this, &DashboardTab::onDGpuComputeUtilChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuMemoryUtilChanged,
           this, &DashboardTab::onDGpuMemoryUtilChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuPstateChanged,
           this, &DashboardTab::onDGpuPstateChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuGrClockOffsetChanged,
           this, &DashboardTab::onDGpuClockOffsetsChanged );
  connect( m_systemMonitor, &SystemMonitor::dGpuMemClockOffsetChanged,
           this, &DashboardTab::onDGpuClockOffsetsChanged );
  connect( m_systemMonitor, &SystemMonitor::waterCoolerFanSpeedChanged,
           this, &DashboardTab::onWaterCoolerFanSpeedChanged );
  connect( m_systemMonitor, &SystemMonitor::waterCoolerPumpLevelChanged,
           this, &DashboardTab::onWaterCoolerPumpLevelChanged );

  // Connect to profile manager for active profile changes
  connect( m_profileManager, &ProfileManager::activeProfileIndexChanged,
           this, [this]() {
             m_activeProfileLabel->setText( m_profileManager->activeProfileName() );
           } );

  Q_UNUSED(m_waterCoolerDbus)

  // Water cooler enable toggle button -> emit signal for cross-tab sync and update status
  connect( m_waterCoolerEnableCheckBox, &QPushButton::toggled,
           this, [this]() {
             updateWaterCoolerStatus();
             emit waterCoolerEnableChanged( m_waterCoolerEnableCheckBox->isChecked() );
           } );

  // GPU toggle button switches between dGPU and iGPU views
  connect( m_gpuToggleButton, &QPushButton::clicked, this, [this]() {
    switchGpuView( !m_showingIGpu );
  } );
}

void DashboardTab::updateWaterCoolerStatus()
{
  if ( not m_waterCoolerDbus || not m_waterCoolerHeader )
    return;

  auto setWCStatus = [ this ]( const bool connected )
  {
    for ( int i = 0; i < m_waterCoolerGrid->count(); ++i )
    {
      if ( QWidget *w = m_waterCoolerGrid->itemAt( i )->widget() )
        w->setVisible( connected );
    }

    m_waterCoolerHeader->setVisible( connected );
  };

  // Check if water cooler is enabled
  bool wcEnabled = m_waterCoolerEnableCheckBox ? m_waterCoolerEnableCheckBox->isChecked() : false;

  // Get water cooler state from daemon
  // Note: GetWaterCoolerAvailable returns true when scanning is active (not when a device is found)
  // GetWaterCoolerConnected returns true only when a device is actually connected
  QDBusReply<bool> scanning = m_waterCoolerDbus->call(QStringLiteral("GetWaterCoolerAvailable"));
  QDBusReply<bool> connected = m_waterCoolerDbus->call(QStringLiteral("GetWaterCoolerConnected"));

  // Compute explicit hex colors from the current palette so styles are consistent.
  QPalette pal = this->palette();
  const QString highlightHex = pal.color(QPalette::Highlight).name();
  const QString searchingColorHex = QStringLiteral("#0066cc");  // Dark blue for searching

  // Helper: emit status bar signal (dashboard label is hidden).
  auto emitStatus = [this]( const QString &statusText, const QString &colorHex )
  {
    emit waterCoolerStatusChanged(
      QString("<span style='color: %1;'>&#9679;</span> Water Cooler: %2").arg( colorHex, statusText ) );
  };

  // Status progression: Disabled -> Disconnected -> Searching -> Connected
  if ( !wcEnabled )
  {
    emitStatus( QStringLiteral("Disabled"), m_ringColorHex );
    setWCStatus( false );
  }
  else if ( connected.isValid() && connected.value() )
  {
    emitStatus( QStringLiteral("Connected"), highlightHex );
    setWCStatus( true );
  }
  else if ( scanning.isValid() && scanning.value() )
  {
    // GetWaterCoolerAvailable == true means the daemon is actively scanning
    emitStatus( QStringLiteral("Searching..."), searchingColorHex );
    setWCStatus( false );
  }
  else
  {
    emitStatus( QStringLiteral("Disconnected"), m_ringColorHex );
    setWCStatus( false );
  }
}

void DashboardTab::refreshWaterCoolerStatus()
{
  updateWaterCoolerStatus();
}

// Dashboard slots
void DashboardTab::onCpuTempChanged()
{
  QString temp = m_systemMonitor->cpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_cpuTempLabel->setText( temp );
  }
  else
  {
    m_cpuTempLabel->setText( "---" );
  }
}

void DashboardTab::onCpuFrequencyChanged()
{
  QString freq = m_systemMonitor->cpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_cpuFrequencyLabel->setText( QString::number( ghz, 'f', 1 ) );
        return;
      }
      m_cpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_cpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onCpuPowerChanged()
{
  QString power = m_systemMonitor->cpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_cpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    return;
  }
  m_cpuPowerLabel->setText( "--" );
}

void DashboardTab::onRamUsageChanged()
{
  if ( !m_ramSummaryLabel )
    return;

  const QJsonDocument doc = QJsonDocument::fromJson( m_systemMonitor->ramUsageJSON().toUtf8() );
  if ( !doc.isObject() )
    return;

  const QJsonObject obj = doc.object();
  const int totalMiB = obj.value( QStringLiteral( "totalMiB" ) ).toInt();
  const int usedMiB = obj.value( QStringLiteral( "usedMiB" ) ).toInt();
  const int availableMiB = obj.value( QStringLiteral( "availableMiB" ) ).toInt();
  if ( totalMiB <= 0 )
    return;

  const auto gib = []( const int mib ) {
    return QString::number( mib / 1024.0, 'f', 1 );
  };

  m_ramSummaryLabel->setText(
    QStringLiteral( "Total %1 GiB | Used %2 GiB | Available %3 GiB" )
      .arg( gib( totalMiB ), gib( usedMiB ), gib( availableMiB ) ) );
}

void DashboardTab::onDramTemperaturesChanged()
{
  if ( !m_ramModulesLabel )
    return;

  QMap< int, int > tempsBySlot;
  const QJsonDocument doc = QJsonDocument::fromJson( m_systemMonitor->dramTemperaturesJSON().toUtf8() );
  if ( doc.isArray() )
  {
    const QJsonArray array = doc.array();
    for ( const QJsonValue &value : array )
    {
      const QJsonObject obj = value.toObject();
      const int slot = obj.value( QStringLiteral( "slot" ) ).toInt( -1 );
      const int temp = obj.value( QStringLiteral( "temp" ) ).toInt( -1 );
      if ( slot >= 0 && temp >= 0 )
        tempsBySlot.insert( slot, temp );
    }
  }

  QString text = m_ramModules;
  if ( !tempsBySlot.isEmpty() )
  {
    QStringList entries = m_ramModules.split( QStringLiteral( " | " ), Qt::SkipEmptyParts );
    if ( !entries.isEmpty() )
    {
      const QList< int > slotKeys = tempsBySlot.keys();
      // Uniwill laptops here are 1-2 SODIMM designs where SMBIOS module order
      // and SPD i2c slot order match in practice, so fold temps into the DIMM
      // inventory instead of showing a separate sensor-only line.
      for ( int i = 0; i < entries.size() && i < slotKeys.size(); ++i )
      {
        const int temp = tempsBySlot.value( slotKeys[ i ], -1 );
        if ( temp >= 0 )
          entries[ i ] += QStringLiteral( " · %1 °C" ).arg( temp );
      }
      text = entries.join( QStringLiteral( " | " ) );
    }
  }

  if ( !text.isEmpty() )
  {
    m_ramModulesLabel->setText( text );
    m_ramModulesLabel->setVisible( true );
  }
}

void DashboardTab::onGpuTempChanged()
{
  QString temp = m_systemMonitor->gpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_gpuTempLabel->setText( temp );
    if ( !m_hasDGpuData ) { m_hasDGpuData = true; updateGpuSwitchVisibility(); }
  }
  else
  {
    m_gpuTempLabel->setText( "---" );
  }
}

void DashboardTab::onGpuFrequencyChanged()
{
  QString freq = m_systemMonitor->gpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_gpuFrequencyLabel->setText( QString::number( ghz, 'f', 1 ) );
        if ( !m_hasDGpuData ) { m_hasDGpuData = true; updateGpuSwitchVisibility(); }
        return;
      }
      m_gpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_gpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onGpuPowerChanged()
{
  QString power = m_systemMonitor->gpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_gpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    return;
  }
  m_gpuPowerLabel->setText( "--" );
}

void DashboardTab::onIGpuFrequencyChanged()
{
  QString freq = m_systemMonitor->iGpuFrequency();

  if ( freq.endsWith( " MHz" ) )
  {
    bool ok = false;
    double mhz = freq.left( freq.size() - 4 ).trimmed().toDouble( &ok );

    if ( ok )
    {
      if ( mhz > 0.0 )
      {
        double ghz = mhz / 1000.0;
        m_iGpuFrequencyLabel->setText( QString::number( ghz, 'f', 2 ) );
        if ( !m_hasIGpuData ) { m_hasIGpuData = true; updateGpuSwitchVisibility(); }
        return;
      }
      m_iGpuFrequencyLabel->setText( "--" );
      return;
    }
  }
  m_iGpuFrequencyLabel->setText( freq.isEmpty() ? "--" : freq );
}

void DashboardTab::onIGpuPowerChanged()
{
  QString power = m_systemMonitor->iGpuPower();
  QString trimmed = power.replace( " W", "" ).trimmed();
  bool ok = false;
  double watts = trimmed.toDouble( &ok );

  if ( ok && watts > 0.0 )
  {
    m_iGpuPowerLabel->setText( QString::number( watts, 'f', 1 ) );
    if ( !m_hasIGpuData ) { m_hasIGpuData = true; updateGpuSwitchVisibility(); }
    return;
  }
  m_iGpuPowerLabel->setText( "--" );
}

void DashboardTab::onIGpuTempChanged()
{
  QString temp = m_systemMonitor->iGpuTemp().replace( "°C", "" ).trimmed();
  bool ok = false;
  int tempValue = temp.toInt( &ok );

  if ( ok && tempValue > 0 )
  {
    m_iGpuTempLabel->setText( temp );
    if ( !m_hasIGpuData ) { m_hasIGpuData = true; updateGpuSwitchVisibility(); }
  }
  else
  {
    m_iGpuTempLabel->setText( "---" );
  }
}

void DashboardTab::onFanSpeedChanged()
{
  m_fanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->cpuFanSpeed() ) );
}

void DashboardTab::onGpuFanSpeedChanged()
{
  m_gpuFanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->gpuFanSpeed() ) );
}

void DashboardTab::onDGpuComputeUtilChanged()
{
  int val = m_systemMonitor->dGpuComputeUtil();
  if ( val >= 0 )
  {
    m_gpuComputeUtilLabel->setText( QString::number( val ) );
    if ( m_dGpuExtraRow && !m_dGpuExtraRow->isVisible() )
    {
      m_dGpuExtraRow->setVisible( true );
      if ( m_dGpuExtraHSep ) m_dGpuExtraHSep->setVisible( true );
      if ( m_dGpuBottomCaps )
      {
        m_dGpuBottomCaps->setVisible( true );
        // Deferred reposition: layout hasn't recalculated yet, so schedule after event loop
        QTimer::singleShot( 0, this, [this]() {
          if ( !m_dGpuGaugeContainer || !m_dGpuBottomCaps ) return;
          auto *c = m_dGpuGaugeContainer;
          m_dGpuBottomCaps->setFixedWidth( c->width() );
          m_dGpuBottomCaps->adjustSize();
          int bCapH = m_dGpuBottomCaps->sizeHint().height();
          constexpr int overlap = 9;
          m_dGpuBottomCaps->move( 0, c->height() - overlap - bCapH / 2 );
          m_dGpuBottomCaps->raise();
        });
      }
    }
  }
  else
    m_gpuComputeUtilLabel->setText( "--" );
}

void DashboardTab::onDGpuMemoryUtilChanged()
{
  int val = m_systemMonitor->dGpuMemoryUtil();
  m_gpuMemoryUtilLabel->setText( val >= 0 ? QString::number( val ) : "--" );
}

void DashboardTab::onDGpuPstateChanged()
{
  int val = m_systemMonitor->dGpuPstate();
  m_gpuPstateLabel->setText( val >= 0 ? QStringLiteral( "P" ) + QString::number( val ) : "--" );
}

void DashboardTab::onDGpuClockOffsetsChanged()
{
  int gr  = m_systemMonitor->dGpuGrClockOffset();
  int mem = m_systemMonitor->dGpuMemClockOffset();
  if ( gr > -999 || mem > -999 )
  {
    auto fmt = []( int v ) -> QString {
      return ( v >= 0 ? QStringLiteral( "+" ) : QString() ) + QString::number( v );
    };
    m_gpuClockOffsetLabel->setText(
      ( gr  > -999 ? fmt( gr )  : "--" ) + " / " +
      ( mem > -999 ? fmt( mem ) : "--" ) );
  }
  else
    m_gpuClockOffsetLabel->setText( "--" );
}

void DashboardTab::onWaterCoolerConnected()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDisconnected()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDiscoveryStarted()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerDiscoveryFinished()
{
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerConnectionError( const QString &error )
{
  Q_UNUSED( error );
  updateWaterCoolerStatus();
}

void DashboardTab::onWaterCoolerFanSpeedChanged()
{
  if ( m_waterCoolerFanSpeedLabel )
    m_waterCoolerFanSpeedLabel->setText( formatFanSpeed( m_systemMonitor->waterCoolerFanSpeed() ) );
}

void DashboardTab::onWaterCoolerPumpLevelChanged()
{
  if ( m_waterCoolerPumpLabel )
  {
    QString val = m_systemMonitor->waterCoolerPumpLevel();
    if ( val.isEmpty() )
      val = "--";
    m_waterCoolerPumpLabel->setText( val );
  }
}

void DashboardTab::setWaterCoolerEnabled( bool enabled )
{
  m_waterCoolerEnableCheckBox->blockSignals( true );
  m_waterCoolerEnableCheckBox->setChecked( enabled );
  m_waterCoolerEnableCheckBox->blockSignals( false );
}

void DashboardTab::switchGpuView( bool showIGpu )
{
  m_showingIGpu = showIGpu;
  m_dGpuGaugeContainer->setVisible( !showIGpu );
  m_iGpuGaugeContainer->setVisible( showIGpu );
  m_gpuToggleButton->setText( showIGpu ? "Show dGPU" : "Show iGPU" );

  // Update header to reflect which GPU is being shown
  if ( m_gpuHeaderLabel )
  {
    const QString headerText = showIGpu
      ? ( m_iGpuModel.isEmpty() ? QStringLiteral( "Integrated GPU" ) : m_iGpuModel )
      : ( m_dGpuModel.isEmpty() ? QStringLiteral( "Discrete GPU" )   : m_dGpuModel );
    m_gpuHeaderLabel->setText( headerText );
  }
}

void DashboardTab::updateGpuSwitchVisibility()
{
  // Show the toggle button only when both dGPU and iGPU data is available
  const bool bothAvailable = m_hasDGpuData && m_hasIGpuData;
  m_gpuToggleButton->setVisible( bothAvailable );

  // If only iGPU data exists (no dGPU), automatically show iGPU view
  if ( m_hasIGpuData && !m_hasDGpuData )
    switchGpuView( true );
}

}
