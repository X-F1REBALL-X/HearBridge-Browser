package com.xf1rebalx.hearbridgebrowser;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Handler;
import android.os.Looper;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.URL;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Finds HearBridge Browser consoles: GET http://<ip>:8096/status with "app":"HearBridge Browser". */
final class Discovery {
    static final int PORT = 8096;
    static final String APP = "HearBridge Browser";

    static final class Console {
        final String host;     // "ip" or "ip:port"
        final String version;
        Console(String host, String version) { this.host = host; this.version = version; }
    }

    interface Listener {
        void onFound(Console c);
        void onDone(int found);
    }

    private final Handler main = new Handler(Looper.getMainLooper());
    private ExecutorService pool;
    private int generation;

    /** "host" or "host:port" -> base URL ending in '/'. */
    static String baseUrl(String host) {
        return "http://" + (host.indexOf(':') >= 0 ? host : host + ":" + PORT) + "/";
    }

    /** Blocking probe. Returns the console or null. */
    static Console probe(String host, int connectMs, int readMs) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(baseUrl(host) + "status").openConnection();
            c.setConnectTimeout(connectMs);
            c.setReadTimeout(readMs);
            c.setUseCaches(false);
            c.setInstanceFollowRedirects(false);
            if (c.getResponseCode() != 200) return null;
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            try (InputStream in = c.getInputStream()) {
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) {
                    bo.write(buf, 0, n);
                    if (bo.size() > 64 * 1024) return null;
                }
            }
            JSONObject j = new JSONObject(bo.toString("UTF-8"));
            if (!APP.equals(j.optString("app"))) return null;
            return new Console(host, j.optString("version", "?"));
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    /** The phone's own LAN IPv4 address (Wi-Fi/Ethernet preferred), or null. */
    @SuppressWarnings("deprecation") // getAllNetworks: fine for a one-shot LAN lookup
    static Inet4Address localAddress(Context ctx) {
        ConnectivityManager cm = (ConnectivityManager) ctx.getSystemService(Context.CONNECTIVITY_SERVICE);
        if (cm != null) {
            Inet4Address fallback = null;
            for (Network n : cm.getAllNetworks()) {
                LinkProperties lp = cm.getLinkProperties(n);
                NetworkCapabilities nc = cm.getNetworkCapabilities(n);
                if (lp == null) continue;
                boolean lan = nc != null && (nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                        || nc.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET));
                for (LinkAddress la : lp.getLinkAddresses()) {
                    InetAddress a = la.getAddress();
                    if (a instanceof Inet4Address && !a.isLoopbackAddress() && a.isSiteLocalAddress()) {
                        if (lan) return (Inet4Address) a;
                        if (fallback == null) fallback = (Inet4Address) a;
                    }
                }
            }
            if (fallback != null) return fallback;
        }
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress a = ia.getAddress();
                    if (a instanceof Inet4Address && a.isSiteLocalAddress()) return (Inet4Address) a;
                }
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    /** "192.168.1." for the phone's /24, or null when not on a LAN. */
    static String prefix24(Inet4Address a) {
        if (a == null) return null;
        byte[] b = a.getAddress();
        return (b[0] & 255) + "." + (b[1] & 255) + "." + (b[2] & 255) + ".";
    }

    /** Probe lastHost first, then every host of the /24 (prefix may be null). Results on the main thread. */
    void start(final String prefix, final String self, final String lastHost, final Listener l) {
        cancel();
        final int gen = ++generation;
        final ExecutorService p = Executors.newFixedThreadPool(48);
        pool = p;
        final List<String> hosts = new ArrayList<>();
        if (lastHost != null && !lastHost.isEmpty()) hosts.add(lastHost);
        if (prefix != null) {
            for (int i = 1; i <= 254; i++) {
                String h = prefix + i;
                if (!h.equals(self) && !h.equals(lastHost)) hosts.add(h);
            }
        }
        final AtomicInteger left = new AtomicInteger(hosts.size());
        final AtomicInteger found = new AtomicInteger();
        if (hosts.isEmpty()) {
            main.post(() -> { if (gen == generation) l.onDone(0); });
            p.shutdown();
            return;
        }
        for (final String h : hosts) {
            p.execute(() -> {
                Console c = probe(h, 700, 1500);
                if (c != null) {
                    found.incrementAndGet();
                    main.post(() -> { if (gen == generation) l.onFound(c); });
                }
                if (left.decrementAndGet() == 0) {
                    main.post(() -> { if (gen == generation) l.onDone(found.get()); });
                }
            });
        }
        p.shutdown();
    }

    void cancel() {
        generation++;
        if (pool != null) {
            pool.shutdownNow();
            try { pool.awaitTermination(10, TimeUnit.MILLISECONDS); } catch (InterruptedException ignored) { }
            pool = null;
        }
    }
}
