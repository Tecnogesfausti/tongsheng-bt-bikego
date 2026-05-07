# tongsheng-bt-bikego

Proyecto de ingeniería y pruebas BLE para display Tongsheng (EKD01-TB) con:

- `esp32c3-ble-bridge`: MITM BLE en ESP32-C3 (puente teléfono <-> display).
- `esp32c3-ekd01-tb-mock`: base de emulador/mock BLE del display EKD01-TB.
- `vamosbici`: APK mínima de navegación offline (PoC pasos -> futuro BLE nav).
- `ble-handshake-apk`: APK Android para escaneo, conexión, handshake, comandos y pruebas de navegación BLE.

## Estructura

- `esp32c3-ble-bridge/`
  - Firmware PlatformIO para ESP32-C3.
  - Objetivo: capturar/reenviar tramas BLE y facilitar reverse engineering.

- `ble-handshake-apk/`
  - App Android de prueba.
  - Funciones actuales: conexión BLE, handshake, control asistencia/luz/unidades/brillo, telemetría básica y pruebas NAV (`F1 03`).

## Notas

- El repositorio excluye artefactos de build (`build/`, `.gradle/`, `.pio/`).
- Para compilar la APK, configura tu Android SDK local (no se versiona `local.properties`).
