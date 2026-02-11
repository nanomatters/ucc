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

#include "DaemonWorker.hpp"
#include <string>
#include <optional>
#include <memory>
#include <functional>
#include <vector>

/**
 * @brief GPU device counts by vendor/type
 */
struct GpuDeviceCounts
{
  int intelIGpuCount = 0;
  int amdIGpuCount = 0;
  int amdDGpuCount = 0;
  int nvidiaCount = 0;
};

/**
 * @brief Data structure for discrete GPU information
 */
struct DGpuInfo
{
  double m_temp = -1.0;
  double m_coreFrequency = -1.0;
  double m_maxCoreFrequency = -1.0;
  double m_powerDraw = -1.0;
  double m_maxPowerLimit = -1.0;
  double m_enforcedPowerLimit = -1.0;
  bool m_d0MetricsUsage = false;

  void print() const noexcept;
};

/**
 * @brief Data structure for integrated GPU information
 */
struct IGpuInfo
{
  double m_temp = -1.0;
  double m_coreFrequency = -1.0;
  double m_maxCoreFrequency = -1.0;
  double m_powerDraw = -1.0;
  std::string m_vendor = "unknown";

  void print() const noexcept;
};

// Forward declarations for internal implementation classes
class GpuDeviceDetector;
class IntelRAPLController;
class PowerController;

/**
 * @brief HardwareMonitorWorker - Unified hardware monitoring
 *
 * Merges GPU information collection, CPU power monitoring, and NVIDIA Prime
 * state detection into a single worker thread.
 *
 * GPU monitoring (every cycle, 800ms):
 *   - Intel iGPU via RAPL energy counters and DRM frequency
 *   - AMD iGPU via hwmon sysfs interface
 *   - AMD dGPU via hwmon sysfs interface
 *   - NVIDIA dGPU via nvidia-smi command
 *
 * CPU power monitoring (every 3rd cycle ≈ 2400ms):
 *   - Intel RAPL power data for CPU package
 *   - Power constraints (PL1/PL2/PL4)
 *
 * Prime state monitoring (every 12th cycle ≈ 9600ms):
 *   - NVIDIA Prime GPU switching status
 *   - Requires prime-select utility (Ubuntu/TUXEDO OS)
 */
class HardwareMonitorWorker : public DaemonWorker
{
public:
  /**
   * @brief Callback function type for GPU data updates
   */
  using GpuDataCallback = std::function< void( const IGpuInfo &, const DGpuInfo & ) >;

  /**
   * @brief Constructor
   * @param cpuPowerUpdateCallback Called with CPU power JSON when updated
   * @param getSensorDataCollectionStatus Returns whether sensor data collection is enabled
   * @param setPrimeStateCallback Called with prime state string when updated
   */
  explicit HardwareMonitorWorker(
    std::function< void( const std::string & ) > cpuPowerUpdateCallback,
    std::function< bool() > getSensorDataCollectionStatus,
    std::function< void( const std::string & ) > setPrimeStateCallback );

  ~HardwareMonitorWorker() override;

  // Prevent copy and move
  HardwareMonitorWorker( const HardwareMonitorWorker & ) = delete;
  HardwareMonitorWorker( HardwareMonitorWorker && ) = delete;
  HardwareMonitorWorker &operator=( const HardwareMonitorWorker & ) = delete;
  HardwareMonitorWorker &operator=( HardwareMonitorWorker && ) = delete;

  /**
   * @brief Callback function type for webcam status updates
   * @param available Whether webcam switch hardware is present
   * @param status    Current webcam on/off state
   */
  using WebcamStatusCallback = std::function< void( bool available, bool status ) >;

  /**
   * @brief Hardware reader that returns (available, status) from the webcam switch
   */
  using WebcamHwReader = std::function< std::pair< bool, bool >() >;

  /**
   * @brief Set callback for GPU data updates
   * @param callback Function called with integrated and discrete GPU info
   */
  void setGpuDataCallback( GpuDataCallback callback ) noexcept;

