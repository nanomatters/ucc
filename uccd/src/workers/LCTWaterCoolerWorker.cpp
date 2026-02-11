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

#include "workers/LCTWaterCoolerWorker.hpp"
#include "UccDBusService.hpp"
#include "workers/DaemonWorker.hpp" // for ucc::wDebug

#include <QBluetoothLocalDevice>
#include <QProcess>
#include <QThread>
#include <iostream>
#include <syslog.h>

// ── Constants ───────────────────────────────────────────────────────
static constexpr int TICK_INTERVAL_MS = 1000;
static constexpr int FAST_RECONNECT_TIMEOUT_SECONDS = 8;
static constexpr int DISCOVERY_RETRY_SECONDS = 3;
static constexpr int DISCOVERY_TIMEOUT_SECONDS = 15;
static constexpr int CONNECTION_TIMEOUT_SECONDS = 12;
static constexpr int ERROR_RETRY_BASE_SECONDS = 5;
static constexpr int ERROR_RETRY_MAX_SECONDS = 120;
static constexpr int ADAPTER_RESET_FAILURE_THRESHOLD = 5;
static constexpr int INITIAL_FAN_SPEED_PERCENT = 10;

// ── Static UUID constants ───────────────────────────────────────────
const QString LCTWaterCoolerWorker::NORDIC_UART_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
const QString LCTWaterCoolerWorker::NORDIC_UART_CHAR_TX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
const QString LCTWaterCoolerWorker::NORDIC_UART_CHAR_RX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

const QBluetoothUuid LCTWaterCoolerWorker::NORDIC_UART_SERVICE_UUID_OBJ = QBluetoothUuid( NORDIC_UART_SERVICE_UUID );
const QBluetoothUuid LCTWaterCoolerWorker::NORDIC_UART_CHAR_TX_OBJ = QBluetoothUuid( NORDIC_UART_CHAR_TX );
const QBluetoothUuid LCTWaterCoolerWorker::NORDIC_UART_CHAR_RX_OBJ = QBluetoothUuid( NORDIC_UART_CHAR_RX );

// ── Construction / Destruction ──────────────────────────────────────

LCTWaterCoolerWorker::LCTWaterCoolerWorker( UccDBusData& dbusData, StatusCallback statusCallback, QObject* parent )
    : QObject( parent ), m_dbusData( dbusData ), m_lastDiscoveryStart( std::chrono::steady_clock::now() ),
      m_statusCallback( std::move( statusCallback ) )
{
  syslog( LOG_INFO, "LCTWaterCoolerWorker: constructed" );

  // Timer for periodic tick — fires on the main thread event loop
  m_tickTimer = new QTimer( this );
  m_tickTimer->setInterval( TICK_INTERVAL_MS );
  connect( m_tickTimer, &QTimer::timeout, this, &LCTWaterCoolerWorker::onTick );

  // Initialize BLE discovery agent
  auto adapters = QBluetoothLocalDevice::allDevices();
  if ( adapters.isEmpty() )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: no Bluetooth adapters found" );
    return;
  }

  m_deviceDiscoveryAgent = new QBluetoothDeviceDiscoveryAgent( this );
  if ( not m_deviceDiscoveryAgent )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: failed to create discovery agent" );
    return;
  }

  syslog( LOG_INFO, "LCTWaterCoolerWorker: found %d Bluetooth adapter(s)", static_cast< int >( adapters.size() ) );
  for( const auto& adapter : adapters )
    syslog( LOG_INFO, "LCTWaterCoolerWorker: adapter %s (%s)", adapter.name().toStdString().c_str(),
            adapter.address().toString().toStdString().c_str() );

  connect( m_deviceDiscoveryAgent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
           &LCTWaterCoolerWorker::onDeviceDiscovered );
  connect( m_deviceDiscoveryAgent, &QBluetoothDeviceDiscoveryAgent::finished, this,
           &LCTWaterCoolerWorker::onDiscoveryFinished );
  connect( m_deviceDiscoveryAgent, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this,
           []( QBluetoothDeviceDiscoveryAgent::Error error )
           { syslog( LOG_ERR, "LCTWaterCoolerWorker: discovery error %d", static_cast< int >( error ) ); } );
}

LCTWaterCoolerWorker::~LCTWaterCoolerWorker()
{
  stop();
  cleanupBleController();
  stopDiscoveryInternal();
}

// ── Start / Stop ────────────────────────────────────────────────────

void LCTWaterCoolerWorker::start()
{
  if ( m_tickTimer and not m_tickTimer->isActive() )
  {
    syslog( LOG_INFO, "LCTWaterCoolerWorker: starting, beginning automatic discovery" );
    requestStartDiscovery();
    m_tickTimer->start();
  }
}

void LCTWaterCoolerWorker::stop()
{
  if ( m_tickTimer and m_tickTimer->isActive() )
  {
    m_tickTimer->stop();
    m_state = WaterCoolerState::Disconnected;
    syslog( LOG_INFO, "LCTWaterCoolerWorker: stopped" );
  }
}

