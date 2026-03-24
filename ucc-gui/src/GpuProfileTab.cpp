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

#include "GpuProfileTab.hpp"

#include <array>
#include <set>
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QMainWindow>
#include <QStatusBar>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDebug>
#include <QMessageBox>
#include <QProgressBar>
#include <QSettings>
#include <QSpinBox>
#include <algorithm>
#include <cmath>
#include <optional>

namespace ucc
{

// Local phase enums — mirror the daemon's phase strings for progress display.
enum class OCPhase  { Idle, Baseline, Searching, Validating, Done };
enum class UVPhase  { Idle, Baseline, CapReduction, Searching,
                      Validating, PowerLimitSweep, Done };

GpuProfileTab::GpuProfileTab( UccdClient *client,
                                ProfileManager *profileManager,
                                QWidget *parent )
  : QWidget( parent )
  , m_uccdClient( client )
  , m_profileManager( profileManager )
{
  // Check if NVIDIA OC is available
  if ( auto avail = m_uccdClient->getNvidiaOCAvailable() )
    m_ocAvailable = *avail;

  setupUI();
  connectSignals();
  reloadGpuProfiles();
  updateButtonStates( m_uccdClient && m_uccdClient->isConnected() );

  // Initial hardware state read
  if ( m_ocAvailable )
  {
    refreshOCState();

    m_liveMetricsTimer = new QTimer( this );
    m_liveMetricsTimer->setInterval( 1000 );
    connect( m_liveMetricsTimer, &QTimer::timeout, this, &GpuProfileTab::refreshLiveMetrics );
    m_liveMetricsTimer->start();
  }
}

// ── UI construction ─────────────────────────────────────────────────

void GpuProfileTab::setupUI()
{
  QVBoxLayout *mainLayout = new QVBoxLayout( this );
  mainLayout->setContentsMargins( 0, 0, 0, 0 );
  mainLayout->setSpacing( 0 );

  // ── Top bar: GPU profile selection ──
  QHBoxLayout *selectLayout = new QHBoxLayout();
  m_gpuProfileCombo = new QComboBox();
  m_gpuProfileCombo->setEditable( true );
  m_gpuProfileCombo->setInsertPolicy( QComboBox::NoInsert );

  m_applyButton = new QPushButton( "Apply" );
  m_applyButton->setMaximumWidth( 80 );
  m_applyButton->setEnabled( false );
  m_applyButton->setToolTip( "Applies current GPU OC settings temporarily. Use Save to persist." );

  m_saveButton = new QPushButton( "Save" );
  m_saveButton->setMaximumWidth( 80 );
  m_saveButton->setEnabled( false );

  m_copyButton = new QPushButton( "Copy" );
  m_copyButton->setMaximumWidth( 60 );
  m_copyButton->setEnabled( false );

  m_removeButton = new QPushButton( "Remove" );
  m_removeButton->setMaximumWidth( 70 );

  m_refreshButton = new QPushButton( "Refresh" );
  m_refreshButton->setMaximumWidth( 100 );
  m_refreshButton->setVisible( m_ocAvailable );

  m_resetButton = new QPushButton( "Reset" );
  m_resetButton->setMaximumWidth( 80 );
  m_resetButton->setVisible( m_ocAvailable );

  selectLayout->addWidget( m_gpuProfileCombo, 1 );
  selectLayout->addWidget( m_applyButton );
  selectLayout->addWidget( m_saveButton );
  selectLayout->addWidget( m_copyButton );
  selectLayout->addWidget( m_removeButton );
  selectLayout->addWidget( m_refreshButton );
  selectLayout->addWidget( m_resetButton );
  mainLayout->addLayout( selectLayout );

  QFrame *separator = new QFrame();
  separator->setFrameShape( QFrame::HLine );
  mainLayout->addWidget( separator );

  // ── Scroll area ──
  QScrollArea *scrollArea = new QScrollArea();
  scrollArea->setWidgetResizable( true );
  QWidget *scrollWidget = new QWidget();
  QVBoxLayout *contentLayout = new QVBoxLayout( scrollWidget );
  contentLayout->setContentsMargins( 15, 10, 15, 10 );
  contentLayout->setSpacing( 12 );

  // Not-available label (shown when NVML isn't loaded)
  m_notAvailableLabel = new QLabel(
    "<b>NVIDIA GPU OC is not available.</b><br>"
    "Ensure that an NVIDIA GPU is present and the NVIDIA driver (with NVML) is installed." );
  m_notAvailableLabel->setWordWrap( true );
  m_notAvailableLabel->setVisible( !m_ocAvailable );
  contentLayout->addWidget( m_notAvailableLabel );

  // === GPU INFO SECTION (unboxed) ===
  QWidget *infoSection = new QWidget();
  infoSection->setVisible( m_ocAvailable );
  QVBoxLayout *infoSectionLayout = new QVBoxLayout( infoSection );
  infoSectionLayout->setContentsMargins( 0, 0, 0, 0 );
  infoSectionLayout->setSpacing( 6 );

  QGridLayout *infoLayout = new QGridLayout();
  infoLayout->setHorizontalSpacing( 18 );
  infoLayout->setVerticalSpacing( 4 );
  QLabel *tempLabel = new QLabel( "Temperature:" );
  QLabel *freqLabel = new QLabel( "Core:" );
  QLabel *powerLabel = new QLabel( "Power:" );
  QLabel *pstateLabel = new QLabel( "P-State:" );

  m_gpuNameLabel = new QLabel( "—" );
  m_tempLabel = new QLabel( "—" );
  m_coreFreqLabel = new QLabel( "—" );
  m_powerDrawLabel = new QLabel( "—" );
  m_currentPstateLabel = new QLabel( "—" );
  QFont valueFont = m_gpuNameLabel->font();
  valueFont.setBold( true );
  m_gpuNameLabel->setFont( valueFont );
  m_tempLabel->setFont( valueFont );
  m_coreFreqLabel->setFont( valueFont );
  m_powerDrawLabel->setFont( valueFont );
  m_currentPstateLabel->setFont( valueFont );

  infoLayout->addWidget( m_gpuNameLabel, 0, 0 );
  infoLayout->addWidget( tempLabel, 0, 2 );
  infoLayout->addWidget( m_tempLabel, 0, 3 );
  infoLayout->addWidget( freqLabel, 0, 4 );
  infoLayout->addWidget( m_coreFreqLabel, 0, 5 );
  infoLayout->addWidget( powerLabel, 0, 6 );
  infoLayout->addWidget( m_powerDrawLabel, 0, 7 );
  infoLayout->addWidget( pstateLabel, 0, 8 );
  infoLayout->addWidget( m_currentPstateLabel, 0, 9 );
  infoLayout->setColumnStretch( 1, 1 );
  infoSectionLayout->addLayout( infoLayout );
  contentLayout->addWidget( infoSection );

  // === CLOCK OFFSETS (grouped by P-state) ===
  m_pstatesLayout = new QVBoxLayout();
  m_pstatesLayout->setSpacing( 8 );

  // === cTGP slider + Auto OC button (inline row) ===
  QWidget *powerRow = new QWidget();
  powerRow->setVisible( m_ocAvailable );
  QHBoxLayout *powerLayout = new QHBoxLayout( powerRow );
  powerLayout->setContentsMargins( 0, 0, 0, 0 );
  powerLayout->setSpacing( 8 );

  m_powerLimitLabel = new QLabel( "Power Limit" );
  QFont ctgpFont = m_powerLimitLabel->font();
  ctgpFont.setBold( true );
  m_powerLimitLabel->setFont( ctgpFont );
  powerLayout->addWidget( m_powerLimitLabel );

  m_powerLimitSlider = new QSlider( Qt::Horizontal );
  m_powerLimitSlider->setMinimum( 40 );
  m_powerLimitSlider->setMaximum( 175 );
  m_powerLimitValue = new QLabel( "0 W" );
  m_powerLimitValue->setMinimumWidth( 60 );
  powerLayout->addWidget( m_powerLimitSlider, 1 );
  powerLayout->addWidget( m_powerLimitValue );

  m_autoOCButton = new QPushButton( "Auto Overclock" );
  m_autoOCButton->setMinimumWidth( 120 );
  m_autoOCButton->setToolTip( "Automatically find the maximum stable GPU/VRAM clock offsets" );
  powerLayout->addWidget( m_autoOCButton );

  m_autoUVButton = new QPushButton( "Auto Undervolt" );
  m_autoUVButton->setMinimumWidth( 120 );
  m_autoUVButton->setToolTip( "Automatically find the lowest GPU frequency cap that sustains FPS for the running app" );
  powerLayout->addWidget( m_autoUVButton );

  contentLayout->addWidget( powerRow );

  // === GPU LOCKED CLOCKS ===
  m_gpuLockedGroup = new QGroupBox( "GPU Core Locked Clocks" );
  m_gpuLockedGroup->setVisible( m_ocAvailable );
  m_gpuLockedGroup->setCheckable( true );
  m_gpuLockedGroup->setChecked( false );
  QVBoxLayout *gpuLockedLayout = new QVBoxLayout( m_gpuLockedGroup );

  QHBoxLayout *gpuLockedRow = new QHBoxLayout();
  gpuLockedRow->addWidget( new QLabel( "Min:" ) );
  m_gpuLockedMinSlider = new QSlider( Qt::Horizontal );
  m_gpuLockedMinSpin = new QSpinBox();
  m_gpuLockedMinSpin->setSuffix( " MHz" );
  gpuLockedRow->addWidget( m_gpuLockedMinSlider, 1 );
  gpuLockedRow->addWidget( m_gpuLockedMinSpin );
  gpuLockedRow->addWidget( new QLabel( "Max:" ) );
  m_gpuLockedMaxSlider = new QSlider( Qt::Horizontal );
  m_gpuLockedMaxSpin = new QSpinBox();
  m_gpuLockedMaxSpin->setSuffix( " MHz" );
  gpuLockedRow->addWidget( m_gpuLockedMaxSlider, 1 );
  gpuLockedRow->addWidget( m_gpuLockedMaxSpin );
  gpuLockedLayout->addLayout( gpuLockedRow );

  contentLayout->addWidget( m_gpuLockedGroup );

  // === VRAM LOCKED CLOCKS ===
  m_vramLockedGroup = new QGroupBox( "VRAM Locked Clocks" );
  m_vramLockedGroup->setVisible( m_ocAvailable );
  m_vramLockedGroup->setCheckable( true );
  m_vramLockedGroup->setChecked( false );
  QVBoxLayout *vramLockedLayout = new QVBoxLayout( m_vramLockedGroup );

  QHBoxLayout *vramLockedRow = new QHBoxLayout();
  vramLockedRow->addWidget( new QLabel( "Min:" ) );
  m_vramLockedMinSlider = new QSlider( Qt::Horizontal );
  m_vramLockedMinSpin = new QSpinBox();
  m_vramLockedMinSpin->setSuffix( " MHz" );
  vramLockedRow->addWidget( m_vramLockedMinSlider, 1 );
  vramLockedRow->addWidget( m_vramLockedMinSpin );
  vramLockedRow->addWidget( new QLabel( "Max:" ) );
  m_vramLockedMaxSlider = new QSlider( Qt::Horizontal );
  m_vramLockedMaxSpin = new QSpinBox();
  m_vramLockedMaxSpin->setSuffix( " MHz" );
  vramLockedRow->addWidget( m_vramLockedMaxSlider, 1 );
  vramLockedRow->addWidget( m_vramLockedMaxSpin );
  vramLockedLayout->addLayout( vramLockedRow );

  contentLayout->addWidget( m_vramLockedGroup );

  // === CLOCK OFFSETS (grouped by P-state) ===
  // Individual P-state groups are added directly; no outer wrapper needed.
  contentLayout->addLayout( m_pstatesLayout );

  contentLayout->addStretch();
  scrollArea->setWidget( scrollWidget );
  mainLayout->addWidget( scrollArea );
}

// ── Signal wiring ───────────────────────────────────────────────────

void GpuProfileTab::connectSignals()
{
  // GPU profile combo - index-based signal
  connect( m_gpuProfileCombo, QOverload< int >::of( &QComboBox::currentIndexChanged ),
           this, [this]( int index ) {
    if ( index >= 0 )
      emit gpuProfileChanged( m_gpuProfileCombo->itemData( index ).toString() );
  } );

  // Rename handling
  if ( m_gpuProfileCombo->lineEdit() )
  {
    connect( m_gpuProfileCombo->lineEdit(), &QLineEdit::editingFinished,
             this, &GpuProfileTab::onGpuProfileComboRenamed );
  }

  // Action buttons
  connect( m_applyButton, &QPushButton::clicked, this, [this]() {
    if ( !ensureOverclockWarningAcknowledged() )
      return;
    emit applyRequested();
  } );
  connect( m_saveButton, &QPushButton::clicked, this, [this]() {
    if ( !ensureOverclockWarningAcknowledged() )
      return;
    emit saveRequested();
  } );
  connect( m_copyButton, &QPushButton::clicked, this, &GpuProfileTab::copyRequested );
  connect( m_removeButton, &QPushButton::clicked, this, &GpuProfileTab::removeRequested );
  connect( m_refreshButton, &QPushButton::clicked, this, &GpuProfileTab::onRefreshClicked );
  connect( m_resetButton, &QPushButton::clicked, this, &GpuProfileTab::onResetClicked );

  if ( m_autoOCButton )
  {
    connect( m_autoOCButton, &QPushButton::clicked, this, &GpuProfileTab::showAutoOCDialog );
  }

  if ( m_autoUVButton )
  {
    connect( m_autoUVButton, &QPushButton::clicked, this, &GpuProfileTab::showAutoUndervoltDialog );
  }

  connect( m_gpuLockedGroup, &QGroupBox::toggled, this, [this]( bool ) { emit changed(); } );
  connect( m_vramLockedGroup, &QGroupBox::toggled, this, [this]( bool ) { emit changed(); } );

  // GPU locked clocks: bidirectional slider <-> spinbox sync
  connect( m_gpuLockedMinSlider, &QSlider::valueChanged, m_gpuLockedMinSpin, &QSpinBox::setValue );
  connect( m_gpuLockedMinSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_gpuLockedMinSlider, &QSlider::setValue );
  connect( m_gpuLockedMaxSlider, &QSlider::valueChanged, m_gpuLockedMaxSpin, &QSpinBox::setValue );
  connect( m_gpuLockedMaxSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_gpuLockedMaxSlider, &QSlider::setValue );
  connect( m_gpuLockedMinSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v > m_gpuLockedMaxSlider->value() ) m_gpuLockedMaxSlider->setValue( v );
    emit changed();
  } );
  connect( m_gpuLockedMaxSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v < m_gpuLockedMinSlider->value() ) m_gpuLockedMinSlider->setValue( v );
    emit changed();
  } );

  // VRAM locked clocks: bidirectional slider <-> spinbox sync
  connect( m_vramLockedMinSlider, &QSlider::valueChanged, m_vramLockedMinSpin, &QSpinBox::setValue );
  connect( m_vramLockedMinSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_vramLockedMinSlider, &QSlider::setValue );
  connect( m_vramLockedMaxSlider, &QSlider::valueChanged, m_vramLockedMaxSpin, &QSpinBox::setValue );
  connect( m_vramLockedMaxSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_vramLockedMaxSlider, &QSlider::setValue );
  connect( m_vramLockedMinSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v > m_vramLockedMaxSlider->value() ) m_vramLockedMaxSlider->setValue( v );
    emit changed();
  } );
  connect( m_vramLockedMaxSlider, &QSlider::valueChanged, this, [this]( int v ) {
    if ( v < m_vramLockedMinSlider->value() ) m_vramLockedMinSlider->setValue( v );
    emit changed();
  } );

  // Power limit slider <-> label
  if ( m_powerLimitSlider )
  {
    connect( m_powerLimitSlider, &QSlider::valueChanged, this, [this]( int v ) {
      m_powerLimitValue->setText( QString::number( v ) + " W" );
      emit changed();
    } );
  }
}

