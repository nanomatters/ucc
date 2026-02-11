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

#include "LCTWaterCoolerController.hpp"

#include <QDBusInterface>
#include <QDBusReply>
#include <QTimer>
#include <QVariant>
#include <QDebug>
#include <iostream>

namespace ucc
{

// GUI proxy: no local Bluetooth UUID constants

LCTWaterCoolerController::LCTWaterCoolerController(QObject *parent)
    : QObject(parent)
{
    m_isDiscovering = false;
    m_isConnected = false;

    // DBus interface to daemon
    m_dbus = std::make_unique<QDBusInterface>(QStringLiteral("com.uniwill.uccd"),
                                             QStringLiteral("/com/uniwill/uccd"),
                                             QStringLiteral("com.uniwill.uccd"),
                                             QDBusConnection::systemBus(), this);

    // Poll daemon for availability/connection and emit signals when state changes
    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &LCTWaterCoolerController::pollDaemonState);
    m_pollTimer->start(1000);
}

LCTWaterCoolerController::~LCTWaterCoolerController()
{
    if (m_pollTimer) {
        m_pollTimer->stop();
    }
}

bool LCTWaterCoolerController::startDiscovery()
{
    if (m_isDiscovering)
        return true;

    m_isDiscovering = true;
    m_discoveredDevices.clear();
    emit discoveryStarted();

    // Query daemon for availability; if available, present a single logical device
    if (m_dbus && m_dbus->isValid()) {
        QDBusReply<bool> avail = m_dbus->call(QStringLiteral("GetWaterCoolerAvailable"));
        if (avail.isValid() && avail.value()) {
            DeviceInfo info;
            info.uuid = QStringLiteral("local");
            info.name = QStringLiteral("System Water Cooler");
            info.rssi = 0;
            m_discoveredDevices.append(info);
            emit deviceDiscovered(info);
        }
    }

    m_isDiscovering = false;
    emit discoveryFinished();
    return true;
}

void LCTWaterCoolerController::stopDiscovery()
{
    if (m_isDiscovering) {
        m_isDiscovering = false;
        emit discoveryFinished();
    }
}

bool LCTWaterCoolerController::isDiscovering() const
{
    return m_isDiscovering;
}

QList<DeviceInfo> LCTWaterCoolerController::getDeviceList() const
{
    return m_discoveredDevices;
}

bool LCTWaterCoolerController::connectToDevice(const QString &deviceUuid)
{
    Q_UNUSED(deviceUuid)

    // Connection management is handled by the daemon. We just reflect its state.
    if (!m_dbus || !m_dbus->isValid()) {
        emit connectionError("DBus interface not available");
        return false;
    }

    QDBusReply<bool> reply = m_dbus->call(QStringLiteral("GetWaterCoolerConnected"));
    if (reply.isValid() && reply.value()) {
        // Already connected
        m_isConnected = true;
        emit connected();
        return true;
    }

    // If not connected, still return true to indicate the request was accepted;
    // the poller will emit connected() when daemon connects.
    return true;
}

void LCTWaterCoolerController::disconnectFromDevice()
{
    // Daemon manages connection lifecycle. Nothing to do here.
}

bool LCTWaterCoolerController::isConnected() const
{
    return m_isConnected;
}

LCTDeviceModel LCTWaterCoolerController::getConnectedModel() const
{
    return m_connectedModel;
}

bool LCTWaterCoolerController::setFanSpeed(int dutyCyclePercent)
{
    if (!m_dbus || !m_dbus->isValid() || dutyCyclePercent < 0 || dutyCyclePercent > 100)
        return false;

    QDBusReply<bool> reply = m_dbus->call(QStringLiteral("SetWaterCoolerFanSpeed"), dutyCyclePercent);
    return reply.isValid() && reply.value();
}

bool LCTWaterCoolerController::setPumpVoltage(PumpVoltage voltage)
{
    if (!m_dbus || !m_dbus->isValid())
        return false;

    int v = static_cast<int>(voltage);
    QDBusReply<bool> reply = m_dbus->call(QStringLiteral("SetWaterCoolerPumpVoltage"), v);
    return reply.isValid() && reply.value();
}

