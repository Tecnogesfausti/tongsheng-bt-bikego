# ESP32-C3 BLE Bridge (Bikego <-> EKD01-TB)

Este firmware hace puente BLE NUS bidireccional:
- El ESP32-C3 **anuncia** como `EKD01-TB` para que Bikego se conecte.
- El ESP32-C3 se conecta como cliente al display real (`REAL_DISPLAY_MAC`).
- Reenvía tráfico:
  - Bikego write -> display write (`6e400002...`)
  - Display notify -> Bikego notify (`6e400003...`)
- Muestra estado en GC9A01A.

## Pines pantalla (GC9A01A)
Configurados según tu ESPHome:
- `RST = GPIO6`
- `CS  = GPIO7`
- `DC  = GPIO8`

SPI por defecto en este proyecto:
- `SCLK = GPIO4`
- `MOSI = GPIO5`

Si tu cableado SPI es distinto, cambia en `src/main.cpp`.

## Antes de compilar
Editar en `src/main.cpp`:
- `REAL_DISPLAY_MAC` con la MAC del display real.

## Compilar y subir
```bash
cd /home/lego/bikego/esp32c3-ble-bridge
pio run
pio run -t upload
pio device monitor -b 115200
```

## Flujo de uso
1. Enciende display real.
2. Enciende ESP32-C3 con este firmware.
3. Espera a que el ESP muestre `DISPLAY: CONNECTED`.
4. Abre Bikego y conecta al `EKD01-TB` (el ESP).
5. Debe mostrar `PHONE: CONNECTED` y empezar el bridge.

## Notas
- Si no conecta al display real: revisar `REAL_DISPLAY_MAC`.
- Si Bikego no conecta: confirmar que no esté conectado directo al display real.
- Logs hex salen por serie (`[TX P->D]`, `[RX D->P]`).
