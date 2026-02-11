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

#include "workers/HardwareMonitorWorker.hpp"
#include "SysfsNode.hpp"
#include "Utils.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <filesystem>
#include <cstdio>
#include <cmath>
#include <thread>
#include <chrono>
#include <syslog.h>

// ============================================================================
// DGpuInfo / IGpuInfo print helpers
// ============================================================================

void DGpuInfo::print() const noexcept
{
  std::cout << "DGPU Info: \n"
     << "  Temperature: " << m_temp << " °C\n"
     << "  Core Frequency: " << m_coreFrequency << " MHz\n"
     << "  Max Core Frequency: " << m_maxCoreFrequency << " MHz\n"
     << "  Power Draw: " << m_powerDraw << " W\n"
     << "  Max Power Limit: " << m_maxPowerLimit << " W\n"
     << "  Enforced Power Limit: " << m_enforcedPowerLimit << " W\n"
     << "  D0 Metrics Usage: " << ( m_d0MetricsUsage ? "Yes" : "No" ) << "\n";
}

void IGpuInfo::print() const noexcept
{
  std::cout << "IGPU Info: \n"
     << "  Vendor: " << m_vendor << "\n"
     << "  Temperature: " << m_temp << " °C\n"
     << "  Core Frequency: " << m_coreFrequency << " MHz\n"
     << "  Max Core Frequency: " << m_maxCoreFrequency << " MHz\n"
     << "  Power Draw: " << m_powerDraw << " W\n";
}

// ============================================================================
// IntelRAPLController (internal)
// ============================================================================

class IntelRAPLController
{
public:
  explicit IntelRAPLController( const std::string &basePath ) noexcept
    : m_basePath( basePath )
    , m_energyUJ( 0 )
    , m_enabled( false )
    , m_name( "unknown" )
    , m_constraint0Name( "unknown" ), m_constraint0MaxPower( -1 ), m_constraint0PowerLimit( -1 )
    , m_constraint1Name( "unknown" ), m_constraint1MaxPower( -1 ), m_constraint1PowerLimit( -1 )
    , m_constraint2Name( "unknown" ), m_constraint2MaxPower( -1 ), m_constraint2PowerLimit( -1 )
  {
  }

  [[nodiscard]] bool getIntelRAPLPowerAvailable() const noexcept
  {
    return m_enabled and m_name == "package-0" and m_energyUJ >= 0;
  }

  [[nodiscard]] bool getIntelRAPLConstraint0Available() const noexcept
  {
    return m_constraint0Name == "long_term" and m_constraint0MaxPower >= 0 and m_constraint0PowerLimit >= 0;
  }

  [[nodiscard]] bool getIntelRAPLConstraint1Available() const noexcept
  {
    return m_constraint1Name == "short_term" and m_constraint1MaxPower >= 0 and m_constraint1PowerLimit >= 0;
  }

  [[nodiscard]] bool getIntelRAPLConstraint2Available() const noexcept
  {
    return m_constraint2Name == "peak_power" and m_constraint2MaxPower >= 0 and m_constraint2PowerLimit >= 0;
  }

  [[nodiscard]] bool getIntelRAPLEnergyAvailable() const noexcept
  {
    return m_energyUJ >= 0;
  }

  [[nodiscard]] int64_t getConstraint0MaxPower() const noexcept { return m_constraint0PowerLimit; }
  [[nodiscard]] int64_t getConstraint1MaxPower() const noexcept { return m_constraint1PowerLimit; }
  [[nodiscard]] int64_t getConstraint2MaxPower() const noexcept { return m_constraint2PowerLimit; }

  [[nodiscard]] int64_t getEnergy() const noexcept
  {
    return readIntegerProperty( "energy_uj", -1 );
  }

  void setPowerPL1Limit( std::optional< int64_t > setPowerLimit = std::nullopt ) noexcept
  {
    if ( not getIntelRAPLConstraint0Available() )
      return;

    int64_t maxPower = getConstraint0MaxPower();
    int64_t powerLimit = maxPower;

    if ( setPowerLimit.has_value() )
      powerLimit = std::max( maxPower / 2, std::min( setPowerLimit.value(), maxPower ) );

    if ( writeIntegerProperty( "constraint_0_power_limit_uw", powerLimit ) )
      m_constraint0PowerLimit = powerLimit;

    writeBoolProperty( "enabled", true );
  }

