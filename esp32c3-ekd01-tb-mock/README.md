# ESP32-C3 EKD01-TB Mock

Firmware para ESP32-C3 que actúa como **mock de display EKD01-TB** (BLE NUS) para pruebas con Bikego.

Base: proyecto `esp32c3-ble-bridge`.

## Objetivo

- Anunciarse por BLE como `EKD01-TB`.
- Aceptar conexión de Bikego.
- Responder/trazar tramas para pruebas de protocolo sin depender siempre del display real.

## Estado actual

Este proyecto parte del bridge funcional y se usa como rama de trabajo para evolucionar a mock completo.

## SDK/Framework estable (importante)

Para evitar reinicios/panics observados con combinaciones más nuevas, este mock queda fijado a:

- PlatformIO `espressif32@2023.10.3`
- Arduino-ESP32 `2.0.14` (resuelto por la plataforma)
- ESP-IDF `4.4.6` (resuelto por la plataforma)

No usar "última versión publicada" sin validar primero estabilidad BLE.

## Compilar y subir

```bash
cd /home/lego/bikego/tongsheng-bt-bikego/esp32c3-ekd01-tb-mock
pio run
pio run -t upload
pio device monitor -b 115200
```

## Notas

- Si quieres modo bridge con display real, usa `esp32c3-ble-bridge/`.
- Aquí iremos separando lógica de emulación (mock) de la lógica MITM.