bool LCTWaterCoolerController::turnOffFan()
{
    if (!m_dbus || !m_dbus->isValid())
        return false;

    QDBusReply<bool> reply = m_dbus->call(QStringLiteral("TurnOffWaterCoolerFan"));
    return reply.isValid() && reply.value();
}

bool LCTWaterCoolerController::turnOffPump()
{
    if (!m_dbus || !m_dbus->isValid())
        return false;

    QDBusReply<bool> reply = m_dbus->call(QStringLiteral("TurnOffWaterCoolerPump"));
    return reply.isValid() && reply.value();
}

bool LCTWaterCoolerController::setLEDColor(int red, int green, int blue, RGBState mode)
{
    if (!m_dbus || !m_dbus->isValid())
        return false;

    QDBusReply<bool> reply = m_dbus->call(QStringLiteral("SetWaterCoolerLEDColor"), red, green, blue, static_cast<int>(mode));
    return reply.isValid() && reply.value();
}

bool LCTWaterCoolerController::turnOffLED()
{
    if (!m_dbus || !m_dbus->isValid())
        return false;

    QDBusReply<bool> reply = m_dbus->call(QStringLiteral("TurnOffWaterCoolerLED"));
    return reply.isValid() && reply.value();
}

// Discovery is handled by daemon; GUI no longer discovers devices locally.

void LCTWaterCoolerController::onDiscoveryFinished()
{
    // noop - discovery is proxied to daemon
}

void LCTWaterCoolerController::onConnected()
{
    Q_UNUSED(m_connectedModel)
}

void LCTWaterCoolerController::onDisconnected()
{
    Q_UNUSED(m_connectedModel)
}

void LCTWaterCoolerController::onServiceDiscoveryFinished()
{
    // GUI proxy: service discovery is handled in daemon; noop here.
}

void LCTWaterCoolerController::onCharacteristicRead(const QByteArray &value)
{
    Q_UNUSED(value)
}

void LCTWaterCoolerController::onCharacteristicWritten(const QByteArray &value)
{
    Q_UNUSED(value)
}

void LCTWaterCoolerController::onError(int error)
{
    Q_UNUSED(error)
}

LCTDeviceModel LCTWaterCoolerController::deviceModelFromName(const QString &name) const
{
    QString lowerName = name.toLower();
    if (lowerName.contains("lct22002")) {
        return LCTDeviceModel::LCT22002;
    } else if (lowerName.contains("lct21001")) {
        return LCTDeviceModel::LCT21001;
    }
    return LCTDeviceModel::Unknown;  // Not an LCT device
}

bool LCTWaterCoolerController::writeCommand(const QByteArray &data)
{
    Q_UNUSED(data)
    // GUI no longer sends raw BLE commands directly.
    emit controlError("Direct write not supported in GUI proxy");
    return false;
}

bool LCTWaterCoolerController::writeReceive(const QByteArray &data)
{
    Q_UNUSED(data)
    emit controlError("Direct write-receive not supported in GUI proxy");
    return false;
}

void LCTWaterCoolerController::pollDaemonState()
{
    if (!m_dbus || !m_dbus->isValid())
        return;

    // Poll availability
    QDBusReply<bool> avail = m_dbus->call(QStringLiteral("GetWaterCoolerAvailable"));
    bool available = avail.isValid() && avail.value();

    // Poll connected
    QDBusReply<bool> conn = m_dbus->call(QStringLiteral("GetWaterCoolerConnected"));
    bool isNowConnected = conn.isValid() && conn.value();

    // Update device list when availability changes
    if (available && m_discoveredDevices.isEmpty()) {
        DeviceInfo info;
        info.uuid = QStringLiteral("local");
        info.name = QStringLiteral("System Water Cooler");
        info.rssi = 0;
        m_discoveredDevices.append(info);
        emit deviceDiscovered(info);
    } else if (!available && !m_discoveredDevices.isEmpty()) {
        m_discoveredDevices.clear();
        emit discoveryFinished();
    }

    // Emit connected/disconnected on change
    if (isNowConnected != m_isConnected) {
        m_isConnected = isNowConnected;
        if (m_isConnected)
            emit connected();
        else
            emit disconnected();
    }
}


}