  void updateFromSysfs() noexcept
  {
    m_name = readStringProperty( "name", "unknown" );
    m_enabled = readBoolProperty( "enabled", false );
    m_energyUJ = readIntegerProperty( "energy_uj", -1 );

    m_constraint0Name = readStringProperty( "constraint_0_name", "unknown" );
    m_constraint0MaxPower = readIntegerProperty( "constraint_0_max_power_uw", -1 );
    m_constraint0PowerLimit = readIntegerProperty( "constraint_0_power_limit_uw", -1 );

    m_constraint1Name = readStringProperty( "constraint_1_name", "unknown" );
    m_constraint1MaxPower = readIntegerProperty( "constraint_1_max_power_uw", -1 );
    m_constraint1PowerLimit = readIntegerProperty( "constraint_1_power_limit_uw", -1 );

    m_constraint2Name = readStringProperty( "constraint_2_name", "unknown" );
    m_constraint2MaxPower = readIntegerProperty( "constraint_2_max_power_uw", -1 );
    m_constraint2PowerLimit = readIntegerProperty( "constraint_2_power_limit_uw", -1 );
  }

  [[nodiscard]] const std::string &getBasePath() const noexcept { return m_basePath; }

private:
  [[nodiscard]] bool isPropertyAvailable( const std::string &propertyName ) const noexcept
  {
    try
    {
      std::string filePath = m_basePath + "/" + propertyName;
      return std::filesystem::exists( filePath ) and std::filesystem::is_regular_file( filePath );
    }
    catch ( ... ) { return false; }
  }

  [[nodiscard]] std::string readStringProperty( const std::string &propertyName,
                                               const std::string &defaultValue ) const noexcept
  {
    try
    {
      if ( not isPropertyAvailable( propertyName ) )
        return defaultValue;

      std::ifstream file( m_basePath + "/" + propertyName );
      if ( not file.is_open() )
        return defaultValue;

      std::string value;
      if ( std::getline( file, value ) )
      {
        while ( not value.empty() and ( value.back() == '\n' or value.back() == '\r' or
                                         value.back() == ' ' or value.back() == '\t' ) )
          value.pop_back();
        return value;
      }
      return defaultValue;
    }
    catch ( ... ) { return defaultValue; }
  }

  [[nodiscard]] int64_t readIntegerProperty( const std::string &propertyName,
                                            int64_t defaultValue ) const noexcept
  {
    try
    {
      if ( not isPropertyAvailable( propertyName ) )
        return defaultValue;

      std::ifstream file( m_basePath + "/" + propertyName );
      if ( not file.is_open() )
        return defaultValue;

      int64_t value = defaultValue;
      if ( file >> value )
        return value;
      return defaultValue;
    }
    catch ( ... ) { return defaultValue; }
  }

  [[nodiscard]] bool readBoolProperty( const std::string &propertyName,
                                      bool defaultValue ) const noexcept
  {
    int64_t value = readIntegerProperty( propertyName, defaultValue ? 1 : 0 );
    return value != 0;
  }

  bool writeIntegerProperty( const std::string &propertyName, int64_t value ) noexcept
  {
    try
    {
      std::ofstream file( m_basePath + "/" + propertyName );
      if ( not file.is_open() )
        return false;
      file << value;
      return file.good();
    }
    catch ( ... ) { return false; }
  }

  bool writeBoolProperty( const std::string &propertyName, bool value ) noexcept
  {
    return writeIntegerProperty( propertyName, value ? 1 : 0 );
  }

  std::string m_basePath;
  int64_t m_energyUJ;
  bool m_enabled;
  std::string m_name;
  std::string m_constraint0Name; int64_t m_constraint0MaxPower; int64_t m_constraint0PowerLimit;
  std::string m_constraint1Name; int64_t m_constraint1MaxPower; int64_t m_constraint1PowerLimit;
  std::string m_constraint2Name; int64_t m_constraint2MaxPower; int64_t m_constraint2PowerLimit;
};

// ============================================================================
// PowerController (internal)
// ============================================================================

class PowerController
{
public:
  explicit PowerController( IntelRAPLController &intelRAPL ) noexcept
    : m_intelRAPL( intelRAPL )
    , m_currentEnergy( 0 )
    , m_lastUpdateTime( std::chrono::system_clock::now() )
    , m_raplPowerStatus( intelRAPL.getIntelRAPLEnergyAvailable() )
  {
  }