  /**
   * @brief Set callbacks for webcam monitoring
   *
   * Must be called before start(). The reader queries hardware for the
   * webcam switch state; the callback pushes the result to DBus data.
   *
   * @param reader   Returns (available, status) from hardware
   * @param callback Called with (available, status) on each poll
   */
  void setWebcamCallbacks( WebcamHwReader reader, WebcamStatusCallback callback ) noexcept;

  /**
   * @brief Check if NVIDIA Prime is supported on this system
   * @return true if Prime is supported
   */
  [[nodiscard]] bool isPrimeSupported() const noexcept;

protected:
  void onStart() override;
  void onWork() override;
  void onExit() override;

private:
  // --- GPU state ---
  std::unique_ptr< GpuDeviceDetector > m_gpuDetector;
  GpuDeviceCounts m_deviceCounts;
  bool m_isNvidiaSmiInstalled;
  GpuDataCallback m_gpuDataCallback;
  std::optional< std::string > m_amdIGpuHwmonPath;
  std::optional< std::string > m_amdDGpuHwmonPath;
  std::optional< std::string > m_intelIGpuDrmPath;
  int m_hwmonIGpuRetryCount;
  int m_hwmonDGpuRetryCount;

  // GPU RAPL for Intel iGPU power
  std::unique_ptr< IntelRAPLController > m_intelRAPLGpu;
  std::unique_ptr< PowerController > m_intelGpuPowerController;

  // --- CPU power state ---
  std::unique_ptr< IntelRAPLController > m_intelRAPLCpu;
  std::unique_ptr< PowerController > m_cpuPowerController;
  bool m_RAPLConstraint0Status;
  bool m_RAPLConstraint1Status;
  bool m_RAPLConstraint2Status;
  std::function< void( const std::string & ) > m_cpuPowerUpdateCallback;
  std::function< bool() > m_getSensorDataCollectionStatus;

  // --- Prime state ---
  std::function< void( const std::string & ) > m_setPrimeState;
  bool m_primeSupported;

  // --- Webcam state ---
  WebcamHwReader m_webcamHwReader;
  WebcamStatusCallback m_webcamStatusCallback;

  // --- Cycle counters for staggered polling ---
  uint32_t m_cycleCounter;

  // GPU methods
  void initGpu();
  [[nodiscard]] bool checkAmdIGpuHwmonPath() noexcept;
  [[nodiscard]] bool checkAmdDGpuHwmonPath() noexcept;
  [[nodiscard]] std::string getAmdIGpuHwmonPathImpl() const noexcept;
  [[nodiscard]] std::string getAmdDGpuHwmonPathImpl() const noexcept;
  [[nodiscard]] std::string getIntelIGpuDrmPathImpl() const noexcept;
  [[nodiscard]] bool checkNvidiaSmiInstalledImpl() const noexcept;
  [[nodiscard]] IGpuInfo getIGpuValues() noexcept;
  [[nodiscard]] IGpuInfo getIntelIGpuValues( const IGpuInfo &base ) const noexcept;
  [[nodiscard]] IGpuInfo getAmdIGpuValues( const IGpuInfo &base ) const noexcept;
  [[nodiscard]] DGpuInfo getDGpuValues() noexcept;
  [[nodiscard]] DGpuInfo getNvidiaDGpuValues() const noexcept;
  [[nodiscard]] DGpuInfo getAmdDGpuValues( const DGpuInfo &base ) const noexcept;
  [[nodiscard]] DGpuInfo parseNvidiaOutput( const std::string &output ) const noexcept;
  [[nodiscard]] double parseNumberWithMetric( const std::string &value ) const noexcept;
  [[nodiscard]] double parseMaxAmdFreq( const std::string &frequencyString ) const noexcept;

  // CPU power methods
  void initCpuPower();
  void updateCpuPower();
  [[nodiscard]] double getCpuCurrentPower();
  [[nodiscard]] double getCpuMaxPowerLimit();

  // Prime methods
  void initPrime();
  void updatePrimeStatus() noexcept;
  [[nodiscard]] bool checkPrimeSupported() const noexcept;
  [[nodiscard]] std::string checkPrimeStatus() const noexcept;
  [[nodiscard]] std::string transformPrimeStatus( const std::string &status ) const noexcept;

  // Webcam methods
  void updateWebcamStatus() noexcept;
};
