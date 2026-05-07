# ESP32-C3 EKD01-TB Mock

Firmware para ESP32-C3 que actúa como **mock de display EKD01-TB** (BLE NUS) para pruebas con Bikego.

Base: proyecto `esp32c3-ble-bridge`.

## Objetivo

- Anunciarse por BLE como `EKD01-TB`.
- Aceptar conexión de Bikego.
- Responder/trazar tramas para pruebas de protocolo sin depender siempre del display real.

## Estado actual

Este proyecto parte del bridge funcional y se usa como rama de trabajo para evolucionar a mock completo.

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