// ── Periodic tick (runs on the main thread) ─────────────────────────

void LCTWaterCoolerWorker::onTick()
{
  // Respect external scanning enable/disable flag
  if ( not m_dbusData.waterCoolerScanningEnabled )
  {
    m_dbusData.waterCoolerAvailable = false;
    m_dbusData.waterCoolerConnected = m_isConnected.load();
    return;
  }

  // Keep DBus connected flag in sync
  m_dbusData.waterCoolerConnected = m_isConnected.load();

  // Notify on state transitions
  if ( m_state != m_previousState )
  {
    ucc::wDebug( "LCTWaterCoolerWorker: state %d -> %d", static_cast< int >( m_previousState ),
                 static_cast< int >( m_state ) );
    if ( m_statusCallback )
      m_statusCallback( m_state );
    m_previousState = m_state;
  }

  switch( m_state )
  {
  case WaterCoolerState::Disconnected:
    handleDisconnected();
    break;

  case WaterCoolerState::Discovering:
    handleDiscovering();
    break;

  case WaterCoolerState::Reconnecting:
    handleReconnecting();
    break;

  case WaterCoolerState::Connecting:
    handleConnecting();
    break;

  case WaterCoolerState::Connected:
    handleConnected();
    break;

  case WaterCoolerState::Error:
    handleError();
    break;
  }
}

// ── State handlers (all direct calls — same thread, no dispatch) ────

void LCTWaterCoolerWorker::handleDisconnected()
{
  setAvailableFlag( false );

  // Fast-path: if we have a known device, try direct reconnect immediately
  if ( m_hasKnownDevice )
  {
    syslog( LOG_INFO, "LCTWaterCoolerWorker: attempting fast reconnect to known device" );
    m_state = WaterCoolerState::Reconnecting;
    m_lastDiscoveryStart = std::chrono::steady_clock::now();

    if ( not connectToKnownDevice() )
    {
      syslog( LOG_WARNING, "LCTWaterCoolerWorker: fast reconnect failed, falling back to discovery" );
      ++m_consecutiveFailures;
      requestStartDiscovery();
    }
    return;
  }

  // No known device — throttle discovery retries
  const auto elapsed = secondsSinceLastDiscovery();
  if ( elapsed > DISCOVERY_RETRY_SECONDS )
  {
    syslog( LOG_INFO, "LCTWaterCoolerWorker: restarting discovery after disconnect" );
    requestStartDiscovery();
  }
}

void LCTWaterCoolerWorker::handleDiscovering()
{
  const bool active = m_deviceDiscoveryAgent and m_deviceDiscoveryAgent->isActive();

  if ( not active )
  {
    // Discovery finished — check for found devices
    syslog( LOG_INFO, "LCTWaterCoolerWorker: discovery finished, checking for devices" );

    if ( not m_discoveredDevices.isEmpty() )
    {
      setAvailableFlag( true );
      syslog( LOG_INFO, "LCTWaterCoolerWorker: found %d device(s), connecting to: %s",
              static_cast< int >( m_discoveredDevices.size() ),
              m_discoveredDevices.first().name.toStdString().c_str() );
      requestConnectToDevice( m_discoveredDevices.first().uuid );
    }
    else
    {
      syslog( LOG_INFO, "LCTWaterCoolerWorker: no LCT devices found, will retry" );
      setAvailableFlag( false );
      m_state = WaterCoolerState::Disconnected;
    }
    return;
  }

  // Still discovering — enforce a hard timeout
  if ( secondsSinceLastDiscovery() > DISCOVERY_TIMEOUT_SECONDS )
  {
    syslog( LOG_WARNING, "LCTWaterCoolerWorker: discovery timeout, stopping" );
    stopDiscoveryInternal();
    m_state = WaterCoolerState::Disconnected;
  }
}

void LCTWaterCoolerWorker::handleReconnecting()
{
  if ( m_isConnected.load() )
  {
    syslog( LOG_INFO, "LCTWaterCoolerWorker: fast reconnect succeeded" );
    m_state = WaterCoolerState::Connected;
    m_consecutiveFailures = 0;
    return;
  }

  if ( secondsSinceLastDiscovery() > FAST_RECONNECT_TIMEOUT_SECONDS )
  {
    syslog( LOG_WARNING, "LCTWaterCoolerWorker: fast reconnect timed out, falling back to discovery" );
    ++m_consecutiveFailures;

    // Cleanup the failed attempt so we start fresh
    cleanupBleController();

    m_state = WaterCoolerState::Disconnected;
    // Force immediate discovery on next cycle
    m_lastDiscoveryStart = std::chrono::steady_clock::time_point{};
  }
}