// ── Public helpers ──────────────────────────────────────────────────

void GpuProfileTab::reloadGpuProfiles()
{
  QString prevId = m_gpuProfileCombo ? m_gpuProfileCombo->currentData().toString() : QString();
  if ( m_gpuProfileCombo )
    m_gpuProfileCombo->clear();

  for ( const auto &v : m_profileManager->gpuProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    if ( !id.isEmpty() )
      m_gpuProfileCombo->addItem( name, id );
  }

  if ( !prevId.isEmpty() )
  {
    for ( int i = 0; i < m_gpuProfileCombo->count(); ++i )
    {
      if ( m_gpuProfileCombo->itemData( i ).toString() == prevId )
      {
        m_gpuProfileCombo->setCurrentIndex( i );
        return;
      }
    }
  }
  if ( m_gpuProfileCombo->count() > 0 )
    m_gpuProfileCombo->setCurrentIndex( 0 );
}

void GpuProfileTab::updateButtonStates( bool uccdConnected )
{
  const QString id = m_gpuProfileCombo ? m_gpuProfileCombo->currentData().toString() : QString();
  const bool hasSelection = !id.isEmpty();
  const bool isBuiltin = hasSelection
                         && !m_profileManager->isProfileEditable( id, m_profileManager->gpuProfilesData() );

  if ( m_applyButton )   m_applyButton->setEnabled( uccdConnected && m_ocAvailable );
  if ( m_saveButton )    m_saveButton->setEnabled( hasSelection && !isBuiltin );
  if ( m_copyButton )    m_copyButton->setEnabled( hasSelection || m_ocAvailable );
  if ( m_removeButton )  m_removeButton->setEnabled( hasSelection && !isBuiltin );
  if ( m_resetButton )   m_resetButton->setEnabled( uccdConnected && m_ocAvailable );

  // Built-in GPU profiles are immutable: lock all editable controls.
  const bool profileEditable = hasSelection && !isBuiltin;
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setEnabled( profileEditable && m_offsetsSupported );
  }
  if ( m_gpuLockedGroup )
    m_gpuLockedGroup->setEnabled( profileEditable && m_lockedSupported );
  if ( m_vramLockedGroup )
    m_vramLockedGroup->setEnabled( profileEditable && m_lockedSupported );
  if ( m_powerLimitSlider )
    m_powerLimitSlider->setEnabled( profileEditable );

  // Allow renaming custom profiles
  if ( m_gpuProfileCombo && m_gpuProfileCombo->lineEdit() )
    m_gpuProfileCombo->lineEdit()->setReadOnly( !hasSelection || isBuiltin );
}

void GpuProfileTab::refreshOCState()
{
  if ( !m_ocAvailable || !m_uccdClient )
    return;

  auto stateOpt = m_uccdClient->getNvidiaOCState( 0 );
  if ( !stateOpt )
    return;

  qDebug() << "[GPU-CTGP] refreshOCState raw state:" << QString::fromStdString( *stateOpt );

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *stateOpt ) );
  if ( !doc.isObject() )
    return;

  QJsonObject state = doc.object();

  // Check if GPU needs reset
  if ( state.contains( "needsReset" ) && state["needsReset"].toBool() )
  {
    m_gpuNameLabel->setText( state["gpuName"].toString( "Unknown" ) +
      "  ⚠ GPU requires reset (reboot or nvidia-smi -r)" );
  }
  else
  {
    m_gpuNameLabel->setText( state["gpuName"].toString( "Unknown" ) );
  }

  // Update info labels
  refreshLiveMetrics();

  // Update power label: "cTGP" for Uniwill/Tuxedo laptops, "Power Limit" otherwise
  if ( m_powerLimitLabel )
  {
    const bool isCTGP = m_uccdClient->getCTGPAdjustmentSupported().value_or( false );
    m_powerLimitLabel->setText( isCTGP ? "cTGP" : "Power Limit" );
  }

  // Power range from NVML constraints
  m_powerMinW = state["powerMinW"].toDouble();
  m_powerMaxW = state["powerMaxW"].toDouble();
  m_powerDefaultW = state["powerDefaultW"].toDouble();

  int defaultPower = static_cast< int >( std::round( m_powerDefaultW ) );
  if ( auto v = m_uccdClient->getNVIDIAPowerCTRLDefaultPowerLimit() )
    defaultPower = *v;

  int minPower = static_cast< int >( std::round( m_powerMinW ) );
  if ( auto v = m_uccdClient->getNVIDIAPowerCTRLMinPowerLimit() )
    minPower = *v;
  if ( minPower <= 0 )
    minPower = defaultPower;

  int maxPower = static_cast< int >( std::round( m_powerMaxW ) );
  if ( auto v = m_uccdClient->getNVIDIAPowerCTRLMaxPowerLimit() )
    maxPower = *v;

  int currentOffset = 0;
  bool haveOffset = false;
  if ( auto v = m_uccdClient->getNVIDIAPowerOffset() )
  {
    currentOffset = *v;
    haveOffset = true;
  }

  int currentPowerFromState = static_cast< int >( std::round( state["powerLimitW"].toDouble( 0.0 ) ) );

  qDebug() << "[GPU-CTGP] refresh inputs"
           << "powerMinW=" << m_powerMinW
           << "powerDefaultW(state)=" << m_powerDefaultW
           << "powerMaxW(state)=" << m_powerMaxW
           << "minPower(iface)=" << minPower
           << "defaultPower(iface)=" << defaultPower
           << "maxPower(iface)=" << maxPower
           << "haveOffset=" << haveOffset
           << "offset(iface)=" << currentOffset
           << "powerLimitW(state)=" << currentPowerFromState;

  if ( minPower > 0 && maxPower >= minPower )
  {
    int currentPower = defaultPower;

    // Prefer the NVML-reported actual power limit (reflects real hardware
    // state including any cTGP offset that was just applied temporarily).
    // Fall back to profile-based offset only when NVML reports nothing.
    if ( currentPowerFromState > 0 )
      currentPower = currentPowerFromState;
    else if ( haveOffset )
      currentPower = defaultPower + currentOffset;

    currentPower = std::clamp( currentPower, minPower, maxPower );

    qDebug() << "[GPU-CTGP] refresh resolved"
             << "sliderMin=" << minPower
             << "sliderMax=" << maxPower
             << "sliderValue=" << currentPower;

    m_powerLimitSlider->blockSignals( true );
    m_powerLimitSlider->setMinimum( minPower );
    m_powerLimitSlider->setMaximum( maxPower );
    m_powerLimitSlider->setValue( currentPower );
    m_powerLimitSlider->blockSignals( false );
    m_powerLimitValue->setText( QString::number( currentPower ) + " W" );

    m_powerDefaultW = defaultPower;
    m_powerMinW = minPower;
    m_powerMaxW = maxPower;
  }

  // GPU clock range for locked clocks
  if ( state.contains( "gpuClockRange" ) )
  {
    QJsonObject r = state["gpuClockRange"].toObject();
    int lo = r["min"].toInt(), hi = r["max"].toInt();
    if ( lo > 0 && hi > lo )
    {
      m_gpuLockedMinSlider->setMinimum( lo );
      m_gpuLockedMinSlider->setMaximum( hi );
      m_gpuLockedMaxSlider->setMinimum( lo );
      m_gpuLockedMaxSlider->setMaximum( hi );
      m_gpuLockedMinSpin->setMinimum( lo );
      m_gpuLockedMinSpin->setMaximum( hi );
      m_gpuLockedMaxSpin->setMinimum( lo );
      m_gpuLockedMaxSpin->setMaximum( hi );

      // Set defaults
      m_gpuLockedMinSlider->setValue( lo );
      m_gpuLockedMaxSlider->setValue( hi );
      if ( m_gpuLockedGroup )
        m_gpuLockedGroup->setEnabled( true );
    }
  }
  else if ( m_gpuLockedGroup )
  {
    m_gpuLockedGroup->setEnabled( false );
  }

  if ( state.contains( "vramClockRange" ) )
  {
    QJsonObject r = state["vramClockRange"].toObject();
    int lo = r["min"].toInt(), hi = r["max"].toInt();
    if ( lo > 0 && hi > lo )
    {
      m_vramLockedMinSlider->setMinimum( lo );
      m_vramLockedMinSlider->setMaximum( hi );
      m_vramLockedMaxSlider->setMinimum( lo );
      m_vramLockedMaxSlider->setMaximum( hi );
      m_vramLockedMinSpin->setMinimum( lo );
      m_vramLockedMinSpin->setMaximum( hi );
      m_vramLockedMaxSpin->setMinimum( lo );
      m_vramLockedMaxSpin->setMaximum( hi );

      m_vramLockedMinSlider->setValue( lo );
      m_vramLockedMaxSlider->setValue( hi );
      if ( m_vramLockedGroup )
        m_vramLockedGroup->setEnabled( true );
    }
  }
  else if ( m_vramLockedGroup )
  {
    m_vramLockedGroup->setEnabled( false );
  }

  // Load existing locked clocks if applied
  if ( m_gpuLockedGroup )
  {
    m_gpuLockedGroup->blockSignals( true );
    m_gpuLockedGroup->setChecked( state.contains( "gpuLockedClocks" ) );
    m_gpuLockedGroup->blockSignals( false );
  }
  if ( state.contains( "gpuLockedClocks" ) )
  {
    QJsonObject lc = state["gpuLockedClocks"].toObject();
    m_gpuLockedMinSlider->setValue( lc["min"].toInt() );
    m_gpuLockedMaxSlider->setValue( lc["max"].toInt() );
  }

  if ( m_vramLockedGroup )
  {
    m_vramLockedGroup->blockSignals( true );
    m_vramLockedGroup->setChecked( state.contains( "vramLockedClocks" ) );
    m_vramLockedGroup->blockSignals( false );
  }
  if ( state.contains( "vramLockedClocks" ) )
  {
    QJsonObject lc = state["vramLockedClocks"].toObject();
    m_vramLockedMinSlider->setValue( lc["min"].toInt() );
    m_vramLockedMaxSlider->setValue( lc["max"].toInt() );
  }

  // Populate P-state offset rows – preserve checked state across refresh
  std::set< unsigned int > checkedPStates;
  for ( const auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox && grp.groupBox->isChecked() )
      checkedPStates.insert( grp.pstate );
  }

  clearPStateWidgets();
  if ( state.contains( "pstates" ) )
    populatePStates( state["pstates"].toArray() );

  // Restore checked state for P-states that were enabled before refresh
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setChecked( checkedPStates.count( grp.pstate ) > 0 );
  }

  // Feature support
  bool offsetsSupported = state["offsetsSupported"].toBool( false );
  bool lockedSupported = state["lockedClocksSupported"].toBool( false );
  m_offsetsSupported = offsetsSupported;
  m_lockedSupported = lockedSupported;

  if ( m_gpuLockedGroup )
    m_gpuLockedGroup->setEnabled( lockedSupported );
  if ( m_vramLockedGroup )
    m_vramLockedGroup->setEnabled( lockedSupported );
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setEnabled( offsetsSupported );
  }

  updateButtonStates( m_uccdClient && m_uccdClient->isConnected() );
}

