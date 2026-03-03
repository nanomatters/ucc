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

#include "../NvmlWrapper.hpp"
#include <functional>
#include <memory>
#include <string>

/**
 * @brief NvidiaOCWorker — daemon-side GPU overclocking controller.
 *
 * Wraps NvmlWrapper and exposes high-level operations that the D-Bus
 * service can call directly.  Lives on the main thread (like
 * ProfileSettingsWorker), no DaemonWorker/QThread inheritance needed
 * since all calls are on-demand.
 */
class NvidiaOCWorker
{
public:
  explicit NvidiaOCWorker( std::shared_ptr< NvmlWrapper > nvml,
                           std::function< void( const std::string & ) > logFunction );
  ~NvidiaOCWorker() = default;

  // Non-copyable
  NvidiaOCWorker( const NvidiaOCWorker & ) = delete;
  NvidiaOCWorker &operator=( const NvidiaOCWorker & ) = delete;

  /** @return true if NVML initialised and at least one GPU found */
  [[nodiscard]] bool isAvailable() const noexcept;

  /** @return JSON string with full OC state for device 0 */
  [[nodiscard]] std::string getOCStateJSON( unsigned int deviceIndex = 0 ) const;

  /** Set clock offset for a specific P-state */
  bool setClockOffset( unsigned int deviceIndex,
                       unsigned int clockType,     // 0=Graphics, 1=SM, 2=Mem
                       unsigned int pstate,
                       int offsetMHz );

  /** Set GPU locked clocks range */
  bool setGpuLockedClocks( unsigned int deviceIndex,
                           unsigned int minMHz,
                           unsigned int maxMHz );

  /** Set VRAM locked clocks range */
  bool setVramLockedClocks( unsigned int deviceIndex,
                            unsigned int minMHz,
                            unsigned int maxMHz );

  /** Reset GPU locked clocks */
  bool resetGpuLockedClocks( unsigned int deviceIndex );

  /** Reset VRAM locked clocks */
  bool resetVramLockedClocks( unsigned int deviceIndex );

  /** Reset all clock offsets to zero */
  bool resetAllClockOffsets( unsigned int deviceIndex );

  /** Set power limit in watts */
  bool setPowerLimit( unsigned int deviceIndex, double watts );

  /** Reset power limit to default */
  bool resetPowerLimit( unsigned int deviceIndex );

  /** Apply a complete GPU OC profile (from JSON). Called during profile activation. */
  bool applyGpuOCProfile( const std::string &profileJSON, unsigned int deviceIndex = 0 );

  /** Reset all OC settings to defaults */
  bool resetAll( unsigned int deviceIndex = 0 );

private:
  void log( const std::string &msg ) const;

  std::shared_ptr< NvmlWrapper > m_nvml;
  std::function< void( const std::string & ) > m_logFunction;
};