void LCTWaterCoolerWorker::handleConnecting()
{
  if ( m_isConnected.load() )
  {
    syslog( LOG_INFO, "LCTWaterCoolerWorker: successfully connected to water cooler" );
    m_state = WaterCoolerState::Connected;
    return;
  }

  if ( secondsSinceLastDiscovery() > CONNECTION_TIMEOUT_SECONDS )
  {
    syslog( LOG_WARNING, "LCTWaterCoolerWorker: connection timeout" );
    ++m_consecutiveFailures;
    m_state = WaterCoolerState::Disconnected;
  }
}

void LCTWaterCoolerWorker::handleConnected()
{
  setAvailableFlag( true );

  if ( not m_isConnected.load() )
  {
    syslog( LOG_WARNING, "LCTWaterCoolerWorker: lost connection to water cooler" );
    ++m_consecutiveFailures;
    m_state = WaterCoolerState::Disconnected;
  }
}

void LCTWaterCoolerWorker::handleError()
{
  // Exponential backoff: 5, 10, 20, 40, 80, capped at 120 seconds
  const int backoffSeconds =
      std::min( ERROR_RETRY_BASE_SECONDS * ( 1 << std::min( m_consecutiveFailures, 5 ) ), ERROR_RETRY_MAX_SECONDS );

  if ( secondsSinceLastDiscovery() > backoffSeconds )
  {
    ++m_consecutiveFailures;

    // After persistent failures, intervene at the system level
    if ( m_consecutiveFailures >= ADAPTER_RESET_FAILURE_THRESHOLD )
    {
      syslog( LOG_WARNING, "LCTWaterCoolerWorker: %d consecutive failures, resetting BT adapter",
              m_consecutiveFailures );

      if ( resetBluetoothAdapter() )
      {
        syslog( LOG_INFO, "LCTWaterCoolerWorker: adapter reset succeeded, retrying" );
        m_consecutiveFailures = 0;
      }
      else
      {
        syslog( LOG_ERR, "LCTWaterCoolerWorker: adapter reset failed" );
      }
    }

    syslog( LOG_INFO, "LCTWaterCoolerWorker: retrying after error (backoff was %d s, failures: %d)", backoffSeconds,
            m_consecutiveFailures );
    m_state = WaterCoolerState::Disconnected;
    m_lastDiscoveryStart = std::chrono::steady_clock::now();
  }
}

// ── Connection ready (called from onServiceStateChanged) ────────────

void LCTWaterCoolerWorker::onConnectionReady()
{
  syslog( LOG_INFO, "LCTWaterCoolerWorker: connection ready" );

  if ( m_state == WaterCoolerState::Connecting or m_state == WaterCoolerState::Reconnecting )
    m_state = WaterCoolerState::Connected;

  // Reset failure counter on successful connection
  m_consecutiveFailures = 0;

  // Set safe initial values: pump off, fan at low speed.
  // Use the public API so tracking atomics are updated too.
  setPumpVoltage( static_cast< int >( ucc::PumpVoltage::Off ) );
  setFanSpeed( INITIAL_FAN_SPEED_PERCENT );
  syslog( LOG_INFO, "LCTWaterCoolerWorker: sent initial pump/fan setup commands" );
}

// ── BLE signal handlers ─────────────────────────────────────────────

void LCTWaterCoolerWorker::onDeviceDiscovered( const QBluetoothDeviceInfo& device )
{
  const QString deviceName = device.name();
  const ucc::LCTDeviceModel model = deviceModelFromName( deviceName );

  if ( model == ucc::LCTDeviceModel::Unknown )
    return; // Not an LCT device

  DeviceInfo info;
  info.uuid = device.address().toString();
  info.name = deviceName;
  info.rssi = device.rssi();
  info.deviceInfo = device;

  m_discoveredDevices.append( info );
  std::cout << "[WC-BLE] Discovered LCT device: " << deviceName.toStdString()
            << " (" << info.uuid.toStdString() << ", RSSI " << info.rssi << ")" << std::endl;
  syslog( LOG_INFO, "LCTWaterCoolerWorker: discovered LCT device %s (%s, RSSI %d)", deviceName.toStdString().c_str(),
          info.uuid.toStdString().c_str(), info.rssi );

  // Stop discovery once we find the first LCT device so the state machine can proceed
  std::cout << "[WC-BLE] Scan stopped early - found target device" << std::endl;
  stopDiscoveryInternal();
}

void LCTWaterCoolerWorker::onDiscoveryFinished()
{
  m_isDiscovering = false;
  std::cout << "[WC-BLE] Scan ended, " << m_discoveredDevices.size() << " LCT device(s) found" << std::endl;
  syslog( LOG_INFO, "LCTWaterCoolerWorker: discovery finished, %d LCT device(s) found",
          static_cast< int >( m_discoveredDevices.size() ) );
}

void LCTWaterCoolerWorker::onBleConnected()
{
  syslog( LOG_INFO, "LCTWaterCoolerWorker: BLE connected, starting service discovery" );
  m_bleController->discoverServices();
  // NOTE: m_isConnected is set later in onServiceStateChanged() once UART is ready
}