  [[nodiscard]] double getCurrentPower() noexcept
  {
    if ( not m_raplPowerStatus )
      return -1.0;

    auto now = std::chrono::system_clock::now();
    int64_t currentEnergy = m_intelRAPL.getEnergy();
    int64_t energyIncrement = currentEnergy - m_currentEnergy;

    auto timeDiff = std::chrono::duration_cast< std::chrono::milliseconds >(
        now - m_lastUpdateTime ).count();
    double delaySec = timeDiff > 0 ? static_cast< double >( timeDiff ) / 1000.0 : -1.0;

    double powerDraw = -1.0;
    if ( delaySec > 0 and m_currentEnergy > 0 )
      powerDraw = static_cast< double >( energyIncrement ) / delaySec / 1000000.0;

    m_currentEnergy += energyIncrement;
    m_lastUpdateTime = now;

    return powerDraw;
  }

private:
  IntelRAPLController &m_intelRAPL;
  int64_t m_currentEnergy;
  std::chrono::system_clock::time_point m_lastUpdateTime;
  bool m_raplPowerStatus;
};

// ============================================================================
// GpuDeviceDetector (internal)
// ============================================================================

class GpuDeviceDetector
{
public:
  explicit GpuDeviceDetector() noexcept = default;

  [[nodiscard]] GpuDeviceCounts detectGpuDevices() const noexcept
  {
    GpuDeviceCounts counts;
    counts.intelIGpuCount = countDevicesMatchingPattern( getIntelIGpuPattern() );
    counts.amdIGpuCount = countDevicesMatchingPattern( getAmdIGpuPattern() );
    counts.amdDGpuCount = countDevicesMatchingPattern( getAmdDGpuPattern() );
    counts.nvidiaCount = countNvidiaDevices();
    return counts;
  }

private:
  [[nodiscard]] std::string getIntelIGpuPattern() const noexcept
  {
    return "8086:(6420|64B0|7D51|7D67|7D41|7DD5|7D45|7D40|"
           "A780|A781|A788|A789|A78A|A782|A78B|A783|A7A0|A7A1|A7A8|A7AA|"
           "A7AB|A7AC|A7AD|A7A9|A721|4680|4690|4688|468A|468B|4682|4692|"
           "4693|46D3|46D4|46D0|46D1|46D2|4626|4628|462A|46A2|46B3|46C2|"
           "46A3|46B2|46C3|46A0|46B0|46C0|46A6|46AA|46A8)";
  }

  [[nodiscard]] std::string getAmdIGpuPattern() const noexcept
  {
    return "1002:(164E|1506|15DD|15D8|15E7|1636|1638|164C|164D|1681|15BF|"
           "15C8|1304|1305|1306|1307|1309|130A|130B|130C|130D|130E|130F|"
           "1310|1311|1312|1313|1315|1316|1317|1318|131B|131C|131D|13C0|"
           "9830|9831|9832|9833|9834|9835|9836|9837|9838|9839|983a|983b|983c|"
           "983d|983e|983f|9850|9851|9852|9853|9854|9855|9856|9857|9858|"
           "9859|985A|985B|985C|985D|985E|985F|9870|9874|9875|9876|9877|"
           "98E4|13FE|143F|74A0|1435|163f|1900|1901|1114|150E)";
  }

  [[nodiscard]] std::string getAmdDGpuPattern() const noexcept
  {
    return "1002:(7480)";
  }

  [[nodiscard]] int countDevicesMatchingPattern( const std::string &pattern ) const noexcept
  {
    try
    {
      std::string command =
          "for f in /sys/bus/pci/devices/*/uevent; do "
          "grep -q 'PCI_CLASS=30000' \"$f\" && grep -q -P 'PCI_ID=" + pattern + "' \"$f\" && echo \"$f\"; "
          "done";

      std::string output = TccUtils::executeCommand( command );
      if ( output.empty() )
        return 0;

      int count = 0;
      for ( char c : output )
        if ( c == '\n' ) count++;
      if ( not output.empty() and output.back() != '\n' )
        count++;
      return count;
    }
    catch ( ... ) { return 0; }
  }