void GpuProfileTab::refreshLiveMetrics()
{
  if ( !m_ocAvailable || !m_uccdClient )
    return;

  std::optional< int > tempC = m_uccdClient->getGpuTemperature();
  std::optional< int > freqMHz = m_uccdClient->getGpuFrequency();
  std::optional< double > powerW = m_uccdClient->getGpuPower();
  std::optional< int > currentPstate = m_uccdClient->getDGpuCurrentPstate();

  if ( !tempC || !powerW )
  {
    auto stateOpt = m_uccdClient->getNvidiaOCState( 0 );
    if ( stateOpt )
    {
      const QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *stateOpt ) );
      if ( doc.isObject() )
      {
        const QJsonObject state = doc.object();
        if ( !tempC && state.contains( "tempC" ) )
          tempC = state["tempC"].toInt();
        if ( !powerW && state.contains( "powerDrawW" ) )
          powerW = state["powerDrawW"].toDouble();
        if ( !currentPstate && state.contains( "currentPstate" ) )
          currentPstate = state["currentPstate"].toInt( -1 );
      }
    }
  }

  if ( m_tempLabel )
    m_tempLabel->setText( tempC ? ( QString::number( *tempC ) + " °C" ) : QStringLiteral( "—" ) );

  if ( m_coreFreqLabel )
    m_coreFreqLabel->setText( freqMHz ? ( QString::number( *freqMHz ) + " MHz" ) : QStringLiteral( "—" ) );

  if ( m_powerDrawLabel )
    m_powerDrawLabel->setText( powerW ? ( QString::number( *powerW, 'f', 1 ) + " W" ) : QStringLiteral( "—" ) );

  if ( m_currentPstateLabel )
  {
    if ( currentPstate && *currentPstate >= 0 )
      m_currentPstateLabel->setText( QStringLiteral( "P" ) + QString::number( *currentPstate ) );
    else
      m_currentPstateLabel->setText( QStringLiteral( "—" ) );
  }
}

void GpuProfileTab::populatePStates( const QJsonArray &pstates )
{
  // Typical-use descriptions for NVIDIA performance states
  static const std::array< const char *, 16 > pstateDesc = {
    "Maximum 3D performance (gaming, compute)",   // P0
    "High 3D performance",                        // P1
    "Balanced 3D performance",                    // P2
    "Mixed 3D / media playback",                  // P3
    "HD video playback",                          // P4
    "Medium performance",                         // P5
    "Low performance",                            // P6
    "Low power",                                  // P7
    "Basic desktop / idle",                       // P8
    "Very low power",                             // P9
    "Lowest GPU clocks",                          // P10
    "Standby",                                    // P11
    "Minimal power",                              // P12
    "Reserved",                                   // P13
    "Reserved",                                   // P14
    "Maximum power saving",                       // P15
  };

  for ( const auto &v : pstates )
  {
    QJsonObject ps = v.toObject();
    unsigned int pstate = static_cast< unsigned int >( ps["pstate"].toInt() );

    QString title = QString( "P-State %1" ).arg( pstate );
    if ( pstate < pstateDesc.size() )
      title += QString( " — %1" ).arg( pstateDesc[pstate] );

    QGroupBox *group = new QGroupBox( title );
    group->setCheckable( true );
    group->setChecked( false );
    QHBoxLayout *groupLayout = new QHBoxLayout( group );
    groupLayout->setSpacing( 8 );
    groupLayout->setContentsMargins( 10, 6, 10, 6 );

    PStateGroup psg{};
    psg.pstate = pstate;
    psg.groupBox = group;

    // ── GPU Core column ──
    if ( ps.contains( "gpu" ) )
    {
      QJsonObject gpu = ps["gpu"].toObject();
      int minMHz = gpu["minMHz"].toInt();
      int maxMHz = gpu["maxMHz"].toInt();
      int minOff = gpu["minOffset"].toInt( -500 );
      int maxOff = gpu["maxOffset"].toInt( 500 );
      int curOff = gpu["currentOffset"].toInt( 0 );

      QVBoxLayout *col = new QVBoxLayout();
      col->setSpacing( 2 );
      QLabel *label = new QLabel(
          minMHz == maxMHz
              ? QString( "GPU Core (%1 MHz)" ).arg( minMHz )
              : QString( "GPU Core (%1–%2 MHz)" ).arg( minMHz ).arg( maxMHz ) );

      QHBoxLayout *ctrlRow = new QHBoxLayout();
      QSlider *slider = new QSlider( Qt::Horizontal );
      slider->setMinimum( minOff ); slider->setMaximum( maxOff ); slider->setValue( curOff );
      slider->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      QSpinBox *spin = new QSpinBox();
      spin->setMinimum( minOff ); spin->setMaximum( maxOff ); spin->setValue( curOff );
      spin->setSuffix( " MHz" );
      spin->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      connect( slider, &QSlider::valueChanged, spin, &QSpinBox::setValue );
      connect( spin, QOverload< int >::of( &QSpinBox::valueChanged ), slider, &QSlider::setValue );
      connect( slider, &QSlider::valueChanged, this, [this]() { emit changed(); } );

      ctrlRow->addWidget( slider, 1 );
      ctrlRow->addWidget( spin );
      col->addWidget( label );
      col->addLayout( ctrlRow );
      groupLayout->addLayout( col, 1 );

      psg.gpuRow = { pstate, true, slider, spin };
    }

    // ── VRAM column ──
    if ( ps.contains( "vram" ) )
    {
      QJsonObject vram = ps["vram"].toObject();
      int minMHz = vram["minMHz"].toInt();
      int maxMHz = vram["maxMHz"].toInt();
      int minOff = vram["minOffset"].toInt( -500 );
      int maxOff = vram["maxOffset"].toInt( 500 );
      int curOff = vram["currentOffset"].toInt( 0 );

      QVBoxLayout *col = new QVBoxLayout();
      col->setSpacing( 2 );
      QLabel *label = new QLabel(
          minMHz == maxMHz
              ? QString( "VRAM (%1 MHz)" ).arg( minMHz )
              : QString( "VRAM (%1–%2 MHz)" ).arg( minMHz ).arg( maxMHz ) );

      QHBoxLayout *ctrlRow = new QHBoxLayout();
      QSlider *slider = new QSlider( Qt::Horizontal );
      slider->setMinimum( minOff ); slider->setMaximum( maxOff ); slider->setValue( curOff );
      slider->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      QSpinBox *spin = new QSpinBox();
      spin->setMinimum( minOff ); spin->setMaximum( maxOff ); spin->setValue( curOff );
      spin->setSuffix( " MHz" );
      spin->setToolTip( QString( "Offset range: %1 .. %2 MHz" ).arg( minOff ).arg( maxOff ) );

      connect( slider, &QSlider::valueChanged, spin, &QSpinBox::setValue );
      connect( spin, QOverload< int >::of( &QSpinBox::valueChanged ), slider, &QSlider::setValue );
      connect( slider, &QSlider::valueChanged, this, [this]() { emit changed(); } );

      ctrlRow->addWidget( slider, 1 );
      ctrlRow->addWidget( spin );
      col->addWidget( label );
      col->addLayout( ctrlRow );
      groupLayout->addLayout( col, 1 );

      psg.vramRow = { pstate, false, slider, spin };
    }

    m_pstatesLayout->addWidget( group );
    connect( group, &QGroupBox::toggled, this, [this]( bool ) { emit changed(); } );
    m_pstateGroups.push_back( psg );
  }
}

void GpuProfileTab::clearPStateWidgets()
{
  // Remove all P-state group boxes
  for ( auto &grp : m_pstateGroups )
    delete grp.groupBox;   // deletes children (sliders, spinboxes, labels) automatically
  m_pstateGroups.clear();
}

QString GpuProfileTab::buildProfileJSON() const
{
  QJsonObject root;

  // Clock offsets grouped by P-state
  {
    QJsonArray offsets;
    for ( const auto &grp : m_pstateGroups )
    {
      if ( grp.groupBox && !grp.groupBox->isChecked() )
        continue;

      QJsonObject o;
      o["pstate"] = static_cast< int >( grp.pstate );
      if ( grp.gpuRow.slider )
      {
        o["gpuOffsetMHz"] = grp.gpuRow.slider->value();
      }
      if ( grp.vramRow.slider )
      {
        o["vramOffsetMHz"] = grp.vramRow.slider->value();
      }
      offsets.append( o );
    }
    if ( !offsets.isEmpty() )
      root["offsets"] = offsets;
  }

  // GPU locked clocks (only include enabled when feature is supported)
  if ( m_gpuLockedGroup && m_gpuLockedGroup->isChecked() )
  {
    const int minValue = m_gpuLockedMinSlider->value();
    const int maxValue = m_gpuLockedMaxSlider->value();
    const bool isFullRange = minValue == m_gpuLockedMinSlider->minimum()
                          && maxValue == m_gpuLockedMaxSlider->maximum();
    if ( !isFullRange )
    {
      QJsonObject gpuLocked;
      gpuLocked["enabled"] = true;
      gpuLocked["min"] = minValue;
      gpuLocked["max"] = maxValue;
      root["gpuLockedClocks"] = gpuLocked;
    }
  }

  // VRAM locked clocks (only include enabled when feature is supported)
  if ( m_vramLockedGroup && m_vramLockedGroup->isChecked() )
  {
    const int minValue = m_vramLockedMinSlider->value();
    const int maxValue = m_vramLockedMaxSlider->value();
    const bool isFullRange = minValue == m_vramLockedMinSlider->minimum()
                          && maxValue == m_vramLockedMaxSlider->maximum();
    if ( !isFullRange )
    {
      QJsonObject vramLocked;
      vramLocked["enabled"] = true;
      vramLocked["min"] = minValue;
      vramLocked["max"] = maxValue;
      root["vramLockedClocks"] = vramLocked;
    }
  }

  // Power limit — include both cTGP offset (for Uniwill/Tuxedo laptops with
  // the sysfs interface) and the absolute NVML powerLimitW (for desktop GPUs
  // that lack cTGP but support nvmlDeviceSetPowerManagementLimit).  The daemon
  // will use whichever path is available.
  if ( m_powerLimitSlider )
  {
    const int sliderValue = m_powerLimitSlider->value();
    const int ctgpOffset = sliderValue - static_cast< int >( std::round( m_powerDefaultW ) );

    QJsonObject nvidiaPowerObj;
    nvidiaPowerObj["cTGPOffset"] = ctgpOffset;
    root["nvidiaPowerCTRLProfile"] = nvidiaPowerObj;

    // Also include NVML power limit for desktop GPUs without cTGP sysfs
    root["powerLimitW"] = static_cast< double >( sliderValue );

    qDebug() << "[GPU-CTGP] buildProfileJSON"
             << "sliderValueW=" << sliderValue
             << "baselineDefaultW=" << m_powerDefaultW
             << "ctgpOffset=" << ctgpOffset
             << "powerLimitW=" << sliderValue;
  }

  QJsonDocument doc( root );
  qDebug() << "[GPU-CTGP] buildProfileJSON payload:" << QString::fromUtf8( doc.toJson( QJsonDocument::Compact ) );
  return QString::fromUtf8( doc.toJson( QJsonDocument::Compact ) );
}