void LCTWaterCoolerWorker::onBleDisconnected()
{
  m_isConnected = false;
  m_connectedModel = ucc::LCTDeviceModel::LCT21001; // Reset to default
  std::cout << "[WC-BLE] Disconnected from device (state was "
            << static_cast< int >( m_state ) << ")" << std::endl;
  syslog( LOG_WARNING, "LCTWaterCoolerWorker: disconnected from device (state was %d)",
          static_cast< int >( m_state ) );

  // Immediately transition state machine so reconnect logic runs on next tick
  // instead of waiting for the tick to poll-detect the disconnection.
  if ( m_state == WaterCoolerState::Connected or m_state == WaterCoolerState::Connecting or
      m_state == WaterCoolerState::Reconnecting )
  {
    m_state = WaterCoolerState::Disconnected;
  }
}

void LCTWaterCoolerWorker::onServiceDiscoveryFinished()
{
  auto services = m_bleController->services();
  syslog( LOG_INFO, "LCTWaterCoolerWorker: BLE service discovery finished, %d service(s)",
          static_cast< int >( services.size() ) );

  // Find the Nordic UART service
  for( const auto& serviceUuid : services )
  {
    if ( serviceUuid != NORDIC_UART_SERVICE_UUID_OBJ )
      continue;

    m_uartService = m_bleController->createServiceObject( serviceUuid, this );
    if ( not m_uartService )
    {
      syslog( LOG_ERR, "LCTWaterCoolerWorker: failed to create UART service object" );
      break;
    }

    connect( m_uartService, &QLowEnergyService::characteristicRead, this, &LCTWaterCoolerWorker::onCharacteristicRead );
    connect( m_uartService, &QLowEnergyService::characteristicWritten, this,
             &LCTWaterCoolerWorker::onCharacteristicWritten );
    connect( m_uartService, &QLowEnergyService::stateChanged, this, &LCTWaterCoolerWorker::onServiceStateChanged );
    connect( m_uartService, &QLowEnergyService::errorOccurred, this, []( QLowEnergyService::ServiceError error )
             { syslog( LOG_ERR, "LCTWaterCoolerWorker: UART service error %d", static_cast< int >( error ) ); } );

    if ( m_uartService->state() == QLowEnergyService::RemoteServiceDiscovered )
      onServiceStateChanged( QLowEnergyService::RemoteServiceDiscovered );
    else
      m_uartService->discoverDetails();

    return; // Wait for detail discovery
  }

  syslog( LOG_ERR, "LCTWaterCoolerWorker: Nordic UART service not found" );
  m_bleController->disconnectFromDevice();
}

void LCTWaterCoolerWorker::onServiceStateChanged( QLowEnergyService::ServiceState state )
{
  if ( state != QLowEnergyService::RemoteServiceDiscovered )
    return;

  // Locate TX and RX characteristics
  for( const auto& ch : m_uartService->characteristics() )
  {
    if ( ch.uuid() == NORDIC_UART_CHAR_TX_OBJ )
      m_txCharacteristic = ch;
    else if ( ch.uuid() == NORDIC_UART_CHAR_RX_OBJ )
      m_rxCharacteristic = ch;
  }

  if ( not m_txCharacteristic.isValid() or not m_rxCharacteristic.isValid() )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: TX or RX characteristic missing (TX %s, RX %s)",
            m_txCharacteristic.isValid() ? "ok" : "MISSING", m_rxCharacteristic.isValid() ? "ok" : "MISSING" );
    m_isConnected = false;
    m_bleController->disconnectFromDevice();
    return;
  }

  m_connectedModel = deviceModelFromName( m_connectedDeviceInfo.name() );
  syslog( LOG_INFO, "LCTWaterCoolerWorker: connected to %s (model %d)",
          m_connectedDeviceInfo.name().toStdString().c_str(), static_cast< int >( m_connectedModel ) );

  // Enable RX notifications via CCCD
  if ( m_rxCharacteristic.isValid() )
  {
    QBluetoothUuid cccdUuid( QStringLiteral( "00002902-0000-1000-8000-00805f9b34fb" ) );
    auto cccd = m_rxCharacteristic.descriptor( cccdUuid );
    if ( cccd.isValid() )
      m_uartService->writeDescriptor( cccd, QByteArray::fromHex( "0100" ) );
    else
      syslog( LOG_WARNING, "LCTWaterCoolerWorker: RX CCCD not found" );
  }

  // Mark connected ONLY after UART characteristics are verified
  m_isConnected = true;

  // Finish connection setup (state machine + initial commands)
  onConnectionReady();
}

void LCTWaterCoolerWorker::onCharacteristicRead( const QLowEnergyCharacteristic& characteristic,
                                                 const QByteArray& value )
{
  Q_UNUSED( characteristic )
  Q_UNUSED( value )
}