  [[nodiscard]] int countNvidiaDevices() const noexcept
  {
    try
    {
      const std::string nvidiaVendorId = "0x10de";
      std::string command =
          "grep -lx '" + nvidiaVendorId + "' /sys/bus/pci/devices/*/vendor 2>/dev/null || echo ''";
      std::string output = TccUtils::executeCommand( command );
      if ( output.empty() )
        return 0;

      std::set< std::string > uniqueDevices;
      std::istringstream iss( output );
      std::string line;

      while ( std::getline( iss, line ) )
      {
        if ( not line.empty() )
        {
          size_t lastSlash = line.rfind( '/' );
          if ( lastSlash != std::string::npos and lastSlash > 0 )
          {
            std::string devicePath = line.substr( 0, lastSlash );
            size_t lastDot = devicePath.rfind( '.' );
            if ( lastDot != std::string::npos )
              devicePath = devicePath.substr( 0, lastDot );
            uniqueDevices.insert( devicePath );
          }
        }
      }
      return static_cast< int >( uniqueDevices.size() );
    }
    catch ( ... ) { return 0; }
  }
};

// ============================================================================
// HardwareMonitorWorker
// ============================================================================

HardwareMonitorWorker::HardwareMonitorWorker(
  std::function< void( const std::string & ) > cpuPowerUpdateCallback,
  std::function< bool() > getSensorDataCollectionStatus,
  std::function< void( const std::string & ) > setPrimeStateCallback )
  : DaemonWorker( std::chrono::milliseconds( 800 ), false )
  , m_gpuDetector( std::make_unique< GpuDeviceDetector >() )
  , m_deviceCounts( m_gpuDetector->detectGpuDevices() )
  , m_isNvidiaSmiInstalled( m_deviceCounts.nvidiaCount > 0 and checkNvidiaSmiInstalledImpl() )
  , m_gpuDataCallback( nullptr )
  , m_hwmonIGpuRetryCount( 3 )
  , m_hwmonDGpuRetryCount( 3 )
  , m_RAPLConstraint0Status( false )
  , m_RAPLConstraint1Status( false )
  , m_RAPLConstraint2Status( false )
  , m_cpuPowerUpdateCallback( std::move( cpuPowerUpdateCallback ) )
  , m_getSensorDataCollectionStatus( std::move( getSensorDataCollectionStatus ) )
  , m_setPrimeState( std::move( setPrimeStateCallback ) )
  , m_primeSupported( false )
  , m_cycleCounter( 0 )
{
}

HardwareMonitorWorker::~HardwareMonitorWorker() = default;

void HardwareMonitorWorker::setGpuDataCallback( GpuDataCallback callback ) noexcept
{
  m_gpuDataCallback = std::move( callback );
}

void HardwareMonitorWorker::setWebcamCallbacks( WebcamHwReader reader, WebcamStatusCallback callback ) noexcept
{
  m_webcamHwReader = std::move( reader );
  m_webcamStatusCallback = std::move( callback );
}

bool HardwareMonitorWorker::isPrimeSupported() const noexcept
{
  return m_primeSupported;
}

// ------------ Lifecycle ------------

void HardwareMonitorWorker::onStart()
{
  initGpu();
  initCpuPower();
  initPrime();

  // initial webcam read so DBus data is populated before first poll cycle
  updateWebcamStatus();
}

void HardwareMonitorWorker::onWork()
{
  m_cycleCounter++;

  // --- GPU info: every cycle (800ms) ---
  // retry AMD iGPU path discovery if not found yet
  if ( m_deviceCounts.amdIGpuCount == 1 and not m_amdIGpuHwmonPath.has_value() and m_hwmonIGpuRetryCount > 0 )
    ( void ) checkAmdIGpuHwmonPath();
  if ( m_deviceCounts.amdDGpuCount == 1 and not m_amdDGpuHwmonPath.has_value() and m_hwmonDGpuRetryCount > 0 )
    ( void ) checkAmdDGpuHwmonPath();

  try
  {
    if ( m_gpuDataCallback )
      m_gpuDataCallback( getIGpuValues(), getDGpuValues() );
  }
  catch ( ... ) { /* ignore callback exceptions */ }

  // --- CPU power: every 3rd cycle (≈ 2400ms, close to original 2000ms) ---
  if ( m_cycleCounter % 3 == 0 )
    updateCpuPower();

  // --- Webcam: every 3rd cycle, offset by 1 (≈ 2400ms) ---
  if ( m_cycleCounter % 3 == 1 )
    updateWebcamStatus();

  // --- Prime state: every 12th cycle (≈ 9600ms, close to original 10000ms) ---
  if ( m_cycleCounter % 12 == 0 )
    updatePrimeStatus();
}

