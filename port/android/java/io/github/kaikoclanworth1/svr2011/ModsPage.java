// WWE SmackDown vs. Raw 2011 - the launcher's Mods tab on the phone (as the PC
// launcher's, arenas branch): installed mods in the game folder's
// Mods/Arenas/<id> (manifest.txt, arena.pac, banner.dds) and Mods/Superstars/<id>
// (manifest.txt, ch.pac, maybe a theme song and an entrance movie); a
// "disabled" file turns one off. A switch for each, Remove, and Add a .svrmod
// (a zip) picked from the phone. Mods are made on the PC with the SvR2011 Mod
// Maker.

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.net.Uri;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Switch;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class ModsPage {
    private final LauncherActivity a_;
    private LinearLayout installed_;

    ModsPage(LauncherActivity a) { a_ = a; }

    static File arenasFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Arenas"); }
    static File superstarsFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Superstars"); }

    View view() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Custom arenas and superstars made with the SvR2011 Mod Maker on the PC. Arenas are "
            + "on the arena select pages after the game's own arenas (move right past the last arena); superstars are "
            + "under the M tile of the character select (up to 50). Changes apply the next time the game starts.",
            14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);
        Button add = a_.button("Add a mod (.svrmod)…", LauncherActivity.kRed);
        add.setOnClickListener(v -> pick());
        c.addView(add, a_.fullWidth(14));
        installed_ = a_.card(c, "Installed mods");
        refresh();
        return c;
    }

    static Map<String, String> manifest(File dir) {
        Map<String, String> m = new HashMap<>();
        File f = new File(dir, "manifest.txt");
        if (!f.isFile()) f = new File(dir, "info.txt");
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                int eq = line.indexOf('=');
                if (eq > 0) m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (IOException ignored) {
        }
        return m;
    }

    void refresh() {
        installed_.removeAllViews();
        File[] arenas = arenasFolder().listFiles(f -> f.isDirectory() && new File(f, "arena.pac").isFile());
        File[] stars = superstarsFolder().listFiles(f -> f.isDirectory() && new File(f, "ch.pac").isFile());
        File[] dirs = new File[(arenas == null ? 0 : arenas.length) + (stars == null ? 0 : stars.length)];
        int k = 0;
        if (arenas != null) for (File f : arenas) dirs[k++] = f;
        if (stars != null) for (File f : stars) dirs[k++] = f;
        if (dirs.length == 0) {
            TextView none = a_.text("None yet.", 15, LauncherActivity.kDim);
            none.setPadding(0, a_.dp(14), 0, a_.dp(14));
            installed_.addView(none);
            return;
        }
        Arrays.sort(dirs, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File d : dirs) {
            Map<String, String> m = manifest(d);
            String name = m.containsKey("name") ? m.get("name") : d.getName();
            boolean star = new File(d, "ch.pac").isFile();
            String hint = (star ? "Superstar" : "Arena")
                + (m.containsKey("author") && !m.get("author").isEmpty() ? " by " + m.get("author") : "")
                + (m.containsKey("version") ? ", v" + m.get("version") : "")
                + ", " + FileOps.human(new File(d, star ? "ch.pac" : "arena.pac").length());
            LinearLayout controls = new LinearLayout(a_);
            Switch on = new Switch(a_);
            on.setChecked(!new File(d, "disabled").exists());
            on.setOnCheckedChangeListener((b, checked) -> {
                File off = new File(d, "disabled");
                try {
                    if (checked) off.delete();
                    else new FileOutputStream(off).close();
                    a_.status(name + (checked ? " is on." : " is off.") + " (from the next game start)");
                } catch (IOException e) {
                    a_.status("Could not change " + name + ": " + e.getMessage());
                }
            });
            Button remove = a_.button("Remove", LauncherActivity.kCard);
            remove.setOnClickListener(v -> {
                deleteTree(d);
                a_.status("Removed " + name + ".");
                refresh();
            });
            controls.addView(on);
            controls.addView(remove);
            a_.row(installed_, name, hint, controls);
        }
    }

    static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    void pick() {
        if (!InstallActivity.installed()) {
            a_.status("Install the game first.");
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            final String[] result = new String[1];
            a_.status("Adding the mod…");
            a_.background(() -> {
                try {
                    result[0] = "Installed " + install(uri) + " (from the next game start).";
                } catch (Exception e) {
                    result[0] = "That mod could not be added: " + e.getMessage();
                }
            }, () -> {
                refresh();
                a_.status(result[0]);
            });
        });
    }

    // A .svrmod: manifest.txt (type=arena or superstar, id=...) and its files -
    // arena.pac and banner.dds, or ch.pac and the song= / movie= files.
    String install(Uri uri) throws IOException {
        File tmp = new File(a_.getCacheDir(), "mod.part");
        Map<String, File> parts = new HashMap<>();
        File stage = new File(a_.getCacheDir(), "mod_stage");
        deleteTree(stage);
        stage.mkdirs();
        try (InputStream raw = a_.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("can't read it");
            ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw, 1 << 16));
            ZipEntry e;
            while ((e = zip.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String n = e.getName();
                n = n.substring(n.lastIndexOf('/') + 1);
                if (n.isEmpty() || n.startsWith(".")) continue;
                File out = new File(stage, n);
                try (OutputStream o = new FileOutputStream(out)) {
                    FileOps.copy(zip, o);
                }
                parts.put(n, out);
            }
        }
        tmp.delete();
        Map<String, String> m = manifest(stage);
        boolean star = "superstar".equalsIgnoreCase(m.get("type"));
        if (star ? !parts.containsKey("ch.pac") : !parts.containsKey("arena.pac"))
            throw new IOException(star ? "it has no ch.pac" : "it has no arena (not a SvR2011 mod?)");
        String id = m.containsKey("id") ? m.get("id").replaceAll("[^A-Za-z0-9_\\-]", "_") : "";
        if (id.isEmpty()) id = star ? "superstar" : "arena";
        File dest = new File(star ? superstarsFolder() : arenasFolder(), id);
        deleteTree(dest);
        dest.getParentFile().mkdirs();
        FileOps.copyTree(stage, dest);  // (the cache and the games folder are different storage)
        deleteTree(stage);
        if (!new File(dest, star ? "ch.pac" : "arena.pac").isFile()) throw new IOException("can't write to the game folder");
        return m.containsKey("name") ? m.get("name") : id;
    }
}