void LCTWaterCoolerWorker::onCharacteristicWritten( const QLowEnergyCharacteristic& characteristic,
                                                    const QByteArray& value )
{
  Q_UNUSED( characteristic )
  Q_UNUSED( value )
}

void LCTWaterCoolerWorker::onBleError( QLowEnergyController::Error error )
{
  std::cout << "[WC-BLE] Error " << static_cast< int >( error )
            << " (state=" << static_cast< int >( m_state ) << ")" << std::endl;
  syslog( LOG_ERR, "LCTWaterCoolerWorker: BLE error %d (state %d)",
          static_cast< int >( error ), static_cast< int >( m_state ) );
  m_isConnected = false;

  // Immediately transition state machine on connection-breaking errors
  // instead of waiting for a timeout in the state handler.
  if ( m_state == WaterCoolerState::Connecting or m_state == WaterCoolerState::Reconnecting or
      m_state == WaterCoolerState::Connected )
  {
    ++m_consecutiveFailures;
    m_state = WaterCoolerState::Disconnected;
  }
}

// ── Thread-safe public API ──────────────────────────────────────────

bool LCTWaterCoolerWorker::isConnected() const
{
  return m_isConnected.load();
}

bool LCTWaterCoolerWorker::setFanSpeed( int dutyCyclePercent )
{
  if ( not m_isConnected.load() or dutyCyclePercent < 0 or dutyCyclePercent > 100 or
      getLastFanSpeed() == dutyCyclePercent )
    return false;

  if ( dutyCyclePercent == 0 )
    return turnOffFan();

  bool result = false;
  if ( QThread::currentThread() == thread() )
    result = setFanSpeedImpl( dutyCyclePercent );
  else
    QMetaObject::invokeMethod( this, "setFanSpeedImpl", Qt::BlockingQueuedConnection, Q_RETURN_ARG( bool, result ),
                               Q_ARG( int, dutyCyclePercent ) );

  if ( result )
    m_lastFanSpeed.store( dutyCyclePercent );
  return result;
}

bool LCTWaterCoolerWorker::setPumpVoltage( int voltage )
{
  const auto pv = static_cast< ucc::PumpVoltage >( voltage );

  if ( not m_isConnected.load() or getLastPumpVoltage() == voltage )
    return false;

  std::cout << "LCTWaterCoolerWorker: setting pump voltage to " << voltage << std::endl;

  if ( pv == ucc::PumpVoltage::Off )
    return turnOffPump();

  bool result = false;
  if ( QThread::currentThread() == thread() )
    result = setPumpVoltageImpl( pv );
  else
    QMetaObject::invokeMethod( this, "setPumpVoltageImpl", Qt::BlockingQueuedConnection, Q_RETURN_ARG( bool, result ),
                               Q_ARG( ucc::PumpVoltage, pv ) );

  if ( result )
    m_lastPumpVoltage.store( voltage );
  return result;
}

int32_t LCTWaterCoolerWorker::getLastPumpVoltage() const
{
  return m_lastPumpVoltage.load();
}

bool LCTWaterCoolerWorker::setLEDColor( int red, int green, int blue, int mode )
{
  if ( not m_isConnected.load() or red < 0 or red > 255 or green < 0 or green > 255 or blue < 0 or blue > 255 )
    return false;

  // Translate int mode to RGBState
  ucc::RGBState rgbState;
  switch( mode )
  {
  case 0:
    rgbState = ucc::RGBState::Static;
    break;
  case 1:
    rgbState = ucc::RGBState::Breathe;
    break;
  case 2:
    rgbState = ucc::RGBState::Colorful;
    break;
  case 3:
    rgbState = ucc::RGBState::BreatheColor;
    break;
  case 4:
    rgbState = ucc::RGBState::Static;
    break; // Temperature mode uses Static
  default:
    return false;
  }

  // Skip if same as last sent values
  const int hwMode = static_cast< int >( rgbState );
  if ( m_lastLedR.load() == red and m_lastLedG.load() == green and m_lastLedB.load() == blue and
      m_lastLedMode.load() == hwMode )
    return true;

  std::cout << "LCTWaterCoolerWorker: setting LED color R=" << red << " G=" << green << " B=" << blue
            << " Mode=" << hwMode << std::endl;

  QByteArray data;
  data.append( static_cast< char >( 0xfe ) );
  data.append( static_cast< char >( CMD_RGB ) );
  data.append( static_cast< char >( 0x01 ) ); // Enable
  data.append( static_cast< char >( red ) );
  data.append( static_cast< char >( green ) );
  data.append( static_cast< char >( blue ) );
  data.append( static_cast< char >( static_cast< uint8_t >( rgbState ) ) );
  data.append( static_cast< char >( 0xef ) );

  const bool result = writeCommand( data );
  if ( result )
  {
    m_lastLedR.store( red );
    m_lastLedG.store( green );
    m_lastLedB.store( blue );
    m_lastLedMode.store( hwMode );
  }
  return result;
}