void HardwareMonitorWorker::onExit()
{
  m_cpuPowerController.reset();
  m_intelRAPLCpu.reset();
  m_intelGpuPowerController.reset();
  m_intelRAPLGpu.reset();
}

// ============================================================================
// GPU implementation
// ============================================================================

void HardwareMonitorWorker::initGpu()
{
  // Check Intel iGPU availability
  if ( m_deviceCounts.intelIGpuCount == 1 and not m_intelIGpuDrmPath.has_value() )
  {
    if ( std::string intelPath = getIntelIGpuDrmPathImpl(); not intelPath.empty() )
    {
      m_intelIGpuDrmPath = intelPath;
      m_intelRAPLGpu = std::make_unique< IntelRAPLController >(
        "/sys/devices/virtual/powercap/intel-rapl/intel-rapl:0/intel-rapl:0:1/" );
      m_intelGpuPowerController = std::make_unique< PowerController >( *m_intelRAPLGpu );
    }
  }

  // Check AMD iGPU availability
  if ( m_deviceCounts.amdIGpuCount == 1 and m_hwmonIGpuRetryCount > 0 )
    ( void ) checkAmdIGpuHwmonPath();

  // Check AMD dGPU availability
  if ( m_deviceCounts.amdDGpuCount == 1 and m_hwmonDGpuRetryCount > 0 and not m_amdDGpuHwmonPath.has_value() )
    ( void ) checkAmdDGpuHwmonPath();
}

bool HardwareMonitorWorker::checkAmdIGpuHwmonPath() noexcept
{
  if ( m_amdIGpuHwmonPath.has_value() )
    return true;

  if ( m_hwmonIGpuRetryCount > 0 )
  {
    m_hwmonIGpuRetryCount--;
    if ( std::string path = getAmdIGpuHwmonPathImpl(); not path.empty() )
    {
      m_amdIGpuHwmonPath = path;
      return true;
    }
  }
  return false;
}

bool HardwareMonitorWorker::checkAmdDGpuHwmonPath() noexcept
{
  if ( m_amdDGpuHwmonPath.has_value() )
    return true;

  if ( m_hwmonDGpuRetryCount > 0 )
  {
    m_hwmonDGpuRetryCount--;
    if ( std::string path = getAmdDGpuHwmonPathImpl(); not path.empty() )
    {
      m_amdDGpuHwmonPath = path;
      return true;
    }
  }
  return false;
}

std::string HardwareMonitorWorker::getIntelIGpuDrmPathImpl() const noexcept
{
  return "";
}

std::string HardwareMonitorWorker::getAmdIGpuHwmonPathImpl() const noexcept
{
  try
  {
    std::string amdIGpuPattern = "1002:(164E|1506|15DD|15D8|15E7|1636|1638|164C|164D|1681|15BF|"
                                 "15C8|1304|1305|1306|1307|1309|130A|130B|130C|130D|130E|130F|"
                                 "1310|1311|1312|1313|1315|1316|1317|1318|131B|131C|131D|13C0|"
                                 "9830|9831|9832|9833|9834|9835|9836|9837|9838|9839|983a|983b|983c|"
                                 "983d|983e|983f|9850|9851|9852|9853|9854|9855|9856|9857|9858|"
                                 "9859|985A|985B|985C|985D|985E|985F|9870|9874|9875|9876|9877|"
                                 "98E4|13FE|143F|74A0|1435|163f|1900|1901|1114|150E)";

    std::string command =
        "for d in /sys/class/hwmon/hwmon*/device/uevent; do "
        "grep -q 'DRIVER=amdgpu' \"$d\" && grep -q -P 'PCI_ID=" + amdIGpuPattern + "' \"$d\" && "
        "dirname \"$d\" | sed 's|/device$||'; "
        "done | head -1";

    std::string output = TccUtils::executeCommand( command );

    while ( not output.empty() and ( output.back() == '\n' or output.back() == '\r' or
                                     output.back() == ' ' or output.back() == '\t' ) )
      output.pop_back();

    return output;
  }
  catch ( ... ) { return ""; }
}

