#pragma once

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

namespace ucc::dbus
{

struct ProfileSummaryDto
{
  QString id;
  QString name;
  bool editable = false;

  QJsonObject toJson() const
  {
    return { { "id", id }, { "name", name }, { "editable", editable } };
  }
};

struct RamModuleDto
{
  QString locator;
  QString bankLocator;
  QString type;
  QString manufacturer;
  QString partNumber;
  QString serialNumber;
  int sizeMiB = 0;
  int configuredSpeedMTs = 0;
  int maxSpeedMTs = 0;
  int configuredVoltageMv = 0;

  QJsonObject toJson() const
  {
    return { { "locator", locator }, { "bankLocator", bankLocator }, { "type", type },
             { "manufacturer", manufacturer }, { "partNumber", partNumber },
             { "serialNumber", serialNumber }, { "sizeMiB", sizeMiB },
             { "configuredSpeedMTs", configuredSpeedMTs }, { "maxSpeedMTs", maxSpeedMTs },
             { "configuredVoltageMv", configuredVoltageMv } };
  }
};

struct SystemInfoDto
{
  QString cpuModel;
  QString iGpuModel;
  QString dGpuModel;
  QString manufacturer;
  QString systemModel;
  QString laptopModel;
  QString productSKU;
  QString boardName;
  QString boardVendor;
  QString sysVendor;
  QString productName;
  QString chassisType;
  int ramTotalMiB = 0;
  int ramAvailableMiB = 0;
  int ramUsedMiB = 0;
  QList< RamModuleDto > ramModules;

  QJsonObject toJson() const
  {
    QJsonArray modules;
    for ( const auto &m : ramModules )
      modules.append( m.toJson() );
    return { { "cpuModel", cpuModel }, { "iGpuModel", iGpuModel }, { "dGpuModel", dGpuModel },
             { "manufacturer", manufacturer }, { "systemModel", systemModel },
             { "laptopModel", laptopModel }, { "productSKU", productSKU },
             { "boardName", boardName }, { "boardVendor", boardVendor },
             { "sysVendor", sysVendor }, { "productName", productName },
             { "chassisType", chassisType }, { "ramTotalMiB", ramTotalMiB },
             { "ramAvailableMiB", ramAvailableMiB }, { "ramUsedMiB", ramUsedMiB },
             { "ramModules", modules } };
  }
};

struct HardwareFanDeviceDto
{
  QString id;
  QString label;
  QString sourceName;
  QString hwmonPath;
  int index = 0;
  bool canRead = false;
  bool canControl = false;
  QString deviceType;

  QJsonObject toJson() const
  {
    return { { "id", id }, { "label", label }, { "sourceName", sourceName },
             { "hwmonPath", hwmonPath }, { "index", index },
             { "canRead", canRead }, { "canControl", canControl },
             { "deviceType", deviceType } };
  }
};

struct HardwareSensorDto
{
  QString id;
  QString label;
  QString category;
  QString source;
  QString sourceDisplay;
  QString displayLabel;
  QString hwmonPath;
  int index = 0;

  QJsonObject toJson() const
  {
    QJsonObject obj{ { "id", id }, { "label", label }, { "category", category },
                     { "source", source }, { "hwmonPath", hwmonPath }, { "index", index } };
    if ( !sourceDisplay.isEmpty() )
      obj[ "sourceDisplay" ] = sourceDisplay;
    if ( !displayLabel.isEmpty() )
      obj[ "displayLabel" ] = displayLabel;
    return obj;
  }
};

struct ThermalSourceDto
{
  QString id;
  QString label;
  QString strategy;
  QStringList sensorIds;
  QList< double > weights;

  QJsonObject toJson() const
  {
    QJsonArray ids;
    for ( const auto &s : sensorIds )
      ids.append( s );
    QJsonArray w;
    for ( double v : weights )
      w.append( v );
    return { { "id", id }, { "label", label }, { "strategy", strategy },
             { "sensorIds", ids }, { "weights", w } };
  }
};

struct FanZoneDto
{
  QString id;
  QString name;
  QString deviceType;
  QString thermalSourceId;
  QStringList fanIds;