bool LCTWaterCoolerWorker::turnOffLED()
{
  return turnOffDevice( CMD_RGB );
}

bool LCTWaterCoolerWorker::turnOffFan()
{
  return turnOffDevice( CMD_FAN );
}

bool LCTWaterCoolerWorker::turnOffPump()
{
  const auto result = turnOffDevice( CMD_PUMP );
  if ( result )
    m_lastPumpVoltage.store( static_cast< int32_t >( ucc::PumpVoltage::Off ) );
  return result;
}

void LCTWaterCoolerWorker::startScanning()
{
  // Dispatch to main thread if called from another thread
  if ( QThread::currentThread() != thread() )
  {
    QMetaObject::invokeMethod( this, [this]() { startScanning(); }, Qt::BlockingQueuedConnection );
    return;
  }

  // Clean up any stale connection and reset state machine for a fresh start
  cleanupBleController();
  m_consecutiveFailures = 0;

  if ( startDiscoveryInternal() )
  {
    m_state = WaterCoolerState::Discovering;
    m_lastDiscoveryStart = std::chrono::steady_clock::now();
    m_dbusData.waterCoolerAvailable = true;
  }
  else
  {
    m_state = WaterCoolerState::Disconnected;
  }
}

void LCTWaterCoolerWorker::stopScanning()
{
  if ( QThread::currentThread() != thread() )
  {
    QMetaObject::invokeMethod( this, [this]() { stopScanning(); }, Qt::BlockingQueuedConnection );
    return;
  }

  stopDiscoveryInternal();
  m_state = WaterCoolerState::Disconnected;
  m_dbusData.waterCoolerAvailable = false;
}

void LCTWaterCoolerWorker::disconnectFromDevice()
{
  if ( QThread::currentThread() != thread() )
  {
    QMetaObject::invokeMethod( this, [this]() { disconnectFromDevice(); }, Qt::BlockingQueuedConnection );
    return;
  }

  disconnectFromDeviceInternal();
  m_dbusData.waterCoolerConnected = false;
}

// ── Invokable implementations (BLE writes, always run on main thread)

bool LCTWaterCoolerWorker::setFanSpeedImpl( int dutyCyclePercent )
{
  if ( not m_isConnected.load() or dutyCyclePercent < 0 or dutyCyclePercent > 100 )
    return false;

  if ( not m_uartService or not m_txCharacteristic.isValid() )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: setFanSpeedImpl - UART unavailable" );
    return false;
  }

  QByteArray data;
  data.append( static_cast< char >( 0xfe ) );
  data.append( static_cast< char >( CMD_FAN ) );
  data.append( static_cast< char >( 0x01 ) ); // Enable
  data.append( static_cast< char >( dutyCyclePercent ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0xef ) );

  m_uartService->writeCharacteristic( m_txCharacteristic, data, QLowEnergyService::WriteWithoutResponse );
  return true;
}

bool LCTWaterCoolerWorker::setPumpVoltageImpl( ucc::PumpVoltage voltage )
{
  if ( not m_isConnected.load() )
    return false;

  if ( not m_uartService or not m_txCharacteristic.isValid() )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: setPumpVoltageImpl - UART unavailable" );
    return false;
  }

  if ( voltage == ucc::PumpVoltage::Off )
    return turnOffPump();

  QByteArray data;
  data.append( static_cast< char >( 0xfe ) );
  data.append( static_cast< char >( CMD_PUMP ) );
  data.append( static_cast< char >( 0x01 ) ); // Enable
  data.append( static_cast< char >( 60 ) );   // Fixed duty cycle
  data.append( static_cast< char >( static_cast< uint8_t >( voltage ) ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0xef ) );

  m_uartService->writeCharacteristic( m_txCharacteristic, data, QLowEnergyService::WriteWithResponse );
  return true;
}

bool LCTWaterCoolerWorker::writeCommandImpl( const QByteArray& data, bool withResponse )
{
  if ( not m_isConnected.load() or not m_txCharacteristic.isValid() or not m_uartService )
    return false;

  const auto mode = withResponse ? QLowEnergyService::WriteWithResponse : QLowEnergyService::WriteWithoutResponse;
  m_uartService->writeCharacteristic( m_txCharacteristic, data, mode );
  return true;
}

// ── Discovery ───────────────────────────────────────────────────────

bool LCTWaterCoolerWorker::startDiscoveryInternal()
{
  if ( not m_deviceDiscoveryAgent )
    return false;

  if ( m_isDiscovering )
    return true; // Already running

  m_discoveredDevices.clear();
  m_deviceDiscoveryAgent->setLowEnergyDiscoveryTimeout( 10000 );
  m_deviceDiscoveryAgent->start();
  m_isDiscovering = true;

  std::cout << "[WC-BLE] Scan started (10s timeout)" << std::endl;
  syslog( LOG_INFO, "LCTWaterCoolerWorker: started BLE discovery (10 s timeout)" );
  return true;
}

