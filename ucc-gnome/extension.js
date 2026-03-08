/*
 * UCC GNOME Shell Extension
 *
 * System tray applet for the Unified Control Center daemon (uccd).
 * Provides monitoring, profile switching, hardware toggles, and
 * water cooler control — mirroring the KDE Plasma applet.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import GObject from 'gi://GObject';
import St from 'gi://St';
import Clutter from 'gi://Clutter';

import { Extension } from 'resource:///org/gnome/shell/extensions/extension.js';
import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as Slider from 'resource:///org/gnome/shell/ui/slider.js';

import { UccdClient } from './uccdClient.js';

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Create a label pair:  "CPU Temp    68 °C" */
function labelRow(labelText) {
    const box = new St.BoxLayout({
        style_class: 'ucc-metric-row',
        x_expand: true,
    });
    const nameLabel = new St.Label({
        text: labelText,
        style_class: 'ucc-metric-name',
        x_expand: true,
        y_align: Clutter.ActorAlign.CENTER,
    });
    const valueLabel = new St.Label({
        text: '—',
        style_class: 'ucc-metric-value',
        y_align: Clutter.ActorAlign.CENTER,
    });
    box.add_child(nameLabel);
    box.add_child(valueLabel);
    return { box, nameLabel, valueLabel };
}

/** Map raw power state string to display string. */
function mapPowerState(raw) {
    if (raw === 'power_ac')  return 'AC';
    if (raw === 'power_bat') return 'Battery';
    if (raw === 'power_wc')  return 'AC w/ Water Cooler';
    return raw ?? '—';
}

/**
 * Unwrap a QSettings @ByteArray()-encoded INI value.
 *
 * Qt's QSettings (IniFormat) stores QByteArray values as:
 *   key="@ByteArray(payload)"
 * with all inner quotes and backslashes escaped (\" and \\).
 *
 * GLib.KeyFile.get_value() returns the raw line value, so we must:
 *   1. Strip the surrounding double-quotes
 *   2. Unescape INI encoding (\x → x)
 *   3. Strip the @ByteArray(...) wrapper
 * before JSON.parse() can consume the payload.
 */
function unwrapQByteArray(raw) {
    if (!raw) return raw;
    let val = raw;
    // 1. Strip surrounding double-quotes added by QSettings IniFormat
    if (val.startsWith('"') && val.endsWith('"'))
        val = val.slice(1, -1);
    // 2. Unescape: \" → ", \\ → \  (any \x → x in one pass)
    val = val.replace(/\\(.)/g, '$1');
    // 3. Strip @ByteArray(...) wrapper
    const m = val.match(/^@ByteArray\((.*)\)$/s);
    return m ? m[1] : val;
}

// ---------------------------------------------------------------------------
// Indicator
// ---------------------------------------------------------------------------

