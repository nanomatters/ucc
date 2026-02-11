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
#include <QLabel>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QDBusInterface>
#include <QTimer>

namespace ucc
{
  class SystemMonitor;
  class ProfileManager;
  

  /**
   * @brief Dashboard tab widget for system monitoring
   */
  class DashboardTab : public QWidget
  {
    Q_OBJECT

  public:
    explicit DashboardTab( SystemMonitor *systemMonitor, ProfileManager *profileManager, bool waterCoolerSupported, QWidget *parent = nullptr );
    ~DashboardTab() override = default;

    /** Update the water-cooler enable checkbox without re-triggering signals. */
    void setWaterCoolerEnabled( bool enabled );

  signals:
    void waterCoolerEnableChanged( bool enabled );

  private slots:
    void onCpuTempChanged();
    void onCpuFrequencyChanged();
    void onCpuPowerChanged();
    void onGpuTempChanged();
    void onGpuFrequencyChanged();
    void onGpuPowerChanged();
    void onFanSpeedChanged();
    void onGpuFanSpeedChanged();
    void onWaterCoolerConnected();
    void onWaterCoolerDisconnected();
    void onWaterCoolerDiscoveryStarted();
    void onWaterCoolerDiscoveryFinished();
    void onWaterCoolerConnectionError(const QString &error);
    void onWaterCoolerFanSpeedChanged();
    void onWaterCoolerPumpLevelChanged();

  private:
    void setupUI();
    void connectSignals();
    void updateWaterCoolerStatus();

    SystemMonitor *m_systemMonitor;
    ProfileManager *m_profileManager;
    // DBus interface for water cooler status
    QDBusInterface *m_waterCoolerDbus = nullptr;
    QTimer *m_waterCoolerPollTimer = nullptr;

    // Dashboard widgets
    QLabel *m_activeProfileLabel = nullptr;
    QLabel *m_waterCoolerStatusLabel = nullptr;
    QLabel *m_cpuTempLabel = nullptr;
    QLabel *m_cpuFrequencyLabel = nullptr;
    QLabel *m_gpuTempLabel = nullptr;
    QLabel *m_gpuFrequencyLabel = nullptr;
    QLabel *m_fanSpeedLabel = nullptr;
    QLabel *m_gpuFanSpeedLabel = nullptr;
    QLabel *m_cpuPowerLabel = nullptr;
    QLabel *m_gpuPowerLabel = nullptr;
    QLabel *m_waterCoolerFanSpeedLabel = nullptr;
    QLabel *m_waterCoolerPumpLabel = nullptr;
    QGridLayout *m_waterCoolerGrid = nullptr;
    QLabel *m_waterCoolerHeader = nullptr;
    QCheckBox *m_waterCoolerEnableCheckBox = nullptr;

    bool m_waterCoolerSupported = false;
  };
}