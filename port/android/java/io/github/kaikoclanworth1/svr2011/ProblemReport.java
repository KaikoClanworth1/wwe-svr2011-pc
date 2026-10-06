// WWE SmackDown vs. Raw 2011 - "Report a problem" on the phone (as the PC
// launcher's, launcher/report.c): one zip in the Download folder with the
// newest game logs, crash reports and svr2011.toml (the account's token and
// password removed), plus report.txt (app version, phone, Android version).

package io.github.kaikoclanworth1.svr2011;

import android.os.Build;
import android.os.Environment;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

final class ProblemReport {
    static final class Result {
        File zip;
        int logs, crashes;
    }

    // Makes the zip; throws if it can't be written.
    static Result make(File gameFolder, String appVersion) throws IOException {
        return make(gameFolder, appVersion, null);
    }

    // ... with the player's description (ReportForm) at the top of report.txt.
    static Result make(File gameFolder, String appVersion, String description) throws IOException {
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        dir.mkdirs();
        Result r = new Result();
        r.zip = new File(dir, "SvR2011-report-" + stamp + ".zip");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(r.zip))) {
            String info = "SvR 2011 port problem report (Android)\n"
                + "app version: " + appVersion + "\n"
                + "phone: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")\n"
                + "Android: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                + "chip: " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : "?") + "\n"
                + "GPU: " + (GpuInfo.known().isEmpty() ? "?" : GpuInfo.known()) + "\n"
                + "made: " + stamp + "\n"
                + "contents: the newest game logs (logs/), crash reports (crashes/) and svr2011.toml "
                + "(account token and password removed)\n";
            // (the player's words first: what a reader looks for)
            if (description != null && !description.isEmpty()) info = description + "\n" + info;
            put(zip, "report.txt", info.getBytes(StandardCharsets.UTF_8));
            r.logs = addNewest(zip, new File(gameFolder, "logs"), "svr2011_", ".log", 3, "logs/", 16 << 20);
            r.crashes = addNewest(zip, new File(gameFolder, "UserData/crashes"), "crash_", ".txt", 3, "crashes/", 1 << 20);
            File toml = new File(gameFolder, "svr2011.toml");
            if (toml.isFile()) {
                StringBuilder out = new StringBuilder();
                for (String line : new String(read(toml, 1 << 20), StandardCharsets.UTF_8).split("\n", -1)) {
                    String t = line.trim();
                    if (t.startsWith("online_token")) out.append("online_token = \"(removed)\"\n");
                    else if (t.startsWith("online_password")) out.append("online_password = \"(removed)\"\n");
                    else out.append(line).append('\n');
                }
                put(zip, "svr2011.toml", out.toString().getBytes(StandardCharsets.UTF_8));
            }
        }
        return r;
    }

    static void put(ZipOutputStream zip, String name, byte[] data) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(data);
        zip.closeEntry();
    }

    // A file's bytes, or its last `max` bytes (the end of a long log: what happened last).
    static byte[] read(File f, int max) throws IOException {
        long length = f.length();
        try (InputStream in = new FileInputStream(f)) {
            if (length > max) {
                long skip = length - max;
                while (skip > 0) skip -= in.skip(skip);
            }
            int n = (int) Math.min(length, max);
            byte[] b = new byte[n];
            int off = 0, k;
            while (off < n && (k = in.read(b, off, n - off)) > 0) off += k;
            return off == n ? b : Arrays.copyOf(b, off);
        }
    }

    static int addNewest(ZipOutputStream zip, File dir, String prefix, String suffix, int want, String folder,
                         int max) throws IOException {
        File[] files = dir.listFiles((d, n) -> n.startsWith(prefix) && n.endsWith(suffix));
        if (files == null) return 0;
        Arrays.sort(files, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        int added = 0;
        for (File f : files) {
            if (added >= want) break;
            put(zip, folder + f.getName(), read(f, max));
            added++;
        }
        return added;
    }
}
