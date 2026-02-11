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
#include <QSlider>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QMainWindow>
#include <QStatusBar>
#include <QListWidget>
#include <QProgressBar>
#include <QMessageBox>

namespace ucc
{

class SystemMonitor;

/**
 * @brief Hardware tab widget for hardware controls
 */
class HardwareTab : public QWidget
{
    Q_OBJECT

public:
    explicit HardwareTab( SystemMonitor *systemMonitor, QWidget *parent = nullptr );
    ~HardwareTab() override = default;

private slots:
    void onDisplayBrightnessSliderChanged( int value );
    void onWebcamToggled( bool checked );
    void onFnLockToggled( bool checked );

    // Feedback from daemon via SystemMonitor
    void onDisplayBrightnessUpdated();
    void onWebcamEnabledUpdated();
    void onFnLockUpdated();

private:
    void setupUI( QWidget *parent );
    void connectSignals();
    void initializeFromMonitor();

    SystemMonitor *m_systemMonitor;
    bool m_updatingFromMonitor = false;

    // Display controls
    QSlider *m_displayBrightnessSlider = nullptr;
    QLabel *m_displayBrightnessValueLabel = nullptr;

    // Quick controls
    QCheckBox *m_webcamCheckBox = nullptr;
    QCheckBox *m_fnLockCheckBox = nullptr;
};

}