  QJsonObject toJson() const
  {
    QJsonArray ids;
    for ( const auto &f : fanIds )
      ids.append( f );
    return { { "id", id }, { "name", name }, { "deviceType", deviceType },
             { "thermalSourceId", thermalSourceId }, { "fanIds", ids } };
  }
};

struct FanCurvePointDto
{
  int temp = 0;
  int speed = 0;

  QJsonObject toJson() const
  {
    return { { "temp", temp }, { "speed", speed } };
  }
};

struct FanZoneCurveDto
{
  QString id;
  QString name;
  QString deviceType;
  QString thermalSourceId;
  QStringList fanIds;
  QList< FanCurvePointDto > curve;
  int hysteresisDeg = 3;
  bool enabled = true;

  QJsonObject toJson() const
  {
    QJsonArray ids;
    for ( const auto &f : fanIds )
      ids.append( f );
    QJsonArray curveArr;
    for ( const auto &pt : curve )
      curveArr.append( pt.toJson() );
    QJsonObject obj{ { "id", id }, { "name", name }, { "deviceType", deviceType },
                     { "thermalSourceId", thermalSourceId }, { "fanIds", ids },
                     { "curve", curveArr }, { "hysteresisDeg", hysteresisDeg },
                     { "enabled", enabled } };
    return obj;
  }
};

struct TimedValueDto
{
  qint64 timestamp = 0;
  int data = 0;

  QJsonObject toJson() const
  {
    return { { "timestamp", timestamp }, { "data", data } };
  }
};

struct FanDataDto
{
  TimedValueDto speed;
  TimedValueDto temp;

  QJsonObject toJson() const
  {
    return { { "speed", speed.toJson() }, { "temp", temp.toJson() } };
  }
};

struct ZoneTelemetryDto
{
  int temp = 0;
  int duty = 0;
  int rpm = -1;

  QJsonObject toJson() const
  {
    return { { "temp", temp }, { "duty", duty }, { "rpm", rpm } };
  }
};

struct AppliedProfilesDto
{
  QString profileId;
  QString profileName;
  QString fanProfileId;
  bool wcAutoControl = false;
  QString keyboardProfileId;
  QString savedGpuProfileId;
  QString appliedGpuProfileId;
  QString appliedByApp;
  qint64 appliedByPid = 0;

  QJsonObject toJson() const
  {
    return { { "profileId", profileId }, { "profileName", profileName },
             { "fanProfileId", fanProfileId }, { "wcAutoControl", wcAutoControl },
             { "keyboardProfileId", keyboardProfileId }, { "savedGpuProfileId", savedGpuProfileId },
             { "appliedGpuProfileId", appliedGpuProfileId }, { "appliedByApp", appliedByApp },
             { "appliedByPid", appliedByPid } };
  }
};

struct DGpuInfoDto
{
  int temp = -1;
  int coreFrequency = -1;
  int vramFrequency = -1;
  int maxCoreFrequency = -1;
  double powerDraw = -1.0;
  int maxPowerLimit = -1;
  int enforcedPowerLimit = -1;
  int computeUtilPct = -1;
  int memoryUtilPct = -1;
  int vramUsedMiB = -1;
  int vramTotalMiB = -1;
  QString perfLimitReason;
  int encoderUtilPct = -1;
  int decoderUtilPct = -1;
  int currentPstate = -1;
  int grClockOffsetMHz = -999;
  int memClockOffsetMHz = -999;
  int coreVoltageMv = -1;
  int fanSpeedPct = -1;
  int thermalMarginC = -1;
  int d0MetricsUsage = -1;

