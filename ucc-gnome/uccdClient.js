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
     * @param {function(boolean)} cb Invoked with true/false on appear/vanish
     */
    watch(cb) {
        this._onConnectionChanged = cb;
        this._watchId = Gio.bus_watch_name_on_connection(
            this._bus, BUS_NAME, Gio.BusNameWatcherFlags.NONE,
            () => { this._connected = true;  cb?.(true);  },
            () => { this._connected = false; cb?.(false); },
        );
    }

    // Low-level helpers

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
     * The daemon returns a{sv} -> { speed: a{sv}{timestamp:x, data:i},
     * temp: a{sv}{timestamp:x, data:i} }
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

    /** Parse a JSON-returning D-Bus method and extract a numeric key. */
    _readJsonNum(method, key) {
        const raw = this._call(method);
        if (!raw) return null;
        try {
            const v = JSON.parse(raw)[key];
            return typeof v === 'number' && v >= 0 ? v : null;
        } catch { return null; }
    }

    // Monitoring - fast poll (temperatures, frequencies, power, fans)

    getCpuTemperature() {
        return this._readFanData('GetFanDataCPU', 'temp') ?? -1;
    }

    getGpuTemperature() {
        // Prefer dGPU JSON, fall back to iGPU JSON
        return this._readJsonNum('GetDGpuInfoValuesJSON', 'temp')
            ?? this._readJsonNum('GetIGpuInfoValuesJSON', 'temp')
            ?? -1;
    }

    getCpuFrequency() { return this._call('GetCpuFrequencyMHz') ?? -1; }

    getGpuFrequency() {
        return this._readJsonNum('GetDGpuInfoValuesJSON', 'coreFrequency')
            ?? this._readJsonNum('GetDGpuInfoValuesJSON', 'coreFreq')
            ?? -1;
    }

    getCpuPower() {
        return this._readJsonNum('GetCpuPowerValuesJSON', 'powerDraw') ?? -1;
    }

    getGpuPower() {
        return this._readJsonNum('GetDGpuInfoValuesJSON', 'powerDraw')
            ?? this._readJsonNum('GetIGpuInfoValuesJSON', 'powerDraw')
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

    // Slow poll - profiles, state, hardware toggles

    getActiveProfileJSON()   { return this._call('GetActiveProfileJSON'); }
    getPowerState()          { return this._call('GetPowerState'); }
    getDefaultProfilesJSON() { return this._call('GetDefaultProfilesJSON'); }
    getCustomProfilesJSON()  { return this._call('GetCustomProfilesJSON'); }
    getFanProfileNames()     { return this._call('GetFanProfileNames'); }

    getCustomFanProfiles()      { return this._call('GetCustomFanProfiles'); }
    getCustomKeyboardProfiles() { return this._call('GetCustomKeyboardProfiles'); }

    getWebcamEnabled()      { return this._call('GetWebcamSWStatus') ?? false; }
    getFnLock()             { return this._call('GetFnLockStatus') ?? false; }
    getDisplayBrightness()  { return this._call('GetDisplayBrightness') ?? 50; }

    getAvailableODMProfiles()           { return this._call('ODMProfilesAvailable') ?? []; }
    getWaterCoolerSupported()           { return this._call('GetWaterCoolerSupported') ?? false; }
    isWaterCoolerEnabled()              { return this._call('IsWaterCoolerEnabled') ?? false; }
    isDeviceSupported()                 { return this._call('IsDeviceSupported') ?? false; }
    getKeyboardBacklightControlEnabled(){ return this._call('GetKeyboardBacklightControlEnabled') ?? false; }
    getSystemInfoJSON()                 { return this._call('GetSystemInfoJSON'); }

    getFanProfile(name) { return this._call('GetFanProfile', [name], 's'); }

    // Setters

    setActiveProfile(id) {
        return this._callVoid('SetActiveProfile', [id], 's');
    }

    applyFanProfiles(json) {
        return this._callVoid('ApplyFanProfiles', [json], 's');
    }

    setKeyboardBacklight(json) {
        return this._callVoid('SetKeyboardBacklightStatesJSON', [json], 's');
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

    // Cleanup

    destroy() {
        if (this._watchId) {
            Gio.bus_unwatch_name(this._watchId);
            this._watchId = 0;
        }
        this._connected = false;
    }
}
