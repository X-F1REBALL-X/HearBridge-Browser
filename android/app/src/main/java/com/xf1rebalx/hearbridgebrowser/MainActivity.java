package com.xf1rebalx.hearbridgebrowser;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.net.Inet4Address;
import java.util.HashSet;
import java.util.Set;
import java.util.regex.Pattern;

/** Start screen: auto-discovery of consoles on the /24, found list, retry, manual IP. */
@SuppressLint("SetTextI18n") // English-only UI for v1
public class MainActivity extends Activity implements Discovery.Listener {
    static final String PREFS = "hb";
    static final String KEY_LAST = "last_host";
    private static final Pattern HOST = Pattern.compile(
            "^((25[0-5]|2[0-4]\\d|1?\\d?\\d)\\.){3}(25[0-5]|2[0-4]\\d|1?\\d?\\d)(:\\d{1,5})?$");

    private final Discovery discovery = new Discovery();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Set<String> shown = new HashSet<>();
    private TextView status, subnet;
    private ProgressBar progress;
    private LinearLayout list;
    private EditText ip;
    private SharedPreferences prefs;
    private boolean autoOpen;
    private boolean scanning;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(R.layout.activity_main);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        status = findViewById(R.id.status);
        subnet = findViewById(R.id.subnet);
        progress = findViewById(R.id.progress);
        list = findViewById(R.id.list);
        ip = findViewById(R.id.ip);
        String last = prefs.getString(KEY_LAST, "");
        ip.setText(last);
        findViewById(R.id.retry).setOnClickListener(v -> scan());
        findViewById(R.id.connect).setOnClickListener(v -> manual());
        ip.setOnEditorActionListener((v, id, e) -> {
            if (id == EditorInfo.IME_ACTION_GO || id == EditorInfo.IME_ACTION_DONE) { manual(); return true; }
            return false;
        });
        // Cold start only: jump straight in when the last console answers.
        autoOpen = saved == null && !last.isEmpty();
        scan();
    }

    @Override
    protected void onDestroy() {
        discovery.cancel();
        super.onDestroy();
    }

    private void scan() {
        shown.clear();
        list.removeAllViews();
        scanning = true;
        progress.setVisibility(View.VISIBLE);
        status.setText(R.string.searching);
        Inet4Address self = Discovery.localAddress(this);
        String prefix = Discovery.prefix24(self);
        subnet.setText(prefix != null ? "Scanning " + prefix + "0/24 on port " + Discovery.PORT
                : "Not on Wi-Fi. Connect to the console's network or enter its IP.");
        discovery.start(prefix, self != null ? self.getHostAddress() : null, prefs.getString(KEY_LAST, ""), this);
    }

    @Override
    public void onFound(Discovery.Console c) {
        if (!shown.add(c.host)) return;
        String last = prefs.getString(KEY_LAST, "");
        boolean isLast = c.host.equals(last);
        if (isLast && autoOpen) {
            autoOpen = false;
            open(c.host);
        }
        TextView t = new TextView(this);
        t.setBackgroundResource(R.drawable.bg_item);
        int p = dp(14);
        t.setPadding(p, dp(12), p, dp(12));
        t.setTextColor(getColor(R.color.fg));
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        t.setText("PS5 · " + c.host + "\nHearBridge Browser v" + c.version + (isLast ? " · last used" : ""));
        t.setOnClickListener(v -> open(c.host));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        if (isLast) list.addView(t, 0, lp); else list.addView(t, lp);
        if (scanning) status.setText("Searching for console… found " + shown.size());
    }

    @Override
    public void onDone(int found) {
        scanning = false;
        autoOpen = false;
        progress.setVisibility(View.GONE);
        int n = shown.size();
        status.setText(n == 0 ? "No console found. Start the payload, then Retry."
                : n == 1 ? "Found 1 console. Tap to open." : "Found " + n + " consoles. Tap one to open.");
    }

    private void manual() {
        final String h = ip.getText().toString().trim();
        if (!HOST.matcher(h).matches()) {
            ip.setError("Enter an IPv4 address, e.g. 192.168.1.50");
            return;
        }
        status.setText("Checking " + h + "…");
        new Thread(() -> {
            final Discovery.Console c = Discovery.probe(h, 2500, 3000);
            main.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (c != null) open(h);
                else status.setText("No HearBridge Browser answered at " + Discovery.baseUrl(h));
            });
        }, "hb-manual").start();
    }

    private void open(String host) {
        discovery.cancel();
        scanning = false;
        progress.setVisibility(View.GONE);
        status.setText("Opened " + host + ". Tap Retry to search again.");
        prefs.edit().putString(KEY_LAST, host).apply();
        ip.setText(host);
        startActivity(new Intent(this, PlayerActivity.class).putExtra(PlayerActivity.EXTRA_HOST, host));
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