void GpuProfileTab::loadProfile( const QString &json )
{
  if ( json.isEmpty() || json == "{}" )
    return;

  QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
  if ( !doc.isObject() )
    return;

  QJsonObject obj = doc.object();

  // Load clock offsets
  for ( auto &grp : m_pstateGroups )
  {
    if ( grp.groupBox )
      grp.groupBox->setChecked( false );
  }

  if ( obj.contains( "offsets" ) )
  {
    QJsonArray offsets = obj["offsets"].toArray();
    for ( const auto &v : offsets )
    {
      QJsonObject o = v.toObject();
      int ps = o["pstate"].toInt();
      for ( auto &grp : m_pstateGroups )
      {
        if ( static_cast< int >( grp.pstate ) == ps )
        {
          if ( grp.groupBox )
            grp.groupBox->setChecked( true );

          if ( o.contains( "gpuOffsetMHz" ) && grp.gpuRow.slider )
          {
            int off = o["gpuOffsetMHz"].toInt();
            grp.gpuRow.slider->blockSignals( true );
            grp.gpuRow.slider->setValue( off );
            grp.gpuRow.slider->blockSignals( false );
            grp.gpuRow.spinBox->blockSignals( true );
            grp.gpuRow.spinBox->setValue( off );
            grp.gpuRow.spinBox->blockSignals( false );
          }
          if ( o.contains( "vramOffsetMHz" ) && grp.vramRow.slider )
          {
            int off = o["vramOffsetMHz"].toInt();
            grp.vramRow.slider->blockSignals( true );
            grp.vramRow.slider->setValue( off );
            grp.vramRow.slider->blockSignals( false );
            grp.vramRow.spinBox->blockSignals( true );
            grp.vramRow.spinBox->setValue( off );
            grp.vramRow.spinBox->blockSignals( false );
          }
          break;
        }
      }
    }
  }

  // Load GPU locked clocks
  if ( m_gpuLockedGroup )
    m_gpuLockedGroup->setChecked( obj.contains( "gpuLockedClocks" ) );

  if ( obj.contains( "gpuLockedClocks" ) )
  {
    QJsonObject lc = obj["gpuLockedClocks"].toObject();
    const int minVal = lc["min"].toInt();
    const int maxVal = lc["max"].toInt();

    m_gpuLockedMinSlider->blockSignals( true );
    m_gpuLockedMinSlider->setValue( minVal );
    m_gpuLockedMinSlider->blockSignals( false );
    m_gpuLockedMaxSlider->blockSignals( true );
    m_gpuLockedMaxSlider->setValue( maxVal );
    m_gpuLockedMaxSlider->blockSignals( false );

    // Slider signals are blocked above, so keep spin labels in sync manually.
    m_gpuLockedMinSpin->blockSignals( true );
    m_gpuLockedMinSpin->setValue( minVal );
    m_gpuLockedMinSpin->blockSignals( false );
    m_gpuLockedMaxSpin->blockSignals( true );
    m_gpuLockedMaxSpin->setValue( maxVal );
    m_gpuLockedMaxSpin->blockSignals( false );
  }

  // Load VRAM locked clocks
  if ( m_vramLockedGroup )
    m_vramLockedGroup->setChecked( obj.contains( "vramLockedClocks" ) );

  if ( obj.contains( "vramLockedClocks" ) )
  {
    QJsonObject lc = obj["vramLockedClocks"].toObject();
    const int minVal = lc["min"].toInt();
    const int maxVal = lc["max"].toInt();

    m_vramLockedMinSlider->blockSignals( true );
    m_vramLockedMinSlider->setValue( minVal );
    m_vramLockedMinSlider->blockSignals( false );
    m_vramLockedMaxSlider->blockSignals( true );
    m_vramLockedMaxSlider->setValue( maxVal );
    m_vramLockedMaxSlider->blockSignals( false );

    // Slider signals are blocked above, so keep spin labels in sync manually.
    m_vramLockedMinSpin->blockSignals( true );
    m_vramLockedMinSpin->setValue( minVal );
    m_vramLockedMinSpin->blockSignals( false );
    m_vramLockedMaxSpin->blockSignals( true );
    m_vramLockedMaxSpin->setValue( maxVal );
    m_vramLockedMaxSpin->blockSignals( false );
  }

  // Load cTGP offset profile data (preferred)
  bool powerLoaded = false;
  if ( obj.contains( "nvidiaPowerCTRLProfile" ) && obj["nvidiaPowerCTRLProfile"].isObject() && m_powerLimitSlider )
  {
    QJsonObject gpuObj = obj["nvidiaPowerCTRLProfile"].toObject();
    int offset = gpuObj["cTGPOffset"].toInt( 0 );
    int valueW = static_cast< int >( std::round( m_powerDefaultW ) ) + offset;

    qDebug() << "[GPU-CTGP] loadProfile"
             << "baselineDefaultW=" << m_powerDefaultW
             << "loadedOffset=" << offset
             << "targetW=" << valueW
             << "sliderMin=" << m_powerLimitSlider->minimum()
             << "sliderMax=" << m_powerLimitSlider->maximum();

    m_powerLimitSlider->blockSignals( true );
    m_powerLimitSlider->setValue( std::clamp( valueW, m_powerLimitSlider->minimum(), m_powerLimitSlider->maximum() ) );
    m_powerLimitSlider->blockSignals( false );
    m_powerLimitValue->setText( QString::number( m_powerLimitSlider->value() ) + " W" );

    qDebug() << "[GPU-CTGP] loadProfile applied sliderW=" << m_powerLimitSlider->value();
    powerLoaded = true;
  }

  // Desktop / generic NVIDIA path: absolute power limit in watts.
  if ( !powerLoaded && obj.contains( "powerLimitW" ) && m_powerLimitSlider )
  {
    const int valueW = static_cast< int >( std::round( obj["powerLimitW"].toDouble( 0.0 ) ) );
    if ( valueW > 0 )
    {
      m_powerLimitSlider->blockSignals( true );
      m_powerLimitSlider->setValue( std::clamp( valueW, m_powerLimitSlider->minimum(), m_powerLimitSlider->maximum() ) );
      m_powerLimitSlider->blockSignals( false );
      m_powerLimitValue->setText( QString::number( m_powerLimitSlider->value() ) + " W" );
      qDebug() << "[GPU-CTGP] loadProfile applied desktop powerLimitW=" << m_powerLimitSlider->value();
    }
  }
}

// ── Private slots ───────────────────────────────────────────────────

void GpuProfileTab::onGpuProfileComboRenamed()
{
  if ( !m_gpuProfileCombo || !m_gpuProfileCombo->lineEdit() )
    return;

  int idx = m_gpuProfileCombo->currentIndex();
  if ( idx < 0 )
    return;

  QString gpuProfileId = m_gpuProfileCombo->itemData( idx ).toString();
  QString oldName = m_gpuProfileCombo->itemText( idx );
  QString newName = m_gpuProfileCombo->currentText().trimmed();

  if ( newName.isEmpty() || newName == oldName )
  {
    m_gpuProfileCombo->setEditText( oldName );
    return;
  }

  if ( m_profileManager->renameGpuProfile( gpuProfileId, newName ) )
  {
    m_gpuProfileCombo->setItemText( idx, newName );
    emit gpuProfileRenamed( oldName, newName );

    if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
    {
      if ( auto *sb = mw->statusBar() )
        sb->showMessage( QString( "GPU profile renamed from '%1' to '%2'" ).arg( oldName, newName ) );
    }
  }
  else
  {
    m_gpuProfileCombo->setEditText( oldName );
  }
}

void GpuProfileTab::onRefreshClicked()
{
  refreshOCState();
  if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
  {
    if ( auto *sb = mw->statusBar() )
      sb->showMessage( "GPU OC state refreshed" );
  }
}

void GpuProfileTab::onResetClicked()
{
  QMessageBox::StandardButton reply = QMessageBox::question(
    this, "Reset GPU OC",
    "Are you sure you want to reset all GPU overclocking settings to their defaults?\n\n"
    "This will clear all clock offsets, locked clocks, and restore the default power limit.",
    QMessageBox::Yes | QMessageBox::No );

  if ( reply == QMessageBox::Yes )
  {
    if ( m_uccdClient->getCTGPAdjustmentSupported().value_or( false ) )
      (void)m_uccdClient->setNVIDIAPowerOffset( 0 );

    bool ok = m_uccdClient->resetNvidiaGpuOCAll( 0 );

    // Fallback for platforms where monolithic reset reports failure
    // due to partially unsupported operations.
    if ( !ok )
    {
      bool anySucceeded = false;

      if ( m_uccdClient->resetNvidiaAllClockOffsets( 0 ) )
        anySucceeded = true;

      if ( m_uccdClient->resetNvidiaGpuLockedClocks( 0 ) )
        anySucceeded = true;

      if ( m_uccdClient->resetNvidiaVramLockedClocks( 0 ) )
        anySucceeded = true;

      if ( m_uccdClient->resetNvidiaGpuPowerLimit( 0 ) )
        anySucceeded = true;

      ok = anySucceeded;
    }

    if ( ok )
    {
      refreshOCState();
      if ( auto *mw = qobject_cast< QMainWindow * >( window() ) )
      {
        if ( auto *sb = mw->statusBar() )
          sb->showMessage( "GPU OC settings reset to defaults" );
      }
    }
    else
    {
      QMessageBox::warning( this, "Error", "Failed to reset GPU OC settings." );
    }
  }
}

