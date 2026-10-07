// WWE SmackDown vs. Raw 2011 - the launcher's Mods tab on the phone (as the PC
// launcher's, arenas branch): installed mods in the game folder's
// Mods/Arenas/<id> (manifest.txt, arena.pac, banner.dds) and Mods/Superstars/<id>
// (manifest.txt, ch.pac, maybe a theme song and an entrance movie); a
// "disabled" file turns one off. A switch for each, Remove, and Add a .svrmod
// (a zip) picked from the phone. Mods are made on the PC with the SvR2011 Mod
// Maker. Match types the port adds are mods too (type=matchtype, a manifest
// only, in Mods/MatchTypes/<id>) so they can be switched off; they come on.

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
    static File signsFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Signs"); }
    static File mediaFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Media"); }
    static File backstageFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Backstage"); }
    static File matchTypesFolder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "MatchTypes"); }

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
        Button bundled = a_.button("Get bundled mods", LauncherActivity.kCard);
        bundled.setOnClickListener(v -> {
            if (!InstallActivity.installed()) a_.status("Install the game first.");
            else BundledMods.offer(a_, true, this::refresh);
        });
        c.addView(bundled, a_.fullWidth(8));
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
        File[] signs = signsFolder().listFiles(f -> f.isDirectory() && new File(f, "manifest.txt").isFile());
        File[] media = mediaFolder().listFiles(f -> f.isDirectory() && new File(f, "manifest.txt").isFile());
        File[] back = backstageFolder().listFiles(f -> f.isDirectory() && new File(f, "arena.pac").isFile());
        File[] types = matchTypesFolder().listFiles(f -> f.isDirectory() && new File(f, "manifest.txt").isFile());
        File[] dirs = new File[(arenas == null ? 0 : arenas.length) + (stars == null ? 0 : stars.length)
            + (signs == null ? 0 : signs.length) + (media == null ? 0 : media.length) + (back == null ? 0 : back.length)
            + (types == null ? 0 : types.length)];
        int k = 0;
        if (arenas != null) for (File f : arenas) dirs[k++] = f;
        if (stars != null) for (File f : stars) dirs[k++] = f;
        if (signs != null) for (File f : signs) dirs[k++] = f;
        if (media != null) for (File f : media) dirs[k++] = f;
        if (back != null) for (File f : back) dirs[k++] = f;
        if (types != null) for (File f : types) dirs[k++] = f;
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
            boolean signPack = d.getParentFile().getName().equals("Signs");
            boolean mediaPack = d.getParentFile().getName().equals("Media");
            boolean backPack = d.getParentFile().getName().equals("Backstage");
            boolean matchType = d.getParentFile().getName().equals("MatchTypes");
            String hint = (matchType ? "Match type" : backPack ? "Backstage area" : mediaPack ? "Media" : signPack ? "Crowd signs" : star ? "Superstar" : "Arena")
                + (m.containsKey("author") && !m.get("author").isEmpty() ? " by " + m.get("author") : "")
                + (m.containsKey("version") ? ", v" + m.get("version") : "")
                + (signPack || mediaPack || matchType ? "" : ", " + FileOps.human(new File(d, star ? "ch.pac" : "arena.pac").length()));
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
                if (matchType) {
                    // (no folder means on - a match type is switched off instead)
                    try {
                        new FileOutputStream(new File(d, "disabled")).close();
                        a_.status("Match types can't be removed - " + name + " is switched off instead. (from the next game start)");
                    } catch (IOException e) {
                        a_.status("Could not change " + name + ": " + e.getMessage());
                    }
                    refresh();
                    return;
                }
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
    // arena.pac and banner.dds, or ch.pac and the song= / movie= files; a
    // match type (type=matchtype) is the manifest alone and comes switched on.
    String install(Uri uri) throws IOException {
        try (InputStream raw = a_.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("can't read it");
            return install(a_, raw, false).name;
        }
    }

    // Where a mod went (Mods/<kind>/<id>), whether it was there already, and
    // for a bundled one whether the player's newer copy was kept instead.
    static final class Installed {
        String name, type;
        File dest;
        boolean existed, kept;
    }

    // Installs the .svrmod read from `raw`. A bundled mod (BundledMods) keeps
    // the player's on / off and never replaces a newer version of it.
    static Installed install(android.content.Context ctx, InputStream raw, boolean bundle) throws IOException {
        Map<String, File> parts = new HashMap<>();
        File stage = new File(ctx.getCacheDir(), "mod_stage");
        deleteTree(stage);
        stage.mkdirs();
        ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw, 1 << 16));
        ZipEntry e;
        while ((e = zip.getNextEntry()) != null) {
            if (e.isDirectory()) continue;
            // (subfolders kept - a superstar mod's moves/motions/ - but
            // nothing outside the mod's folder)
            String n = e.getName().replace('\\', '/');
            String leaf = n.substring(n.lastIndexOf('/') + 1);
            if (leaf.isEmpty() || leaf.startsWith(".") || n.startsWith("/") || n.contains(":")
                    || ("/" + n + "/").contains("/../"))
                continue;
            File out = new File(stage, n);
            File dir = out.getParentFile();
            if (dir != null) dir.mkdirs();
            try (OutputStream o = new FileOutputStream(out)) {
                FileOps.copy(zip, o);
            }
            parts.put(n, out);
        }
        Map<String, String> m = manifest(stage);
        String type = m.containsKey("type") ? m.get("type").toLowerCase() : "arena";
        boolean star = type.equals("superstar");
        boolean signPack = type.equals("signs");
        boolean mediaPack = type.equals("media");
        boolean backPack = type.equals("backstage");
        boolean matchType = type.equals("matchtype");
        if (!signPack && !mediaPack && !matchType && (star ? !parts.containsKey("ch.pac") : !parts.containsKey("arena.pac"))) {
            deleteTree(stage);
            throw new IOException(star ? "it has no ch.pac" : "it has no arena (not a SvR2011 mod?)");
        }
        String id = m.containsKey("id") ? m.get("id").replaceAll("[^A-Za-z0-9_\\-]", "_") : "";
        if (id.isEmpty()) id = matchType ? "matchtype" : backPack ? "backstage" : mediaPack ? "media" : signPack ? "signs" : star ? "superstar" : "arena";
        File dest = new File(matchType ? matchTypesFolder() : backPack ? backstageFolder() : mediaPack ? mediaFolder() : signPack ? signsFolder() : star ? superstarsFolder() : arenasFolder(), id);
        Installed r = new Installed();
        r.name = m.containsKey("name") ? m.get("name") : id;
        r.type = type;
        r.dest = dest;
        r.existed = dest.exists();
        boolean off = false;
        if (bundle && r.existed) {
            String mine = manifest(dest).get("version"), theirs = m.get("version");
            if (mine != null && theirs != null && Updater.newer(mine, theirs)) {
                deleteTree(stage);
                r.kept = true;
                return r;
            }
            off = new File(dest, "disabled").exists();
        }
        deleteTree(dest);
        dest.getParentFile().mkdirs();
        FileOps.copyTree(stage, dest);  // (the cache and the games folder are different storage)
        deleteTree(stage);
        if (!new File(dest, signPack || mediaPack || matchType ? "manifest.txt" : star ? "ch.pac" : "arena.pac").isFile())
            throw new IOException("can't write to the game folder");
        if (off) new FileOutputStream(new File(dest, "disabled")).close();
        return r;
    }
}