  QJsonObject toJson() const
  {
    return { { "temp", temp }, { "coreFrequency", coreFrequency },
             { "vramFrequency", vramFrequency }, { "maxCoreFrequency", maxCoreFrequency },
             { "powerDraw", powerDraw }, { "maxPowerLimit", maxPowerLimit },
             { "enforcedPowerLimit", enforcedPowerLimit }, { "computeUtilPct", computeUtilPct },
             { "memoryUtilPct", memoryUtilPct }, { "vramUsedMiB", vramUsedMiB },
             { "vramTotalMiB", vramTotalMiB }, { "perfLimitReason", perfLimitReason },
             { "encoderUtilPct", encoderUtilPct }, { "decoderUtilPct", decoderUtilPct },
             { "currentPstate", currentPstate }, { "grClockOffsetMHz", grClockOffsetMHz },
             { "memClockOffsetMHz", memClockOffsetMHz }, { "coreVoltageMv", coreVoltageMv },
             { "fanSpeedPct", fanSpeedPct }, { "thermalMarginC", thermalMarginC },
             { "d0MetricsUsage", d0MetricsUsage } };
  }
};

struct IGpuInfoDto
{
  int temp = -1;
  int coreFrequency = -1;
  int maxCoreFrequency = -1;
  double powerDraw = -1.0;
  QString vendor;

  QJsonObject toJson() const
  {
    return { { "temp", temp }, { "coreFrequency", coreFrequency },
             { "maxCoreFrequency", maxCoreFrequency }, { "powerDraw", powerDraw },
             { "vendor", vendor } };
  }
};

struct CpuPowerDto
{
  double powerDraw = -1.0;

  QJsonObject toJson() const
  {
    return { { "powerDraw", powerDraw } };
  }
};

struct KeyboardBacklightCapabilitiesDto
{
  QList< int > modes;
  int zones = 0;
  int maxBrightness = 0;

  QJsonObject toJson() const
  {
    QJsonArray modesArray;
    for ( int m : modes )
      modesArray.append( m );
    return { { "modes", modesArray }, { "zones", zones }, { "maxBrightness", maxBrightness } };
  }
};

struct OdmPowerLimitDto
{
  int current = 0;
  int min = 0;
  int max = 0;

  QJsonObject toJson() const
  {
    return { { "current", current }, { "min", min }, { "max", max } };
  }
};

struct MonitorSourceDto
{
  QString key;
  QString label;
  QString group;
  QString unit;

  QJsonObject toJson() const
  {
    return { { "key", key }, { "label", label }, { "group", group }, { "unit", unit } };
  }
};

struct FpsSourcesDto
{
  QString selectedApp;
  QString currentApp;
  qint64 currentPid = 0;
  QStringList apps;

  QJsonObject toJson() const
  {
    QJsonArray appsArray;
    for ( const auto &a : apps )
      appsArray.append( a );
    return { { "selectedApp", selectedApp }, { "currentApp", currentApp },
             { "currentPid", currentPid }, { "apps", appsArray } };
  }
};

struct AutoProgressStatusDto
{
  bool running = false;
  bool resumeAvailable = false;
  QString message;
  QString suspendReason;
  QString checkpointApp;