void GpuProfileTab::showAutoOCDialog()
{
  if ( !ensureOverclockWarningAcknowledged() )
    return;

  // Check if already running
  if ( auto running = m_uccdClient->getAutoOCRunning(); running && *running )
  {
    QMessageBox::information( this, "Auto Overclock",
      "An auto overclock scan is already in progress." );
    return;
  }

  QDialog *dlg = new QDialog( this );
  dlg->setWindowTitle( "Auto Overclock" );
  dlg->setMinimumWidth( 480 );
  dlg->setAttribute( Qt::WA_DeleteOnClose );

  QVBoxLayout *layout = new QVBoxLayout( dlg );
  layout->setSpacing( 12 );

  // ── Component selector row ──
  QHBoxLayout *selectRow = new QHBoxLayout();
  selectRow->addWidget( new QLabel( "Scan:" ) );
  QComboBox *componentCombo = new QComboBox();
  componentCombo->addItem( "Core + VRAM", "both" );
  componentCombo->addItem( "Core only", "core" );
  componentCombo->addItem( "VRAM only", "vram" );
  componentCombo->setToolTip( "Choose which clock domain(s) to tune during auto overclock" );
  selectRow->addWidget( componentCombo, 1 );
  layout->addLayout( selectRow );

  // ── Info label ──
  QLabel *infoLabel = new QLabel(
    "<b>Note:</b> Run a GPU-intensive workload (game, benchmark, compute) "
    "during the scan for accurate results." );
  infoLabel->setWordWrap( true );
  infoLabel->setToolTip( "Auto overclock requires sustained GPU load and FPS telemetry" );
  layout->addWidget( infoLabel );

  // ── Progress section ──
  QProgressBar *progressBar = new QProgressBar();
  progressBar->setRange( 0, 100 );
  progressBar->setValue( 0 );
  progressBar->setTextVisible( true );
  progressBar->setFormat( "Idle" );
  progressBar->setToolTip( "Shows scan progress for baseline, search, and validation phases" );
  layout->addWidget( progressBar );

  // ── Metrics grid ──
  QGridLayout *metricsGrid = new QGridLayout();
  metricsGrid->setHorizontalSpacing( 16 );
  metricsGrid->setVerticalSpacing( 4 );

  QLabel *phaseValueLabel = new QLabel( "—" );
  QLabel *coreValueLabel = new QLabel( "—" );
  QLabel *vramValueLabel = new QLabel( "—" );
  QLabel *tempValueLabel = new QLabel( "—" );
  QLabel *clockValueLabel = new QLabel( "—" );
  QLabel *vramClockValueLabel = new QLabel( "—" );
  QLabel *utilValueLabel = new QLabel( "—" );
  QLabel *fpsValueLabel = new QLabel( "—" );

  QFont boldFont = phaseValueLabel->font();
  boldFont.setBold( true );
  phaseValueLabel->setFont( boldFont );
  coreValueLabel->setFont( boldFont );
  vramValueLabel->setFont( boldFont );

  metricsGrid->addWidget( new QLabel( "Phase:" ), 0, 0 );
  metricsGrid->addWidget( phaseValueLabel, 0, 1 );
  metricsGrid->addWidget( new QLabel( "Temp:" ), 0, 2 );
  metricsGrid->addWidget( tempValueLabel, 0, 3 );
  metricsGrid->addWidget( new QLabel( "Core offset:" ), 1, 0 );
  metricsGrid->addWidget( coreValueLabel, 1, 1 );
  metricsGrid->addWidget( new QLabel( "GPU clock:" ), 1, 2 );
  metricsGrid->addWidget( clockValueLabel, 1, 3 );
  metricsGrid->addWidget( new QLabel( "VRAM offset:" ), 2, 0 );
  metricsGrid->addWidget( vramValueLabel, 2, 1 );
  metricsGrid->addWidget( new QLabel( "VRAM clock:" ), 2, 2 );
  metricsGrid->addWidget( vramClockValueLabel, 2, 3 );
  metricsGrid->addWidget( new QLabel( "GPU util:" ), 3, 0 );
  metricsGrid->addWidget( utilValueLabel, 3, 1 );
  metricsGrid->addWidget( new QLabel( "FPS:" ), 3, 2 );
  metricsGrid->addWidget( fpsValueLabel, 3, 3 );
  layout->addLayout( metricsGrid );

  // ── Status label ──
  QLabel *statusLabel = new QLabel( "Ready to start." );
  statusLabel->setWordWrap( true );
  statusLabel->setToolTip( "Live status messages from the daemon" );
  layout->addWidget( statusLabel );

  if ( auto ocProgress = m_uccdClient->getAutoOCProgress(); ocProgress.has_value() )
  {
    const QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *ocProgress ).toUtf8() );
    if ( doc.isObject() )
    {
      const QJsonObject obj = doc.object();
      if ( !obj.value( "running" ).toBool( false ) && obj.value( "resumeAvailable" ).toBool( false ) )
      {
        statusLabel->setText( obj.value( "message" ).toString(
          "Resume available. Start the game/application, then click Resume to continue." ) );
      }
    }
  }

  // ── Advanced settings ──
  QGridLayout *advGrid = new QGridLayout();
  advGrid->setHorizontalSpacing( 12 );

  advGrid->addWidget( new QLabel( "Step size:" ), 0, 0 );
  QSpinBox *stepSizeSpin = new QSpinBox();
  stepSizeSpin->setRange( 1, 100 );
  stepSizeSpin->setValue( 5 );
  stepSizeSpin->setSuffix( " MHz" );
  stepSizeSpin->setToolTip( "Binary search granularity (resolution)" );
  advGrid->addWidget( stepSizeSpin, 0, 1 );

  advGrid->addWidget( new QLabel( "Max offset:" ), 0, 2 );
  QSpinBox *maxOffsetSpin = new QSpinBox();
  maxOffsetSpin->setRange( 10, 2000 );
  maxOffsetSpin->setValue( 700 );
  maxOffsetSpin->setSuffix( " MHz" );
  maxOffsetSpin->setToolTip( "Upper limit for the offset search" );
  advGrid->addWidget( maxOffsetSpin, 0, 3 );

  advGrid->addWidget( new QLabel( "Stability period:" ), 1, 0 );
  QSpinBox *stabilitySpin = new QSpinBox();
  stabilitySpin->setRange( 1, 120 );
  stabilitySpin->setValue( 15 );
  stabilitySpin->setSuffix( " s" );
  stabilitySpin->setSingleStep( 1 );
  stabilitySpin->setToolTip( "Per-step stability test duration in seconds" );
  advGrid->addWidget( stabilitySpin, 1, 1 );

  layout->addLayout( advGrid );

  // Load saved OC advanced settings from uccrc
  {
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    stepSizeSpin->setValue( settings.value( "gpu/ocStepSizeMHz", 5 ).toInt() );
    maxOffsetSpin->setValue( settings.value( "gpu/ocMaxOffsetMHz", 700 ).toInt() );
    const int stabilitySec = settings.contains( "gpu/ocStabilitySec" )
                           ? settings.value( "gpu/ocStabilitySec", 15 ).toInt()
                           : ( settings.value( "gpu/ocStabilityMs", 15000 ).toInt() / 1000 );
    stabilitySpin->setValue( stabilitySec > 0 ? stabilitySec : 15 );
  }

  auto saveOCAdvanced = [stepSizeSpin, maxOffsetSpin, stabilitySpin]() {
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    settings.setValue( "gpu/ocStepSizeMHz", stepSizeSpin->value() );
    settings.setValue( "gpu/ocMaxOffsetMHz", maxOffsetSpin->value() );
    settings.setValue( "gpu/ocStabilitySec", stabilitySpin->value() );
    settings.setValue( "gpu/ocStabilityMs", stabilitySpin->value() * 1000 );
    settings.sync();
  };
  connect( stepSizeSpin, QOverload< int >::of( &QSpinBox::valueChanged ), dlg, saveOCAdvanced );
  connect( maxOffsetSpin, QOverload< int >::of( &QSpinBox::valueChanged ), dlg, saveOCAdvanced );
  connect( stabilitySpin, QOverload< int >::of( &QSpinBox::valueChanged ), dlg, saveOCAdvanced );

  // ── Buttons ──
  const bool ocHasCheckpoint = m_uccdClient->hasAutoOCCheckpoint();
  QHBoxLayout *btnRow = new QHBoxLayout();
  QPushButton *startBtn = new QPushButton( "Start" );
  QPushButton *resumeBtn = new QPushButton( "Resume" );
  QPushButton *pauseBtn = new QPushButton( "Pause" );
  QPushButton *stopBtn = new QPushButton( "Stop" );
  QPushButton *closeBtn = new QPushButton( "Close" );
  startBtn->setToolTip( "Start a new overclock scan with the current settings" );
  resumeBtn->setToolTip( "Resume from a saved checkpoint" );
  pauseBtn->setToolTip( "Pause scan and save progress for resume" );
  stopBtn->setToolTip( "Request a safe stop after the current step" );
  closeBtn->setToolTip( "Close this dialog" );
  pauseBtn->setEnabled( false );
  stopBtn->setEnabled( false );
  resumeBtn->setEnabled( ocHasCheckpoint );
  btnRow->addStretch();
  btnRow->addWidget( startBtn );
  btnRow->addWidget( resumeBtn );
  btnRow->addWidget( pauseBtn );
  btnRow->addWidget( stopBtn );
  btnRow->addWidget( closeBtn );
  layout->addLayout( btnRow );

  // ── D-Bus signal connections (cleaned up on dialog close) ──
  QMetaObject::Connection progressConn = connect(
    m_uccdClient, &UccdClient::autoOCProgressChanged,
    dlg, [=]( const QString &json )
    {
      QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
      if ( !doc.isObject() )
        return;
      QJsonObject obj = doc.object();

      // Phase name (daemon sends string: "idle","baseline","searching","validating","done")
      QString phaseStr = obj["phase"].toString( "idle" );
      auto phase = ( phaseStr == "baseline" )   ? OCPhase::Baseline
                  : ( phaseStr == "searching" )  ? OCPhase::Searching
                  : ( phaseStr == "validating" ) ? OCPhase::Validating
                  : ( phaseStr == "done" )       ? OCPhase::Done
                                                  : OCPhase::Idle;

      static const char *phaseNames[] = { "Idle", "Baseline", "Searching", "Validating", "Done" };
      phaseValueLabel->setText( phaseNames[static_cast< int >( phase )] );

      // Component (daemon sends string: "core" or "vram")
      QString compStr = obj["component"].toString( "core" );
      bool isVram = ( compStr == "vram" );
      QString compName = isVram ? "VRAM" : "Core";

      // Offsets (daemon keys: "currentOffset", "bestStable")
      int currentOff = obj["currentOffset"].toInt( 0 );
      int bestStable = obj["bestStable"].toInt( 0 );

      if ( isVram )
      {
        vramValueLabel->setText( QString( "+%1 MHz (best: +%2)" )
          .arg( currentOff ).arg( bestStable ) );
      }
      else
      {
        coreValueLabel->setText( QString( "+%1 MHz (best: +%2)" )
          .arg( currentOff ).arg( bestStable ) );
      }

      // Live metrics (daemon keys: "temp", "gpuClock", "vramClock", "gpuUtil", "fps")
      int tempC = obj["temp"].toInt( 0 );
      int gpuClk = obj["gpuClock"].toInt( 0 );
      int vramClk = obj["vramClock"].toInt( 0 );
      int gpuUtil = obj["gpuUtil"].toInt( 0 );
      double fps = obj["fps"].toDouble( -1.0 );

      tempValueLabel->setText( tempC > 0 ? QString( "%1 °C" ).arg( tempC ) : "—" );
      clockValueLabel->setText( gpuClk > 0 ? QString( "%1 MHz" ).arg( gpuClk ) : "—" );
      vramClockValueLabel->setText( vramClk > 0 ? QString( "%1 MHz" ).arg( vramClk ) : "—" );
      utilValueLabel->setText( gpuUtil >= 0 ? QString( "%1%" ).arg( gpuUtil ) : "—" );
      fpsValueLabel->setText( fps >= 0.0 ? QString( "%1 fps" ).arg( fps, 0, 'f', 1 ) : "—" );

      // Progress bar
      int iter = obj["iteration"].toInt( 0 );
      int maxIter = obj["maxIterations"].toInt( 1 );
      if ( phase == OCPhase::Baseline )
      {
        progressBar->setRange( 0, 0 );
        progressBar->setFormat( "Baseline..." );
      }
      else if ( phase == OCPhase::Searching )
      {
        progressBar->setRange( 0, maxIter );
        progressBar->setValue( iter );
        progressBar->setFormat( compName + " — step %v/%m" );
      }
      else if ( phase == OCPhase::Validating )
      {
        progressBar->setRange( 0, 100 );
        progressBar->setValue( 100 );
        progressBar->setFormat( "Validating " + compName + "..." );
      }

      // Status message
      QString msg = obj["message"].toString();
      if ( !msg.isEmpty() )
        statusLabel->setText( msg );
    } );

  QMetaObject::Connection finishedConn = connect(
    m_uccdClient, &UccdClient::autoOCFinished,
    dlg, [this, startBtn, resumeBtn, pauseBtn, stopBtn, componentCombo, progressBar, coreValueLabel, vramValueLabel, statusLabel, dlg]( int coreOff, int vramOff, bool success, const QString &message )
    {
      startBtn->setEnabled( true );
      pauseBtn->setEnabled( false );
      stopBtn->setEnabled( false );
      componentCombo->setEnabled( true );

      const bool suspended = message.startsWith( QStringLiteral( "Suspended:" ) );
      resumeBtn->setEnabled( suspended || m_uccdClient->hasAutoOCCheckpoint() );

      progressBar->setRange( 0, 100 );
      progressBar->setValue( success ? 100 : 0 );
      progressBar->setFormat( success ? "Complete" : ( suspended ? "Suspended" : "Failed" ) );

      if ( coreOff > 0 )
        coreValueLabel->setText( QString( "+%1 MHz" ).arg( coreOff ) );
      if ( vramOff > 0 )
        vramValueLabel->setText( QString( "+%1 MHz" ).arg( vramOff ) );

      statusLabel->setText( message );

      if ( suspended )
      {
        QMessageBox::warning( dlg, "Auto Overclock — Application Crash Detected",
          message + "\n\nClick Resume when your application is running again to continue from the last saved step." );
      }
      else if ( success )
      {
        QString summary = QString( "Auto overclock complete!\n\n"
                                   "Core offset: +%1 MHz\n"
                                   "VRAM offset: +%2 MHz" )
                            .arg( coreOff ).arg( vramOff );
        summary += "\n\nSettings have been applied. Use Save to persist them.";

        QMessageBox::information( dlg, "Auto Overclock", summary );

        // Refresh tab to show new offsets
        refreshOCState();
      }
    } );

  // Disconnect D-Bus signals when dialog closes
  connect( dlg, &QDialog::destroyed, this, [=]() {
    disconnect( progressConn );
    disconnect( finishedConn );
  } );

  // ── Button handlers ──
  auto launchOC = [this, componentCombo, startBtn, resumeBtn, pauseBtn, stopBtn, statusLabel, coreValueLabel, vramValueLabel, progressBar, stepSizeSpin, maxOffsetSpin, stabilitySpin]() {
    QString comp = componentCombo->currentData().toString();
    bool ok = m_uccdClient->startAutoOC( comp.toStdString(), 0,
      stepSizeSpin->value(), maxOffsetSpin->value(), stabilitySpin->value() * 1000 );
    if ( ok )
    {
      startBtn->setEnabled( false );
      resumeBtn->setEnabled( false );
      pauseBtn->setEnabled( true );
      stopBtn->setEnabled( true );
      componentCombo->setEnabled( false );
      statusLabel->setText( "Starting auto overclock scan..." );
      coreValueLabel->setText( "—" );
      vramValueLabel->setText( "—" );
      progressBar->setRange( 0, 0 );
      progressBar->setFormat( "Starting..." );
    }
    else
    {
      statusLabel->setText( "Failed to start auto overclock. "
                            "Check that the daemon is running and NVML is available." );
    }
  };

  connect( startBtn, &QPushButton::clicked, dlg, [this, dlg, launchOC](){
    if ( m_uccdClient->hasAutoOCCheckpoint() )
    {
      auto reply = QMessageBox::question( dlg, "Start New Overclock Scan",
        "A suspended overclock process exists. Starting a new scan will discard "
        "the saved progress.\n\nAre you sure you want to start from scratch?",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No );

      if ( reply == QMessageBox::No )
        return;

      m_uccdClient->clearAutoOCCheckpoint();
    }

    launchOC();
  } );

  connect( resumeBtn, &QPushButton::clicked, dlg,
    [this, dlg, componentCombo, startBtn, resumeBtn, pauseBtn, stopBtn, statusLabel,
     coreValueLabel, vramValueLabel, progressBar, stepSizeSpin, maxOffsetSpin, stabilitySpin]()
    {
      handleResumeAutoOC( dlg, componentCombo, startBtn, resumeBtn, pauseBtn, stopBtn,
                          statusLabel, coreValueLabel, vramValueLabel, progressBar,
                          stepSizeSpin, maxOffsetSpin, stabilitySpin );
    } );

  connect( pauseBtn, &QPushButton::clicked, dlg, [this, pauseBtn, stopBtn, statusLabel]() {
    m_uccdClient->pauseAutoOC();
    pauseBtn->setEnabled( false );
    stopBtn->setEnabled( false );
    statusLabel->setText( "Pause requested..." );
  } );

  connect( stopBtn, &QPushButton::clicked, dlg, [this, stopBtn, statusLabel]() {
    m_uccdClient->stopAutoOC();
    stopBtn->setEnabled( false );
    statusLabel->setText( "Stop requested..." );
  } );

  connect( closeBtn, &QPushButton::clicked, dlg, &QDialog::close );

  dlg->show();
}