std::string HardwareMonitorWorker::getAmdDGpuHwmonPathImpl() const noexcept
{
  return "";
}

bool HardwareMonitorWorker::checkNvidiaSmiInstalledImpl() const noexcept
{
  try
  {
    std::string output = TccUtils::executeCommand( "which nvidia-smi" );
    while ( not output.empty() and ( output.back() == '\n' or output.back() == '\r' or
                                     output.back() == ' ' or output.back() == '\t' ) )
      output.pop_back();
    return not output.empty();
  }
  catch ( ... ) { return false; }
}

IGpuInfo HardwareMonitorWorker::getIGpuValues() noexcept
{
  IGpuInfo values{};

  if ( m_intelIGpuDrmPath.has_value() )
    values = getIntelIGpuValues( values );
  else if ( m_amdIGpuHwmonPath.has_value() )
    values = getAmdIGpuValues( values );

  return values;
}

IGpuInfo HardwareMonitorWorker::getIntelIGpuValues( const IGpuInfo &base ) const noexcept
{
  IGpuInfo values = base;

  if ( not m_intelIGpuDrmPath.has_value() )
    return values;

  if ( m_intelGpuPowerController )
    values.m_powerDraw = m_intelGpuPowerController->getCurrentPower();

  return values;
}

IGpuInfo HardwareMonitorWorker::getAmdIGpuValues( const IGpuInfo &base ) const noexcept
{
  IGpuInfo values = base;

  if ( not m_amdIGpuHwmonPath.has_value() )
    return values;

  std::string hwmonPath = m_amdIGpuHwmonPath.value();
  values.m_vendor = "amd";

  if ( int64_t tempValue = SysfsNode< int64_t >( hwmonPath + "/temp1_input" ).read().value_or( -1 ); tempValue >= 0 )
    values.m_temp = static_cast< double >( tempValue ) / 1000.0;

  if ( int64_t curFreqValue = SysfsNode< int64_t >( hwmonPath + "/freq1_input" ).read().value_or( -1 ); curFreqValue >= 0 )
    values.m_coreFrequency = static_cast< double >( curFreqValue ) / 1000000.0;

  if ( std::string maxFreqString = SysfsNode< std::string >( hwmonPath + "/device/pp_dpm_sclk" ).read().value_or( "" ); not maxFreqString.empty() )
    values.m_maxCoreFrequency = parseMaxAmdFreq( maxFreqString );

  if ( int64_t powerValue = SysfsNode< int64_t >( hwmonPath + "/power1_input" ).read().value_or( -1 ); powerValue >= 0 )
    values.m_powerDraw = static_cast< double >( powerValue ) / 1000.0;

  return values;
}

DGpuInfo HardwareMonitorWorker::getDGpuValues() noexcept
{
  DGpuInfo values{};
  bool metricsUsage = true;

  if ( m_deviceCounts.nvidiaCount == 1 and m_isNvidiaSmiInstalled and metricsUsage )
  {
    values = getNvidiaDGpuValues();
  }
  else if ( m_deviceCounts.amdDGpuCount == 1 and metricsUsage )
  {
    if ( m_amdDGpuHwmonPath.has_value() or checkAmdDGpuHwmonPath() )
      values = getAmdDGpuValues( values );
  }

  values.m_d0MetricsUsage = metricsUsage;
  return values;
}

DGpuInfo HardwareMonitorWorker::getNvidiaDGpuValues() const noexcept
{
  if ( m_isNvidiaSmiInstalled )
  {
    std::string command = "nvidia-smi --query-gpu=temperature.gpu,power.draw,power.max_limit,"
                         "enforced.power.limit,clocks.gr,clocks.max.gr --format=csv,noheader";
    std::string output = TccUtils::executeCommand( command );
    if ( not output.empty() )
      return parseNvidiaOutput( output );
  }
  return DGpuInfo{};
}

