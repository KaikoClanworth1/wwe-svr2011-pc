// WWE SmackDown vs. Raw 2011 - the mods that come with the port, on the phone
// (as the PC launcher's mods_install_bundled, launcher/mods_tab.c). The APK
// has no room for them: the release's SvR2011-Mods-v<version>.zip (Bundled
// Mods\*.svrmod) is downloaded from GitHub - offered after a disc install,
// after an app update, and by the Mods tab's "Get bundled mods" - or read from
// a copy in the games or Download folder. Its size is shown first (and mobile
// data said), and it can be stopped.
//
// The rules, as the PC's: Mods/.bundled keeps "name|size|crc<hex>" of the
// bundles installed, so each installs once and again only when the shipped
// file changes (one the player removed stays removed); a bundle installed for
// the first time comes switched off ("disabled"), except a match type; a new
// version keeps the player's on / off and never replaces a newer copy. Lines
// the PC launcher wrote (name|size|file time, the folder came in the PC's game
// package) count as installed when the name and size match. Retired bundles
// are switched off once.

package io.github.kaikoclanworth1.svr2011;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.os.Environment;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class BundledMods {
    // (the app version whose bundles were installed, or offered and put off)
    static final String kPref = "bundled_mods_version";
    // Bundles no longer shipped: switched off once (the player may switch them
    // back on). hurricane.svrmod (2.0.5): The Hurricane is the playable manager
    // on the M tile already.
    static final String[][] kRetired = { {"hurricane.svrmod", "Superstars/the_hurricane"} };

    private static boolean busy_;

    static File modsFolder() { return new File(InstallActivity.gameFolder(), "Mods"); }

    // A copy the player put on the phone (SvR2011-Mods*.zip), the newest.
    static File findLocal() {
        File root = Environment.getExternalStorageDirectory();
        File best = null;
        for (File dir : new File[] {new File(root, "games"), new File(root, Environment.DIRECTORY_DOWNLOADS)}) {
            File[] list = dir.listFiles(f -> f.isFile() && f.getName().startsWith("SvR2011-Mods")
                && f.getName().toLowerCase().endsWith(".zip"));
            if (list != null) for (File f : list) if (best == null || f.lastModified() > best.lastModified()) best = f;
        }
        return best;
    }

    // At the launcher's start: once for each app version (an update, or the
    // first start of an app that didn't do this yet).
    static void atStart(LauncherActivity a) {
        if (!InstallActivity.installed()) return;
        String last = a.getPreferences(Context.MODE_PRIVATE).getString(kPref, null);
        if (!a.versionName().equals(last)) offer(a, false, null);
    }

    static void remember(LauncherActivity a) {
        a.getPreferences(Context.MODE_PRIVATE).edit().putString(kPref, a.versionName()).apply();
    }

    // Asks to get them (asked: the player pressed the button - say why not,
    // when not); `after` runs on the UI thread when they're in.
    static void offer(LauncherActivity a, boolean asked, Runnable after) {
        if (busy_) {
            if (asked) a.status("The bundled mods are being installed already.");
            return;
        }
        if (GameActivity.running) {
            if (asked) a.status("Close the game first.");
            return;
        }
        File local = findLocal();
        if (local != null) {
            ask(a, "Install the mods that come with the port from " + local.getAbsolutePath() + " ("
                + FileOps.human(local.length()) + ")? " + rules(), "Install", asked, () -> run(a, null, local, after));
            return;
        }
        if (asked) a.status("Looking for the bundled mods…");
        final Updater.Release[] rel = new Updater.Release[1];
        final String[] err = new String[1];
        a.background(() -> {
            try {
                rel[0] = Updater.release(a.versionName());
            } catch (Exception e) {
                err[0] = e.getMessage();
            }
        }, () -> {
            if (err[0] != null) {
                if (asked) a.status("Couldn't look for the bundled mods: " + err[0] + ".");
                return;
            }
            if (rel[0] == null || rel[0].modsUrl == null) {
                if (asked) a.status("The release has no bundled mods to download.");
                return;
            }
            Updater.Release r = rel[0];
            String size = r.modsSize > 0 ? FileOps.human(r.modsSize) : "size unknown";
            boolean metered = false;
            try {
                ConnectivityManager cm = (ConnectivityManager) a.getSystemService(Context.CONNECTIVITY_SERVICE);
                metered = cm != null && cm.isActiveNetworkMetered();
            } catch (Exception ignored) {
            }
            ask(a, "Download the mods that come with version " + r.version + " (" + size + ")? " + rules()
                + (metered ? "\n\nYou're on mobile data: this downloads " + size + "." : ""),
                metered ? "Download on mobile data" : "Download", asked, () -> run(a, r, null, after));
        });
    }

    static String rules() {
        return "New ones come switched off (match types on): switch them on in the Mods tab. Mods you have keep "
            + "their on / off, and ones you removed stay removed.";
    }

    static void ask(LauncherActivity a, String message, String action, boolean asked, Runnable onYes) {
        new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Bundled mods")
            .setMessage(message)
            .setPositiveButton(action, (d, w) -> onYes.run())
            .setNegativeButton("Not now", (d, w) -> {
                if (!asked) {
                    remember(a);
                    a.status("The bundled mods can be got later: Mods tab → Get bundled mods.");
                }
            })
            .setCancelable(false)
            .show();
    }

    // Downloads (rel) or reads (local) the zip and installs it, with a dialog
    // showing how far it is and a Cancel.
    static void run(LauncherActivity a, Updater.Release rel, File local, Runnable after) {
        busy_ = true;
        AtomicBoolean cancel = new AtomicBoolean();
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(a.dp(20), a.dp(16), a.dp(20), a.dp(8));
        TextView text = a.text(rel != null ? "Downloading…" : "Installing…", 15, LauncherActivity.kText);
        ProgressBar bar = new ProgressBar(a, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setProgressTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        box.addView(text);
        box.addView(bar, a.fullWidth(10));
        AlertDialog dialog = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Bundled mods")
            .setView(box)
            .setNegativeButton("Cancel", (d, w) -> {
                cancel.set(true);
                a.status("Stopping the bundled mods…");
            })
            .setCancelable(false)
            .show();
        final String[] result = new String[1];
        final boolean[] done = new boolean[1];
        a.background(() -> {
            File tmp = new File(a.getCacheDir(), "bundled_mods.zip");
            try {
                File zip = local;
                if (rel != null) {
                    if (rel.modsSize > 0 && a.getCacheDir().getUsableSpace() < rel.modsSize + (64L << 20))
                        throw new IOException("not enough free space (" + FileOps.human(rel.modsSize + (64L << 20)) + " needed)");
                    download(rel, tmp, (got, total) -> a.runOnUiThread(() -> {
                        if (total > 0) bar.setProgress((int) (got * 1000 / total));
                        text.setText("Downloading… " + FileOps.human(got) + (total > 0 ? " of " + FileOps.human(total) : ""));
                    }), cancel);
                    zip = tmp;
                }
                result[0] = installAll(a, zip, cancel, (i, n) -> a.runOnUiThread(() -> {
                    bar.setProgress(n > 0 ? (int) (i * 1000 / n) : 0);
                    text.setText("Installing the mods… " + i + " of " + n);
                }));
                done[0] = !cancel.get();
            } catch (Exception e) {
                result[0] = cancel.get() ? "Stopped: the bundled mods not installed yet are got next time."
                                         : "The bundled mods couldn't be got: " + e.getMessage() + ".";
            } finally {
                tmp.delete();
            }
        }, () -> {
            busy_ = false;
            if (dialog.isShowing()) dialog.dismiss();
            if (done[0]) remember(a);
            a.status(result[0]);
            if (after != null) after.run();
        });
    }

    static void download(Updater.Release rel, File to, Updater.Progress progress, AtomicBoolean cancel) throws IOException {
        HttpURLConnection c = Updater.connect(rel.modsUrl);
        if (c.getResponseCode() != 200) throw new IOException("the download answered " + c.getResponseCode());
        long total = c.getContentLengthLong() > 0 ? c.getContentLengthLong() : rel.modsSize;
        try (InputStream in = new Updater.CountingStream(c.getInputStream(), total, progress, cancel);
             OutputStream out = new FileOutputStream(to)) {
            FileOps.copy(in, out);
        }
        if (cancel.get()) throw new IOException("stopped");
    }

    interface Step { void at(int i, int n); }

    // Installs the zip's *.svrmod by the rules above; returns what it did.
    static String installAll(Context ctx, File zipFile, AtomicBoolean cancel, Step step) throws IOException {
        File mods = modsFolder();
        mods.mkdirs();
        File listFile = new File(mods, ".bundled");
        List<String> known = new ArrayList<>();
        if (listFile.isFile()) {
            byte[] b = new byte[(int) listFile.length()];
            try (InputStream in = new FileInputStream(listFile)) {
                int at = 0, n;
                while (at < b.length && (n = in.read(b, at, b.length - at)) > 0) at += n;
            }
            for (String l : new String(b, StandardCharsets.UTF_8).replace("﻿", "").split("\r?\n"))
                if (!l.trim().isEmpty()) known.add(l.trim());
        }
        boolean changed = false;
        for (String[] r : kRetired) {
            boolean had = false;
            for (int i = known.size() - 1; i >= 0; i--)
                if (known.get(i).startsWith(r[0] + "|")) {
                    known.remove(i);
                    had = true;
                }
            if (!had) continue;
            File folder = new File(mods, r[1]);
            if (folder.isDirectory()) new FileOutputStream(new File(folder, "disabled")).close();
            changed = true;
        }
        int added = 0, updated = 0, kept = 0, failed = 0;
        StringBuilder bad = new StringBuilder();
        try (ZipFile zip = new ZipFile(zipFile)) {
            List<ZipEntry> all = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                if (!z.isDirectory() && z.getName().toLowerCase().endsWith(".svrmod")) all.add(z);
            }
            if (all.isEmpty()) throw new IOException("that zip has no mods in it");
            for (int i = 0; i < all.size(); i++) {
                if (cancel.get()) break;
                ZipEntry z = all.get(i);
                String n = z.getName().replace('\\', '/');
                String leaf = n.substring(n.lastIndexOf('/') + 1);
                String line = leaf + "|" + z.getSize() + "|crc" + Long.toHexString(z.getCrc());
                step.at(i, all.size());
                if (isKnown(known, leaf, z.getSize(), line)) continue;
                boolean fresh = true;
                for (String l : known) if (l.startsWith(leaf + "|")) fresh = false;
                try (InputStream in = zip.getInputStream(z)) {
                    ModsPage.Installed r = ModsPage.install(ctx, in, true);
                    if (r.kept) kept++;
                    else if (r.existed) updated++;
                    else added++;
                    if (fresh && !r.existed && !r.kept && !"matchtype".equals(r.type))
                        new FileOutputStream(new File(r.dest, "disabled")).close();
                } catch (IOException e) {
                    failed++;
                    bad.append(bad.length() > 0 ? ", " : "").append(leaf);
                    continue;  // (not in .bundled: tried again next time)
                }
                for (int k = known.size() - 1; k >= 0; k--) if (known.get(k).startsWith(leaf + "|")) known.remove(k);
                known.add(line);
                changed = true;
                // (.bundled after each: a stopped install carries on next time)
                write(listFile, known);
            }
            step.at(all.size(), all.size());
        }
        if (changed) write(listFile, known);
        String s = cancel.get() ? "Stopped: " : "Bundled mods: ";
        s += added + " new (switched off - match types on), " + updated + " updated";
        if (kept > 0) s += ", " + kept + " kept (yours are newer)";
        if (failed > 0) s += ", " + failed + " couldn't be installed (" + bad + ")";
        return s + (cancel.get() ? "; the rest next time." : ".");
    }

    // Installed already: the same line, or a PC launcher's line (name|size|file
    // time) for the same name and size.
    static boolean isKnown(List<String> known, String leaf, long size, String line) {
        String pc = leaf + "|" + size + "|";
        for (String l : known) {
            if (l.equals(line)) return true;
            if (l.startsWith(pc) && !l.substring(pc.length()).startsWith("crc")) return true;
        }
        return false;
    }

    static void write(File listFile, List<String> known) throws IOException {
        StringBuilder b = new StringBuilder();
        for (String l : known) b.append(l).append('\n');
        File part = new File(listFile.getPath() + ".part");
        try (OutputStream out = new FileOutputStream(part)) {
            out.write(b.toString().getBytes(StandardCharsets.UTF_8));
        }
        if (listFile.exists()) listFile.delete();
        if (!part.renameTo(listFile)) throw new IOException("can't write " + listFile.getName());
    }
}
