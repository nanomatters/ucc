#pragma once

#include <QLatin1StringView>

namespace ucc::dbus::keys
{

namespace common
{
inline constexpr auto id = QLatin1StringView( "id" );
inline constexpr auto name = QLatin1StringView( "name" );
inline constexpr auto editable = QLatin1StringView( "editable" );
inline constexpr auto label = QLatin1StringView( "label" );
inline constexpr auto timestamp = QLatin1StringView( "timestamp" );
inline constexpr auto data = QLatin1StringView( "data" );
}

namespace systemInfo
{
inline constexpr auto cpuModel = QLatin1StringView( "cpuModel" );
inline constexpr auto iGpuModel = QLatin1StringView( "iGpuModel" );
inline constexpr auto dGpuModel = QLatin1StringView( "dGpuModel" );
inline constexpr auto manufacturer = QLatin1StringView( "manufacturer" );
inline constexpr auto systemModel = QLatin1StringView( "systemModel" );
inline constexpr auto laptopModel = QLatin1StringView( "laptopModel" );
inline constexpr auto productSKU = QLatin1StringView( "productSKU" );
inline constexpr auto boardName = QLatin1StringView( "boardName" );
inline constexpr auto boardVendor = QLatin1StringView( "boardVendor" );
inline constexpr auto sysVendor = QLatin1StringView( "sysVendor" );
inline constexpr auto productName = QLatin1StringView( "productName" );
inline constexpr auto chassisType = QLatin1StringView( "chassisType" );
inline constexpr auto ramTotalMiB = QLatin1StringView( "ramTotalMiB" );
inline constexpr auto ramAvailableMiB = QLatin1StringView( "ramAvailableMiB" );
inline constexpr auto ramUsedMiB = QLatin1StringView( "ramUsedMiB" );
inline constexpr auto ramModules = QLatin1StringView( "ramModules" );
inline constexpr auto locator = QLatin1StringView( "locator" );
inline constexpr auto bankLocator = QLatin1StringView( "bankLocator" );
inline constexpr auto type = QLatin1StringView( "type" );
inline constexpr auto partNumber = QLatin1StringView( "partNumber" );
inline constexpr auto serialNumber = QLatin1StringView( "serialNumber" );
inline constexpr auto sizeMiB = QLatin1StringView( "sizeMiB" );
inline constexpr auto configuredSpeedMTs = QLatin1StringView( "configuredSpeedMTs" );
inline constexpr auto maxSpeedMTs = QLatin1StringView( "maxSpeedMTs" );
inline constexpr auto configuredVoltageMv = QLatin1StringView( "configuredVoltageMv" );
}

namespace thermal
{
inline constexpr auto strategy = QLatin1StringView( "strategy" );
inline constexpr auto sensorIds = QLatin1StringView( "sensorIds" );
inline constexpr auto weights = QLatin1StringView( "weights" );
inline constexpr auto thermalSourceId = QLatin1StringView( "thermalSourceId" );
inline constexpr auto fanIds = QLatin1StringView( "fanIds" );
inline constexpr auto deviceType = QLatin1StringView( "deviceType" );
inline constexpr auto sourceName = QLatin1StringView( "sourceName" );
inline constexpr auto source = QLatin1StringView( "source" );
inline constexpr auto sourceDisplay = QLatin1StringView( "sourceDisplay" );
inline constexpr auto displayLabel = QLatin1StringView( "displayLabel" );
inline constexpr auto hwmonPath = QLatin1StringView( "hwmonPath" );
inline constexpr auto index = QLatin1StringView( "index" );
inline constexpr auto canRead = QLatin1StringView( "canRead" );
inline constexpr auto canControl = QLatin1StringView( "canControl" );
inline constexpr auto category = QLatin1StringView( "category" );
}

namespace profile
{
inline constexpr auto profileId = QLatin1StringView( "profileId" );
inline constexpr auto profileName = QLatin1StringView( "profileName" );
inline constexpr auto fanProfileId = QLatin1StringView( "fanProfileId" );
inline constexpr auto keyboardProfileId = QLatin1StringView( "keyboardProfileId" );
inline constexpr auto savedGpuProfileId = QLatin1StringView( "savedGpuProfileId" );
inline constexpr auto appliedGpuProfileId = QLatin1StringView( "appliedGpuProfileId" );
inline constexpr auto appliedByApp = QLatin1StringView( "appliedByApp" );
inline constexpr auto appliedByPid = QLatin1StringView( "appliedByPid" );
}

namespace telemetry
{
inline constexpr auto temp = QLatin1StringView( "temp" );
inline constexpr auto speed = QLatin1StringView( "speed" );
inline constexpr auto duty = QLatin1StringView( "duty" );
inline constexpr auto rpm = QLatin1StringView( "rpm" );
inline constexpr auto powerDraw = QLatin1StringView( "powerDraw" );
}

namespace monitor
{
inline constexpr auto key = QLatin1StringView( "key" );
inline constexpr auto group = QLatin1StringView( "group" );
inline constexpr auto unit = QLatin1StringView( "unit" );
inline constexpr auto selectedApp = QLatin1StringView( "selectedApp" );
inline constexpr auto currentApp = QLatin1StringView( "currentApp" );
inline constexpr auto currentPid = QLatin1StringView( "currentPid" );
inline constexpr auto apps = QLatin1StringView( "apps" );
}

namespace keyboard
{
inline constexpr auto modes = QLatin1StringView( "modes" );
inline constexpr auto zones = QLatin1StringView( "zones" );
inline constexpr auto maxBrightness = QLatin1StringView( "maxBrightness" );
}

namespace autoTune
{
inline constexpr auto running = QLatin1StringView( "running" );
inline constexpr auto resumeAvailable = QLatin1StringView( "resumeAvailable" );
inline constexpr auto message = QLatin1StringView( "message" );
inline constexpr auto suspendReason = QLatin1StringView( "suspendReason" );
inline constexpr auto checkpointApp = QLatin1StringView( "checkpointApp" );
inline constexpr auto lastApp = QLatin1StringView( "lastApp" );
inline constexpr auto lastPid = QLatin1StringView( "lastPid" );
inline constexpr auto lastGpuProfileId = QLatin1StringView( "lastGpuProfileId" );
inline constexpr auto mappedGpuProfileId = QLatin1StringView( "mappedGpuProfileId" );
}

} // namespace ucc::dbus::keys