void GpuProfileTab::showAutoUndervoltDialog()
{
  // Check if already running
  if ( m_uccdClient->getAutoUndervoltRunning())
  {
    QMessageBox::information( this, "Auto Undervolt",
      "An auto undervolt scan is already in progress." );
    return;
  }

  QDialog *dlg = new QDialog( this );
  dlg->setWindowTitle( "Auto Undervolt (Per-App)" );
  dlg->setMinimumWidth( 500 );
  dlg->setAttribute( Qt::WA_DeleteOnClose );

  QVBoxLayout *layout = new QVBoxLayout( dlg );
  layout->setSpacing( 12 );

  // ── Info label ──
  QLabel *infoLabel = new QLabel(
    "<b>Auto Undervolt</b> finds the lowest GPU frequency cap that maintains "
    "your current FPS. This reduces power consumption and heat while keeping "
    "performance.<br><br>"
    "<b>Requirements:</b> A Vulkan/OpenGL game or benchmark must be running "
    "with the ucc-fps-layer active and <tt>UCC_FPS_HOOK=1</tt> set for that "
    "process (FPS data must be streaming).<br><br>"
    "The result is stored per-application based on the process name." );
  infoLabel->setWordWrap( true );
  infoLabel->setToolTip( "Run your game/benchmark with FPS layer enabled before starting" );
  layout->addWidget( infoLabel );

  // ── Progress section ──
  QProgressBar *progressBar = new QProgressBar();
  progressBar->setRange( 0, 100 );
  progressBar->setValue( 0 );
  progressBar->setTextVisible( true );
  progressBar->setFormat( "Idle" );
  progressBar->setToolTip( "Shows baseline, search, and validation progress" );
  layout->addWidget( progressBar );

  // ── Metrics grid ──
  QGridLayout *metricsGrid = new QGridLayout();
  metricsGrid->setHorizontalSpacing( 16 );
  metricsGrid->setVerticalSpacing( 4 );

  QLabel *phaseValueLabel = new QLabel( "—" );
  QLabel *appValueLabel = new QLabel( "—" );
  QLabel *capValueLabel = new QLabel( "—" );
  QLabel *fpsValueLabel = new QLabel( "—" );
  QLabel *baselineFpsLabel = new QLabel( "—" );
  QLabel *tempValueLabel = new QLabel( "—" );
  QLabel *clockValueLabel = new QLabel( "—" );
  QLabel *powerValueLabel = new QLabel( "—" );
  QLabel *utilValueLabel = new QLabel( "—" );
  QLabel *efficiencyValueLabel = new QLabel( "—" );

  QFont boldFont = phaseValueLabel->font();
  boldFont.setBold( true );
  phaseValueLabel->setFont( boldFont );
  capValueLabel->setFont( boldFont );
  appValueLabel->setFont( boldFont );

  int row = 0;
  metricsGrid->addWidget( new QLabel( "Phase:" ), row, 0 );
  metricsGrid->addWidget( phaseValueLabel, row, 1 );
  metricsGrid->addWidget( new QLabel( "App:" ), row, 2 );
  metricsGrid->addWidget( appValueLabel, row, 3 );
  ++row;
  metricsGrid->addWidget( new QLabel( "Freq cap:" ), row, 0 );
  metricsGrid->addWidget( capValueLabel, row, 1 );
  metricsGrid->addWidget( new QLabel( "GPU clock:" ), row, 2 );
  metricsGrid->addWidget( clockValueLabel, row, 3 );
  ++row;
  metricsGrid->addWidget( new QLabel( "FPS:" ), row, 0 );
  metricsGrid->addWidget( fpsValueLabel, row, 1 );
  metricsGrid->addWidget( new QLabel( "Baseline FPS:" ), row, 2 );
  metricsGrid->addWidget( baselineFpsLabel, row, 3 );
  ++row;
  metricsGrid->addWidget( new QLabel( "Temp:" ), row, 0 );
  metricsGrid->addWidget( tempValueLabel, row, 1 );
  metricsGrid->addWidget( new QLabel( "Power:" ), row, 2 );
  metricsGrid->addWidget( powerValueLabel, row, 3 );
  ++row;
  metricsGrid->addWidget( new QLabel( "GPU util:" ), row, 0 );
  metricsGrid->addWidget( utilValueLabel, row, 1 );
  metricsGrid->addWidget( new QLabel( "Efficiency:" ), row, 2 );
  metricsGrid->addWidget( efficiencyValueLabel, row, 3 );
  layout->addLayout( metricsGrid );

  // ── Status label ──
  QLabel *statusLabel = new QLabel( "Ready — start a GPU workload and click Start." );
  statusLabel->setWordWrap( true );
  statusLabel->setToolTip( "Live status messages from the daemon" );
  layout->addWidget( statusLabel );

  if ( auto uvProgress = m_uccdClient->getAutoUndervoltProgress(); uvProgress.has_value() )
  {
    const QJsonDocument doc = QJsonDocument::fromJson( QString::fromStdString( *uvProgress ).toUtf8() );
    if ( doc.isObject() )
    {
      const QJsonObject obj = doc.object();
      if ( !obj.value( "running" ).toBool( false ) && obj.value( "resumeAvailable" ).toBool( false ) )
      {
        statusLabel->setText( obj.value( "message" ).toString(
          "Resume available. Start the game/application, then click Resume to continue." ) );
      }
    }
  }

  // ── Target FPS controls ──
  QCheckBox *targetFpsCheck = new QCheckBox( "Target FPS" );
  QSpinBox *targetFpsSpin = new QSpinBox();
  targetFpsSpin->setRange( 10, 999 );
  targetFpsSpin->setSuffix( " FPS" );
  targetFpsSpin->setEnabled( false );
  targetFpsCheck->setToolTip( "Reduce clocks until FPS reaches this target" );
  targetFpsSpin->setToolTip( "Desired FPS threshold used by target mode" );

  // Load saved target FPS settings from uccrc
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  targetFpsCheck->setChecked( settings.value( "gpu/undervoltTargetFpsEnabled", false ).toBool() );
  targetFpsSpin->setValue( settings.value( "gpu/undervoltTargetFps", 60 ).toInt() );
  targetFpsSpin->setEnabled( targetFpsCheck->isChecked() );

  connect( targetFpsCheck, &QCheckBox::toggled, dlg, [targetFpsSpin]( bool checked ) {
    targetFpsSpin->setEnabled( checked );
  } );

  // ── Extended validation checkbox ──
  QCheckBox *extendedValCheck = new QCheckBox( "Extended validation" );
  QCheckBox *powerLimitCheck = new QCheckBox( "Power limit mode" );
  extendedValCheck->setToolTip( "Run a longer final stability validation pass" );
  powerLimitCheck->setToolTip( "Use power-limit sweep instead of core-offset sweep" );
  QHBoxLayout *modeRow = new QHBoxLayout();
  modeRow->addWidget( targetFpsCheck );
  modeRow->addWidget( targetFpsSpin );
  modeRow->addWidget( extendedValCheck );
  modeRow->addWidget( powerLimitCheck );
  layout->addLayout( modeRow );

  // Load advanced settings from uccrc
  extendedValCheck->setChecked( settings.value( "gpu/undervoltExtendedValidation", false ).toBool() );
  powerLimitCheck->setChecked( settings.value( "gpu/undervoltPowerLimitMode", false ).toBool() );

  // Save target FPS settings when changed
  auto saveTargetFps = [targetFpsCheck, targetFpsSpin, extendedValCheck, powerLimitCheck]() {
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    settings.setValue( "gpu/undervoltTargetFpsEnabled", targetFpsCheck->isChecked() );
    settings.setValue( "gpu/undervoltTargetFps", targetFpsSpin->value() );
    settings.setValue( "gpu/undervoltExtendedValidation", extendedValCheck->isChecked() );
    settings.setValue( "gpu/undervoltPowerLimitMode", powerLimitCheck->isChecked() );
    settings.sync();
  };

  connect( targetFpsCheck, &QCheckBox::toggled, dlg, saveTargetFps );
  connect( targetFpsSpin, QOverload< int >::of( &QSpinBox::valueChanged ), dlg, saveTargetFps );
  connect( extendedValCheck, &QCheckBox::toggled, dlg, saveTargetFps );
  connect( powerLimitCheck, &QCheckBox::toggled, dlg, saveTargetFps );

  // ── Advanced settings ──
  QGridLayout *uvAdvGrid = new QGridLayout();
  uvAdvGrid->setHorizontalSpacing( 12 );

  uvAdvGrid->addWidget( new QLabel( "Offset step:" ), 0, 0 );
  QSpinBox *stepSizeSpin = new QSpinBox();
  stepSizeSpin->setRange( 1, 200 );
  stepSizeSpin->setValue( 25 );
  stepSizeSpin->setSuffix( " MHz" );
  stepSizeSpin->setToolTip( "Core-offset search granularity" );
  uvAdvGrid->addWidget( stepSizeSpin, 0, 1 );

  uvAdvGrid->addWidget( new QLabel( "Max offset:" ), 0, 2 );
  QSpinBox *maxOffsetSpin = new QSpinBox();
  maxOffsetSpin->setRange( 10, 1000 );
  maxOffsetSpin->setValue( 500 );
  maxOffsetSpin->setSuffix( " MHz" );
  maxOffsetSpin->setToolTip( "Upper limit for the core-offset search" );
  uvAdvGrid->addWidget( maxOffsetSpin, 0, 3 );

  uvAdvGrid->addWidget( new QLabel( "Stability period:" ), 0, 4 );
  QSpinBox *stabilitySpin = new QSpinBox();
  stabilitySpin->setRange( 1, 120 );
  stabilitySpin->setValue( 20 );
  stabilitySpin->setSuffix( " s" );
  stabilitySpin->setSingleStep( 1 );
  stabilitySpin->setToolTip( "Per-step stability test duration in seconds" );
  uvAdvGrid->addWidget( stabilitySpin, 0, 5 );

  layout->addLayout( uvAdvGrid );

  // Load saved UV advanced settings from uccrc
  stepSizeSpin->setValue( settings.value( "gpu/uvStepSizeMHz", 25 ).toInt() );
  maxOffsetSpin->setValue( settings.value( "gpu/uvMaxOffsetMHz", 500 ).toInt() );
  const int stabilitySec = settings.contains( "gpu/uvStabilitySec" )
                          ? settings.value( "gpu/uvStabilitySec", 20 ).toInt()
                          : ( settings.value( "gpu/uvStabilityMs", 20000 ).toInt() / 1000 );
  stabilitySpin->setValue( stabilitySec > 0 ? stabilitySec : 20 );

  auto saveUVAdvanced = [stepSizeSpin, maxOffsetSpin, stabilitySpin]() {
    QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
    settings.setValue( "gpu/uvStepSizeMHz", stepSizeSpin->value() );
    settings.setValue( "gpu/uvMaxOffsetMHz", maxOffsetSpin->value() );
    settings.setValue( "gpu/uvStabilitySec", stabilitySpin->value() );
    settings.setValue( "gpu/uvStabilityMs", stabilitySpin->value() * 1000 );
    settings.sync();
  };

  connect( stepSizeSpin, QOverload< int >::of( &QSpinBox::valueChanged ), dlg, saveUVAdvanced );
  connect( maxOffsetSpin, QOverload< int >::of( &QSpinBox::valueChanged ), dlg, saveUVAdvanced );
  connect( stabilitySpin, QOverload< int >::of( &QSpinBox::valueChanged ), dlg, saveUVAdvanced );

  // ── Buttons ──
  const bool uvHasCheckpoint = m_uccdClient->hasAutoUndervoltCheckpoint();
  QHBoxLayout *btnRow = new QHBoxLayout();
  QPushButton *startBtn = new QPushButton( "Start" );
  QPushButton *resumeBtn = new QPushButton( "Resume" );
  QPushButton *pauseBtn = new QPushButton( "Pause" );
  QPushButton *stopBtn = new QPushButton( "Stop" );
  QPushButton *closeBtn = new QPushButton( "Close" );
  startBtn->setToolTip( "Start a new undervolt scan for the current app" );
  resumeBtn->setToolTip( "Resume from a saved checkpoint" );
  pauseBtn->setToolTip( "Pause scan and save progress for resume" );
  stopBtn->setToolTip( "Request a safe stop after the current step" );
  closeBtn->setToolTip( "Close this dialog" );
  pauseBtn->setEnabled( false );
  stopBtn->setEnabled( false );
  resumeBtn->setEnabled( uvHasCheckpoint );
  btnRow->addStretch();
  btnRow->addWidget( startBtn );
  btnRow->addWidget( resumeBtn );
  btnRow->addWidget( pauseBtn );
  btnRow->addWidget( stopBtn );
  btnRow->addWidget( closeBtn );
  layout->addLayout( btnRow );

  // ── D-Bus signal connections ──
  QMetaObject::Connection progressConn = connect(
    m_uccdClient, &UccdClient::autoUndervoltProgressChanged,
    dlg, [=]( const QString &json )
    {
      QJsonDocument doc = QJsonDocument::fromJson( json.toUtf8() );
      if ( !doc.isObject() )
        return;
      QJsonObject obj = doc.object();

      QString phaseStr = obj["phase"].toString( "idle" );
      auto phase = ( phaseStr == "baseline" )         ? UVPhase::Baseline
                  : ( phaseStr == "capReduction" )    ? UVPhase::CapReduction
                  : ( phaseStr == "searching" )       ? UVPhase::Searching
                  : ( phaseStr == "validating" )      ? UVPhase::Validating
                  : ( phaseStr == "powerLimitSweep" ) ? UVPhase::PowerLimitSweep
                  : ( phaseStr == "done" )            ? UVPhase::Done
                                                        : UVPhase::Idle;

      static const char *phaseNames[] = {
        "Idle", "Baseline", "Cap Reduction", "Searching", "Validating",
        "Power Limit Sweep", "Done" };
      phaseValueLabel->setText( phaseNames[static_cast< int >( phase )] );

      QString app = obj["app"].toString();
      if ( !app.isEmpty() )
        appValueLabel->setText( app );

      int currentCap = obj["currentCapMHz"].toInt( 0 );
      int bestCap = obj["bestCapMHz"].toInt( 0 );
      int plW = obj["currentPowerLimitW"].toInt( 0 );
      capValueLabel->setText(
        ( plW > 0 && currentCap > 0 ) ? QString( "%1 MHz (best: %2) / %3 W" ).arg( currentCap ).arg( bestCap ).arg( plW )
        : ( plW > 0 )                  ? QString( "%1 W (power limit)" ).arg( plW )
        : ( currentCap > 0 )            ? QString( "%1 MHz (best: %2)" ).arg( currentCap ).arg( bestCap )
                                        : capValueLabel->text() );

      int tempC = obj["temp"].toInt( 0 );
      int gpuClk = obj["gpuClock"].toInt( 0 );
      int powerW = obj["powerDraw"].toInt( 0 );
      int gpuUtil = obj["gpuUtil"].toInt( 0 );
      double fps = obj["fps"].toDouble( -1.0 );
      double blFps = obj["baselineFps"].toDouble( -1.0 );

      tempValueLabel->setText( tempC > 0 ? QString( "%1 °C" ).arg( tempC ) : "—" );
      clockValueLabel->setText( gpuClk > 0 ? QString( "%1 MHz" ).arg( gpuClk ) : "—" );
      powerValueLabel->setText( powerW > 0 ? QString( "%1 W" ).arg( powerW ) : "—" );
      utilValueLabel->setText( gpuUtil >= 0 ? QString( "%1%" ).arg( gpuUtil ) : "—" );
      fpsValueLabel->setText( fps >= 0.0 ? QString( "%1" ).arg( fps, 0, 'f', 1 ) : "—" );
      baselineFpsLabel->setText( blFps >= 0.0 ? QString( "%1" ).arg( blFps, 0, 'f', 1 ) : "—" );

      double fpsPerWatt = obj["fpsPerWatt"].toDouble( 0.0 );
      double baselineFpw = obj["baselineFpsPerWatt"].toDouble( 0.0 );
      if ( fpsPerWatt > 0.0 && baselineFpw > 0.0 )
      {
        double gainPct = ( ( fpsPerWatt - baselineFpw ) / baselineFpw ) * 100.0;
        efficiencyValueLabel->setText( QString( "%1 FPS/W (%2%3%)" )
          .arg( fpsPerWatt, 0, 'f', 2 )
          .arg( gainPct >= 0.0 ? "+" : "" )
          .arg( gainPct, 0, 'f', 1 ) );
      }
      else if ( fpsPerWatt > 0.0 )
        efficiencyValueLabel->setText( QString( "%1 FPS/W" ).arg( fpsPerWatt, 0, 'f', 2 ) );
      else
        efficiencyValueLabel->setText( "—" );

      int iter = obj["iteration"].toInt( 0 );
      int maxIter = obj["maxIterations"].toInt( 1 );
      if ( phase == UVPhase::Baseline )
      {
        progressBar->setRange( 0, 0 );
        progressBar->setFormat( "Baseline..." );
      }
      else if ( phase == UVPhase::CapReduction )
      {
        progressBar->setRange( 0, 0 );
        progressBar->setFormat( "Cap Reduction..." );
      }
      else if ( phase == UVPhase::Searching )
      {
        progressBar->setRange( 0, maxIter );
        progressBar->setValue( iter );
        progressBar->setFormat( "Searching — step %v/%m" );
      }
      else if ( phase == UVPhase::Validating )
      {
        progressBar->setRange( 0, 100 );
        progressBar->setValue( 100 );
        progressBar->setFormat( "Validating..." );
      }
      else if ( phase == UVPhase::PowerLimitSweep )
      {
        progressBar->setRange( 0, maxIter );
        progressBar->setValue( iter );
        progressBar->setFormat( "Power Limit Sweep — step %v/%m" );
      }

      QString msg = obj["message"].toString();
      if ( !msg.isEmpty() )
        statusLabel->setText( msg );
    } );

  QMetaObject::Connection finishedConn = connect(
    m_uccdClient, &UccdClient::autoUndervoltFinished,
    dlg, [this, startBtn, resumeBtn, pauseBtn, stopBtn, progressBar, capValueLabel, statusLabel, dlg](
      int gpuFreqCapMHz, bool success, const QString &message, const QString &appName )
    {
      startBtn->setEnabled( true );
      pauseBtn->setEnabled( false );
      stopBtn->setEnabled( false );

      const bool suspended = message.startsWith( QStringLiteral( "Suspended:" ) );
      resumeBtn->setEnabled( suspended || m_uccdClient->hasAutoUndervoltCheckpoint() );

      progressBar->setRange( 0, 100 );
      progressBar->setValue( success ? 100 : 0 );
      progressBar->setFormat( success ? "Complete" : ( suspended ? "Suspended" : "Failed" ) );

      if ( success && gpuFreqCapMHz > 0 )
        capValueLabel->setText( QString( "%1 MHz (applied)" ).arg( gpuFreqCapMHz ) );

      statusLabel->setText( message );

      if ( suspended )
      {
        QMessageBox::warning( dlg, "Auto Undervolt — Application Crash Detected",
          message + "\n\nClick Resume when your application is running again to continue from the last saved step." );
      }
      else if ( success )
      {
        if ( m_profileManager )
        {
          m_profileManager->refreshGpuProfiles();
          reloadGpuProfiles();
        }

        QString summary = QString(
          "Auto undervolt complete for '%1'!\n\n"
          "GPU frequency cap: %2 MHz\n\n"
          "The GPU will now run at a lower power point while maintaining FPS.\n"
          "This profile is saved and will be auto-applied when '%1' runs again." )
          .arg( appName ).arg( gpuFreqCapMHz );

        QMessageBox::information( dlg, "Auto Undervolt", summary );
        refreshOCState();
      }
    } );

  connect( dlg, &QDialog::destroyed, this, [=]() {
    disconnect( progressConn );
    disconnect( finishedConn );
  } );

  // ── Button handlers ──
  auto launchUV = [this, startBtn, resumeBtn, pauseBtn, stopBtn, statusLabel, capValueLabel, progressBar,
     targetFpsCheck, targetFpsSpin, extendedValCheck, powerLimitCheck,
     stepSizeSpin, maxOffsetSpin, stabilitySpin]()
    {
      bool ok = m_uccdClient->startAutoUndervolt(
        0, targetFpsCheck->isChecked(), targetFpsSpin->value(),
        extendedValCheck->isChecked(), powerLimitCheck->isChecked(),
        stepSizeSpin->value(), maxOffsetSpin->value(), stabilitySpin->value() * 1000 );
      if ( ok )
      {
        startBtn->setEnabled( false );
        resumeBtn->setEnabled( false );
        pauseBtn->setEnabled( true );
        stopBtn->setEnabled( true );
        statusLabel->setText( "Starting auto undervolt scan..." );
        capValueLabel->setText( "—" );
        progressBar->setRange( 0, 0 );
        progressBar->setFormat( "Starting..." );
      }
      else
      {
        statusLabel->setText(
          "Failed to start. Ensure a game is running with the FPS layer active, "
          "UCC_FPS_HOOK=1 is set for that process, and the daemon is connected." );
      }
    };

  connect( startBtn, &QPushButton::clicked, dlg,
    [this, dlg, launchUV]()
    {
      if ( m_uccdClient->hasAutoUndervoltCheckpoint() )
      {
        auto reply = QMessageBox::question( dlg, "Start New Undervolt Scan",
          "A suspended undervolt process exists. Starting a new scan will discard "
          "the saved progress.\n\nAre you sure you want to start from scratch?",
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No );
        if ( reply != QMessageBox::Yes )
          return;
        m_uccdClient->clearAutoUndervoltCheckpoint();
      }
      launchUV();
    } );

  connect( resumeBtn, &QPushButton::clicked, dlg,
    [this, dlg, startBtn, resumeBtn, pauseBtn, stopBtn, statusLabel, capValueLabel, progressBar,
     targetFpsCheck, targetFpsSpin, extendedValCheck, powerLimitCheck,
     stepSizeSpin, maxOffsetSpin, stabilitySpin]()
    {
      handleResumeAutoUV( dlg, startBtn, resumeBtn, pauseBtn, stopBtn, statusLabel,
                          capValueLabel, progressBar,
                          targetFpsCheck, targetFpsSpin,
                          extendedValCheck, powerLimitCheck,
                          stepSizeSpin, maxOffsetSpin, stabilitySpin );
    } );

  connect( pauseBtn, &QPushButton::clicked, dlg, [this, pauseBtn, stopBtn, statusLabel]() {
    m_uccdClient->pauseAutoUndervolt();
    pauseBtn->setEnabled( false );
    stopBtn->setEnabled( false );
    statusLabel->setText( "Pause requested..." );
  } );

  connect( stopBtn, &QPushButton::clicked, dlg, [this, stopBtn, statusLabel]() {
    m_uccdClient->stopAutoUndervolt();
    stopBtn->setEnabled( false );
    statusLabel->setText( "Stop requested..." );
  } );

  connect( closeBtn, &QPushButton::clicked, dlg, &QDialog::close );

  dlg->show();
}

