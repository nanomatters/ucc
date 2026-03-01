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
#include <QFrame>
#include <QGroupBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QMainWindow>
#include <QStatusBar>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QMessageBox>

namespace ucc
{

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

  // Initial hardware state read
  if ( m_ocAvailable )
    refreshOCState();
}

// ── UI construction ─────────────────────────────────────────────────

void GpuProfileTab::setupUI()
{
  QVBoxLayout *mainLayout = new QVBoxLayout( this );
  mainLayout->setContentsMargins( 0, 0, 0, 0 );
  mainLayout->setSpacing( 0 );

  // ── Top bar: GPU profile selection ──
  QHBoxLayout *selectLayout = new QHBoxLayout();
  QLabel *profileLabel = new QLabel( "Active GPU Profile:" );
  profileLabel->setStyleSheet( "font-weight: bold;" );
  m_gpuProfileCombo = new QComboBox();
  m_gpuProfileCombo->setEditable( true );
  m_gpuProfileCombo->setInsertPolicy( QComboBox::NoInsert );

  // Load custom GPU profiles
  for ( const auto &v : m_profileManager->customGpuProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
    m_gpuProfileCombo->addItem( name, id );
  }

  m_applyButton = new QPushButton( "Apply" );
  m_applyButton->setMaximumWidth( 80 );
  m_applyButton->setEnabled( false );

  m_saveButton = new QPushButton( "Save" );
  m_saveButton->setMaximumWidth( 80 );
  m_saveButton->setEnabled( false );

  m_copyButton = new QPushButton( "Copy" );
  m_copyButton->setMaximumWidth( 60 );
  m_copyButton->setEnabled( false );

  m_removeButton = new QPushButton( "Remove" );
  m_removeButton->setMaximumWidth( 70 );

  selectLayout->addWidget( profileLabel );
  selectLayout->addWidget( m_gpuProfileCombo, 1 );
  selectLayout->addWidget( m_applyButton );
  selectLayout->addWidget( m_saveButton );
  selectLayout->addWidget( m_copyButton );
  selectLayout->addWidget( m_removeButton );
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

  // Warning label
  m_warningLabel = new QLabel(
    "<span style='color: #e65100;'>"
    "&#9888; <b>Warning:</b> Overclocking your GPU may cause system instability, crashes, or "
    "hardware damage. Use at your own risk. Changes take effect immediately when applied.</span>" );
  m_warningLabel->setWordWrap( true );
  m_warningLabel->setVisible( m_ocAvailable );
  contentLayout->addWidget( m_warningLabel );

  // === GPU INFO SECTION ===
  QGroupBox *infoGroup = new QGroupBox( "GPU Information" );
  infoGroup->setVisible( m_ocAvailable );
  QGridLayout *infoLayout = new QGridLayout( infoGroup );
  m_gpuNameLabel = new QLabel( "—" );
  m_tempLabel = new QLabel( "—" );
  m_powerDrawLabel = new QLabel( "—" );
  infoLayout->addWidget( new QLabel( "GPU:" ), 0, 0 );
  infoLayout->addWidget( m_gpuNameLabel, 0, 1 );
  infoLayout->addWidget( new QLabel( "Temperature:" ), 0, 2 );
  infoLayout->addWidget( m_tempLabel, 0, 3 );
  infoLayout->addWidget( new QLabel( "Power draw:" ), 0, 4 );
  infoLayout->addWidget( m_powerDrawLabel, 0, 5 );
  infoLayout->setColumnStretch( 1, 1 );
  contentLayout->addWidget( infoGroup );

  // === CLOCK OFFSETS (grouped by P-state) ===
  QGroupBox *offsetsGroup = new QGroupBox( "Clock Offsets (per P-state)" );
  offsetsGroup->setVisible( m_ocAvailable );
  m_pstatesLayout = new QVBoxLayout( offsetsGroup );
  m_pstatesLayout->setSpacing( 8 );
  contentLayout->addWidget( offsetsGroup );

  // === GPU LOCKED CLOCKS ===
  QGroupBox *gpuLockedGroup = new QGroupBox( "GPU Core Locked Clocks" );
  gpuLockedGroup->setVisible( m_ocAvailable );
  QVBoxLayout *gpuLockedLayout = new QVBoxLayout( gpuLockedGroup );

  m_gpuLockedClocksEnable = new QCheckBox( "Enable GPU locked clocks" );
  gpuLockedLayout->addWidget( m_gpuLockedClocksEnable );

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

  m_gpuLockedRangeLabel = new QLabel();
  gpuLockedLayout->addWidget( m_gpuLockedRangeLabel );
  contentLayout->addWidget( gpuLockedGroup );

  // === VRAM LOCKED CLOCKS ===
  QGroupBox *vramLockedGroup = new QGroupBox( "VRAM Locked Clocks" );
  vramLockedGroup->setVisible( m_ocAvailable );
  QVBoxLayout *vramLockedLayout = new QVBoxLayout( vramLockedGroup );

  m_vramLockedClocksEnable = new QCheckBox( "Enable VRAM locked clocks" );
  vramLockedLayout->addWidget( m_vramLockedClocksEnable );

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

  m_vramLockedRangeLabel = new QLabel();
  vramLockedLayout->addWidget( m_vramLockedRangeLabel );
  contentLayout->addWidget( vramLockedGroup );

  // === POWER LIMIT ===
  QGroupBox *powerGroup = new QGroupBox( "GPU Power Limit" );
  powerGroup->setVisible( m_ocAvailable );
  QHBoxLayout *powerLayout = new QHBoxLayout( powerGroup );
  powerLayout->addWidget( new QLabel( "Power limit:" ) );
  m_powerLimitSlider = new QSlider( Qt::Horizontal );
  m_powerLimitSlider->setMinimum( 0 );
  m_powerLimitSlider->setMaximum( 600 );
  m_powerLimitValue = new QLabel( "0 W" );
  m_powerLimitValue->setMinimumWidth( 60 );
  powerLayout->addWidget( m_powerLimitSlider, 1 );
  powerLayout->addWidget( m_powerLimitValue );
  contentLayout->addWidget( powerGroup );

  m_powerLimitRangeLabel = new QLabel();
  m_powerLimitRangeLabel->setVisible( m_ocAvailable );
  contentLayout->addWidget( m_powerLimitRangeLabel );

  // Action buttons
  QHBoxLayout *actionLayout = new QHBoxLayout();
  m_refreshButton = new QPushButton( "Refresh" );
  m_refreshButton->setMaximumWidth( 100 );
  m_refreshButton->setVisible( m_ocAvailable );
  m_resetButton = new QPushButton( "Reset All to Defaults" );
  m_resetButton->setMaximumWidth( 180 );
  m_resetButton->setVisible( m_ocAvailable );
  actionLayout->addWidget( m_refreshButton );
  actionLayout->addWidget( m_resetButton );
  actionLayout->addStretch();
  contentLayout->addLayout( actionLayout );

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
  connect( m_applyButton, &QPushButton::clicked, this, &GpuProfileTab::applyRequested );
  connect( m_saveButton, &QPushButton::clicked, this, &GpuProfileTab::saveRequested );
  connect( m_copyButton, &QPushButton::clicked, this, &GpuProfileTab::copyRequested );
  connect( m_removeButton, &QPushButton::clicked, this, &GpuProfileTab::removeRequested );
  connect( m_refreshButton, &QPushButton::clicked, this, &GpuProfileTab::onRefreshClicked );
  connect( m_resetButton, &QPushButton::clicked, this, &GpuProfileTab::onResetClicked );

  // Locked clocks enable/disable
  auto connectLockedClocks = [this]( QCheckBox *cb, QSlider *minS, QSlider *maxS,
                                     QSpinBox *minSp, QSpinBox *maxSp ) {
    connect( cb, &QCheckBox::toggled, this, [minS, maxS, minSp, maxSp, this]( bool en ) {
      minS->setEnabled( en ); maxS->setEnabled( en );
      minSp->setEnabled( en ); maxSp->setEnabled( en );
      emit changed();
    } );
  };

  if ( m_gpuLockedClocksEnable )
  {
    connectLockedClocks( m_gpuLockedClocksEnable,
                         m_gpuLockedMinSlider, m_gpuLockedMaxSlider,
                         m_gpuLockedMinSpin, m_gpuLockedMaxSpin );
    // Bidirectional slider <-> spinbox sync
    connect( m_gpuLockedMinSlider, &QSlider::valueChanged, m_gpuLockedMinSpin, &QSpinBox::setValue );
    connect( m_gpuLockedMinSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_gpuLockedMinSlider, &QSlider::setValue );
    connect( m_gpuLockedMaxSlider, &QSlider::valueChanged, m_gpuLockedMaxSpin, &QSpinBox::setValue );
    connect( m_gpuLockedMaxSpin, QOverload< int >::of( &QSpinBox::valueChanged ), m_gpuLockedMaxSlider, &QSlider::setValue );
    // Enforce min <= max
    connect( m_gpuLockedMinSlider, &QSlider::valueChanged, this, [this]( int v ) {
      if ( v > m_gpuLockedMaxSlider->value() ) m_gpuLockedMaxSlider->setValue( v );
      emit changed();
    } );
    connect( m_gpuLockedMaxSlider, &QSlider::valueChanged, this, [this]( int v ) {
      if ( v < m_gpuLockedMinSlider->value() ) m_gpuLockedMinSlider->setValue( v );
      emit changed();
    } );

    // Initial disable
    m_gpuLockedMinSlider->setEnabled( false );
    m_gpuLockedMaxSlider->setEnabled( false );
    m_gpuLockedMinSpin->setEnabled( false );
    m_gpuLockedMaxSpin->setEnabled( false );
  }

  if ( m_vramLockedClocksEnable )
  {
    connectLockedClocks( m_vramLockedClocksEnable,
                         m_vramLockedMinSlider, m_vramLockedMaxSlider,
                         m_vramLockedMinSpin, m_vramLockedMaxSpin );
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

    m_vramLockedMinSlider->setEnabled( false );
    m_vramLockedMaxSlider->setEnabled( false );
    m_vramLockedMinSpin->setEnabled( false );
    m_vramLockedMaxSpin->setEnabled( false );
  }

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

  for ( const auto &v : m_profileManager->customGpuProfilesData() )
  {
    QJsonObject o = v.toObject();
    QString id = o["id"].toString();
    QString name = o["name"].toString();
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

  if ( m_applyButton )   m_applyButton->setEnabled( uccdConnected && m_ocAvailable );
  if ( m_saveButton )    m_saveButton->setEnabled( hasSelection );
  if ( m_copyButton )    m_copyButton->setEnabled( hasSelection || m_ocAvailable );
  if ( m_removeButton )  m_removeButton->setEnabled( hasSelection );

  // Allow renaming custom profiles
  if ( m_gpuProfileCombo && m_gpuProfileCombo->lineEdit() )
    m_gpuProfileCombo->lineEdit()->setReadOnly( !hasSelection );
}

void GpuProfileTab::refreshOCState()
{
  if ( !m_ocAvailable || !m_uccdClient )
    return;

  auto stateOpt = m_uccdClient->getNvidiaOCState( 0 );
  if ( !stateOpt )
    return;

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( *stateOpt ) );
  if ( !doc.isObject() )
    return;

  QJsonObject state = doc.object();

  // Update info labels
  m_gpuNameLabel->setText( state["gpuName"].toString( "Unknown" ) );
  m_tempLabel->setText( QString::number( state["tempC"].toInt() ) + " °C" );
  m_powerDrawLabel->setText( QString::number( state["powerDrawW"].toDouble(), 'f', 1 ) + " W" );

  // Power limit range
  m_powerMinW = state["powerMinW"].toDouble();
  m_powerMaxW = state["powerMaxW"].toDouble();
  m_powerDefaultW = state["powerDefaultW"].toDouble();

  if ( m_powerMaxW > 0 )
  {
    m_powerLimitSlider->blockSignals( true );
    m_powerLimitSlider->setMinimum( static_cast< int >( m_powerMinW ) );
    m_powerLimitSlider->setMaximum( static_cast< int >( m_powerMaxW ) );
    m_powerLimitSlider->setValue( static_cast< int >( state["powerLimitW"].toDouble() ) );
    m_powerLimitSlider->blockSignals( false );
    m_powerLimitValue->setText( QString::number( state["powerLimitW"].toDouble(), 'f', 0 ) + " W" );
    m_powerLimitRangeLabel->setText(
      QString( "Range: %1 – %2 W  (Default: %3 W)" )
        .arg( m_powerMinW, 0, 'f', 0 )
        .arg( m_powerMaxW, 0, 'f', 0 )
        .arg( m_powerDefaultW, 0, 'f', 0 ) );
  }

  // GPU clock range for locked clocks
  if ( state.contains( "gpuClockRange" ) )
  {
    QJsonObject r = state["gpuClockRange"].toObject();
    int lo = r["min"].toInt(), hi = r["max"].toInt();
    m_gpuLockedMinSlider->setMinimum( lo );
    m_gpuLockedMinSlider->setMaximum( hi );
    m_gpuLockedMaxSlider->setMinimum( lo );
    m_gpuLockedMaxSlider->setMaximum( hi );
    m_gpuLockedMinSpin->setMinimum( lo );
    m_gpuLockedMinSpin->setMaximum( hi );
    m_gpuLockedMaxSpin->setMinimum( lo );
    m_gpuLockedMaxSpin->setMaximum( hi );
    m_gpuLockedRangeLabel->setText( QString( "Range: %1 – %2 MHz" ).arg( lo ).arg( hi ) );

    // Set defaults
    m_gpuLockedMinSlider->setValue( lo );
    m_gpuLockedMaxSlider->setValue( hi );
  }

  if ( state.contains( "vramClockRange" ) )
  {
    QJsonObject r = state["vramClockRange"].toObject();
    int lo = r["min"].toInt(), hi = r["max"].toInt();
    m_vramLockedMinSlider->setMinimum( lo );
    m_vramLockedMinSlider->setMaximum( hi );
    m_vramLockedMaxSlider->setMinimum( lo );
    m_vramLockedMaxSlider->setMaximum( hi );
    m_vramLockedMinSpin->setMinimum( lo );
    m_vramLockedMinSpin->setMaximum( hi );
    m_vramLockedMaxSpin->setMinimum( lo );
    m_vramLockedMaxSpin->setMaximum( hi );
    m_vramLockedRangeLabel->setText( QString( "Range: %1 – %2 MHz" ).arg( lo ).arg( hi ) );

    m_vramLockedMinSlider->setValue( lo );
    m_vramLockedMaxSlider->setValue( hi );
  }

  // Load existing locked clocks if applied
  if ( state.contains( "gpuLockedClocks" ) )
  {
    QJsonObject lc = state["gpuLockedClocks"].toObject();
    m_gpuLockedClocksEnable->setChecked( true );
    m_gpuLockedMinSlider->setValue( lc["min"].toInt() );
    m_gpuLockedMaxSlider->setValue( lc["max"].toInt() );
  }
  else
  {
    m_gpuLockedClocksEnable->setChecked( false );
  }

  if ( state.contains( "vramLockedClocks" ) )
  {
    QJsonObject lc = state["vramLockedClocks"].toObject();
    m_vramLockedClocksEnable->setChecked( true );
    m_vramLockedMinSlider->setValue( lc["min"].toInt() );
    m_vramLockedMaxSlider->setValue( lc["max"].toInt() );
  }
  else
  {
    m_vramLockedClocksEnable->setChecked( false );
  }

  // Populate P-state offset rows
  clearPStateWidgets();
  if ( state.contains( "pstates" ) )
    populatePStates( state["pstates"].toArray() );

  // Feature support
  bool offsetsSupported = state["offsetsSupported"].toBool( false );
  bool lockedSupported = state["lockedClocksSupported"].toBool( false );

  if ( m_gpuLockedClocksEnable )
    m_gpuLockedClocksEnable->setVisible( lockedSupported );
  if ( m_vramLockedClocksEnable )
    m_vramLockedClocksEnable->setVisible( lockedSupported );

  // Disable offset sliders if not supported
  for ( auto &grp : m_pstateGroups )
  {
    grp.gpuRow.slider->setEnabled( offsetsSupported );
    grp.gpuRow.spinBox->setEnabled( offsetsSupported );
    grp.vramRow.slider->setEnabled( offsetsSupported );
    grp.vramRow.spinBox->setEnabled( offsetsSupported );
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
      QLabel *label = new QLabel( QString( "GPU Core (%1–%2 MHz)" ).arg( minMHz ).arg( maxMHz ) );

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
      QLabel *label = new QLabel( QString( "VRAM (%1–%2 MHz)" ).arg( minMHz ).arg( maxMHz ) );

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
  QJsonArray offsets;
  for ( const auto &grp : m_pstateGroups )
  {
    QJsonObject o;
    o["pstate"] = static_cast< int >( grp.pstate );
    if ( grp.gpuRow.slider )
      o["gpuOffsetMHz"] = grp.gpuRow.slider->value();
    if ( grp.vramRow.slider )
      o["vramOffsetMHz"] = grp.vramRow.slider->value();
    offsets.append( o );
  }
  root["offsets"] = offsets;

  // GPU locked clocks
  if ( m_gpuLockedClocksEnable )
  {
    QJsonObject lc;
    lc["enabled"] = m_gpuLockedClocksEnable->isChecked();
    lc["min"] = m_gpuLockedMinSlider->value();
    lc["max"] = m_gpuLockedMaxSlider->value();
    root["gpuLockedClocks"] = lc;
  }

  // VRAM locked clocks
  if ( m_vramLockedClocksEnable )
  {
    QJsonObject lc;
    lc["enabled"] = m_vramLockedClocksEnable->isChecked();
    lc["min"] = m_vramLockedMinSlider->value();
    lc["max"] = m_vramLockedMaxSlider->value();
    root["vramLockedClocks"] = lc;
  }

  // Power limit
  if ( m_powerLimitSlider )
    root["powerLimitW"] = m_powerLimitSlider->value();

  QJsonDocument doc( root );
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

  // Backwards compat: load old separate gpuCoreOffsets/vramOffsets format
  if ( obj.contains( "gpuCoreOffsets" ) )
  {
    QJsonArray offsets = obj["gpuCoreOffsets"].toArray();
    for ( const auto &v : offsets )
    {
      QJsonObject o = v.toObject();
      int ps = o["pstate"].toInt();
      int off = o["offsetMHz"].toInt();
      for ( auto &grp : m_pstateGroups )
      {
        if ( static_cast< int >( grp.pstate ) == ps && grp.gpuRow.slider )
        {
          grp.gpuRow.slider->blockSignals( true );
          grp.gpuRow.slider->setValue( off );
          grp.gpuRow.slider->blockSignals( false );
          grp.gpuRow.spinBox->blockSignals( true );
          grp.gpuRow.spinBox->setValue( off );
          grp.gpuRow.spinBox->blockSignals( false );
          break;
        }
      }
    }
  }
  if ( obj.contains( "vramOffsets" ) )
  {
    QJsonArray offsets = obj["vramOffsets"].toArray();
    for ( const auto &v : offsets )
    {
      QJsonObject o = v.toObject();
      int ps = o["pstate"].toInt();
      int off = o["offsetMHz"].toInt();
      for ( auto &grp : m_pstateGroups )
      {
        if ( static_cast< int >( grp.pstate ) == ps && grp.vramRow.slider )
        {
          grp.vramRow.slider->blockSignals( true );
          grp.vramRow.slider->setValue( off );
          grp.vramRow.slider->blockSignals( false );
          grp.vramRow.spinBox->blockSignals( true );
          grp.vramRow.spinBox->setValue( off );
          grp.vramRow.spinBox->blockSignals( false );
          break;
        }
      }
    }
  }

  // Load GPU locked clocks
  if ( m_gpuLockedClocksEnable && obj.contains( "gpuLockedClocks" ) )
  {
    QJsonObject lc = obj["gpuLockedClocks"].toObject();
    m_gpuLockedClocksEnable->blockSignals( true );
    m_gpuLockedClocksEnable->setChecked( lc["enabled"].toBool( false ) );
    m_gpuLockedClocksEnable->blockSignals( false );

    bool en = lc["enabled"].toBool( false );
    m_gpuLockedMinSlider->setEnabled( en );
    m_gpuLockedMaxSlider->setEnabled( en );
    m_gpuLockedMinSpin->setEnabled( en );
    m_gpuLockedMaxSpin->setEnabled( en );

    m_gpuLockedMinSlider->blockSignals( true );
    m_gpuLockedMinSlider->setValue( lc["min"].toInt() );
    m_gpuLockedMinSlider->blockSignals( false );
    m_gpuLockedMaxSlider->blockSignals( true );
    m_gpuLockedMaxSlider->setValue( lc["max"].toInt() );
    m_gpuLockedMaxSlider->blockSignals( false );
  }

  // Load VRAM locked clocks
  if ( m_vramLockedClocksEnable && obj.contains( "vramLockedClocks" ) )
  {
    QJsonObject lc = obj["vramLockedClocks"].toObject();
    m_vramLockedClocksEnable->blockSignals( true );
    m_vramLockedClocksEnable->setChecked( lc["enabled"].toBool( false ) );
    m_vramLockedClocksEnable->blockSignals( false );

    bool en = lc["enabled"].toBool( false );
    m_vramLockedMinSlider->setEnabled( en );
    m_vramLockedMaxSlider->setEnabled( en );
    m_vramLockedMinSpin->setEnabled( en );
    m_vramLockedMaxSpin->setEnabled( en );

    m_vramLockedMinSlider->blockSignals( true );
    m_vramLockedMinSlider->setValue( lc["min"].toInt() );
    m_vramLockedMinSlider->blockSignals( false );
    m_vramLockedMaxSlider->blockSignals( true );
    m_vramLockedMaxSlider->setValue( lc["max"].toInt() );
    m_vramLockedMaxSlider->blockSignals( false );
  }

  // Load power limit
  if ( obj.contains( "powerLimitW" ) && m_powerLimitSlider )
  {
    m_powerLimitSlider->blockSignals( true );
    m_powerLimitSlider->setValue( static_cast< int >( obj["powerLimitW"].toDouble() ) );
    m_powerLimitSlider->blockSignals( false );
    m_powerLimitValue->setText( QString::number( obj["powerLimitW"].toDouble(), 'f', 0 ) + " W" );
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
    if ( m_uccdClient->resetNvidiaGpuOCAll( 0 ) )
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

} // namespace ucc
