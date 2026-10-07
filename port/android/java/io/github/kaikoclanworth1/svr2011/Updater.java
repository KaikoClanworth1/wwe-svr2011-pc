// WWE SmackDown vs. Raw 2011 - updates on the phone (as the PC launcher's
// updater.c checks them): the newest release of the GitHub repository; its
// APK (SvR2011-Android-v<version>.apk, or the one inside the PC zip) is
// downloaded and handed to Android's package installer, which asks the
// player and installs it over this app (same signing key: the saves and the
// game folder stay). The release's SvR2011-Mods-v<version>.zip (Bundled
// Mods\*.svrmod) is BundledMods' download.

package io.github.kaikoclanworth1.svr2011;

import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class Updater {
    static final String kRepo = "KaikoClanworth1/wwe-svr2011-pc";
    static final String kAgent = "SvR2011-Android-Launcher";
    static final String kInstallAction = "io.github.kaikoclanworth1.svr2011.INSTALL_STATUS";

    static final class Release {
        String version, page, assetUrl, assetName;
        long assetSize;
        boolean prerelease, zip;  // zip: the APK is inside the PC zip
        String modsUrl, modsName;  // the bundled mods (SvR2011-Mods-v<version>.zip), if it has them
        long modsSize;
    }

    interface Progress { void at(long done, long total); }

    // a.b.c newer than b.c.d (numbers compared dot by dot)?
    static boolean newer(String a, String b) {
        String[] x = a.split("\\."), y = b.split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? num(x[i]) : 0, q = i < y.length ? num(y[i]) : 0;
            if (p != q) return p > q;
        }
        return false;
    }

    static int num(String s) {
        int n = 0;
        for (char c : s.toCharArray()) {
            if (c < '0' || c > '9') break;
            n = n * 10 + (c - '0');
        }
        return n;
    }

    static HttpURLConnection connect(String url) throws IOException {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        c.setRequestProperty("User-Agent", kAgent);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        c.setConnectTimeout(15000);
        c.setReadTimeout(30000);
        c.setInstanceFollowRedirects(true);
        return c;
    }

    // The newest release (pre-releases included, as the PC launcher does).
    static Release latest() throws IOException { return release(null); }

    // The release of `version` (tag v<version>) when it's among the newest, else the newest.
    static Release release(String version) throws IOException {
        HttpURLConnection c = connect("https://api.github.com/repos/" + kRepo + "/releases?per_page=10");
        int code = c.getResponseCode();
        if (code == 403) throw new IOException("GitHub limits how often it can be asked; try again in an hour");
        if (code != 200) throw new IOException("GitHub answered " + code);
        String body;
        try (InputStream in = c.getInputStream()) {
            java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
            FileOps.copy(in, b);
            body = b.toString("UTF-8");
        }
        try {
            JSONArray list = new JSONArray(body);
            if (list.length() == 0) return null;
            JSONObject r = list.getJSONObject(0);
            for (int i = 0; version != null && i < list.length(); i++) {
                String t = list.getJSONObject(i).optString("tag_name");
                if (t.equals("v" + version) || t.equals(version)) r = list.getJSONObject(i);
            }
            Release rel = new Release();
            String tag = r.getString("tag_name");
            rel.version = tag.startsWith("v") ? tag.substring(1) : tag;
            rel.page = r.optString("html_url");
            rel.prerelease = r.optBoolean("prerelease");
            JSONArray assets = r.getJSONArray("assets");
            String prefix = "https://github.com/" + kRepo + "/releases/download/";
            for (int pass = 0; pass < 2 && rel.assetUrl == null; pass++) {
                for (int i = 0; i < assets.length(); i++) {
                    JSONObject a = assets.getJSONObject(i);
                    String name = a.getString("name"), url = a.getString("browser_download_url");
                    boolean match = pass == 0 ? name.startsWith("SvR2011-Android-") && name.endsWith(".apk")
                                              : name.startsWith("SvR2011-PC-") && name.endsWith(".zip");
                    if (match && url.startsWith(prefix)) {
                        rel.assetUrl = url;
                        rel.assetName = name;
                        rel.assetSize = a.optLong("size", -1);
                        rel.zip = pass == 1;
                        break;
                    }
                }
            }
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                String name = a.getString("name"), url = a.getString("browser_download_url");
                if (name.startsWith("SvR2011-Mods-") && name.endsWith(".zip") && url.startsWith(prefix)) {
                    rel.modsUrl = url;
                    rel.modsName = name;
                    rel.modsSize = a.optLong("size", -1);
                }
            }
            return rel;
        } catch (org.json.JSONException e) {
            throw new IOException("GitHub's answer can't be read");
        }
    }

    // Downloads the release's APK into the cache (from the zip if it's inside one).
    static File download(Context ctx, Release rel, Progress progress, java.util.concurrent.atomic.AtomicBoolean cancel)
        throws IOException {
        File dir = new File(ctx.getCacheDir(), "update");
        FileOps.deleteTree(dir);
        dir.mkdirs();
        File apk = new File(dir, "SvR2011.apk");
        HttpURLConnection c = connect(rel.assetUrl);
        if (c.getResponseCode() != 200) throw new IOException("the download answered " + c.getResponseCode());
        long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : rel.assetSize;
        try (InputStream raw = c.getInputStream()) {
            CountingStream in = new CountingStream(raw, total, progress, cancel);
            if (rel.zip) {
                ZipInputStream zip = new ZipInputStream(in);
                ZipEntry e;
                boolean found = false;
                while ((e = zip.getNextEntry()) != null) {
                    if (e.getName().replace('\\', '/').equalsIgnoreCase("Android/SvR2011.apk")) {
                        try (OutputStream out = new FileOutputStream(apk)) {
                            FileOps.copy(zip, out);
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) throw new IOException("this release has no Android app");
            } else {
                try (OutputStream out = new FileOutputStream(apk)) {
                    FileOps.copy(in, out);
                }
            }
        }
        if (cancel.get()) throw new IOException("stopped");
        return apk;
    }

    // Hands the APK to the package installer; its answers come to the
    // launcher as kInstallAction intents (the player's confirmation first).
    static void install(Context ctx, File apk) throws IOException {
        PackageInstaller pi = ctx.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(
            PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(ctx.getPackageName());
        int id = pi.createSession(params);
        try (PackageInstaller.Session s = pi.openSession(id)) {
            try (InputStream in = new FileInputStream(apk); OutputStream out = s.openWrite("base.apk", 0, apk.length())) {
                FileOps.copy(in, out);
                s.fsync(out);
            }
            Intent status = new Intent(ctx, LauncherActivity.class).setAction(kInstallAction);
            PendingIntent pending = PendingIntent.getActivity(ctx, id, status,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            s.commit(pending.getIntentSender());
        }
    }

    // An input stream reporting progress (and stopping when asked).
    static final class CountingStream extends java.io.FilterInputStream {
        final long total;
        final Progress progress;
        final java.util.concurrent.atomic.AtomicBoolean cancel;
        long done, last;

        CountingStream(InputStream in, long total, Progress progress, java.util.concurrent.atomic.AtomicBoolean cancel) {
            super(in);
            this.total = total;
            this.progress = progress;
            this.cancel = cancel;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            if (cancel.get()) throw new IOException("stopped");
            int n = super.read(b, off, len);
            if (n > 0) {
                done += n;
                if (done - last > (1 << 20)) {
                    last = done;
                    progress.at(done, total);
                }
            }
            return n;
        }

        @Override
        public int read() throws IOException {
            int c = super.read();
            if (c >= 0) done++;
            return c;
        }
    }
}