const UccIndicator = GObject.registerClass(
class UccIndicator extends PanelMenu.Button {

    _init(extensionObj) {
        super._init(0.5, 'UCC');
        this._ext = extensionObj;

        // Panel icon — use the project's own tray icon (installed to share/pixmaps)
        this._icon = new St.Icon({
            icon_name: 'ucc-tray',
            style_class: 'system-status-icon',
        });
        this.add_child(this._icon);

        // D-Bus client
        this._client = new UccdClient();

        // Cached state
        this._state = {
            connected: false,
            // Monitoring
            cpuTemp: -1, gpuTemp: -1,
            cpuFreq: -1, gpuFreq: -1,
            cpuPower: -1, gpuPower: -1,
            cpuFanRPM: -1, gpuFanRPM: -1,
            cpuFanPct: -1, gpuFanPct: -1,
            wcFanSpeed: -1, wcPumpLevel: -1,
            wcConnected: false,
            // Extended NVIDIA dGPU metrics
            gpuComputeUtilPct: -1, gpuMemoryUtilPct: -1,
            gpuVramUsedMiB: -1, gpuVramTotalMiB: -1,
            gpuPerfLimitReason: '',
            gpuEncoderUtilPct: -1, gpuDecoderUtilPct: -1,
            gpuCurrentPstate: -1,
            gpuGrClockOffsetMHz: -999, gpuMemClockOffsetMHz: -999,
            gpuVramFreqMHz: -1, gpuCoreVoltageMv: -1,
            // System info
            laptopModel: '', cpuModel: '', gpuModel: '',
            // Profiles
            profileNames: [], profileIds: [],
            activeProfileId: '', activeProfileName: '',
            fanProfileNames: [], fanProfileIds: [],
            activeProfileFanId: '',
            keyboardProfileNames: [], keyboardProfileIds: [],
            keyboardProfilesData: [],
            activeKeyboardProfileId: '',
            gpuProfileNames: [], gpuProfileIds: [],
            activeProfileGpuId: '',
            powerState: '',
            // ODM
            availableODMProfiles: [], odmPerformanceProfile: '',
            // Hardware
            webcamEnabled: false, fnLock: false,
            displayBrightness: 50,
            // Water cooler
            waterCoolerSupported: false,
            wcEnabled: false,
            wcAutoControl: true,
            wcFanPercent: 50,
            wcPumpVoltageCode: 4,
            wcLedEnabled: true,
            wcLedR: 255, wcLedG: 0, wcLedB: 0, wcLedMode: 0,
        };

        // Build the popup UI
        this._buildPopup();

        // Watch daemon
        this._client.watch(connected => {
            this._state.connected = connected;
            this._updateConnectionUI();
            if (connected) {
                this._loadCapabilities();
                this._loadProfiles();
                this._pollMetrics();
                this._pollSlowState();
            }
        });

        // Watch ~/.config/uccrc for changes (so we pick up edits from the GUI)
        this._uccrcPath = GLib.build_filenamev([GLib.get_home_dir(), '.config', 'uccrc']);
        this._uccrcMonitor = null;
        this._startUccrcMonitor();

        // Timers
        this._fastTimerId = 0;
        this._slowTimerId = 0;
        this._startTimers();
    }

    // -----------------------------------------------------------------------
    // Popup layout
    // -----------------------------------------------------------------------

    _buildPopup() {
        // --- Tab bar ---
        const tabItem = new PopupMenu.PopupBaseMenuItem({ reactive: false, can_focus: false });
        this._tabBar = new St.BoxLayout({ style_class: 'ucc-tab-bar', x_expand: true });
        tabItem.add_child(this._tabBar);
        this.menu.addMenuItem(tabItem);

        this._tabs = {};
        this._tabButtons = {};
        const tabDefs = [
            ['dashboard', 'Dashboard'],
            ['profile',   'Profile'],
            ['hardware',  'Hardware'],
        ];
        // Water cooler tab added conditionally later

        for (const [id, label] of tabDefs) {
            this._addTab(id, label);
        }

        // Separator
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        // --- Tab content container ---
        const contentItem = new PopupMenu.PopupBaseMenuItem({
            reactive: false,
            can_focus: false,
        });
        this._contentBin = new St.Bin({
            x_expand: true,
            y_expand: true,
        });
        contentItem.add_child(this._contentBin);
        this.menu.addMenuItem(contentItem);

        // Build each tab's content
        this._buildDashboardTab();
        this._buildProfileTab();
        this._buildHardwareTab();
        this._buildWaterCoolerTab(); // built but hidden until supported

        // Separator before footer
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());

        // --- Footer: connection status + power state ---
        const footerItem = new PopupMenu.PopupBaseMenuItem({ reactive: false, can_focus: false });
        const footerBox = new St.BoxLayout({ style_class: 'ucc-footer', x_expand: true });
        this._connLabel = new St.Label({
            text: 'Disconnected',
            style_class: 'ucc-conn-label ucc-disconnected',
            x_expand: true,
        });
        this._powerLabel = new St.Label({
            text: '',
            style_class: 'ucc-power-label',
        });
        footerBox.add_child(this._connLabel);
        footerBox.add_child(this._powerLabel);
        footerItem.add_child(footerBox);
        this.menu.addMenuItem(footerItem);

        // Select default tab
        this._selectTab('dashboard');
    }

    _addTab(id, label) {
        const btn = new St.Button({
            label,
            style_class: 'ucc-tab-button',
            toggle_mode: true,
            x_expand: true,
        });
        btn.connect('clicked', () => this._selectTab(id));
        this._tabBar.add_child(btn);
        this._tabButtons[id] = btn;
    }

    _selectTab(id) {
        for (const [tid, btn] of Object.entries(this._tabButtons)) {
            btn.checked = (tid === id);
        }
        const content = this._tabs[id];
        if (content) {
            this._contentBin.set_child(content);
        }
    }

    // -----------------------------------------------------------------------
    // Dashboard tab
    // -----------------------------------------------------------------------

    _buildDashboardTab() {
        const box = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-tab-content',
            x_expand: true,
        });

        // System model title (hidden when empty)
        this._sysModelLabel = new St.Label({
            text: '',
            style_class: 'ucc-system-model',
        });
        this._sysModelLabel.visible = false;
        box.add_child(this._sysModelLabel);

        // ── CPU Section ──
        this._cpuTitleLabel = new St.Label({
            text: 'CPU',
            style_class: 'ucc-cpu-gpu-model',
        });
        box.add_child(this._cpuTitleLabel);

        const cpuGrid = new St.BoxLayout({ vertical: true, style_class: 'ucc-grid' });
        this._lCpuTemp  = labelRow('Temp');   cpuGrid.add_child(this._lCpuTemp.box);
        this._lCpuFreq  = labelRow('Freq');   cpuGrid.add_child(this._lCpuFreq.box);
        this._lCpuPower = labelRow('Power');  cpuGrid.add_child(this._lCpuPower.box);
        this._lCpuFan   = labelRow('Fan');    cpuGrid.add_child(this._lCpuFan.box);
        box.add_child(cpuGrid);

        // ── GPU Section ──
        this._gpuTitleLabel = new St.Label({
            text: 'GPU',
            style_class: 'ucc-cpu-gpu-model',
        });
        box.add_child(this._gpuTitleLabel);

        const gpuGrid = new St.BoxLayout({ vertical: true, style_class: 'ucc-grid' });
        this._lGpuTemp  = labelRow('Temp');   gpuGrid.add_child(this._lGpuTemp.box);
        this._lGpuFreq  = labelRow('Freq');   gpuGrid.add_child(this._lGpuFreq.box);
        this._lGpuPower = labelRow('Power');  gpuGrid.add_child(this._lGpuPower.box);
        this._lGpuFan   = labelRow('Fan');    gpuGrid.add_child(this._lGpuFan.box);
        // Extended NVIDIA metrics (hidden when data unavailable)
        this._lGpuLoad       = labelRow('GPU Load');      gpuGrid.add_child(this._lGpuLoad.box);
        this._lVramLoad      = labelRow('VRAM Load');     gpuGrid.add_child(this._lVramLoad.box);
        this._lPstate        = labelRow('P-State');       gpuGrid.add_child(this._lPstate.box);
        this._lVramFreq      = labelRow('VRAM Freq');     gpuGrid.add_child(this._lVramFreq.box);
        this._lCoreVoltage   = labelRow('Core Voltage');  gpuGrid.add_child(this._lCoreVoltage.box);
        this._lClockOffsets  = labelRow('Clock Offsets'); gpuGrid.add_child(this._lClockOffsets.box);
        this._lVram          = labelRow('VRAM');          gpuGrid.add_child(this._lVram.box);
        this._lPerfLimit     = labelRow('Perf Limit');    gpuGrid.add_child(this._lPerfLimit.box);
        this._lNvencDec      = labelRow('NVENC/DEC');     gpuGrid.add_child(this._lNvencDec.box);
        // Initially hide all extended rows
        for (const r of [this._lGpuLoad, this._lVramLoad, this._lPstate, this._lVramFreq,
                         this._lCoreVoltage, this._lClockOffsets, this._lVram,
                         this._lPerfLimit, this._lNvencDec]) {
            r.box.visible = false;
        }
        box.add_child(gpuGrid);

        // Water cooler metrics (hidden until supported)
        this._wcMetricsBox = new St.BoxLayout({ vertical: true, style_class: 'ucc-cpu-gpu-model' });
        this._wcMetricsBox.visible = false;
        box.add_child(new St.Label({ text: 'Water Cooler', style_class: 'ucc-section-title' }));
        this._lWcFan  = labelRow('Fan');   this._wcMetricsBox.add_child(this._lWcFan.box);
        this._lWcPump = labelRow('Pump');  this._wcMetricsBox.add_child(this._lWcPump.box);
        box.add_child(this._wcMetricsBox);

        this._tabs['dashboard'] = box;
    }

    // -----------------------------------------------------------------------
    // Profile tab  (GNOME-native PopupMenu with radio-dot ornaments,
    //               like the Power Mode / Wi-Fi / Bluetooth selectors)
    // -----------------------------------------------------------------------

    _buildProfileTab() {
        // We use a PopupMenuSection so we get native GNOME menu items
        // with ornament support (Ornament.DOT for the active choice).
        const scroll = new St.ScrollView({
            style_class: 'ucc-profile-scroll',
            x_expand: true,
            y_expand: true,
            overlay_scrollbars: true,
        });
        const box = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-tab-content',
            x_expand: true,
        });
        scroll.set_child(box);

        // ── System Profile ──
        box.add_child(new St.Label({ text: 'System Profile', style_class: 'ucc-section-title' }));
        this._profileListBox = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-chooser-list',
            x_expand: true,
        });
        box.add_child(this._profileListBox);

        // ── Fan Profile ──
        this._fanProfileHeader = new St.Label({ text: 'Fan Profile', style_class: 'ucc-section-title' });
        box.add_child(this._fanProfileHeader);
        this._fanProfileListBox = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-chooser-list',
            x_expand: true,
        });
        box.add_child(this._fanProfileListBox);

        // ── Keyboard Profile ──
        this._kbProfileHeader = new St.Label({ text: 'Keyboard Profile', style_class: 'ucc-section-title' });
        box.add_child(this._kbProfileHeader);
        this._kbProfileListBox = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-chooser-list',
            x_expand: true,
        });
        box.add_child(this._kbProfileListBox);

        // ── GPU OC Profile ──
        this._gpuProfileHeader = new St.Label({ text: 'GPU OC Profile', style_class: 'ucc-section-title' });
        box.add_child(this._gpuProfileHeader);
        this._gpuProfileListBox = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-chooser-list',
            x_expand: true,
        });
        box.add_child(this._gpuProfileListBox);

        // ── ODM Performance Mode ──
        this._odmHeader = new St.Label({ text: 'ODM Performance Mode', style_class: 'ucc-section-title' });
        box.add_child(this._odmHeader);
        this._odmListBox = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-chooser-list',
            x_expand: true,
        });
        box.add_child(this._odmListBox);

        this._tabs['profile'] = scroll;
    }

    // -----------------------------------------------------------------------
    // Hardware tab
    // -----------------------------------------------------------------------

    _buildHardwareTab() {
        const box = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-tab-content',
            x_expand: true,
        });

        // Fn Lock toggle
        const fnRow = new St.BoxLayout({ style_class: 'ucc-hw-row', x_expand: true });
        fnRow.add_child(new St.Label({
            text: 'Fn Lock',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        }));
        this._fnLockSwitch = new St.Button({
            style_class: 'ucc-toggle',
            toggle_mode: true,
            label: 'OFF',
        });
        this._fnLockSwitch.connect('clicked', () => {
            const val = this._fnLockSwitch.checked;
            this._client.setFnLock(val);
            this._state.fnLock = val;
            this._fnLockSwitch.label = val ? 'ON' : 'OFF';
        });
        fnRow.add_child(this._fnLockSwitch);
        box.add_child(fnRow);

        // Webcam toggle
        const camRow = new St.BoxLayout({ style_class: 'ucc-hw-row', x_expand: true });
        camRow.add_child(new St.Label({
            text: 'Webcam',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        }));
        this._webcamSwitch = new St.Button({
            style_class: 'ucc-toggle',
            toggle_mode: true,
            label: 'OFF',
        });
        this._webcamSwitch.connect('clicked', () => {
            const val = this._webcamSwitch.checked;
            this._client.setWebcamEnabled(val);
            this._state.webcamEnabled = val;
            this._webcamSwitch.label = val ? 'ON' : 'OFF';
        });
        camRow.add_child(this._webcamSwitch);
        box.add_child(camRow);

        // Display Brightness slider
        box.add_child(new St.Label({ text: 'Display Brightness', style_class: 'ucc-section-title' }));
        const brightnessRow = new St.BoxLayout({ style_class: 'ucc-hw-row', x_expand: true });
        this._brightnessSlider = new Slider.Slider(0.5);
        this._brightnessSlider.x_expand = true;
        this._brightnessSlider.connect('notify::value', () => {
            const val = Math.round(this._brightnessSlider.value * 100);
            this._client.setDisplayBrightness(val);
            this._state.displayBrightness = val;
            this._brightnessValueLabel.text = `${val}%`;
        });
        this._brightnessValueLabel = new St.Label({
            text: '50%',
            style_class: 'ucc-slider-value',
            y_align: Clutter.ActorAlign.CENTER,
        });
        brightnessRow.add_child(this._brightnessSlider);
        brightnessRow.add_child(this._brightnessValueLabel);
        box.add_child(brightnessRow);

        // Open Control Center button
        const openBtn = new St.Button({
            label: 'Open Control Center',
            style_class: 'ucc-open-btn',
            x_expand: true,
        });
        openBtn.connect('clicked', () => {
            GLib.spawn_command_line_async('/usr/bin/env ucc-gui');
            this.menu.close();
        });
        box.add_child(openBtn);

        this._tabs['hardware'] = box;
    }

    // -----------------------------------------------------------------------
    // Water Cooler tab
    // -----------------------------------------------------------------------

    _buildWaterCoolerTab() {
        const box = new St.BoxLayout({
            vertical: true,
            style_class: 'ucc-tab-content',
            x_expand: true,
        });

        // Enable toggle
        const enRow = new St.BoxLayout({ style_class: 'ucc-hw-row', x_expand: true });
        enRow.add_child(new St.Label({
            text: 'Water Cooler',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        }));
        this._wcEnableSwitch = new St.Button({
            style_class: 'ucc-toggle',
            toggle_mode: true,
            label: 'OFF',
        });
        this._wcEnableSwitch.connect('clicked', () => {
            const val = this._wcEnableSwitch.checked;
            this._client.enableWaterCooler(val);
            this._state.wcEnabled = val;
            this._wcEnableSwitch.label = val ? 'ON' : 'OFF';
        });
        enRow.add_child(this._wcEnableSwitch);
        box.add_child(enRow);

        // Fan speed slider
        box.add_child(new St.Label({ text: 'Fan Speed', style_class: 'ucc-section-title' }));
        const fanRow = new St.BoxLayout({ style_class: 'ucc-hw-row', x_expand: true });
        this._wcFanSlider = new Slider.Slider(0.5);
        this._wcFanSlider.x_expand = true;
        this._wcFanSlider.connect('notify::value', () => {
            const val = Math.round(this._wcFanSlider.value * 100);
            this._client.setWaterCoolerFanSpeed(val);
            this._state.wcFanPercent = val;
            this._wcFanValueLabel.text = `${val}%`;
        });
        this._wcFanValueLabel = new St.Label({
            text: '50%',
            style_class: 'ucc-slider-value',
            y_align: Clutter.ActorAlign.CENTER,
        });
        fanRow.add_child(this._wcFanSlider);
        fanRow.add_child(this._wcFanValueLabel);
        box.add_child(fanRow);

        // Pump voltage combo
        // PumpVoltage enum: V11=0, V12=1(reserved), V7=2, V8=3, Off=4
        // Ordered as Off / 7V / 8V / 11V to match KDE applet
        box.add_child(new St.Label({ text: 'Pump Voltage', style_class: 'ucc-section-title' }));
        this._pumpVoltageBox = new St.BoxLayout({
            vertical: false,
            style_class: 'ucc-combo-row',
            x_expand: true,
        });
        const voltageOptions = [
            [4, 'Off'],  [2, '7 V'],  [3, '8 V'],  [0, '11 V'],
        ];
        this._pumpVoltageButtons = [];
        for (const [code, label] of voltageOptions) {
            const btn = new St.Button({
                label,
                style_class: 'ucc-combo-btn',
                toggle_mode: true,
                x_expand: true,
            });
            btn._voltageCode = code;
            btn.connect('clicked', () => {
                this._client.setWaterCoolerPumpVoltage(code);
                this._state.wcPumpVoltageCode = code;
                this._updatePumpVoltageUI();
            });
            this._pumpVoltageBox.add_child(btn);
            this._pumpVoltageButtons.push(btn);
        }
        box.add_child(this._pumpVoltageBox);

        // LED toggle
        const ledRow = new St.BoxLayout({ style_class: 'ucc-hw-row', x_expand: true });
        ledRow.add_child(new St.Label({
            text: 'LED',
            x_expand: true,
            y_align: Clutter.ActorAlign.CENTER,
        }));
        this._wcLedSwitch = new St.Button({
            style_class: 'ucc-toggle',
            toggle_mode: true,
            label: 'OFF',
        });
        this._wcLedSwitch.connect('clicked', () => {
            const val = this._wcLedSwitch.checked;
            if (val) {
                this._client.setWaterCoolerLEDColor(
                    this._state.wcLedR, this._state.wcLedG,
                    this._state.wcLedB, this._state.wcLedMode,
                );
            } else {
                this._client.turnOffWaterCoolerLED();
            }
            this._state.wcLedEnabled = val;
            this._wcLedSwitch.label = val ? 'ON' : 'OFF';
        });
        ledRow.add_child(this._wcLedSwitch);
        box.add_child(ledRow);

        this._tabs['watercooler'] = box;
    }

    // -----------------------------------------------------------------------
    // Profile UI rebuild  (radio-dot ornament style, like GNOME Power Mode)
    // -----------------------------------------------------------------------

    /** Build a single chooser row with a radio-dot icon for the active item. */
    _makeChooserRow(name, isActive, onActivate) {
        const row = new St.BoxLayout({
            style_class: isActive ? 'ucc-chooser-row ucc-chooser-active' : 'ucc-chooser-row',
            x_expand: true,
            reactive: true,
            track_hover: true,
        });
        // Use GNOME Shell's own ornament-dot icons for a native look
        const dot = new St.Icon({
            icon_name: isActive ? 'ornament-dot-checked-symbolic' : 'ornament-dot-unchecked-symbolic',
            style_class: 'ucc-chooser-dot',
            icon_size: 16,
            y_align: Clutter.ActorAlign.CENTER,
        });
        const label = new St.Label({
            text: name,
            style_class: 'ucc-chooser-label',
            y_align: Clutter.ActorAlign.CENTER,
            x_expand: true,
        });
        row.add_child(dot);
        row.add_child(label);
        row.connect('button-press-event', () => {
            onActivate();
            return Clutter.EVENT_STOP;
        });
        return row;
    }

    _rebuildProfileButtons() {
        this._profileListBox.destroy_all_children();

        const s = this._state;
        for (let i = 0; i < s.profileIds.length; i++) {
            const id = s.profileIds[i];
            const name = s.profileNames[i] ?? id;
            const isActive = id === s.activeProfileId;
            const row = this._makeChooserRow(name, isActive, () => {
                if (this._client.setActiveProfile(id)) {
                    s.activeProfileId = id;
                    s.activeProfileName = name;
                    this._rebuildProfileButtons();
                    this._pollSlowState(); // refresh fan/keyboard sub-profiles
                }
            });
            this._profileListBox.add_child(row);
        }
    }

    _rebuildFanProfileButtons() {
        this._fanProfileListBox.destroy_all_children();

        const s = this._state;
        // Hide section if no fan profiles
        this._fanProfileHeader.visible = s.fanProfileIds.length > 0;
        this._fanProfileListBox.visible = s.fanProfileIds.length > 0;

        for (let i = 0; i < s.fanProfileIds.length; i++) {
            const id = s.fanProfileIds[i];
            const name = s.fanProfileNames[i] ?? id;
            const isActive = id === s.activeProfileFanId;
            const row = this._makeChooserRow(name, isActive, () => {
                this._applyFanProfile(id);
                s.activeProfileFanId = id;
                this._rebuildFanProfileButtons();
            });
            this._fanProfileListBox.add_child(row);
        }
    }

    _rebuildKeyboardProfileButtons() {
        this._kbProfileListBox.destroy_all_children();

        const s = this._state;
        // Hide section if no keyboard profiles
        this._kbProfileHeader.visible = s.keyboardProfileIds.length > 0;
        this._kbProfileListBox.visible = s.keyboardProfileIds.length > 0;

        for (let i = 0; i < s.keyboardProfileIds.length; i++) {
            const id = s.keyboardProfileIds[i];
            const name = s.keyboardProfileNames[i] ?? id;
            const isActive = id === s.activeKeyboardProfileId;
            const row = this._makeChooserRow(name, isActive, () => {
                this._applyKeyboardProfile(id);
                s.activeKeyboardProfileId = id;
                this._rebuildKeyboardProfileButtons();
            });
            this._kbProfileListBox.add_child(row);
        }
    }

    _applyFanProfile(fanProfileId) {
        const raw = this._client.getFanProfile(fanProfileId);
        if (!raw) return;
        try {
            const src = JSON.parse(raw);
            const dst = {};
            if (src.tableCPU)            dst.cpu            = src.tableCPU;
            if (src.tableGPU)            dst.gpu            = src.tableGPU;
            if (src.tablePump)           dst.pump           = src.tablePump;
            if (src.tableWaterCoolerFan) dst.waterCoolerFan = src.tableWaterCoolerFan;
            this._client.applyFanProfiles(JSON.stringify(dst));
        } catch { /* ignore parse errors */ }
    }

    _applyKeyboardProfile(kbProfileId) {
        const s = this._state;
        const idx = s.keyboardProfileIds.indexOf(kbProfileId);
        if (idx < 0) return;
        const profileData = s.keyboardProfilesData[idx];
        if (!profileData) return;
        const json = profileData.json;
        if (json) {
            this._client.setKeyboardBacklight(json);
        }
    }

    _rebuildGpuProfileButtons() {
        this._gpuProfileListBox.destroy_all_children();

        const s = this._state;
        this._gpuProfileHeader.visible = s.gpuProfileIds.length > 0;
        this._gpuProfileListBox.visible = s.gpuProfileIds.length > 0;

        for (let i = 0; i < s.gpuProfileIds.length; i++) {
            const id = s.gpuProfileIds[i];
            const name = s.gpuProfileNames[i] ?? id;
            const isActive = id === s.activeProfileGpuId;
            const row = this._makeChooserRow(name, isActive, () => {
                this._applyGpuProfile(id);
                s.activeProfileGpuId = id;
                this._rebuildGpuProfileButtons();
            });
            this._gpuProfileListBox.add_child(row);
        }
    }

    _applyGpuProfile(gpuProfileId) {
        // Try daemon built-in profile first
        let profileJson = this._client.getGpuProfile(gpuProfileId);

        // Fall back to custom profile from uccrc
        if (!profileJson || profileJson === '{}') {
            try {
                const uccrc = GLib.build_filenamev([GLib.get_home_dir(), '.config', 'uccrc']);
                const kf = new GLib.KeyFile();
                kf.load_from_file(uccrc, GLib.KeyFileFlags.NONE);
                const raw = unwrapQByteArray(kf.get_value('General', 'customGpuProfiles'));
                if (raw) {
                    for (const p of JSON.parse(raw)) {
                        if (p.id === gpuProfileId && p.json) {
                            profileJson = p.json;
                            break;
                        }
                    }
                }
            } catch { /* ignore */ }
        }

        if (!profileJson || profileJson === '{}') return;

        try {
            const obj = JSON.parse(profileJson);

            // Apply cTGP offset if present
            if (obj.nvidiaPowerCTRLProfile?.cTGPOffset !== undefined) {
                if (this._client.getCTGPAdjustmentSupported()) {
                    this._client.setNVIDIAPowerOffset(obj.nvidiaPowerCTRLProfile.cTGPOffset);
                }
            }

            // Stamp the gpuProfileId and send to daemon
            obj.gpuProfileId = gpuProfileId;
            this._client.applyNvidiaGpuOCProfile(JSON.stringify(obj));
        } catch { /* ignore parse errors */ }
    }

    _rebuildODMProfileButtons() {
        this._odmListBox.destroy_all_children();

        const s = this._state;
        this._odmHeader.visible = s.availableODMProfiles.length > 0;
        this._odmListBox.visible = s.availableODMProfiles.length > 0;

        for (const profile of s.availableODMProfiles) {
            const isActive = profile === s.odmPerformanceProfile;
            const row = this._makeChooserRow(profile, isActive, () => {
                this._client.setODMPerformanceProfile(profile);
                s.odmPerformanceProfile = profile;
                this._rebuildODMProfileButtons();
            });
            this._odmListBox.add_child(row);
        }
    }

    _updatePumpVoltageUI() {
        for (const btn of this._pumpVoltageButtons) {
            btn.checked = (btn._voltageCode === this._state.wcPumpVoltageCode);
        }
    }

    // -----------------------------------------------------------------------
    // Connection UI
    // -----------------------------------------------------------------------

    _updateConnectionUI() {
        const c = this._state.connected;
        this._connLabel.text = c ? 'Connected' : 'Disconnected';
        this._connLabel.style_class = c
            ? 'ucc-conn-label ucc-connected'
            : 'ucc-conn-label ucc-disconnected';
        this._icon.icon_name = c
            ? 'ucc-tray'
            : 'dialog-warning-symbolic';
    }

    // -----------------------------------------------------------------------
    // Data loading
    // -----------------------------------------------------------------------

    _loadCapabilities() {
        const capsJson = this._client.getCapabilitiesJSON();
        const caps = capsJson ? JSON.parse(capsJson) : [];
        if (caps.length === 0) {
            log('[UCC] No hardware capabilities detected — hiding indicator');
            this._stopTimers();
            this.visible = false;
            return;
        }

        const wc = this._client.getWaterCoolerSupported();
        if (wc !== this._state.waterCoolerSupported) {
            this._state.waterCoolerSupported = wc;
            this._wcMetricsBox.visible = wc;
            // Show/hide water cooler tab
            if (wc && !this._tabButtons['watercooler']) {
                this._addTab('watercooler', 'Water Cooler');
            }
        }

        const sysInfoRaw = this._client.getSystemInfoJSON();
        if (sysInfoRaw) {
            try {
                const si = JSON.parse(sysInfoRaw);
                this._state.laptopModel = si.laptopModel ?? '';
                this._state.cpuModel    = si.cpuModel    ?? '';
                // prefer dGPU model, fall back to iGPU
                this._state.gpuModel    = si.dGpuModel   || si.iGpuModel || '';
                this._updateSystemInfoLabels();
            } catch { /* ignore */ }
        }
    }

    _loadProfiles() {
        const s = this._state;

        // Built-in profiles
        const names = [], ids = [];
        const raw = this._client.getDefaultProfilesJSON();
        if (raw) {
            try {
                for (const p of JSON.parse(raw)) {
                    if (p.id) { ids.push(p.id); names.push(p.name ?? p.id); }
                }
            } catch { /* ignore */ }
        }

        // Load uccrc once — QSettings (IniFormat) wraps byte arrays with
        // @ByteArray(...) which GLib.KeyFile returns verbatim.
        // We must use get_value() (raw) instead of get_string() because
        // GLib cannot interpret the @ByteArray encoding.
        let uccrcKf = null;
        try {
            const uccrc = GLib.build_filenamev([GLib.get_home_dir(), '.config', 'uccrc']);
            uccrcKf = new GLib.KeyFile();
            uccrcKf.load_from_file(uccrc, GLib.KeyFileFlags.NONE);
        } catch { /* file may not exist */ }

        // Custom profiles from uccrc
        if (uccrcKf) {
            try {
                const cp = unwrapQByteArray(uccrcKf.get_value('General', 'customProfiles'));
                if (cp) {
                    for (const p of JSON.parse(cp)) {
                        if (p.id && !ids.includes(p.id)) {
                            ids.push(p.id); names.push(p.name ?? p.id);
                        }
                    }
                }
            } catch { /* ignore parse errors */ }
        }

        s.profileNames = names;
        s.profileIds = ids;

        // Active profile
        const ap = this._client.getActiveProfileJSON();
        if (ap) {
            try {
                const obj = JSON.parse(ap);
                s.activeProfileId = obj.id ?? '';
                s.activeProfileName = obj.name ?? '';
                s.activeProfileFanId = obj.fan?.fanProfile ?? '';
                s.wcAutoControl = obj.fan?.autoControlWC ?? true;
            } catch { /* ignore */ }
        }

        // Fan profiles
        const fanNames = [], fanIds = [];
        const fpRaw = this._client.getFanProfileNames();
        if (fpRaw) {
            try {
                for (const p of JSON.parse(fpRaw)) {
                    if (p.id) { fanIds.push(p.id); fanNames.push(p.name ?? p.id); }
                }
            } catch { /* ignore */ }
        }

        // Custom fan profiles from uccrc
        if (uccrcKf) {
            try {
                const cfp = unwrapQByteArray(uccrcKf.get_value('General', 'customFanProfiles'));
                if (cfp) {
                    for (const p of JSON.parse(cfp)) {
                        if (p.id && !fanIds.includes(p.id)) {
                            fanIds.push(p.id); fanNames.push(p.name ?? p.id);
                        }
                    }
                }
            } catch { /* ignore */ }
        }

        s.fanProfileNames = fanNames;
        s.fanProfileIds = fanIds;

        // Keyboard profiles from uccrc
        const kbNames = [], kbIds = [], kbData = [];
        if (uccrcKf) {
            try {
                const ckp = unwrapQByteArray(uccrcKf.get_value('General', 'customKeyboardProfiles'));
                if (ckp) {
                    for (const p of JSON.parse(ckp)) {
                        if (p.id) {
                            kbIds.push(p.id);
                            kbNames.push(p.name ?? p.id);
                            kbData.push(p);
                        }
                    }
                }
            } catch { /* ignore */ }
        }

        s.keyboardProfileNames = kbNames;
        s.keyboardProfileIds = kbIds;
        s.keyboardProfilesData = kbData;

        // Extract active keyboard profile from the active profile JSON
        if (ap) {
            try {
                const obj = JSON.parse(ap);
                const kbRef = obj.selectedKeyboardProfile ?? '';
                // Resolve: may be a UUID or a display name
                s.activeKeyboardProfileId = this._resolveKeyboardProfileId(kbRef);
            } catch { /* ignore */ }
        }

        // GPU OC profiles — built-in from daemon + custom from uccrc
        const gpuNames = [], gpuIds = [];
        const gpuRaw = this._client.getGpuProfilesJSON();
        if (gpuRaw) {
            try {
                for (const p of JSON.parse(gpuRaw)) {
                    if (p.id) { gpuIds.push(p.id); gpuNames.push(p.name ?? p.id); }
                }
            } catch { /* ignore */ }
        }
        if (uccrcKf) {
            try {
                const cgp = unwrapQByteArray(uccrcKf.get_value('General', 'customGpuProfiles'));
                if (cgp) {
                    for (const p of JSON.parse(cgp)) {
                        if (p.id && !gpuIds.includes(p.id)) {
                            gpuIds.push(p.id); gpuNames.push(p.name ?? p.id);
                        }
                    }
                }
            } catch { /* ignore */ }
        }
        s.gpuProfileNames = gpuNames;
        s.gpuProfileIds = gpuIds;

        // Extract active GPU profile from the active profile JSON
        if (ap) {
            try {
                const obj = JSON.parse(ap);
                s.activeProfileGpuId = obj.gpuProfileId ?? '';
            } catch { /* ignore */ }
        }

        // ODM performance profiles
        const odmProfiles = this._client.getAvailableODMProfiles();
        s.availableODMProfiles = Array.isArray(odmProfiles) ? odmProfiles : [];
        const odmCurrent = this._client.getODMPerformanceProfile();
        if (odmCurrent) s.odmPerformanceProfile = odmCurrent;

        this._rebuildProfileButtons();
        this._rebuildFanProfileButtons();
        this._rebuildKeyboardProfileButtons();
        this._rebuildGpuProfileButtons();
        this._rebuildODMProfileButtons();
    }

    /** Resolve a keyboard profile reference (UUID or display name) to its canonical UUID. */
    _resolveKeyboardProfileId(ref) {
        if (!ref) return '';
        const s = this._state;
        // If it's already a known ID, use as-is
        if (s.keyboardProfileIds.includes(ref)) return ref;
        // Otherwise look up by name
        const idx = s.keyboardProfileNames.indexOf(ref);
        return idx >= 0 ? s.keyboardProfileIds[idx] : ref;
    }

    // -----------------------------------------------------------------------
    // Polling
    // -----------------------------------------------------------------------

    _startTimers() {
        this._fastTimerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 1500, () => {
            this._pollMetrics();
            return GLib.SOURCE_CONTINUE;
        });
        this._slowTimerId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 5000, () => {
            this._pollSlowState();
            return GLib.SOURCE_CONTINUE;
        });
    }

    _stopTimers() {
        if (this._fastTimerId) { GLib.source_remove(this._fastTimerId); this._fastTimerId = 0; }
        if (this._slowTimerId) { GLib.source_remove(this._slowTimerId); this._slowTimerId = 0; }
    }

    _pollMetrics() {
        if (!this._client.connected) return;
        const s = this._state;

        s.cpuTemp   = this._client.getCpuTemperature();
        s.gpuTemp   = this._client.getGpuTemperature();
        s.cpuFreq   = this._client.getCpuFrequency();
        s.gpuFreq   = this._client.getGpuFrequency();
        s.cpuPower  = this._client.getCpuPower();
        s.gpuPower  = this._client.getGpuPower();
        s.cpuFanRPM = this._client.getFanSpeedRPM();
        s.gpuFanRPM = this._client.getGpuFanSpeedRPM();
        s.cpuFanPct = this._client.getFanSpeedPercent();
        s.gpuFanPct = this._client.getGpuFanSpeedPercent();

        // Extended NVIDIA dGPU metrics (single D-Bus call, parse once)
        const dgpuRaw = this._client._call('GetDGpuInfoValuesJSON');
        let dgpu = null;
        if (dgpuRaw) { try { dgpu = JSON.parse(dgpuRaw); } catch {} }
        s.gpuComputeUtilPct   = dgpu?.computeUtilPct   ?? -1;
        s.gpuMemoryUtilPct    = dgpu?.memoryUtilPct    ?? -1;
        s.gpuVramUsedMiB      = dgpu?.vramUsedMiB      ?? -1;
        s.gpuVramTotalMiB     = dgpu?.vramTotalMiB     ?? -1;
        s.gpuPerfLimitReason  = dgpu?.perfLimitReason  ?? '';
        s.gpuEncoderUtilPct   = dgpu?.encoderUtilPct   ?? -1;
        s.gpuDecoderUtilPct   = dgpu?.decoderUtilPct   ?? -1;
        s.gpuCurrentPstate    = dgpu?.currentPstate    ?? -1;
        const grOff = dgpu?.grClockOffsetMHz;
        s.gpuGrClockOffsetMHz = (grOff !== undefined && grOff !== -999) ? grOff : -999;
        const memOff = dgpu?.memClockOffsetMHz;
        s.gpuMemClockOffsetMHz= (memOff !== undefined && memOff !== -999) ? memOff : -999;
        s.gpuVramFreqMHz      = dgpu?.vramFrequency    ?? -1;
        s.gpuCoreVoltageMv    = dgpu?.coreVoltageMv    ?? -1;

        if (s.waterCoolerSupported) {
            s.wcFanSpeed  = this._client.getWaterCoolerFanSpeed();
            s.wcPumpLevel = this._client.getWaterCoolerPumpLevel();
        }

        this._updateDashboard();
    }

    _pollSlowState() {
        if (!this._client.connected) return;
        const s = this._state;

        // Active profile
        const ap = this._client.getActiveProfileJSON();
        if (ap) {
            try {
                const obj = JSON.parse(ap);
                const newId = obj.id ?? '';
                if (newId !== s.activeProfileId) {
                    s.activeProfileId = newId;
                    s.activeProfileName = obj.name ?? '';
                    s.activeProfileFanId = obj.fan?.fanProfile ?? '';
                    // Extract keyboard profile reference
                    const kbRef = obj.selectedKeyboardProfile ?? '';
                    s.activeKeyboardProfileId = this._resolveKeyboardProfileId(kbRef);
                    // Extract GPU profile reference
                    s.activeProfileGpuId = obj.gpuProfileId ?? '';
                    this._rebuildProfileButtons();
                    this._rebuildFanProfileButtons();
                    this._rebuildKeyboardProfileButtons();
                    this._rebuildGpuProfileButtons();
                }
                const oldAutoControl = s.wcAutoControl;
                s.wcAutoControl = obj.fan?.autoControlWC ?? true;
                if (oldAutoControl !== s.wcAutoControl) {
                    this._updateWaterCoolerControlsEnabled();
                }
            } catch { /* ignore */ }
        }

        // Power state
        const ps = this._client.getPowerState();
        if (ps && ps !== s.powerState) {
            s.powerState = ps;
            this._powerLabel.text = mapPowerState(ps);
            // Derive wcConnected from power state (matches KDE applet)
            const wasWcConnected = s.wcConnected;
            s.wcConnected = (mapPowerState(ps) === 'AC w/ Water Cooler');
            if (s.wcConnected !== wasWcConnected) {
                this._updateWaterCoolerControlsEnabled();
            }
        }

        // Hardware toggles
        const webcam = this._client.getWebcamEnabled();
        if (webcam !== s.webcamEnabled) {
            s.webcamEnabled = webcam;
            this._webcamSwitch.checked = webcam;
            this._webcamSwitch.label = webcam ? 'ON' : 'OFF';
        }

        const fn = this._client.getFnLock();
        if (fn !== s.fnLock) {
            s.fnLock = fn;
            this._fnLockSwitch.checked = fn;
            this._fnLockSwitch.label = fn ? 'ON' : 'OFF';
        }

        const br = this._client.getDisplayBrightness();
        if (br !== s.displayBrightness) {
            s.displayBrightness = br;
            this._brightnessSlider.value = br / 100;
            this._brightnessValueLabel.text = `${br}%`;
        }

        // Water cooler
        if (s.waterCoolerSupported) {
            const wcEn = this._client.isWaterCoolerEnabled();
            if (wcEn !== s.wcEnabled) {
                s.wcEnabled = wcEn;
                this._wcEnableSwitch.checked = wcEn;
                this._wcEnableSwitch.label = wcEn ? 'ON' : 'OFF';
            }
        }

        // ODM performance profile
        if (s.availableODMProfiles.length > 0) {
            const odmCurrent = this._client.getODMPerformanceProfile();
            if (odmCurrent && odmCurrent !== s.odmPerformanceProfile) {
                s.odmPerformanceProfile = odmCurrent;
                this._rebuildODMProfileButtons();
            }
        }
    }

    // -----------------------------------------------------------------------
    // Dashboard update
    // -----------------------------------------------------------------------

    _updateDashboard() {
        const s = this._state;
        const fmt = (v, unit) => v >= 0 ? `${Math.round(v)} ${unit}` : '—';
        const fmtF = (v, unit) => v >= 0 ? `${v.toFixed(1)} ${unit}` : '—';

        this._lCpuTemp.valueLabel.text   = fmt(s.cpuTemp, '°C');
        this._lGpuTemp.valueLabel.text   = fmt(s.gpuTemp, '°C');
        this._lCpuFreq.valueLabel.text   = fmt(s.cpuFreq, 'MHz');
        this._lGpuFreq.valueLabel.text   = fmt(s.gpuFreq, 'MHz');
        this._lCpuPower.valueLabel.text  = fmtF(s.cpuPower, 'W');
        this._lGpuPower.valueLabel.text  = fmtF(s.gpuPower, 'W');
        this._lCpuFan.valueLabel.text    = s.cpuFanRPM >= 0
            ? `${s.cpuFanRPM} RPM (${s.cpuFanPct}%)`
            : '—';
        this._lGpuFan.valueLabel.text    = s.gpuFanRPM >= 0
            ? `${s.gpuFanRPM} RPM (${s.gpuFanPct}%)`
            : '—';

        // Extended NVIDIA metrics — show/hide based on data availability
        const showIf = (row, cond, text) => { row.box.visible = cond; if (cond) row.valueLabel.text = text; };
        showIf(this._lGpuLoad,      s.gpuComputeUtilPct >= 0,    `${s.gpuComputeUtilPct} %`);
        showIf(this._lVramLoad,     s.gpuMemoryUtilPct >= 0,     `${s.gpuMemoryUtilPct} %`);
        showIf(this._lPstate,       s.gpuCurrentPstate >= 0,     `P${s.gpuCurrentPstate}`);
        showIf(this._lVramFreq,     s.gpuVramFreqMHz >= 0,       `${s.gpuVramFreqMHz} MHz`);
        showIf(this._lCoreVoltage,  s.gpuCoreVoltageMv >= 0,     `${s.gpuCoreVoltageMv} mV`);
        showIf(this._lClockOffsets, s.gpuGrClockOffsetMHz !== -999,
            `${s.gpuGrClockOffsetMHz >= 0 ? '+' : ''}${s.gpuGrClockOffsetMHz}` +
            ` / ${s.gpuMemClockOffsetMHz >= 0 ? '+' : ''}${s.gpuMemClockOffsetMHz} MHz`);
        showIf(this._lVram,         s.gpuVramTotalMiB > 0,
            `${s.gpuVramUsedMiB} / ${s.gpuVramTotalMiB} MiB`);
        showIf(this._lPerfLimit,    s.gpuPerfLimitReason.length > 0, s.gpuPerfLimitReason);
        showIf(this._lNvencDec,
            s.gpuEncoderUtilPct >= 0 || s.gpuDecoderUtilPct >= 0,
            `${s.gpuEncoderUtilPct >= 0 ? s.gpuEncoderUtilPct : '--'} / ${s.gpuDecoderUtilPct >= 0 ? s.gpuDecoderUtilPct : '--'} %`);

        if (s.waterCoolerSupported) {
            this._lWcFan.valueLabel.text  = fmt(s.wcFanSpeed, '%');
            // Display pump level as human-readable label (matches KDE applet)
            const pumpLabels = ['High', 'Max', 'Low', 'Medium', 'Off'];
            this._lWcPump.valueLabel.text = (s.wcPumpLevel >= 0 && s.wcPumpLevel < pumpLabels.length)
                ? pumpLabels[s.wcPumpLevel]
                : '—';
        }
    }

    _updateSystemInfoLabels() {
        const s = this._state;

        // Laptop model title
        if (this._sysModelLabel) {
            this._sysModelLabel.text    = s.laptopModel;
            this._sysModelLabel.visible = s.laptopModel.length > 0;
        }

        // CPU section title
        if (this._cpuTitleLabel) {
            this._cpuTitleLabel.text = s.cpuModel || 'CPU';
        }

        // GPU section title
        if (this._gpuTitleLabel) {
            this._gpuTitleLabel.text = s.gpuModel || 'GPU';
        }
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Water cooler control enable/disable (matches KDE wcConnected logic)
    // -----------------------------------------------------------------------

    _updateWaterCoolerControlsEnabled() {
        const enabled = this._state.wcConnected && !this._state.wcAutoControl;
        const wcConnected = this._state.wcConnected;

        // Fan slider & pump voltage buttons: require wcConnected AND !autoControl
        if (this._wcFanSlider) this._wcFanSlider.reactive = enabled;
        for (const btn of (this._pumpVoltageButtons ?? [])) {
            btn.reactive = enabled;
        }

        // Enable/LED toggles: require wcConnected
        if (this._wcLedSwitch) this._wcLedSwitch.reactive = wcConnected;
    }

    // -----------------------------------------------------------------------
    // uccrc file monitor
    // -----------------------------------------------------------------------

    _startUccrcMonitor() {
        try {
            const file = Gio.File.new_for_path(this._uccrcPath);
            // Monitor the parent directory — some editors replace the file
            // (delete + create) which breaks a direct file monitor
            const parent = file.get_parent();
            if (!parent) return;

            this._uccrcMonitor = parent.monitor_directory(
                Gio.FileMonitorFlags.NONE, null);

            this._uccrcMonitor.connect('changed', (_monitor, changedFile, _otherFile, eventType) => {
                // React to CREATED / CHANGED / CHANGES_DONE_HINT for uccrc
                if (changedFile.get_basename() !== 'uccrc') return;
                if (eventType !== Gio.FileMonitorEvent.CHANGES_DONE_HINT &&
                    eventType !== Gio.FileMonitorEvent.CREATED &&
                    eventType !== Gio.FileMonitorEvent.CHANGED) return;

                // Debounce: delay reload slightly (some editors do delete + create)
                if (this._uccrcReloadId) {
                    GLib.source_remove(this._uccrcReloadId);
                }
                this._uccrcReloadId = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 500, () => {
                    this._uccrcReloadId = 0;
                    log('[UCC] uccrc changed, reloading profiles…');
                    this._loadProfiles();
                    return GLib.SOURCE_REMOVE;
                });
            });
        } catch (e) {
            logError(e, '[UCC] Failed to start uccrc monitor');
        }
    }

    _stopUccrcMonitor() {
        if (this._uccrcReloadId) {
            GLib.source_remove(this._uccrcReloadId);
            this._uccrcReloadId = 0;
        }
        if (this._uccrcMonitor) {
            this._uccrcMonitor.cancel();
            this._uccrcMonitor = null;
        }
    }

    destroy() {
        this._stopTimers();
        this._stopUccrcMonitor();
        this._client?.destroy();
        super.destroy();
    }
});  // end GObject.registerClass

// ---------------------------------------------------------------------------
// Extension entry point
// ---------------------------------------------------------------------------

export default class UccExtension extends Extension {
    enable() {
        this._indicator = new UccIndicator(this);
        Main.panel.addToStatusArea('ucc-indicator', this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
    }
}
