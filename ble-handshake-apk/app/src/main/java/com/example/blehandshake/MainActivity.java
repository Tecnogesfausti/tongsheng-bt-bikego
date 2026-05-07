package com.example.blehandshake;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanSettings;
import android.bluetooth.le.ScanResult;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.ListView;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

public class MainActivity extends Activity {
    private static final UUID NUS_SERVICE = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID NUS_RX = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID NUS_TX = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");

    private static final byte[] AES_KEY = "2CTDU40qNyCgTjb1".getBytes(StandardCharsets.US_ASCII);

    private final Handler h = new Handler(Looper.getMainLooper());

    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic chRxWrite;

    private Button btnScan;
    private Button btnConnect;
    private Button btnConnectLast;
    private Button btnLight;
    private Button btnNavStart;
    private Button btnNavLeft;
    private Button btnNavStraight;
    private Button btnNavRight;
    private Button btnKmh;
    private Button btnMph;
    private Button btnA1;
    private Button btnA2;
    private Button btnA3;
    private Button btnA4;
    private Button btnA5;
    private Button btnB1;
    private Button btnB2;
    private Button btnB3;
    private Button btnB4;
    private TextView txtStatus;
    private TextView txtState;
    private TextView txtLog;
    private TextView txtBatteryPct;
    private ProgressBar pbBattery;
    private CheckBox cbVerboseLog;
    private ListView listDevices;

    private final List<BluetoothDevice> devices = new ArrayList<>();
    private final List<String> deviceRows = new ArrayList<>();
    private ArrayAdapter<String> listAdapter;
    private int selected = -1;
    private boolean scanning = false;
    private boolean verboseLog = true;