DGpuInfo HardwareMonitorWorker::getAmdDGpuValues( const DGpuInfo &base ) const noexcept
{
  DGpuInfo values = base;

  if ( not m_amdDGpuHwmonPath.has_value() )
    return values;

  try
  {
    const std::string &basePath = m_amdDGpuHwmonPath.value();

    int64_t tempMilli = SysfsNode< int64_t >( basePath + "/temp1_input" ).read().value_or( -1000 );
    if ( tempMilli > -1000 )
      values.m_temp = static_cast< double >( tempMilli ) / 1000.0;

    int64_t freqHz = SysfsNode< int64_t >( basePath + "/freq1_input" ).read().value_or( -1 );
    if ( freqHz > 0 )
      values.m_coreFrequency = static_cast< double >( freqHz ) / 1000000.0;

    int64_t powerMicro = SysfsNode< int64_t >( basePath + "/power1_average" ).read().value_or( -1 );
    if ( powerMicro > 0 )
      values.m_powerDraw = static_cast< double >( powerMicro ) / 1000000.0;
  }
  catch ( ... ) { }

  return values;
}

DGpuInfo HardwareMonitorWorker::parseNvidiaOutput( const std::string &output ) const noexcept
{
  DGpuInfo values{};

  std::vector< std::string > fields;
  std::stringstream ss( output );
  std::string field;

  while ( std::getline( ss, field, ',' ) )
  {
    size_t start = field.find_first_not_of( " \t\n\r" );
    size_t end = field.find_last_not_of( " \t\n\r" );
    if ( start != std::string::npos )
      fields.push_back( field.substr( start, end - start + 1 ) );
  }

  if ( fields.size() >= 6 )
  {
    values.m_temp = parseNumberWithMetric( fields[0] );
    values.m_powerDraw = parseNumberWithMetric( fields[1] );
    values.m_maxPowerLimit = parseNumberWithMetric( fields[2] );
    values.m_enforcedPowerLimit = parseNumberWithMetric( fields[3] );
    values.m_coreFrequency = parseNumberWithMetric( fields[4] );
    values.m_maxCoreFrequency = parseNumberWithMetric( fields[5] );
  }

  return values;
}

double HardwareMonitorWorker::parseNumberWithMetric( const std::string &value ) const noexcept
{
  std::regex numberRegex( R"(\d+(\.\d+)?)" );
  std::smatch match;

  if ( std::regex_search( value, match, numberRegex ) )
  {
    try { return std::stod( match.str() ); }
    catch ( ... ) { return -1.0; }
  }
  return -1.0;
}

double HardwareMonitorWorker::parseMaxAmdFreq( const std::string &frequencyString ) const noexcept
{
  std::regex mhzRegex( R"(\d+Mhz)" );
  std::smatch match;
  double maxFreq = -1.0;

  std::string::const_iterator searchStart( frequencyString.cbegin() );
  while ( std::regex_search( searchStart, frequencyString.cend(), match, mhzRegex ) )
  {
    try
    {
      std::string numStr = match.str();
      numStr = numStr.substr( 0, numStr.length() - 3 );
      double freq = std::stod( numStr );
      if ( freq > maxFreq )
        maxFreq = freq;
    }
    catch ( ... ) { }
    searchStart = match.suffix().first;
  }
  return maxFreq;
}

// ============================================================================
// CPU power implementation
// ============================================================================

void HardwareMonitorWorker::initCpuPower()
{
  m_intelRAPLCpu = std::make_unique< IntelRAPLController >(
    "/sys/devices/virtual/powercap/intel-rapl/intel-rapl:0/" );
  m_intelRAPLCpu->updateFromSysfs();
  m_cpuPowerController = std::make_unique< PowerController >( *m_intelRAPLCpu );

  m_RAPLConstraint0Status = m_intelRAPLCpu->getIntelRAPLConstraint0Available();
  m_RAPLConstraint1Status = m_intelRAPLCpu->getIntelRAPLConstraint1Available();
  m_RAPLConstraint2Status = m_intelRAPLCpu->getIntelRAPLConstraint2Available();

  // Run first update immediately
  updateCpuPower();
}

void HardwareMonitorWorker::updateCpuPower()
{
  std::ostringstream jsonStream;
  jsonStream << "{";

  if ( m_getSensorDataCollectionStatus() )
  {
    double powerDraw = getCpuCurrentPower();
    jsonStream << "\"powerDraw\":" << powerDraw;

    double maxPowerLimit = getCpuMaxPowerLimit();
    if ( maxPowerLimit > 0 )
      jsonStream << ",\"maxPowerLimit\":" << maxPowerLimit;
  }
  else
  {
    jsonStream << "\"powerDraw\":-1";
  }

  jsonStream << "}";
  m_cpuPowerUpdateCallback( jsonStream.str() );
}

