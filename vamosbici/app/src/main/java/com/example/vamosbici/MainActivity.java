package com.example.vamosbici;

import android.app.Activity;
import android.os.Bundle;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.TextView;

import org.w3c.dom.Document;
import org.w3c.dom.NodeList;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import javax.xml.parsers.DocumentBuilderFactory;

public class MainActivity extends Activity {
    private EditText etFrom;
    private EditText etTo;
    private Button btnCalc;
    private TextView txtStatus;
    private ListView listSteps;
    private ArrayAdapter<String> stepsAdapter;
    private final List<String> rows = new ArrayList<>();

    // Minimal GPX-like sample with turn hints style fields.
    private static final String SAMPLE_GPX =
            "<gpx><trk><name>demo</name><trkseg>" +
                    "<trkpt lat='41.3851' lon='2.1734'><extensions><turn>STRAIGHT</turn><distance>5800</distance></extensions></trkpt>" +
                    "<trkpt lat='41.3900' lon='2.1700'><extensions><turn>LEFT</turn><distance>52</distance></extensions></trkpt>" +
                    "<trkpt lat='41.3920' lon='2.1660'><extensions><turn>LEFT</turn><distance>32</distance></extensions></trkpt>" +
                    "<trkpt lat='41.3970' lon='2.1600'><extensions><turn>ARRIVE</turn><distance>0</distance></extensions></trkpt>" +
                    "</trkseg></trk></gpx>";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        etFrom = findViewById(R.id.etFrom);
        etTo = findViewById(R.id.etTo);
        btnCalc = findViewById(R.id.btnCalc);
        txtStatus = findViewById(R.id.txtStatus);
        listSteps = findViewById(R.id.listSteps);

        stepsAdapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, rows);
        listSteps.setAdapter(stepsAdapter);

        etFrom.setText("41.3851,2.1734");
        etTo.setText("41.3970,2.1600");

        btnCalc.setOnClickListener(v -> runOfflineDemo());
    }

    private void runOfflineDemo() {
        String from = etFrom.getText().toString().trim();
        String to = etTo.getText().toString().trim();
        txtStatus.setText("Estado: calculando offline demo...");

        List<NavStep> steps = parseStepsFromGpx(SAMPLE_GPX);
        rows.clear();
        rows.add(String.format(Locale.US, "from=%s", from));
        rows.add(String.format(Locale.US, "to=%s", to));
        rows.add("---");
        for (int i = 0; i < steps.size(); i++) {
            NavStep s = steps.get(i);
            rows.add(String.format(Locale.US, "%d) %s - %dm", i + 1, s.turn, s.distanceM));
        }
        stepsAdapter.notifyDataSetChanged();
        txtStatus.setText("Estado: OK (pasos=" + steps.size() + ")");
    }

    private static List<NavStep> parseStepsFromGpx(String gpx) {
        List<NavStep> out = new ArrayList<>();
        try {
            DocumentBuilderFactory dbf = DocumentBuilderFactory.newInstance();
            Document doc = dbf.newDocumentBuilder().parse(new ByteArrayInputStream(gpx.getBytes(StandardCharsets.UTF_8)));
            NodeList turns = doc.getElementsByTagName("turn");
            NodeList dists = doc.getElementsByTagName("distance");
            int n = Math.min(turns.getLength(), dists.getLength());
            for (int i = 0; i < n; i++) {
                String t = turns.item(i).getTextContent().trim();
                int d = Integer.parseInt(dists.item(i).getTextContent().trim());
                out.add(new NavStep(t, d));
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    private static class NavStep {
        final String turn;
        final int distanceM;

        NavStep(String turn, int distanceM) {
            this.turn = turn;
            this.distanceM = distanceM;
        }
    }
}