void LCTWaterCoolerWorker::stopDiscoveryInternal()
{
  if ( m_deviceDiscoveryAgent and m_deviceDiscoveryAgent->isActive() )
    m_deviceDiscoveryAgent->stop();
  m_isDiscovering = false;
}

// ── Connection management ───────────────────────────────────────────

bool LCTWaterCoolerWorker::connectToDevice( const QString& deviceUuid )
{
  syslog( LOG_INFO, "LCTWaterCoolerWorker: connecting to device %s", deviceUuid.toStdString().c_str() );

  if ( m_isConnected.load() )
  {
    syslog( LOG_WARNING, "LCTWaterCoolerWorker: already connected" );
    return false;
  }

  // Find the device in discovered list
  QBluetoothDeviceInfo deviceInfo;
  bool found = false;
  for( const auto& device : m_discoveredDevices )
  {
    if ( device.uuid == deviceUuid )
    {
      deviceInfo = device.deviceInfo;
      found = true;
      break;
    }
  }

  if ( not found )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: device %s not in discovered list", deviceUuid.toStdString().c_str() );
    return false;
  }

  m_connectedDeviceInfo = deviceInfo;

  if ( not setupBleController( deviceInfo ) )
    return false;

  // Cache this device for fast reconnect later
  m_lastKnownDeviceInfo = deviceInfo;
  m_hasKnownDevice = true;

  m_bleController->connectToDevice();
  syslog( LOG_INFO, "LCTWaterCoolerWorker: connectToDevice() initiated for %s",
          deviceInfo.name().toStdString().c_str() );
  return true;
}

bool LCTWaterCoolerWorker::connectToKnownDevice()
{
  if ( not m_hasKnownDevice )
  {
    syslog( LOG_WARNING, "LCTWaterCoolerWorker: no known device to reconnect to" );
    return false;
  }

  if ( m_isConnected.load() )
  {
    syslog( LOG_WARNING, "LCTWaterCoolerWorker: already connected" );
    return false;
  }

  syslog( LOG_INFO, "LCTWaterCoolerWorker: fast-reconnecting to known device %s",
          m_lastKnownDeviceInfo.name().toStdString().c_str() );

  m_connectedDeviceInfo = m_lastKnownDeviceInfo;

  if ( not setupBleController( m_lastKnownDeviceInfo ) )
    return false;

  m_bleController->connectToDevice();
  return true;
}

void LCTWaterCoolerWorker::disconnectFromDeviceInternal()
{
  if ( m_bleController and m_isConnected.load() )
  {
    // Send reset command before disconnecting, then disconnect after a short delay
    writeCommand( QByteArray::fromHex( "fe190001000000ef" ) );
    QTimer::singleShot( 100, this,
                        [this]()
                        {
                          if ( m_bleController )
                            m_bleController->disconnectFromDevice();
                        } );
  }
  else if ( m_bleController )
  {
    m_bleController->disconnectFromDevice();
  }
}

// ── BLE helpers ─────────────────────────────────────────────────────

bool LCTWaterCoolerWorker::setupBleController( const QBluetoothDeviceInfo& deviceInfo )
{
  cleanupBleController();

  if ( m_bleController = QLowEnergyController::createCentral( deviceInfo, this ); not m_bleController )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: failed to create BLE controller" );
    return false;
  }

  connect( m_bleController, &QLowEnergyController::connected, this, &LCTWaterCoolerWorker::onBleConnected );
  connect( m_bleController, &QLowEnergyController::disconnected, this, &LCTWaterCoolerWorker::onBleDisconnected );
  connect( m_bleController, &QLowEnergyController::errorOccurred, this, &LCTWaterCoolerWorker::onBleError );
  connect( m_bleController, &QLowEnergyController::serviceDiscovered, this,
           [this]( const QBluetoothUuid& serviceUuid )
           {
             if ( serviceUuid == NORDIC_UART_SERVICE_UUID_OBJ )
               std::cout << "LCTWaterCoolerWorker: found Nordic UART service" << std::endl;
           } );
  connect( m_bleController, &QLowEnergyController::discoveryFinished, this,
           &LCTWaterCoolerWorker::onServiceDiscoveryFinished );

  return true;
}

void LCTWaterCoolerWorker::cleanupBleController()
{
  if ( m_uartService )
  {
    m_uartService->disconnect();
    delete m_uartService;
    m_uartService = nullptr;
  }
  m_txCharacteristic = QLowEnergyCharacteristic();
  m_rxCharacteristic = QLowEnergyCharacteristic();

  if ( m_bleController )
  {
    // Disconnect OUR signal-slot connections to prevent callbacks during teardown
    QObject::disconnect( m_bleController, nullptr, this, nullptr );

    if ( m_bleController->state() == QLowEnergyController::UnconnectedState )
    {
      delete m_bleController;
    }
    else
    {
      // Controller is still closing or connected — request disconnect and use deferred
      // deletion to let BlueZ finish processing asynchronously.  This avoids the
      // "Low Energy Controller is not Unconnected when deleted" warning.
      std::cout << "[WC-BLE] Controller still in state "
                << static_cast< int >( m_bleController->state() )
                << ", using deferred deletion" << std::endl;
      m_bleController->disconnectFromDevice();
      m_bleController->deleteLater();
    }
    m_bleController = nullptr;
  }
  m_isConnected = false;
}

