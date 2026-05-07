# VamosBici

PoC Android para navegación offline orientada a enviar pasos al display EKD01 por BLE.

## Estado actual

- Scaffold Android compilable.
- Pantalla mínima:
  - origen/destino (texto)
  - botón `Calcular pasos offline (demo)`
  - lista de pasos extraídos
- Parser básico de pasos desde GPX de ejemplo.

## Objetivo siguiente (offline real)

1. Integrar motor BRouter local en Android.
2. Consumir ruta real offline (no demo).
3. Extraer maniobras (`turn`, `distance`) desde salida BRouter.
4. Convertir a tramas `F1 03` para EKD01.
5. Reutilizar capa BLE de `ble-handshake-apk`.

## Compilar

```bash
cd /home/lego/bikego/tongsheng-bt-bikego/vamosbici
./gradlew assembleDebug
```