    private static final int REQ_PERM = 7;
    private static final String PREFS = "ble_handshake_prefs";
    private static final String PREF_LAST_ADDR = "last_addr";
    private static final String PREF_LAST_NAME = "last_name";
    private int lastAssist = -1;
    private int lastLight = -1;
    private String lastUnit = "?";
    private int lastBrightness = -1;
    private int lastBattery = -1;
    private int lastSpeedRaw = -1;
    private SharedPreferences prefs;
    private long lastBackPressMs = 0L;
    private int navTurnType = 0x02;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        btnScan = findViewById(R.id.btnScan);
        btnConnect = findViewById(R.id.btnConnect);
        btnConnectLast = findViewById(R.id.btnConnectLast);
        btnLight = findViewById(R.id.btnLight);
        btnNavStart = findViewById(R.id.btnNavStart);
        btnNavLeft = findViewById(R.id.btnNavLeft);
        btnNavStraight = findViewById(R.id.btnNavStraight);
        btnNavRight = findViewById(R.id.btnNavRight);
        btnKmh = findViewById(R.id.btnKmh);
        btnMph = findViewById(R.id.btnMph);
        btnA1 = findViewById(R.id.btnA1);
        btnA2 = findViewById(R.id.btnA2);
        btnA3 = findViewById(R.id.btnA3);
        btnA4 = findViewById(R.id.btnA4);
        btnA5 = findViewById(R.id.btnA5);
        btnB1 = findViewById(R.id.btnB1);
        btnB2 = findViewById(R.id.btnB2);
        btnB3 = findViewById(R.id.btnB3);
        btnB4 = findViewById(R.id.btnB4);
        txtStatus = findViewById(R.id.txtStatus);
        txtState = findViewById(R.id.txtState);
        txtLog = findViewById(R.id.txtLog);
        txtBatteryPct = findViewById(R.id.txtBatteryPct);
        pbBattery = findViewById(R.id.pbBattery);
        cbVerboseLog = findViewById(R.id.cbVerboseLog);
        listDevices = findViewById(R.id.listDevices);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        listAdapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_single_choice, deviceRows);
        listDevices.setAdapter(listAdapter);
        listDevices.setChoiceMode(ListView.CHOICE_MODE_SINGLE);
        listDevices.setOnItemClickListener((p, v, pos, id) -> {
            selected = pos;
            log("Seleccionado: " + deviceRows.get(pos));
        });

        BluetoothManager bm = getSystemService(BluetoothManager.class);
        adapter = bm.getAdapter();
        scanner = adapter.getBluetoothLeScanner();

        btnScan.setOnClickListener(v -> startScan());
        btnConnect.setOnClickListener(v -> connectSelected());
        btnConnectLast.setOnClickListener(v -> connectLastSaved());
        btnLight.setOnClickListener(v -> lightOnOffOn());
        btnNavStart.setOnClickListener(v -> sendNavSequence());
        btnNavLeft.setOnClickListener(v -> {
            navTurnType = 0x02;
            log("NAV tipo seleccionado: IZQ (0x02)");
            sendNavSequence();
        });
        btnNavStraight.setOnClickListener(v -> {
            navTurnType = 0x04;
            log("NAV tipo seleccionado: RECTO (0x04)");
            sendNavSequence();
        });
        btnNavRight.setOnClickListener(v -> {
            navTurnType = 0x03;
            log("NAV tipo seleccionado: DER (0x03)");
            sendNavSequence();
        });
        btnKmh.setOnClickListener(v -> setUnit(false));
        btnMph.setOnClickListener(v -> setUnit(true));
        btnA1.setOnClickListener(v -> setAssist(1));
        btnA2.setOnClickListener(v -> setAssist(2));
        btnA3.setOnClickListener(v -> setAssist(3));
        btnA4.setOnClickListener(v -> setAssist(4));
        btnA5.setOnClickListener(v -> setAssist(5));
        btnB1.setOnClickListener(v -> setBrightness(1));
        btnB2.setOnClickListener(v -> setBrightness(2));
        btnB3.setOnClickListener(v -> setBrightness(3));
        btnB4.setOnClickListener(v -> setBrightness(4));
        cbVerboseLog.setOnCheckedChangeListener((buttonView, isChecked) -> {
            verboseLog = isChecked;
            log("Verbose RX/TX: " + (verboseLog ? "ON" : "OFF"));
        });

        refreshLastButton();
        ensurePerms();
    }

    private void setStatus(String s) {
        txtStatus.setText("Estado: " + s);
    }

    private void log(String s) {
        String prev = txtLog.getText().toString();
        txtLog.setText(prev + s + "\n");
    }

    private void refreshState() {
        String assistTxt = lastAssist < 0 ? "?" : String.valueOf(lastAssist);
        String lightTxt = lastLight < 0 ? "?" : (lastLight == 0 ? "OFF" : "ON");
        String briTxt = lastBrightness < 0 ? "?" : String.valueOf(lastBrightness);
        String speedTxt = lastSpeedRaw < 0 ? "?" : String.valueOf(lastSpeedRaw);
        String speedHuman = "?";
        if (lastSpeedRaw >= 0) {
            double kmh = lastSpeedRaw / 10.0;
            double mph = kmh * 0.621371;
            if ("MPH".equals(lastUnit)) {
                speedHuman = String.format(Locale.US, "%.1f mph (%.1f km/h)", mph, kmh);
            } else {
                speedHuman = String.format(Locale.US, "%.1f km/h (%.1f mph)", kmh, mph);
            }
        }
        txtState.setText("Estado display: assist=" + assistTxt + ", luz=" + lightTxt + ", unidad=" + lastUnit + ", brillo=" + briTxt + ", speed_raw=" + speedTxt + ", speed=" + speedHuman);
        if (lastBattery >= 0 && lastBattery <= 100) {
            pbBattery.setProgress(lastBattery);
            txtBatteryPct.setText(lastBattery + "%");
        } else {
            pbBattery.setProgress(0);
            txtBatteryPct.setText("--%");
        }
    }

    private void refreshLastButton() {
        String addr = prefs.getString(PREF_LAST_ADDR, "");
        String name = prefs.getString(PREF_LAST_NAME, "");
        if (addr == null || addr.isEmpty()) {
            btnConnectLast.setText("Conectar último: -");
            btnConnectLast.setEnabled(false);
            return;
        }
        String label = (name == null || name.isEmpty()) ? addr : (name + " (" + addr + ")");
        btnConnectLast.setText("Conectar último: " + label);
        btnConnectLast.setEnabled(true);
    }

    private void saveLastDevice(BluetoothDevice d) {
        if (d == null) return;
        String name = d.getName() == null ? "" : d.getName();
        prefs.edit().putString(PREF_LAST_ADDR, d.getAddress()).putString(PREF_LAST_NAME, name).apply();
        refreshLastButton();
    }

    private boolean ensurePerms() {
        List<String> need = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.BLUETOOTH_SCAN);
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.BLUETOOTH_CONNECT);
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.ACCESS_FINE_LOCATION);
        } else {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        if (!need.isEmpty()) {
            log("Pidiendo permisos: " + need);
            ActivityCompat.requestPermissions(this, need.toArray(new String[0]), REQ_PERM);
            return false;
        }
        log("Permisos BLE: OK");
        return true;
    }

    @SuppressLint("MissingPermission")
    private void startScan() {
        log("Scan solicitado");
        if (!ensurePerms()) return;
        if (adapter == null) {
            log("Bluetooth adapter no disponible");
            return;
        }
        if (!adapter.isEnabled()) {
            log("Bluetooth está apagado. Enciéndelo y reintenta.");
            return;
        }
        log("BT encendido: OK");
        LocationManager lm = getSystemService(LocationManager.class);
        if (lm != null) {
            boolean gps = false;
            boolean net = false;
            try { gps = lm.isProviderEnabled(LocationManager.GPS_PROVIDER); } catch (Exception ignored) {}
            try { net = lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER); } catch (Exception ignored) {}
            if (!gps && !net) {
                log("Ubicación está apagada. En Samsung esto puede bloquear BLE scan.");
            }
        }
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            log("Scanner BLE nulo (BT recién encendido o ocupado).");
            return;
        }
        if (scanning) {
            try { scanner.stopScan(scanCb); } catch (Exception ignored) {}
            scanning = false;
        }
        devices.clear();
        deviceRows.clear();
        listAdapter.notifyDataSetChanged();
        selected = -1;
        setStatus("escaneando...");
        log("Scan start (LOW_LATENCY, 8s)");
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();
        scanner.startScan(null, settings, scanCb);
        scanning = true;
        h.postDelayed(() -> {
            if (scanning && devices.isEmpty()) {
                log("Sin resultados aún: reiniciando scan...");
                try { scanner.stopScan(scanCb); } catch (Exception ignored) {}
                try { scanner.startScan(null, settings, scanCb); } catch (Exception e) { log("Re-scan error: " + e.getMessage()); }
            }
        }, 3500);
        h.postDelayed(() -> {
            try { scanner.stopScan(scanCb); } catch (Exception ignored) {}
            scanning = false;
            setStatus("scan listo (" + devices.size() + " dispositivos)");
            log("Scan stop, encontrados=" + devices.size());
        }, 8000);
    }

    private final ScanCallback scanCb = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice d = result.getDevice();
            String addr = d.getAddress();
            for (BluetoothDevice ex : devices) {
                if (ex.getAddress().equals(addr)) return;
            }
            devices.add(d);
            String name = d.getName() == null ? "" : d.getName();
            String row = String.format(Locale.US, "%s  (%s) RSSI=%d", addr, name, result.getRssi());
            deviceRows.add(row);
            listAdapter.notifyDataSetChanged();
            runOnUiThread(() -> log("Found: " + row));
        }

        @Override
        public void onBatchScanResults(List<ScanResult> results) {
            runOnUiThread(() -> log("Batch results: " + results.size()));
            for (ScanResult r : results) onScanResult(0, r);
        }

        @Override
        public void onScanFailed(int errorCode) {
            runOnUiThread(() -> {
                scanning = false;
                setStatus("scan error");
                log("Scan failed errorCode=" + errorCode);
            });
        }
    };

    @SuppressLint("MissingPermission")
    private void connectSelected() {
        if (!ensurePerms()) return;
        if (selected < 0 || selected >= devices.size()) {
            log("Selecciona un dispositivo primero");
            return;
        }
        BluetoothDevice d = devices.get(selected);
        saveLastDevice(d);
        setStatus("conectando " + d.getAddress());
        if (gatt != null) {
            try { gatt.close(); } catch (Exception ignored) {}
        }
        gatt = d.connectGatt(this, false, gattCb, BluetoothDevice.TRANSPORT_LE);
    }

    @SuppressLint("MissingPermission")
    private void connectLastSaved() {
        if (!ensurePerms()) return;
        if (adapter == null) return;
        String addr = prefs.getString(PREF_LAST_ADDR, "");
        if (addr == null || addr.isEmpty()) {
            log("No hay último dispositivo guardado");
            return;
        }
        BluetoothDevice d;
        try {
            d = adapter.getRemoteDevice(addr);
        } catch (Exception e) {
            log("MAC guardada inválida: " + addr);
            return;
        }
        setStatus("conectando último " + addr);
        if (gatt != null) {
            try { gatt.close(); } catch (Exception ignored) {}
        }
        gatt = d.connectGatt(this, false, gattCb, BluetoothDevice.TRANSPORT_LE);
    }

    private final BluetoothGattCallback gattCb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            runOnUiThread(() -> log("onConnectionStateChange status=" + status + " state=" + newState));
            if (newState == BluetoothGatt.STATE_CONNECTED) {
                runOnUiThread(() -> setStatus("conectado, descubriendo servicios"));
                g.requestMtu(255);
                g.discoverServices();
            } else if (newState == BluetoothGatt.STATE_DISCONNECTED) {
                runOnUiThread(() -> setStatus("desconectado"));
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            BluetoothGattService s = g.getService(NUS_SERVICE);
            if (s == null) {
                runOnUiThread(() -> log("NUS no encontrado"));
                return;
            }
            chRxWrite = s.getCharacteristic(NUS_RX);
            BluetoothGattCharacteristic chTxNotify = s.getCharacteristic(NUS_TX);
            if (chRxWrite == null || chTxNotify == null) {
                runOnUiThread(() -> log("NUS chars no encontrados"));
                return;
            }
            g.setCharacteristicNotification(chTxNotify, true);
            BluetoothGattDescriptor d = chTxNotify.getDescriptor(CCCD);
            if (d != null) {
                d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                g.writeDescriptor(d);
            }
            runOnUiThread(() -> {
                setStatus("conectado + notify");
                log("Notify habilitado. Lanzando handshake...");
            });
            h.postDelayed(MainActivity.this::startHandshake, 250);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            byte[] v = c.getValue();
            onFrame(v);
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            runOnUiThread(() -> log("WRITE status=" + status));
        }
    };

    private void onFrame(byte[] v) {
        if (verboseLog) runOnUiThread(() -> log("RX " + hex(v)));
        if (v == null || v.length < 10) return;

        if (v.length >= 30 && u(v[0]) == 0x55 && u(v[1]) == 0xAA && u(v[2]) == 0x15 && u(v[3]) == 0x10 && u(v[4]) == 0x11 && u(v[5]) == 0x06) {
            // Based on captured frames:
            // ... 06 01 00 00 00 01 LL AA 55 ...
            // LL toggles with light (A6), AA toggles with assist (A4)
            int light = u(v[11]) & 0x01;
            int assist = u(v[12]) & 0xFF;
            int battery = u(v[14]) & 0xFF;
            int speedRaw = (u(v[16]) | (u(v[17]) << 8));
            lastLight = light;
            if (assist <= 9) lastAssist = assist;
            if (battery <= 100) lastBattery = battery;
            lastSpeedRaw = speedRaw;
            runOnUiThread(this::refreshState);
        }
        if (v.length >= 25 && u(v[0]) == 0x55 && u(v[1]) == 0xAA && u(v[2]) == 0x10 && u(v[3]) == 0x10 && u(v[4]) == 0x11 && u(v[5]) == 0x06) {
            // In captures:
            // byte 20 -> brightness marker 0x41..0x44
            // byte 21 -> unit 00/01 (kmh/mph)
            int b = u(v[20]);
            if (b >= 0x41 && b <= 0x44) lastBrightness = b - 0x40;
            int unit = u(v[21]) & 0x01;
            lastUnit = (unit == 1) ? "MPH" : "KMH";
            runOnUiThread(this::refreshState);
        }

        // challenge response: 55 AA 04 10 11 04 00 ch0 ch1 ch2 ch3 ...
        if (v.length >= 13 && u(v[0]) == 0x55 && u(v[1]) == 0xAA && u(v[2]) == 0x04 && u(v[3]) == 0x10 && u(v[4]) == 0x11 && u(v[5]) == 0x04 && u(v[6]) == 0x00) {
            byte[] ch = new byte[]{v[7], v[8], v[9], v[10]};
            sendAuth(ch);
            return;
        }

        // auth ack: ... 10 11 20 ..
        if (u(v[3]) == 0x10 && u(v[4]) == 0x11 && u(v[5]) == 0x20 && u(v[7]) == 0x00) {
            runOnUiThread(() -> log("Auth ACK OK -> post-init"));
            postInit();
        }
    }

    private void startHandshake() {
        write(hexToBytes("55 AA 01 11 10 01 00 04 D8 FF"));
    }

    private void sendAuth(byte[] challenge4) {
        try {
            byte[] plain = new byte[16];
            System.arraycopy(challenge4, 0, plain, 0, 4);
            Cipher c = Cipher.getInstance("AES/ECB/NoPadding");
            c.init(Cipher.ENCRYPT_MODE, new SecretKeySpec(AES_KEY, "AES"));
            byte[] crypt = c.doFinal(plain);

            byte[] frame = new byte[25];
            frame[0] = 0x55;
            frame[1] = (byte) 0xAA;
            frame[2] = 0x10;
            frame[3] = 0x11;
            frame[4] = 0x10;
            frame[5] = 0x20;
            frame[6] = 0x00;
            System.arraycopy(crypt, 0, frame, 7, 16);
            int crc = crcLikeBikego(frame);
            frame[23] = (byte) (crc & 0xFF);
            frame[24] = (byte) ((crc >> 8) & 0xFF);
            write(frame);
            runOnUiThread(() -> log("Auth sent"));
        } catch (Exception e) {
            runOnUiThread(() -> log("Auth error: " + e.getMessage()));
        }
    }

    private void postInit() {
        write(hexToBytes("55 AA 01 11 A5 01 88 18 A7 FE"));
        h.postDelayed(() -> write(hexToBytes("55 AA 01 11 A5 01 18 18 17 FF")), 180);
        h.postDelayed(() -> write(hexToBytes("55 AA 01 11 F1 01 01 1A E0 FE")), 360);
        h.postDelayed(this::startPolling, 600);
    }

    private void startPolling() {
        runOnUiThread(() -> log("Polling start"));
        h.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (gatt == null || chRxWrite == null) return;
                write(hexToBytes("55 AA 04 11 10 02 42 20 1C 00 00 5A FF"));
                write(hexToBytes("55 AA 04 11 10 02 46 FB 58 FB 69 DB FC"));
                h.postDelayed(this, 700);
            }
        }, 700);
    }

    private void lightOnOffOn() {
        setLight(true);
        h.postDelayed(() -> setLight(false), 300);
        h.postDelayed(() -> setLight(true), 600);
    }

    private void setAssist(int level) {
        if (level < 0 || level > 5) return;
        writeA5(0xA4, level);
        log("Assist set -> " + level);
    }

    private void setLight(boolean on) {
        writeA5(0xA6, on ? 1 : 0);
        log("Light set -> " + (on ? "ON" : "OFF"));
    }

    private void setUnit(boolean mph) {
        writeA5Single(0xE0, mph ? 1 : 0);
        log("Unit set -> " + (mph ? "MPH" : "KMH") + " [offset E0]");
    }

    private void setBrightness(int level) {
        if (level < 1 || level > 4) return;
        writeA5Single(0xA7, level);
        log("Brightness set -> " + level + " [offset A7]");
    }

    private void sendNavSequence() {
        log(String.format(Locale.US, "NAV test sequence start (tipo=0x%02X)", navTurnType));
        write(hexToBytes("55 AA 02 11 F1 03 00 01 01 F6 FE"));

        // Valid frames captured from Bikego/display session (left-turn baseline: tipo 0x02).
        String[] frames = new String[]{
                "55 AA 12 11 F1 03 00 02 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 02 FD",
                "55 AA 12 11 F1 03 00 03 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 01 FD",
                "55 AA 12 11 F1 03 00 04 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 00 FD",
                "55 AA 12 11 F1 03 00 05 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 FF FC",
                "55 AA 12 11 F1 03 00 06 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 FE FC",
                "55 AA 12 11 F1 03 00 07 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 FD FC",
                "55 AA 12 11 F1 03 00 08 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 FC FC",
                "55 AA 12 11 F1 03 00 09 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 FB FC",
                "55 AA 12 11 F1 03 00 0A 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 FA FC",
                "55 AA 12 11 F1 03 00 0B 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F9 FC",
                "55 AA 12 11 F1 03 00 0C 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F8 FC",
                "55 AA 12 11 F1 03 00 0D 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F7 FC",
                "55 AA 12 11 F1 03 00 0E 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F6 FC",
                "55 AA 12 11 F1 03 00 0F 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F5 FC",
                "55 AA 12 11 F1 03 00 10 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F4 FC",
                "55 AA 12 11 F1 03 00 11 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F3 FC",
                "55 AA 12 11 F1 03 00 12 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F2 FC",
                "55 AA 12 11 F1 03 00 13 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F1 FC",
                "55 AA 12 11 F1 03 00 14 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 F0 FC",
                "55 AA 12 11 F1 03 00 15 02 45 00 00 02 58 00 00 02 3B 00 00 02 EC 18 00 00 EF FC",
                "55 AA 12 11 F1 03 00 16 02 20 00 00 02 3B 00 00 02 56 00 00 03 B8 16 00 00 4A FD",
                "55 AA 12 11 F1 03 00 17 02 20 00 00 02 3B 00 00 02 56 00 00 03 B8 16 00 00 49 FD"
        };

        int delta = navTurnType - 0x02;
        for (int i = 0; i < frames.length; i++) {
            byte[] f = hexToBytes(frames[i]);
            f[8] = (byte) (navTurnType & 0xFF); // first maneuver type
            // Adjust checksum from known-valid base frame by byte delta.
            int crc = (u(f[f.length - 2]) | (u(f[f.length - 1]) << 8));
            crc = (crc - delta) & 0xFFFF;
            f[f.length - 2] = (byte) (crc & 0xFF);
            f[f.length - 1] = (byte) ((crc >> 8) & 0xFF);
            final byte[] send = f;
            h.postDelayed(() -> write(send), (i + 1) * 120L);
        }
    }

    private void writeA5Single(int offset, int value) {
        byte[] f = new byte[10];
        f[0] = 0x55;
        f[1] = (byte) 0xAA;
        f[2] = 0x01;
        f[3] = 0x11;
        f[4] = (byte) 0xA5;
        f[5] = 0x02;
        f[6] = (byte) offset;
        f[7] = (byte) value;
        int crc = crcLikeBikego(f);
        f[8] = (byte) (crc & 0xFF);
        f[9] = (byte) ((crc >> 8) & 0xFF);
        write(f);
    }

    private void writeA5(int offset, int value) {
        byte[] f = new byte[11];
        f[0] = 0x55;
        f[1] = (byte) 0xAA;
        f[2] = 0x02;
        f[3] = 0x11;
        f[4] = (byte) 0xA5;
        f[5] = 0x02;
        f[6] = (byte) offset;
        f[7] = (byte) value;
        f[8] = 0x00;
        int crc = crcLikeBikego(f);
        f[9] = (byte) (crc & 0xFF);
        f[10] = (byte) ((crc >> 8) & 0xFF);
        write(f);
    }

    @SuppressLint("MissingPermission")
    private void write(byte[] data) {
        if (gatt == null || chRxWrite == null || data == null) return;
        chRxWrite.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        chRxWrite.setValue(data);
        boolean ok = gatt.writeCharacteristic(chRxWrite);
        if (verboseLog) runOnUiThread(() -> log((ok ? "TX " : "TX_FAIL ") + hex(data)));
    }

    private static int u(byte b) {
        return b & 0xFF;
    }

    private static int crcLikeBikego(byte[] frame) {
        int sum = 0;
        for (int i = 2; i < frame.length - 2; i++) sum += u(frame[i]);
        return (0xFFFF ^ (sum & 0xFFFF)) & 0xFFFF;
    }

    private static String hex(byte[] b) {
        if (b == null) return "null";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < b.length; i++) {
            if (i > 0) sb.append(' ');
            sb.append(String.format(Locale.US, "%02X", u(b[i])));
        }
        return sb.toString();
    }

    private static byte[] hexToBytes(String s) {
        String[] p = s.trim().split("\\s+");
        byte[] out = new byte[p.length];
        for (int i = 0; i < p.length; i++) out[i] = (byte) Integer.parseInt(p[i], 16);
        return out;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        h.removeCallbacksAndMessages(null);
        if (gatt != null) {
            try { gatt.disconnect(); } catch (Exception ignored) {}
            try { gatt.close(); } catch (Exception ignored) {}
            gatt = null;
        }
    }

    @Override
    public void onBackPressed() {
        long now = SystemClock.elapsedRealtime();
        if (now - lastBackPressMs > 1800) {
            lastBackPressMs = now;
            Toast.makeText(this, "Pulsa atrás otra vez para salir", Toast.LENGTH_SHORT).show();
            return;
        }
        h.removeCallbacksAndMessages(null);
        if (gatt != null) {
            try { gatt.disconnect(); } catch (Exception ignored) {}
            try { gatt.close(); } catch (Exception ignored) {}
            gatt = null;
        }
        super.onBackPressed();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_PERM) {
            boolean ok = true;
            for (int r : grantResults) if (r != PackageManager.PERMISSION_GRANTED) ok = false;
            if (!ok) log("Permisos BLE denegados");
        }
    }
}
