%global _hardened_build 1

Name:           ucc
Version:        0.1.0
Release:        1%{?dist}
Summary:        Uniwill Control Center - System control application suite
License:        GPL-3.0-or-later

URL:            https://github.com/tuxedocomputers/uniwill-control-center
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  extra-cmake-modules
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel >= 6.0
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  qt6-qtconnectivity-devel
BuildRequires:  kf6-kwindowsystem-devel
BuildRequires:  libplasma-devel
BuildRequires:  systemd-devel
BuildRequires:  libXrandr-devel
BuildRequires:  libgudev-devel

Requires:       systemd
Requires:       qt6-qtbase >= 6.0
Requires:       qt6-qtdeclarative
Requires:       qt6-qtconnectivity
Requires:       tuxedo-drivers
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
Uniwill Control Center (UCC) is a comprehensive system control application 
suite for Uniwill computers. It provides:

- Daemon (uccd): Background service for system control and monitoring
- GUI (ucc-gui): Main graphical user interface
- System Tray (ucc-tray): Quick access tray applet
- Plasma Widgets: Desktop integration widgets

Features include fan control, keyboard backlight management, display settings,
CPU power management, and water cooler control for supported systems.

%prep
%autosetup

%build
%cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_LIBDIR=%{_libdir} \
  -DCMAKE_INSTALL_RPATH="" \
  -DBUILD_GUI=ON \
  -DBUILD_TRAY=ON \
  -DBUILD_WIDGETS=ON
%cmake_build

%install
%cmake_install

# Create configuration directory
mkdir -p %{buildroot}%{_sysconfdir}/ucc

# Set permissions
chmod 755 %{buildroot}%{_sysconfdir}/ucc

%post
%systemd_post uccd.service
systemctl daemon-reload > /dev/null 2>&1 || true

%preun
%systemd_preun uccd.service

%postun
%systemd_postun_with_restart uccd.service

%files
%license LICENSE
%doc README.md
%{_bindir}/uccd
%{_bindir}/ucc-gui
%{_bindir}/ucc-tray
%{_libdir}/libucc-dbus.so*
%{_unitdir}/uccd.service
%{_unitdir}/uccd-sleep.service
%{_datadir}/dbus-1/system-services/com.uniwill.uccd.service
%{_datadir}/dbus-1/system.d/com.uniwill.uccd.conf
%dir %{_sysconfdir}/ucc
%{_datadir}/applications/ucc-gui.desktop
%{_datadir}/applications/ucc-tray.desktop
# WIDDGET # %{_datadir}/plasma/plasmoids/org.uniwill.ucc.*/
# Icons (SVG and PNG, all sizes)
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_16.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_24.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_32.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_48.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_64.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-gui_128.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_16.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_24.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_32.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_48.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_64.svg
%{_datadir}/icons/hicolor/scalable/apps/ucc-tray_128.svg
%{_datadir}/icons/hicolor/16x16/apps/ucc-gui.png
%{_datadir}/icons/hicolor/24x24/apps/ucc-gui.png
%{_datadir}/icons/hicolor/32x32/apps/ucc-gui.png
%{_datadir}/icons/hicolor/48x48/apps/ucc-gui.png
%{_datadir}/icons/hicolor/64x64/apps/ucc-gui.png
%{_datadir}/icons/hicolor/128x128/apps/ucc-gui.png
%{_datadir}/icons/hicolor/16x16/apps/ucc-tray.png
%{_datadir}/icons/hicolor/24x24/apps/ucc-tray.png
%{_datadir}/icons/hicolor/32x32/apps/ucc-tray.png
%{_datadir}/icons/hicolor/48x48/apps/ucc-tray.png
%{_datadir}/icons/hicolor/64x64/apps/ucc-tray.png
%{_datadir}/icons/hicolor/128x128/apps/ucc-tray.png
%{_datadir}/pixmaps/ucc-gui.svg
%{_datadir}/pixmaps/ucc-tray.svg
