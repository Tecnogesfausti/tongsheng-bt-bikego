# Bikego <-> EKD01-TB Pairing Protocol (BLE NUS)

Este documento resume lo observado en capturas reales y pruebas MITM/mock.

## 1) Capa BLE

- Nombre anunciado: `EKD01-TB ` (con espacio final en varios casos reales).
- Servicio BLE: `6e400001-b5a3-f393-e0a9-e50e24dcca9e` (Nordic UART Service).
- Característica write (Phone -> Display): `6e400002-b5a3-f393-e0a9-e50e24dcca9e`.
- Característica notify (Display -> Phone): `6e400003-b5a3-f393-e0a9-e50e24dcca9e`.

## 2) Formato de trama

Estructura general observada:

- Cabecera: `55 AA`
- Longitud/payload/tipo: bytes siguientes (varía por comando)
- CRC final: 2 bytes little-endian

CRC usado en este ecosistema (observado):

- `sum = bytes[2 .. len-3]`
- `crc = 0xFFFF ^ (sum & 0xFFFF)`
- `lo = crc & 0xFF`, `hi = crc >> 8`

## 3) Handshake de vinculación

### Paso A: challenge request (Phone -> Display)

`55 AA 01 11 10 01 00 04 D8 FF`

Interpretación:

- `10 01` = read challenge
- `00 04` = offset/longitud solicitada (4 bytes)

### Paso B: challenge response (Display -> Phone)

Ejemplo:

`55 AA 04 10 11 04 00 47 5A 61 13 C1 FE`

Los 4 bytes de challenge están en `47 5A 61 13` (posición 7..10).

### Paso C: auth frame (Phone -> Display)

El phone cifra un bloque de 16 bytes con AES-128-ECB:

- Bloque claro: `challenge[0..3] + 12 bytes 0x00`
- Clave AES (ASCII): `2CTDU40qNyCgTjb1`

Comando auth:

- Tipo: `... 10 20 00 ...`
- Incluye 16 bytes cifrados (resultado AES)
- CRC al final

### Paso D: auth ACK (Display -> Phone)

`55 AA 01 10 11 20 00 00 BD FF`

Con esto Bikego da por aceptada la sesión.

## 4) Post-auth típico

Tras ACK, Bikego suele lanzar:

- `A5 01 88` (lectura string, ej. nombre corto)
- `A5 01 18` (lectura string, ej. modelo)
- `F1 01 01` (lectura bloque de info/config)
- `A5 01 E0` (lectura unidad kmh/mph)
- `A6 01 1C` (lectura brillo)
- `10 02 3E/42/46` (poll puntual con ACK por offset)

Respuestas típicas:

- `A5 11 04 88 ... "EKD01-TB "...`
- `A5 11 04 18 ... "EKD01_TB_N22"...`
- `F1 11 04 01 ...` (bloque binario de config)
- `01 A5 11 04 E0 <00|01> ...` (unidad actual)
- `01 A5 11 04 1C <01..04> ...` (brillo)
- `01 10 11 05 3E ...`, `01 10 11 05 42 ...`, `01 10 11 05 46 ...` (ACK de poll)

## 5) Polling de estado en marcha

Bikego repite:

- `55 AA 04 11 10 02 42 ...`
- `55 AA 04 11 10 02 46 ...`

Y el display envía notificaciones periódicas tipo:

- `55 AA 15 10 11 06 ...` (estado principal)
- `55 AA 10 10 11 06 ...` (estado adicional)

Campos confirmados por pruebas:

- `A4` = asistencia (write)
- `A6` = luz (write)
- `A7` = brillo (write)
- `E0` = unidades (0=KMH, 1=MPH) (write)
- `speed_raw` en `0x15` (little-endian), con relación: `km/h = speed_raw / 10`
- batería en `%` presente en `0x15` (byte observado coherente con valor real)

## 6) Navegación (Bikego -> Display)

Canal visto:

- `F1 03` en tramas `55 AA 12 11 F1 03 ...`

Contiene varios “slots” con tipo+distancia, por ejemplo:

- `02 20 00 00` -> tipo `0x02`, distancia `0x0020` (32 m)
- `03 B8 16 00 00` -> objetivo final `0x16B8` (5816 m ≈ 5.8 km)

Se requiere más mapeo para cerrar iconografía completa (izq/der/recto exacto por tipo).

## 7) Estado de implementación en este repo

- APK de pruebas BLE: `ble-handshake-apk/`
- Bridge MITM ESP32-C3: `esp32c3-ble-bridge/`
- Mock EKD01-TB ESP32-C3: `esp32c3-ekd01-tb-mock/`

El mock ya implementa handshake base y estado fake suficiente para que Bikego establezca sesión BLE y procese telemetría.