bool LCTWaterCoolerWorker::writeCommand( const QByteArray& data )
{
  if ( not m_isConnected.load() or not m_txCharacteristic.isValid() )
    return false;

  if ( QThread::currentThread() == thread() )
    return writeCommandImpl( data, false );

  bool result = false;
  QMetaObject::invokeMethod( this, "writeCommandImpl", Qt::BlockingQueuedConnection, Q_RETURN_ARG( bool, result ),
                             Q_ARG( QByteArray, data ), Q_ARG( bool, false ) );
  return result;
}

bool LCTWaterCoolerWorker::writeReceive( const QByteArray& data )
{
  if ( not m_isConnected.load() or not m_txCharacteristic.isValid() or not m_rxCharacteristic.isValid() )
    return false;

  return writeCommand( data );
}

bool LCTWaterCoolerWorker::turnOffDevice( uint8_t cmd )
{
  if ( not m_isConnected.load() )
    return false;

  QByteArray data;
  data.append( static_cast< char >( 0xfe ) );
  data.append( static_cast< char >( cmd ) );
  data.append( static_cast< char >( 0x00 ) ); // Disable
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0x00 ) );
  data.append( static_cast< char >( 0xef ) );

  return writeCommand( data );
}

bool LCTWaterCoolerWorker::resetBluetoothAdapter()
{
  syslog( LOG_WARNING, "LCTWaterCoolerWorker: resetting Bluetooth adapter via bluetoothctl" );

  cleanupBleController();

  // Power-cycle the adapter
  QProcess powerOff;
  powerOff.start( "bluetoothctl", { "power", "off" } );
  if ( not powerOff.waitForFinished( 5000 ) )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: bluetoothctl power off timed out" );
    return false;
  }
  if ( powerOff.exitCode() != 0 )
    syslog( LOG_ERR, "LCTWaterCoolerWorker: bluetoothctl power off failed (exit %d)", powerOff.exitCode() );

  // Brief pause to let the adapter fully quiesce
  QThread::msleep( 500 );

  QProcess powerOn;
  powerOn.start( "bluetoothctl", { "power", "on" } );
  if ( not powerOn.waitForFinished( 5000 ) )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: bluetoothctl power on timed out" );
    return false;
  }
  if ( powerOn.exitCode() != 0 )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: bluetoothctl power on failed (exit %d)", powerOn.exitCode() );
    return false;
  }

  syslog( LOG_INFO, "LCTWaterCoolerWorker: Bluetooth adapter reset successfully" );

  // Give the adapter a moment to stabilize before BLE operations resume
  QThread::msleep( 1000 );
  return true;
}

// ── State machine helpers ───────────────────────────────────────────

void LCTWaterCoolerWorker::requestStartDiscovery()
{
  m_state = WaterCoolerState::Discovering;
  m_lastDiscoveryStart = std::chrono::steady_clock::now();

  if ( not startDiscoveryInternal() )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: failed to start discovery" );
    m_state = WaterCoolerState::Error;
  }
}

void LCTWaterCoolerWorker::requestConnectToDevice( const QString& uuid )
{
  m_state = WaterCoolerState::Connecting;
  m_lastDiscoveryStart = std::chrono::steady_clock::now();

  if ( not connectToDevice( uuid ) )
  {
    syslog( LOG_ERR, "LCTWaterCoolerWorker: failed to initiate connection" );
    m_state = WaterCoolerState::Error;
  }
}

void LCTWaterCoolerWorker::setAvailableFlag( bool available )
{
  m_dbusData.waterCoolerAvailable = available;
}

int64_t LCTWaterCoolerWorker::secondsSinceLastDiscovery() const
{
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast< std::chrono::seconds >( now - m_lastDiscoveryStart ).count();
}

ucc::LCTDeviceModel LCTWaterCoolerWorker::deviceModelFromName( const QString& name ) const
{
  const QString lower = name.toLower();
  if ( not lower.contains( "lct" ) )
    return ucc::LCTDeviceModel::Unknown;

  if ( lower.contains( "22002" ) )
    return ucc::LCTDeviceModel::LCT22002;
  if ( lower.contains( "21001" ) )
    return ucc::LCTDeviceModel::LCT21001;

  // Generic LCT device — treat as LCT21001
  syslog( LOG_INFO, "LCTWaterCoolerWorker: unknown LCT model '%s', treating as LCT21001", name.toStdString().c_str() );
  return ucc::LCTDeviceModel::LCT21001;
}
