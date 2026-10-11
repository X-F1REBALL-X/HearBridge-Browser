package com.xf1rebalx.hearbridgebrowser;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.util.Base64;
import android.view.WindowManager;
import android.webkit.JavascriptInterface;
import android.webkit.URLUtil;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.HashMap;
import java.util.Map;

/** Full-screen WebView on http://<console>:8096/. */
public class PlayerActivity extends Activity {
    static final String EXTRA_HOST = "host";
    private static final int REQ_STORAGE = 7;
    private static final long POLL_MS = 1500;

    /* Tapping an <a download href="blob:..."> is caught here, the blob is read in
       slices and streamed to the Java bridge, which writes it to Downloads. */
    private static final String INJECT =
            "(function(){if(window.__hbDl)return;window.__hbDl=1;var CH=393216;" +
            "function save(href,name,type){fetch(href).then(function(r){return r.blob();}).then(function(b){" +
            "var id=HBAndroid.begin(name||'hearbridge-export',type||b.type||'application/octet-stream',b.size);" +
            "if(id<0)return;var off=0;function next(){if(off>=b.size){HBAndroid.end(id);return;}" +
            "var fr=new FileReader();fr.onload=function(){var s=fr.result;HBAndroid.chunk(id,s.substring(s.indexOf(',')+1));off+=CH;next();};" +
            "fr.onerror=function(){HBAndroid.fail(id,String(fr.error));};fr.readAsDataURL(b.slice(off,off+CH));}next();" +
            "}).catch(function(e){HBAndroid.fail(-1,String(e));});}" +
            "window.__hbSave=save;" +
            "document.addEventListener('click',function(e){var a=e.target&&e.target.closest?e.target.closest('a[download]'):null;" +
            "if(!a)return;var h=a.href||'';if(h.indexOf('blob:')!==0&&h.indexOf('data:')!==0)return;" +
            "e.preventDefault();e.stopPropagation();save(h,a.getAttribute('download'),a.getAttribute('type')||'');},true);})();";

    private static final String IS_PLAYING =
            "(function(){try{return typeof playing!=='undefined'&&!!playing;}catch(e){return false;}})()";

