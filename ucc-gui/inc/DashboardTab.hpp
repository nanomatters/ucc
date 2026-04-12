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
#include <QFrame>
#include <QLabel>
#include <QPushButton>
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
    explicit DashboardTab( SystemMonitor *systemMonitor, ProfileManager *profileManager,
                          const QString &laptopModel = {},
                          const QString &cpuModel = {},
                          const QString &dGpuModel = {},
                          const QString &iGpuModel = {},
                          const QString &ramSummary = {},
                          const QString &ramModules = {},
                          QWidget *parent = nullptr );
    ~DashboardTab() override = default;

  signals:

  private slots:
    void onCpuTempChanged();
    void onCpuFrequencyChanged();
    void onCpuPowerChanged();
    void onGpuTempChanged();
    void onGpuFrequencyChanged();
    void onGpuPowerChanged();
    void onIGpuFrequencyChanged();
    void onIGpuPowerChanged();
    void onIGpuTempChanged();
    void onFanSpeedChanged();
    void onGpuFanSpeedChanged();
    void onDGpuComputeUtilChanged();
    void onDGpuMemoryUtilChanged();
    void onDGpuPstateChanged();
    void onDGpuClockOffsetsChanged();

  private:
    void setupUI();
    void connectSignals();
    void switchGpuView( bool showIGpu );
    void updateGpuSwitchVisibility();

    SystemMonitor *m_systemMonitor;
    ProfileManager *m_profileManager;

    // Dashboard widgets
    QLabel *m_activeProfileLabel = nullptr;
    QLabel *m_cpuTempLabel = nullptr;
    QLabel *m_cpuFrequencyLabel = nullptr;
    QLabel *m_gpuTempLabel = nullptr;
    QLabel *m_gpuFrequencyLabel = nullptr;
    QLabel *m_fanSpeedLabel = nullptr;
    QLabel *m_gpuFanSpeedLabel = nullptr;
    QLabel *m_cpuPowerLabel = nullptr;
    QLabel *m_ramSummaryLabel = nullptr;
    QLabel *m_ramModulesLabel = nullptr;
    QLabel *m_gpuPowerLabel = nullptr;
    QLabel *m_iGpuTempLabel = nullptr;
    QLabel *m_iGpuFanSpeedLabel = nullptr;
    QLabel *m_iGpuFrequencyLabel = nullptr;
    QLabel *m_iGpuPowerLabel = nullptr;
    // Extended dGPU metrics (second row, NVIDIA-only)
    QLabel *m_gpuComputeUtilLabel = nullptr;
    QLabel *m_gpuMemoryUtilLabel  = nullptr;
    QLabel *m_gpuPstateLabel      = nullptr;
    QLabel *m_gpuClockOffsetLabel = nullptr;
    QWidget *m_dGpuExtraRow  = nullptr;
    QFrame  *m_dGpuExtraHSep = nullptr;
    QWidget *m_dGpuBottomCaps = nullptr;

    // System hardware info strings
    QString m_laptopModel;
    QString m_cpuModel;
    QString m_dGpuModel;
    QString m_iGpuModel;
    QString m_ramSummary;
    QString m_ramModules;

    // GPU section header label (updated when toggling dGPU / iGPU)
    QLabel *m_gpuHeaderLabel = nullptr;

    // GPU view toggle (dGPU / iGPU)
    QPushButton *m_gpuToggleButton = nullptr;
    QWidget *m_dGpuGaugeContainer = nullptr;
    QWidget *m_iGpuGaugeContainer = nullptr;
    bool m_showingIGpu = false;
    bool m_hasDGpuData = false;
    bool m_hasIGpuData = false;

    // Theme colors
    QString m_ringColorHex = "#d32f2f";  // Red for disconnected/disabled state
  };
}