double HardwareMonitorWorker::getCpuCurrentPower()
{
  if ( not m_cpuPowerController )
    return -1.0;
  return m_cpuPowerController->getCurrentPower();
}

double HardwareMonitorWorker::getCpuMaxPowerLimit()
{
  if ( not m_RAPLConstraint0Status and not m_RAPLConstraint1Status and not m_RAPLConstraint2Status )
    return -1.0;

  double maxPowerLimit = -1.0;

  if ( m_RAPLConstraint0Status )
  {
    int64_t c0 = m_intelRAPLCpu->getConstraint0MaxPower();
    if ( c0 > 0 )
      maxPowerLimit = static_cast< double >( c0 );
  }

  if ( m_RAPLConstraint1Status )
  {
    int64_t c1 = m_intelRAPLCpu->getConstraint1MaxPower();
    if ( c1 > 0 and static_cast< double >( c1 ) > maxPowerLimit )
      maxPowerLimit = static_cast< double >( c1 );
  }

  if ( m_RAPLConstraint2Status )
  {
    int64_t c2 = m_intelRAPLCpu->getConstraint2MaxPower();
    if ( c2 > 0 and static_cast< double >( c2 ) > maxPowerLimit )
      maxPowerLimit = static_cast< double >( c2 );
  }

  // Convert from micro watts to watts
  return maxPowerLimit / 1000000.0;
}

// ============================================================================
// Prime implementation
// ============================================================================

void HardwareMonitorWorker::initPrime()
{
  // Wait for requires_offloading file to be updated after boot
  std::this_thread::sleep_for( std::chrono::milliseconds( 2000 ) );
  updatePrimeStatus();
}

void HardwareMonitorWorker::updatePrimeStatus() noexcept
{
  const bool primeSupported = checkPrimeSupported();

  if ( primeSupported )
  {
    m_setPrimeState( checkPrimeStatus() );
    m_primeSupported = primeSupported;
  }
  else
  {
    m_setPrimeState( "-1" );
    m_primeSupported = false;
  }
}

bool HardwareMonitorWorker::checkPrimeSupported() const noexcept
{
  const bool offloadingStatus = std::filesystem::exists(
    "/var/lib/ubuntu-drivers-common/requires_offloading" );

  if ( not offloadingStatus )
    return false;

  try
  {
    auto pipe = popen( "which prime-select 2>/dev/null", "r" );
    if ( not pipe )
      return false;

    char buffer[256];
    std::string result;
    if ( fgets( buffer, sizeof( buffer ), pipe ) )
      result = buffer;
    pclose( pipe );

    while ( not result.empty() and ( result.back() == '\n' or result.back() == ' ' ) )
      result.pop_back();

    return not result.empty();
  }
  catch ( ... ) { return false; }
}

std::string HardwareMonitorWorker::checkPrimeStatus() const noexcept
{
  try
  {
    auto pipe = popen( "prime-select query 2>/dev/null", "r" );
    if ( not pipe )
      return "off";

    char buffer[256];
    std::string result;
    if ( fgets( buffer, sizeof( buffer ), pipe ) )
      result = buffer;
    pclose( pipe );

    while ( not result.empty() and ( result.back() == '\n' or result.back() == ' ' or result.back() == '\r' ) )
      result.pop_back();

    return transformPrimeStatus( result );
  }
  catch ( ... ) { return "off"; }
}

// ============================================================================
// Webcam implementation
// ============================================================================

void HardwareMonitorWorker::updateWebcamStatus() noexcept
{
  if ( not m_webcamHwReader or not m_webcamStatusCallback )
    return;

  try
  {
    auto [available, status] = m_webcamHwReader();
    m_webcamStatusCallback( available, status );
  }
  catch ( ... ) { /* ignore */ }
}

std::string HardwareMonitorWorker::transformPrimeStatus( const std::string &status ) const noexcept
{
  if ( status == "nvidia" )
    return "dGPU";
  else if ( status == "intel" )
    return "iGPU";
  else if ( status == "on-demand" )
    return "on-demand";
  else
    return "off";
}