    private final Handler main = new Handler(Looper.getMainLooper());
    private WebView web;
    private Uri consoleUri;
    private boolean keepOn;
    private PendingFile waitingForPermission;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (web == null) return;
            web.evaluateJavascript(IS_PLAYING, v -> setKeepScreenOn("true".equals(v)));
            main.postDelayed(this, POLL_MS);
        }
    };

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        String host = getIntent().getStringExtra(EXTRA_HOST);
        if (host == null) { finish(); return; }
        String base = Discovery.baseUrl(host);
        consoleUri = Uri.parse(base);

        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            WebView.setWebContentsDebuggingEnabled(true);
        }
        web = new WebView(this);
        web.setBackgroundColor(getColor(R.color.bg));
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);           // the player is all JS
        s.setDomStorageEnabled(true);           // settings saved per device (localStorage)
        s.setMediaPlaybackRequiresUserGesture(false);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setUserAgentString(s.getUserAgentString() + " HearBridgeAndroid/1.0.0");

        web.addJavascriptInterface(new Bridge(), "HBAndroid");
        web.setWebChromeClient(new WebChromeClient()); // default alert/confirm dialogs
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                return route(r.getUrl());
            }

            @Override
            public void onPageStarted(WebView v, String url, Bitmap favicon) {
                setKeepScreenOn(false);
            }

            @Override
            public void onPageFinished(WebView v, String url) {
                v.evaluateJavascript(INJECT, null);
            }

            @Override
            public void onReceivedError(WebView v, WebResourceRequest r, WebResourceError e) {
                if (r.isForMainFrame()) {
                    Toast.makeText(PlayerActivity.this, "Console not reachable at " + consoleUri.getAuthority(),
                            Toast.LENGTH_LONG).show();
                    finish();
                }
            }
        });
        web.setDownloadListener((url, ua, cd, mime, len) -> {
            if (url.startsWith("blob:") || url.startsWith("data:")) {
                String name = URLUtil.guessFileName(url, cd, mime);
                web.evaluateJavascript("window.__hbSave&&__hbSave(" + JSONObject.quote(url) + ","
                        + JSONObject.quote(name) + "," + JSONObject.quote(mime == null ? "" : mime) + ")", null);
            } else {
                openExternal(Uri.parse(url));
            }
        });

        if (saved != null) web.restoreState(saved);
        else web.loadUrl(base);
    }

    @Override
    protected void onSaveInstanceState(Bundle out) {
        super.onSaveInstanceState(out);
        if (web != null) web.saveState(out);
    }

    @Override
    protected void onStart() {
        super.onStart();
        main.removeCallbacks(poll);
        main.postDelayed(poll, POLL_MS);
    }

    @Override
    protected void onStop() {
        main.removeCallbacks(poll);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        main.removeCallbacks(poll);
        if (web != null) {
            web.destroy();
            web = null;
        }
        super.onDestroy();
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        if (web != null && web.canGoBack()) web.goBack();
        else super.onBackPressed();
    }

    private void setKeepScreenOn(boolean on) {
        if (on == keepOn) return;
        keepOn = on;
        if (on) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    /** true = handled outside the WebView. Only the console's own origin stays inside. */
    private boolean route(Uri u) {
        String scheme = u.getScheme() == null ? "" : u.getScheme();
        boolean same = scheme.equals("http") && u.getHost() != null && u.getHost().equals(consoleUri.getHost())
                && u.getPort() == consoleUri.getPort();
        if (same) return false;
        openExternal(u);
        return true;
    }

    private void openExternal(Uri u) {
        String scheme = u.getScheme() == null ? "" : u.getScheme();
        if (!scheme.equals("http") && !scheme.equals("https") && !scheme.equals("mailto")) return;
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, u).addCategory(Intent.CATEGORY_BROWSABLE));
        } catch (ActivityNotFoundException e) {
            toast("No app can open " + u);
        }
    }

    private void toast(final String msg) {
        runOnUiThread(() -> Toast.makeText(this, msg, Toast.LENGTH_LONG).show());
    }

    /* ---------------- downloads ---------------- */

    private static final class PendingFile {
        final File tmp;
        final String name, mime;
        OutputStream out;
        PendingFile(File tmp, String name, String mime) { this.tmp = tmp; this.name = name; this.mime = mime; }
    }

    static String safeName(String n) {
        n = n == null ? "" : n.replaceAll("[\\\\/:*?\"<>|\\p{Cntrl}]", "_").trim();
        while (n.startsWith(".")) n = n.substring(1);
        if (n.isEmpty()) n = "hearbridge-export";
        if (n.length() > 120) n = n.substring(n.length() - 120);
        return n;
    }

    /** Runs on the WebView's JavaBridge thread (one call at a time). */
    final class Bridge {
        private final Map<Integer, PendingFile> files = new HashMap<>();
        private int nextId = 1;

        @JavascriptInterface
        public int begin(String name, String mime, double size) {
            try {
                File dir = new File(getCacheDir(), "dl");
                if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("no cache dir");
                int id = nextId++;
                PendingFile f = new PendingFile(new File(dir, id + ".part"), safeName(name),
                        mime == null || mime.isEmpty() ? "application/octet-stream" : mime);
                f.out = new FileOutputStream(f.tmp);
                files.put(id, f);
                toast("Saving " + f.name + "…");
                return id;
            } catch (IOException e) {
                toast("Download failed: " + e.getMessage());
                return -1;
            }
        }

        @JavascriptInterface
        public void chunk(int id, String b64) {
            PendingFile f = files.get(id);
            if (f == null) return;
            try {
                f.out.write(Base64.decode(b64, Base64.DEFAULT));
            } catch (Exception e) {
                drop(id, e.getMessage());
            }
        }

        @JavascriptInterface
        public void end(int id) {
            PendingFile f = files.remove(id);
            if (f == null) return;
            try {
                f.out.close();
                publishOrAsk(f);
            } catch (IOException e) {
                //noinspection ResultOfMethodCallIgnored
                f.tmp.delete();
                toast("Download failed: " + e.getMessage());
            }
        }

        @JavascriptInterface
        public void fail(int id, String msg) {
            drop(id, msg);
        }

        private void drop(int id, String msg) {
            PendingFile f = files.remove(id);
            if (f != null) {
                try { f.out.close(); } catch (IOException ignored) { }
                //noinspection ResultOfMethodCallIgnored
                f.tmp.delete();
            }
            toast("Download failed: " + msg);
        }
    }

    private void publishOrAsk(final PendingFile f) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q
                && checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            runOnUiThread(() -> {
                waitingForPermission = f;
                requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_STORAGE);
            });
            return;
        }
        publish(f);
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] res) {
        if (req != REQ_STORAGE || waitingForPermission == null) return;
        final PendingFile f = waitingForPermission;
        waitingForPermission = null;
        if (res.length > 0 && res[0] == PackageManager.PERMISSION_GRANTED) {
            new Thread(() -> publish(f), "hb-save").start();
        } else {
            //noinspection ResultOfMethodCallIgnored
            f.tmp.delete();
            toast("Storage permission is needed to save to Downloads.");
        }
    }

    private void publish(PendingFile f) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) publishMediaStore(f);
            else publishLegacy(f);
            toast("Saved to Downloads: " + f.name);
        } catch (Exception e) {
            toast("Could not save " + f.name + ": " + e.getMessage());
        } finally {
            //noinspection ResultOfMethodCallIgnored
            f.tmp.delete();
        }
    }

    @SuppressLint("NewApi")
    private void publishMediaStore(PendingFile f) throws IOException {
        ContentResolver cr = getContentResolver();
        ContentValues v = new ContentValues();
        v.put(MediaStore.MediaColumns.DISPLAY_NAME, f.name);
        v.put(MediaStore.MediaColumns.MIME_TYPE, f.mime);
        v.put(MediaStore.MediaColumns.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS);
        v.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri uri = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
        if (uri == null) throw new IOException("MediaStore insert failed");
        try (InputStream in = new FileInputStream(f.tmp); OutputStream out = cr.openOutputStream(uri)) {
            if (out == null) throw new IOException("cannot open output");
            copy(in, out);
        } catch (IOException e) {
            cr.delete(uri, null, null);
            throw e;
        }
        v.clear();
        v.put(MediaStore.MediaColumns.IS_PENDING, 0);
        cr.update(uri, v, null, null);
    }

    @SuppressWarnings("deprecation")
    private void publishLegacy(PendingFile f) throws IOException {
        File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("no Downloads folder");
        String base = f.name, ext = "";
        int dot = base.lastIndexOf('.');
        if (dot > 0) { ext = base.substring(dot); base = base.substring(0, dot); }
        File dst = new File(dir, f.name);
        for (int i = 1; dst.exists(); i++) dst = new File(dir, base + " (" + i + ")" + ext);
        try (InputStream in = new FileInputStream(f.tmp); OutputStream out = new FileOutputStream(dst)) {
            copy(in, out);
        }
        MediaScannerConnection.scanFile(this, new String[]{dst.getAbsolutePath()}, new String[]{f.mime}, null);
    }

    private static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buf = new byte[64 * 1024];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
    }
}
