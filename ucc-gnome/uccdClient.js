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

/**
 * D-Bus client for the uccd daemon.
 *
 * Mirrors the functionality of libucc-dbus/UccdClient but in pure GJS/Gio.
 * All calls go to the system bus: com.uniwill.uccd /com/uniwill/uccd.
 */

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

const BUS_NAME    = 'com.uniwill.uccd';
const OBJECT_PATH = '/com/uniwill/uccd';
const IFACE_NAME  = 'com.uniwill.uccd';

/**
 * Synchronous D-Bus wrapper around uccd.
 *
 * Connection state is tracked via Gio.bus_watch_name_on_connection().
 */
export class UccdClient {
    constructor() {
        this._bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, null);
        this._connected = false;
        this._watchId = 0;
        this._onConnectionChanged = null;
    }

    get connected() { return this._connected; }

    /**
     * Start watching the daemon service on the system bus.
     * @param {function(boolean)} cb  Invoked with true/false on appear/vanish
     */
    watch(cb) {
        this._onConnectionChanged = cb;
        this._watchId = Gio.bus_watch_name_on_connection(
            this._bus, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            () => { this._connected = true;  cb?.(true);  },
            () => { this._connected = false; cb?.(false); },
        );
    }

    // -----------------------------------------------------------------------
    // Low-level helpers
    // -----------------------------------------------------------------------

    /** Call a D-Bus method and return the first result value (or null). */
    _call(method, args = null, signature = null) {
        if (!this._connected) return null;
        try {
            const params = args !== null
                ? new GLib.Variant(`(${signature})`, args)
                : null;
            const result = this._bus.call_sync(
                BUS_NAME, OBJECT_PATH, IFACE_NAME,
                method, params, null,
                Gio.DBusCallFlags.NONE, 2000, null,
            );
            if (!result) return null;
            const child = result.get_child_value(0);
            return child.get_type_string() === 'v'
                ? child.recursiveUnpack()
                : child.recursiveUnpack();
        } catch (_e) {
            return null;
        }
    }

    /** Call a D-Bus method and return the first result as a raw GVariant. */
    _callRaw(method, args = null, signature = null) {
        if (!this._connected) return null;
        try {
            const params = args !== null
                ? new GLib.Variant(`(${signature})`, args)
                : null;
            const result = this._bus.call_sync(
                BUS_NAME, OBJECT_PATH, IFACE_NAME,
                method, params, null,
                Gio.DBusCallFlags.NONE, 2000, null,
            );
            return result ? result.get_child_value(0) : null;
        } catch (_e) {
            return null;
        }
    }

    /** Call a void-returning method; returns true on success. */
    _callVoid(method, args = null, signature = null) {
        if (!this._connected) return false;
        try {
            const params = args !== null
                ? new GLib.Variant(`(${signature})`, args)
                : null;
            this._bus.call_sync(
                BUS_NAME, OBJECT_PATH, IFACE_NAME,
                method, params, null,
                Gio.DBusCallFlags.NONE, 2000, null,
            );
            return true;
        } catch (_e) {
            return false;
        }
    }

    /**
     * Extract a value from fan data maps (GetFanDataCPU / GPU1 / GPU2).
     * Returns null when data is missing or has timestamp==0.
     *
     * The daemon returns a{sv} → { speed: a{sv}{timestamp:x, data:i},
     *                               temp:  a{sv}{timestamp:x, data:i} }
     */
    _readFanData(method, key) {
        const outer = this._call(method);
        if (!outer) return null;
        const inner = outer[key];
        if (!inner) return null;
        // Treat timestamp==0 as missing data (daemon not yet populated)
        if (inner.timestamp === 0 || inner.timestamp === 0n) return null;
        const v = Number(inner.data);
        return v >= 0 ? v : null;
    }

    /** Extract a numeric value from a variant-map-returning D-Bus method. */
    _readNum(method, key) {
        const obj = this._call(method);
        if (!obj) return null;
        const v = obj[key];
        return typeof v === 'number' && v >= 0 ? v : null;
    }

    // -----------------------------------------------------------------------
    // Monitoring — fast poll (temperatures, frequencies, power, fans)
    // -----------------------------------------------------------------------

    getCpuTemperature() {
        return this._readFanData('GetFanDataCPU', 'temp') ?? -1;
    }

    getGpuTemperature() {
        // Prefer dGPU, fall back to iGPU
        return this._readNum('GetDGpuInfoValues', 'temp')
            ?? this._readNum('GetIGpuInfoValues', 'temp')
            ?? -1;
    }

    getCpuFrequency() { return this._call('GetCpuFrequencyMHz') ?? -1; }

    getGpuFrequency() {
        return this._readNum('GetDGpuInfoValues', 'coreFrequency')
            ?? this._readNum('GetDGpuInfoValues', 'coreFreq')
            ?? -1;
    }

    getCpuPower() {
        return this._readNum('GetCpuPowerValues', 'powerDraw') ?? -1;
    }

    getGpuPower() {
        return this._readNum('GetDGpuInfoValues', 'powerDraw')
            ?? this._readNum('GetIGpuInfoValues', 'powerDraw')
            ?? -1;
    }

    getFanSpeedRPM() {
        const pct = this._readFanData('GetFanDataCPU', 'speed');
        return pct !== null ? pct * 60 : -1;
    }

    getGpuFanSpeedRPM() {
        const g1 = this._readFanData('GetFanDataGPU1', 'speed');
        const g2 = this._readFanData('GetFanDataGPU2', 'speed');
        if (g1 !== null && g2 !== null) return Math.round((g1 + g2) / 2) * 60;
        if (g1 !== null) return g1 * 60;
        if (g2 !== null) return g2 * 60;
        return -1;
    }

    getFanSpeedPercent() {
        return this._readFanData('GetFanDataCPU', 'speed') ?? -1;
    }

    getGpuFanSpeedPercent() {
        const g1 = this._readFanData('GetFanDataGPU1', 'speed');
        const g2 = this._readFanData('GetFanDataGPU2', 'speed');
        if (g1 !== null && g2 !== null) return Math.round((g1 + g2) / 2);
        return g1 ?? g2 ?? -1;
    }

    getWaterCoolerFanSpeed()  { return this._call('GetWaterCoolerFanSpeed')  ?? -1; }
    getWaterCoolerPumpLevel() { return this._call('GetWaterCoolerPumpLevel') ?? -1; }

    // -----------------------------------------------------------------------
    // Slow poll — profiles, state, hardware toggles
    // -----------------------------------------------------------------------

    getActiveProfile()   { return this._call('GetActiveProfile'); }
    getPowerState()          { return this._call('GetPowerState'); }
    getDefaultProfiles() { return this._call('GetDefaultProfiles'); }
    getFanProfiles()     { return this._call('GetFanProfiles'); }

    getWebcamEnabled()      { return this._call('GetWebcamSWStatus') ?? false; }
    getFnLock()             { return this._call('GetFnLockStatus') ?? false; }
    getDisplayBrightness()  { return this._call('GetDisplayBrightness') ?? 50; }

    getAvailableODMProfiles() { return this._call('ODMProfilesAvailable') ?? []; }
    getODMPerformanceProfile() { return this._call('GetODMPerformanceProfile') ?? ''; }
    getWaterCoolerSupported() { return this._call('GetWaterCoolerSupported') ?? false; }
    isWaterCoolerEnabled()    { return this._call('IsWaterCoolerEnabled') ?? false; }
    isDeviceSupported()       { return this._call('IsDeviceSupported') ?? false; }
    getCapabilities()     { return this._call('GetCapabilities'); }
    getSystemInfo()       { return this._call('GetSystemInfo'); }

    getFanProfileZonesRaw(id) { return this._callRaw('GetFanProfileZones', [id], 's'); }
    getFanProfileSourcesRaw(id) { return this._callRaw('GetFanProfileSources', [id], 's'); }
    getGpuProfile(id)   { return this._call('GetGpuProfile', [id], 's'); }
    getGpuProfiles() { return this._call('GetGpuProfiles'); }
    getCTGPAdjustmentSupported() { return this._call('GetCTGPAdjustmentSupported') ?? false; }

    // -----------------------------------------------------------------------
    // Extended NVIDIA dGPU metrics (all from GetDGpuInfoValues)
    // -----------------------------------------------------------------------

    /** Return the dGPU info as a JS object (from variant map). */
    _getDGpuInfo() {
        return this._call('GetDGpuInfoValues') ?? null;
    }

    getDGpuComputeUtilPct()   { return this._getDGpuInfo()?.computeUtilPct   ?? -1; }
    getDGpuMemoryUtilPct()    { return this._getDGpuInfo()?.memoryUtilPct    ?? -1; }
    getDGpuVramUsedMiB()      { return this._getDGpuInfo()?.vramUsedMiB      ?? -1; }
    getDGpuVramTotalMiB()     { return this._getDGpuInfo()?.vramTotalMiB     ?? -1; }
    getDGpuPerfLimitReason()  { return this._getDGpuInfo()?.perfLimitReason  ?? ''; }
    getDGpuEncoderUtilPct()   { return this._getDGpuInfo()?.encoderUtilPct   ?? -1; }
    getDGpuDecoderUtilPct()   { return this._getDGpuInfo()?.decoderUtilPct   ?? -1; }
    getDGpuCurrentPstate()    { return this._getDGpuInfo()?.currentPstate    ?? -1; }
    getDGpuGrClockOffsetMHz() { const v = this._getDGpuInfo()?.grClockOffsetMHz; return (v !== undefined && v !== -999) ? v : -999; }
    getDGpuMemClockOffsetMHz(){ const v = this._getDGpuInfo()?.memClockOffsetMHz; return (v !== undefined && v !== -999) ? v : -999; }
    getDGpuVramFreqMHz()      { return this._getDGpuInfo()?.vramFrequency    ?? -1; }
    getDGpuCoreVoltageMv()    { return this._getDGpuInfo()?.coreVoltageMv    ?? -1; }

    // -----------------------------------------------------------------------
    // Setters
    // -----------------------------------------------------------------------

    setActiveProfile(id) {
        return this._callVoid('SetActiveProfile', [id], 's');
    }

    applyFanProfiles(zonesVariant, sourcesVariant, fanProfileId) {
        if (!this._connected) return false;
        try {
            const params = GLib.Variant.new_tuple([
                zonesVariant,
                sourcesVariant,
                new GLib.Variant('s', fanProfileId),
            ]);
            this._bus.call_sync(
                BUS_NAME, OBJECT_PATH, IFACE_NAME,
                'ApplyFanProfiles', params, null,
                Gio.DBusCallFlags.NONE, 2000, null,
            );
            return true;
        } catch (_e) {
            return false;
        }
    }

    setKeyboardBacklight(json) {
        return this._callVoid('SetKeyboardBacklightStatesJSON', [json], 's');
    }

    setODMPerformanceProfile(profile) {
        return this._callVoid('SetODMPerformanceProfile', [profile], 's');
    }

    setNVIDIAPowerOffset(offset) {
        return this._callVoid('SetNVIDIAPowerOffset', [offset], 'i');
    }

    applyNvidiaGpuOCProfile(profileJSON, deviceIndex = 0) {
        return this._call('ApplyNvidiaGpuOCProfile', [profileJSON, deviceIndex], 'si') ?? false;
    }

    setWebcamEnabled(v) {
        // Try both method names for compatibility
        return this._callVoid('SetWebcam', [v], 'b');
    }

    setFnLock(v) {
        return this._callVoid('SetFnLockStatus', [v], 'b');
    }

    setDisplayBrightness(v) {
        return this._callVoid('SetDisplayBrightness', [v], 'i');
    }

    enableWaterCooler(v) {
        return this._callVoid('EnableWaterCooler', [v], 'b');
    }

    setWaterCoolerFanSpeed(percent) {
        return this._callVoid('SetWaterCoolerFanSpeed', [percent], 'i');
    }

    setWaterCoolerPumpVoltage(code) {
        return this._callVoid('SetWaterCoolerPumpVoltage', [code], 'i');
    }

    setWaterCoolerLEDColor(r, g, b, mode) {
        return this._callVoid('SetWaterCoolerLEDColor', [r, g, b, mode], 'iiii');
    }

    turnOffWaterCoolerLED() {
        return this._callVoid('TurnOffWaterCoolerLED');
    }

    // -----------------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------------

    destroy() {
        if (this._watchId) {
            Gio.bus_unwatch_name(this._watchId);
            this._watchId = 0;
        }
        this._connected = false;
    }
}