// ─── Resume helpers ─────────────────────────────────────────────────────────

std::string GpuProfileTab::askResumeMode( QWidget *parent, const QString &title,
                                          const QString &suspendReason ) const
{
  if ( suspendReason == QStringLiteral( "crash_detected" ) )
  {
    auto reply = QMessageBox::question( parent, title,
      "The previous scan was interrupted because FPS data stopped.\n\n"
      "Did your application crash, or did you close it yourself?\n\n"
      "\u2022 Yes = I closed it (repeat the last step)\n"
      "\u2022 No = It crashed (go back one step for safety)",
      QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
      QMessageBox::No );
    if ( reply == QMessageBox::Cancel )
      return {};  // empty → caller should abort
    return ( reply == QMessageBox::Yes ) ? "repeat_step" : "step_back";
  }
  // No suspendReason (daemon crash) → always step back
  return "step_back";
}

void GpuProfileTab::handleResumeAutoOC( QDialog *dlg, QComboBox *componentCombo,
                                        QPushButton *startBtn, QPushButton *resumeBtn,
                                        QPushButton *pauseBtn, QPushButton *stopBtn,
                                        QLabel *statusLabel,
                                        QLabel *coreValueLabel, QLabel *vramValueLabel,
                                        QProgressBar *progressBar,
                                        QSpinBox *stepSizeSpin, QSpinBox *maxOffsetSpin,
                                        QSpinBox *stabilitySpin )
{
  // Determine resume mode from checkpoint's suspendReason
  QString suspendReason;
  if ( auto progressJson = m_uccdClient->getAutoOCProgress() )
  {
    QJsonDocument pdoc = QJsonDocument::fromJson(
      QString::fromStdString( *progressJson ).toUtf8() );
    suspendReason = pdoc.object().value( "suspendReason" ).toString();
  }

  std::string resumeMode = askResumeMode( dlg, "Resume Overclock Scan", suspendReason );
  if ( resumeMode.empty() )
    return;  // user cancelled

  // Verify an FPS client is connected
  if ( auto fpsJson = m_uccdClient->getFpsSourcesJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson(
      QString::fromStdString( *fpsJson ).toUtf8() );
    if ( doc.object().value( "currentApp" ).toString().isEmpty() )
    {
      QMessageBox::warning( dlg, "Cannot Resume",
        "No application is delivering FPS data.\n\n"
        "Start the game or application with the FPS layer active "
        "(UCC_FPS_HOOK=1) before resuming." );
      return;
    }
  }

  QString comp = componentCombo->currentData().toString();
  if ( m_uccdClient->resumeAutoOC( resumeMode, comp.toStdString(), 0,
      stepSizeSpin->value(), maxOffsetSpin->value(), stabilitySpin->value() * 1000 ) )
  {
    startBtn->setEnabled( false );
    resumeBtn->setEnabled( false );
    pauseBtn->setEnabled( true );
    stopBtn->setEnabled( true );
    componentCombo->setEnabled( false );
    statusLabel->setText( "Resuming auto overclock scan..." );
    coreValueLabel->setText( "\u2014" );
    vramValueLabel->setText( "\u2014" );
    progressBar->setRange( 0, 0 );
    progressBar->setFormat( "Resuming..." );
  }
  else
  {
    statusLabel->setText( "Failed to resume. Check that the daemon is running." );
  }
}