  QJsonObject toJson() const
  {
    return { { "running", running }, { "resumeAvailable", resumeAvailable },
             { "message", message }, { "suspendReason", suspendReason },
             { "checkpointApp", checkpointApp } };
  }
};

template< typename T >
inline QDBusArgument &writeScalarStruct( QDBusArgument &argument, const T &value )
{
  return argument << value;
}

inline QDBusArgument &operator<<( QDBusArgument &argument, const ProfileSummaryDto &value )
{
  argument.beginStructure();
  argument << value.id << value.name << value.editable;
  argument.endStructure();
  return argument;
}

inline const QDBusArgument &operator>>( const QDBusArgument &argument, ProfileSummaryDto &value )
{
  argument.beginStructure();
  argument >> value.id >> value.name >> value.editable;
  argument.endStructure();
  return argument;
}

#define UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS(TypeName, WriteFields, ReadFields) \
  inline QDBusArgument &operator<<( QDBusArgument &argument, const TypeName &value ) \
  { \
    argument.beginStructure(); \
    WriteFields \
    argument.endStructure(); \
    return argument; \
  } \
  inline const QDBusArgument &operator>>( const QDBusArgument &argument, TypeName &value ) \
  { \
    argument.beginStructure(); \
    ReadFields \
    argument.endStructure(); \
    return argument; \
  }

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( RamModuleDto,
  argument << value.locator << value.bankLocator << value.type << value.manufacturer
           << value.partNumber << value.serialNumber << value.sizeMiB
           << value.configuredSpeedMTs << value.maxSpeedMTs << value.configuredVoltageMv;,
  argument >> value.locator >> value.bankLocator >> value.type >> value.manufacturer
           >> value.partNumber >> value.serialNumber >> value.sizeMiB
           >> value.configuredSpeedMTs >> value.maxSpeedMTs >> value.configuredVoltageMv; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( SystemInfoDto,
  argument << value.cpuModel << value.iGpuModel << value.dGpuModel << value.manufacturer
           << value.systemModel << value.laptopModel << value.productSKU << value.boardName
           << value.boardVendor << value.sysVendor << value.productName << value.chassisType
           << value.ramTotalMiB << value.ramAvailableMiB << value.ramUsedMiB << value.ramModules;,
  argument >> value.cpuModel >> value.iGpuModel >> value.dGpuModel >> value.manufacturer
           >> value.systemModel >> value.laptopModel >> value.productSKU >> value.boardName
           >> value.boardVendor >> value.sysVendor >> value.productName >> value.chassisType
           >> value.ramTotalMiB >> value.ramAvailableMiB >> value.ramUsedMiB >> value.ramModules; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( HardwareFanDeviceDto,
  argument << value.id << value.label << value.sourceName << value.hwmonPath
           << value.index << value.canRead << value.canControl << value.deviceType;,
  argument >> value.id >> value.label >> value.sourceName >> value.hwmonPath
           >> value.index >> value.canRead >> value.canControl >> value.deviceType; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( HardwareSensorDto,
  argument << value.id << value.label << value.category << value.source << value.sourceDisplay
           << value.displayLabel << value.hwmonPath << value.index;,
  argument >> value.id >> value.label >> value.category >> value.source >> value.sourceDisplay
           >> value.displayLabel >> value.hwmonPath >> value.index; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( ThermalSourceDto,
  argument << value.id << value.label << value.strategy << value.sensorIds << value.weights;,
  argument >> value.id >> value.label >> value.strategy >> value.sensorIds >> value.weights; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( FanZoneDto,
  argument << value.id << value.name << value.deviceType << value.thermalSourceId << value.fanIds;,
  argument >> value.id >> value.name >> value.deviceType >> value.thermalSourceId >> value.fanIds; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( FanCurvePointDto,
  argument << value.temp << value.speed;,
  argument >> value.temp >> value.speed; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( FanZoneCurveDto,
  argument << value.id << value.name << value.deviceType << value.thermalSourceId << value.fanIds
           << value.curve << value.hysteresisDeg << value.enabled;,
  argument >> value.id >> value.name >> value.deviceType >> value.thermalSourceId >> value.fanIds
           >> value.curve >> value.hysteresisDeg >> value.enabled; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( TimedValueDto,
  argument << value.timestamp << value.data;,
  argument >> value.timestamp >> value.data; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( FanDataDto,
  argument << value.speed << value.temp;,
  argument >> value.speed >> value.temp; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( ZoneTelemetryDto,
  argument << value.temp << value.duty << value.rpm;,
  argument >> value.temp >> value.duty >> value.rpm; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( AppliedProfilesDto,
  argument << value.profileId << value.profileName << value.fanProfileId << value.wcAutoControl
           << value.keyboardProfileId << value.savedGpuProfileId << value.appliedGpuProfileId
           << value.appliedByApp << value.appliedByPid;,
  argument >> value.profileId >> value.profileName >> value.fanProfileId >> value.wcAutoControl
           >> value.keyboardProfileId >> value.savedGpuProfileId >> value.appliedGpuProfileId
           >> value.appliedByApp >> value.appliedByPid; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( DGpuInfoDto,
  argument << value.temp << value.coreFrequency << value.vramFrequency << value.maxCoreFrequency
           << value.powerDraw << value.maxPowerLimit << value.enforcedPowerLimit
           << value.computeUtilPct << value.memoryUtilPct << value.vramUsedMiB << value.vramTotalMiB
           << value.perfLimitReason << value.encoderUtilPct << value.decoderUtilPct
           << value.currentPstate << value.grClockOffsetMHz << value.memClockOffsetMHz
           << value.coreVoltageMv << value.fanSpeedPct << value.thermalMarginC << value.d0MetricsUsage;,
  argument >> value.temp >> value.coreFrequency >> value.vramFrequency >> value.maxCoreFrequency
           >> value.powerDraw >> value.maxPowerLimit >> value.enforcedPowerLimit
           >> value.computeUtilPct >> value.memoryUtilPct >> value.vramUsedMiB >> value.vramTotalMiB
           >> value.perfLimitReason >> value.encoderUtilPct >> value.decoderUtilPct
           >> value.currentPstate >> value.grClockOffsetMHz >> value.memClockOffsetMHz
           >> value.coreVoltageMv >> value.fanSpeedPct >> value.thermalMarginC >> value.d0MetricsUsage; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( IGpuInfoDto,
  argument << value.temp << value.coreFrequency << value.maxCoreFrequency << value.powerDraw << value.vendor;,
  argument >> value.temp >> value.coreFrequency >> value.maxCoreFrequency >> value.powerDraw >> value.vendor; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( CpuPowerDto,
  argument << value.powerDraw;,
  argument >> value.powerDraw; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( KeyboardBacklightCapabilitiesDto,
  argument << value.modes << value.zones << value.maxBrightness;,
  argument >> value.modes >> value.zones >> value.maxBrightness; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( OdmPowerLimitDto,
  argument << value.current << value.min << value.max;,
  argument >> value.current >> value.min >> value.max; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( MonitorSourceDto,
  argument << value.key << value.label << value.group << value.unit;,
  argument >> value.key >> value.label >> value.group >> value.unit; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( FpsSourcesDto,
  argument << value.selectedApp << value.currentApp << value.currentPid << value.apps;,
  argument >> value.selectedApp >> value.currentApp >> value.currentPid >> value.apps; )

UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS( AutoProgressStatusDto,
  argument << value.running << value.resumeAvailable << value.message << value.suspendReason
           << value.checkpointApp;,
  argument >> value.running >> value.resumeAvailable >> value.message >> value.suspendReason
           >> value.checkpointApp; )

#undef UCC_DBUS_DEFINE_STRUCT_STREAM_OPERATORS

using ProfileSummaryDtoList = QList< ProfileSummaryDto >;
using RamModuleDtoList = QList< RamModuleDto >;
using HardwareFanDeviceDtoList = QList< HardwareFanDeviceDto >;
using HardwareSensorDtoList = QList< HardwareSensorDto >;
using ThermalSourceDtoList = QList< ThermalSourceDto >;
using FanZoneDtoList = QList< FanZoneDto >;
using FanCurvePointDtoList = QList< FanCurvePointDto >;
using FanZoneCurveDtoList = QList< FanZoneCurveDto >;
using MonitorSourceDtoList = QList< MonitorSourceDto >;
using OdmPowerLimitDtoList = QList< OdmPowerLimitDto >;

inline void registerDbusTypes()
{
  static const bool registered = []() {
    qDBusRegisterMetaType< ProfileSummaryDto >();
    qDBusRegisterMetaType< ProfileSummaryDtoList >();
    qDBusRegisterMetaType< RamModuleDto >();
    qDBusRegisterMetaType< RamModuleDtoList >();
    qDBusRegisterMetaType< SystemInfoDto >();
    qDBusRegisterMetaType< HardwareFanDeviceDto >();
    qDBusRegisterMetaType< HardwareFanDeviceDtoList >();
    qDBusRegisterMetaType< HardwareSensorDto >();
    qDBusRegisterMetaType< HardwareSensorDtoList >();
    qDBusRegisterMetaType< ThermalSourceDto >();
    qDBusRegisterMetaType< ThermalSourceDtoList >();
    qDBusRegisterMetaType< FanZoneDto >();
    qDBusRegisterMetaType< FanZoneDtoList >();
    qDBusRegisterMetaType< FanCurvePointDto >();
    qDBusRegisterMetaType< FanCurvePointDtoList >();
    qDBusRegisterMetaType< FanZoneCurveDto >();
    qDBusRegisterMetaType< FanZoneCurveDtoList >();
    qDBusRegisterMetaType< TimedValueDto >();
    qDBusRegisterMetaType< FanDataDto >();
    qDBusRegisterMetaType< ZoneTelemetryDto >();
    qDBusRegisterMetaType< AppliedProfilesDto >();
    qDBusRegisterMetaType< DGpuInfoDto >();
    qDBusRegisterMetaType< IGpuInfoDto >();
    qDBusRegisterMetaType< CpuPowerDto >();
    qDBusRegisterMetaType< KeyboardBacklightCapabilitiesDto >();
    qDBusRegisterMetaType< OdmPowerLimitDto >();
    qDBusRegisterMetaType< OdmPowerLimitDtoList >();
    qDBusRegisterMetaType< MonitorSourceDto >();
    qDBusRegisterMetaType< MonitorSourceDtoList >();
    qDBusRegisterMetaType< FpsSourcesDto >();
    qDBusRegisterMetaType< AutoProgressStatusDto >();
    return true;
  }();

  Q_UNUSED( registered );
}

} // namespace ucc::dbus

Q_DECLARE_METATYPE( ucc::dbus::ProfileSummaryDto )
Q_DECLARE_METATYPE( ucc::dbus::ProfileSummaryDtoList )
Q_DECLARE_METATYPE( ucc::dbus::RamModuleDto )
Q_DECLARE_METATYPE( ucc::dbus::RamModuleDtoList )
Q_DECLARE_METATYPE( ucc::dbus::SystemInfoDto )
Q_DECLARE_METATYPE( ucc::dbus::HardwareFanDeviceDto )
Q_DECLARE_METATYPE( ucc::dbus::HardwareFanDeviceDtoList )
Q_DECLARE_METATYPE( ucc::dbus::HardwareSensorDto )
Q_DECLARE_METATYPE( ucc::dbus::HardwareSensorDtoList )
Q_DECLARE_METATYPE( ucc::dbus::ThermalSourceDto )
Q_DECLARE_METATYPE( ucc::dbus::ThermalSourceDtoList )
Q_DECLARE_METATYPE( ucc::dbus::FanZoneDto )
Q_DECLARE_METATYPE( ucc::dbus::FanZoneDtoList )
Q_DECLARE_METATYPE( ucc::dbus::FanCurvePointDto )
Q_DECLARE_METATYPE( ucc::dbus::FanCurvePointDtoList )
Q_DECLARE_METATYPE( ucc::dbus::FanZoneCurveDto )
Q_DECLARE_METATYPE( ucc::dbus::FanZoneCurveDtoList )
Q_DECLARE_METATYPE( ucc::dbus::TimedValueDto )
Q_DECLARE_METATYPE( ucc::dbus::FanDataDto )
Q_DECLARE_METATYPE( ucc::dbus::ZoneTelemetryDto )
Q_DECLARE_METATYPE( ucc::dbus::AppliedProfilesDto )
Q_DECLARE_METATYPE( ucc::dbus::DGpuInfoDto )
Q_DECLARE_METATYPE( ucc::dbus::IGpuInfoDto )
Q_DECLARE_METATYPE( ucc::dbus::CpuPowerDto )
Q_DECLARE_METATYPE( ucc::dbus::KeyboardBacklightCapabilitiesDto )
Q_DECLARE_METATYPE( ucc::dbus::OdmPowerLimitDto )
Q_DECLARE_METATYPE( ucc::dbus::OdmPowerLimitDtoList )
Q_DECLARE_METATYPE( ucc::dbus::MonitorSourceDto )
Q_DECLARE_METATYPE( ucc::dbus::MonitorSourceDtoList )
Q_DECLARE_METATYPE( ucc::dbus::FpsSourcesDto )
Q_DECLARE_METATYPE( ucc::dbus::AutoProgressStatusDto )