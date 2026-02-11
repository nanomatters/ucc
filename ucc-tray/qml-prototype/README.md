UCC Tray QML Prototype

This prototype is a small, standalone QML application to preview the 3-tab tray UI:
- Dashboard
- Performance
- Hardware

Build & run (simple local build):

  mkdir -p src/ucc/ucc-tray/qml-prototype/build && cd src/ucc/ucc-tray/qml-prototype/build
  cmake ..
  cmake --build .
  ./ucc-tray-proto

Notes:
- Uses Qt Quick Controls (Material style). Adjust `QT_QUICK_CONTROLS_STYLE` or CMake compile definition if you want a different style.
- This is a visual prototype only; no DBus integration yet. I can wire a `TrayController` to expose DBus properties if you want realistic data updates.

Next steps (pick one):
- Connect controls to DBus (UCCD) to read/write real system state.
- Add animations, smaller polish, and keyboard accessibility.
- Replace placeholders (fan curve) with an interactive `FanCurveEditor` component.