void GpuProfileTab::handleResumeAutoUV( QDialog *dlg,
                                        QPushButton *startBtn, QPushButton *resumeBtn,
                                        QPushButton *pauseBtn, QPushButton *stopBtn,
                                        QLabel *statusLabel,
                                        QLabel *capValueLabel, QProgressBar *progressBar,
                                        QCheckBox *targetFpsCheck, QSpinBox *targetFpsSpin,
                                        QCheckBox *extendedValCheck, QCheckBox *powerLimitCheck,
                                        QSpinBox *stepSizeSpin, QSpinBox *maxOffsetSpin,
                                        QSpinBox *stabilitySpin )
{
  // Read checkpoint metadata
  QString expectedApp;
  QString suspendReason;
  if ( auto uvProgress = m_uccdClient->getAutoUndervoltProgress() )
  {
    QJsonDocument pdoc = QJsonDocument::fromJson(
      QString::fromStdString( *uvProgress ).toUtf8() );
    QJsonObject pobj = pdoc.object();
    expectedApp = pobj.value( "checkpointApp" ).toString();
    suspendReason = pobj.value( "suspendReason" ).toString();
  }

  std::string resumeMode = askResumeMode( dlg, "Resume Undervolt Scan", suspendReason );
  if ( resumeMode.empty() )
    return;  // user cancelled

  // Verify the expected application is running and delivering FPS
  if ( auto fpsJson = m_uccdClient->getFpsSourcesJSON() )
  {
    QJsonDocument doc = QJsonDocument::fromJson(
      QString::fromStdString( *fpsJson ).toUtf8() );
    QString currentApp = doc.object().value( "currentApp" ).toString();
    if ( currentApp.isEmpty() )
    {
      QString msg = "No application is delivering FPS data.\n\n";
      if ( !expectedApp.isEmpty() )
        msg += QString( "Start '%1' with the FPS layer active "
                        "(UCC_FPS_HOOK=1) before resuming." ).arg( expectedApp );
      else
        msg += "Start the game or application with the FPS layer active "
               "(UCC_FPS_HOOK=1) before resuming.";
      QMessageBox::warning( dlg, "Cannot Resume", msg );
      return;
    }
    if ( !expectedApp.isEmpty() && currentApp != expectedApp )
    {
      QMessageBox::warning( dlg, "Cannot Resume",
        QString( "The checkpoint was saved for '%1', but '%2' is currently running.\n\n"
                 "Start '%1' or click Start to begin a new scan for '%2'." )
          .arg( expectedApp, currentApp ) );
      return;
    }
  }

  if ( m_uccdClient->resumeAutoUndervolt(
         resumeMode, 0, targetFpsCheck->isChecked(), targetFpsSpin->value(),
         extendedValCheck->isChecked(), powerLimitCheck->isChecked(),
      stepSizeSpin->value(), maxOffsetSpin->value(), stabilitySpin->value() * 1000 ) )
  {
    startBtn->setEnabled( false );
    resumeBtn->setEnabled( false );
    pauseBtn->setEnabled( true );
    stopBtn->setEnabled( true );
    statusLabel->setText( "Resuming auto undervolt scan..." );
    capValueLabel->setText( "\u2014" );
    progressBar->setRange( 0, 0 );
    progressBar->setFormat( "Resuming..." );
  }
  else
  {
    statusLabel->setText(
      "Failed to resume. Ensure a game is running with the FPS layer active, "
      "UCC_FPS_HOOK=1 is set for that process, and the daemon is connected." );
  }
}

bool GpuProfileTab::ensureOverclockWarningAcknowledged()
{
  if ( !m_ocAvailable )
    return false;

  if ( isOverclockWarningAcknowledged() )
    return true;

  return showOverclockWarningDialog();
}

bool GpuProfileTab::isOverclockWarningAcknowledged() const
{
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  // Canonical key + migration cleanup for legacy lowercase variant.
  const QVariant canonical = settings.value( "gpu/ocWarningAcknowledged", QVariant() );
  const QVariant legacy = settings.value( "gpu/ocwarningacknowledged", QVariant() );

  if ( canonical.isValid() )
  {
    if ( legacy.isValid() )
    {
      settings.remove( "gpu/ocwarningacknowledged" );
      settings.sync();
    }
    return canonical.toBool();
  }

  if ( legacy.isValid() )
  {
    const bool migrated = legacy.toBool();
    settings.setValue( "gpu/ocWarningAcknowledged", migrated );
    settings.remove( "gpu/ocwarningacknowledged" );
    settings.sync();
    return migrated;
  }

  return false;
}

void GpuProfileTab::setOverclockWarningAcknowledged( bool acknowledged )
{
  QSettings settings( QDir::homePath() + "/.config/uccrc", QSettings::IniFormat );
  settings.setValue( "gpu/ocWarningAcknowledged", acknowledged );
  settings.remove( "gpu/ocwarningacknowledged" );
  settings.sync();
}

bool GpuProfileTab::showOverclockWarningDialog()
{
  QDialog dialog( this );
  dialog.setWindowTitle( "GPU Overclocking Warning" );
  dialog.setModal( true );

  QVBoxLayout *layout = new QVBoxLayout( &dialog );
  QLabel *msg = new QLabel(
    "Overclocking your GPU may cause instability, crashes, or hardware damage. "
    "Changes take effect immediately when applied." );
  msg->setWordWrap( true );
  layout->addWidget( msg );

  QCheckBox *ack = new QCheckBox( "I understand" );
  layout->addWidget( ack );

  QDialogButtonBox *buttons = new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel );
  QPushButton *okButton = buttons->button( QDialogButtonBox::Ok );
  if ( okButton )
    okButton->setEnabled( false );

  connect( ack, &QCheckBox::toggled, this, [okButton]( bool checked ) {
    if ( okButton )
      okButton->setEnabled( checked );
  } );
  connect( buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept );
  connect( buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject );
  layout->addWidget( buttons );

  if ( dialog.exec() == QDialog::Accepted && ack->isChecked() )
  {
    setOverclockWarningAcknowledged( true );
    return true;
  }

  return false;
}

} // namespace ucc
