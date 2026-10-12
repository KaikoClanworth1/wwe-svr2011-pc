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
                + "(account token and password removed); IP addresses blanked (x.x.x.x)\n";
            // (the player's words first: what a reader looks for)
            if (description != null && !description.isEmpty()) info = description + "\n" + info;
            put(zip, "report.txt", info.getBytes(StandardCharsets.UTF_8));
            r.logs = addLogs(zip, new File(gameFolder, "logs"), 16 << 20);
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
            put(zip, folder + f.getName(), redactIps(read(f, max)));
            added++;
        }
        return added;
    }

    // The game's logs: svr2011_<n>.log, rotated at 5 MB into svr2011_<n>.<k>.log
    // (the higher k, the older; up to 20). A freeze can fill several parts with
    // the same fault line, pushing out what came before it, so the newest
    // game's parts all go in, oldest first, with the repeated fault lines left
    // out after the first ones - its first part (the device, the first faults'
    // host pc) always, then the newest parts back to `budget` bytes - and the
    // two games before it (their last part).
    static final String kFault = "Unhandled guest access violation";
    static final int kFaultsKept = 30;

    static int addLogs(ZipOutputStream zip, File dir, int budget) throws IOException {
        File[] files = dir.listFiles((d, n) -> n.startsWith("svr2011_") && n.endsWith(".log"));
        if (files == null || files.length == 0) return 0;
        java.util.Map<String, java.util.List<File>> games = new java.util.HashMap<>();
        for (File f : files) {
            String n = f.getName();
            String game = n.substring(0, n.indexOf('.'));
            games.computeIfAbsent(game, k -> new java.util.ArrayList<>()).add(f);
        }
        // (games newest first, by their newest file)
        java.util.List<String> order = new java.util.ArrayList<>(games.keySet());
        java.util.Map<String, Long> newest = new java.util.HashMap<>();
        for (String g : order) {
            long t = 0;
            for (File f : games.get(g)) t = Math.max(t, f.lastModified());
            newest.put(g, t);
        }
        order.sort((a, b) -> Long.compare(newest.get(b), newest.get(a)));
        int added = 0;
        java.util.List<File> parts = games.get(order.get(0));
        parts.sort((a, b) -> Integer.compare(part(b), part(a)));  // (oldest first: highest k, the plain .log last)
        // (the fault lines before each part: the game's first ones are kept)
        int[] before = new int[parts.size()];
        for (int i = 1; i < parts.size(); i++) before[i] = before[i - 1] + countFaults(parts.get(i - 1));
        byte[][] kept = new byte[parts.size()][];
        long left = budget;
        // The first part, then the newest ones back, while they fit.
        java.util.List<Integer> pick = new java.util.ArrayList<>();
        pick.add(0);
        for (int i = parts.size() - 1; i > 0; i--) pick.add(i);
        for (int i : pick) {
            if (left <= 0) break;
            byte[] b = condense(read(parts.get(i), 64 << 20), new int[] {before[i]});
            if (b.length > left) b = Arrays.copyOfRange(b, (int) (b.length - left), b.length);
            kept[i] = b;
            left -= b.length;
        }
        for (int i = 0; i < parts.size(); i++) {
            if (kept[i] == null) continue;
            put(zip, "logs/" + parts.get(i).getName(), redactIps(kept[i]));
            added++;
        }
        // The games before (their last part: how they ended).
        for (int g = 1; g < order.size() && g <= 2 && left > 0; g++) {
            File last = null;
            for (File f : games.get(order.get(g))) if (part(f) == 0) last = f;
            if (last == null) continue;
            byte[] b = condense(read(last, (int) Math.min(left, 4 << 20)), new int[1]);
            put(zip, "logs/" + last.getName(), redactIps(b));
            left -= b.length;
            added++;
        }
        return added;
    }

    static int countFaults(File f) throws IOException {
        int n = 0;
        try (java.io.BufferedReader r = new java.io.BufferedReader(
                new java.io.InputStreamReader(new FileInputStream(f), StandardCharsets.ISO_8859_1), 1 << 16)) {
            String line;
            while ((line = r.readLine()) != null) if (line.contains(kFault)) n++;
        }
        return n;
    }

    // svr2011_12.3.log -> 3, svr2011_12.log -> 0.
    static int part(File f) {
        String[] p = f.getName().split("\\.");
        if (p.length < 3) return 0;
        try {
            return Integer.parseInt(p[1]);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // The log without the repeated fault lines after the first kFaultsKept
    // (counted over the game's parts in `faults`), saying how many went.
    static byte[] condense(byte[] data, int[] faults) {
        String text = new String(data, StandardCharsets.ISO_8859_1);
        StringBuilder out = new StringBuilder(Math.min(text.length(), 1 << 20));
        int dropped = 0;
        for (String line : text.split("\n", -1)) {
            if (line.contains(kFault) && ++faults[0] > kFaultsKept) {
                dropped++;
                continue;
            }
            if (dropped > 0) {
                out.append("(report: ").append(dropped).append(" more '").append(kFault).append("' lines left out)\n");
                dropped = 0;
            }
            out.append(line).append('\n');
        }
        if (dropped > 0) out.append("(report: ").append(dropped).append(" more '").append(kFault).append("' lines left out)\n");
        return out.toString().getBytes(StandardCharsets.ISO_8859_1);
    }

    private static final java.util.regex.Pattern IPV4 = java.util.regex.Pattern.compile(
        "(?<![\\d.])(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})\\.(\\d{1,3})(?!\\d|\\.\\d)");

    // Every IPv4 address (the P2P lines log the player's public one) becomes
    // x.x.x.x, as the PC launcher's report does. Loopback stays, and so do lines
    // with "ersion" in them (driver / API versions such as 1.3.1.1).
    static byte[] redactIps(byte[] data) {
        String text = new String(data, StandardCharsets.ISO_8859_1);  // (bytes kept as they are)
        StringBuilder out = new StringBuilder(text.length());
        boolean first = true;
        for (String line : text.split("\n", -1)) {
            if (!first) out.append('\n');
            first = false;
            if (line.contains("ersion")) {
                out.append(line);
                continue;
            }
            java.util.regex.Matcher m = IPV4.matcher(line);
            StringBuffer sb = new StringBuffer();
            while (m.find()) {
                boolean ip = true;
                for (int g = 1; g <= 4; g++) ip &= Integer.parseInt(m.group(g)) <= 255;
                m.appendReplacement(sb, ip && !m.group(1).equals("127") ? "x.x.x.x" : m.group());
            }
            m.appendTail(sb);
            out.append(sb);
        }
        return out.toString().getBytes(StandardCharsets.ISO_8859_1);
    }
}
