// WWE SmackDown vs. Raw 2011 - the bundled mods' rules on the phone, as the
// PC launcher's (launcher/mods_tab.c mods_install_bundled). No Android here:
// BundledMods gives it the bundles, installs them (ModsPage.install) and asks
// the questions; a desktop JVM can run it over a test folder.
//
// Mods/.bundled_state - the PC's record, the same file - keeps per bundle its
// version, folder (kind\id) and state: "on" (installed by the launcher),
// "removed" (the player removed its folder: it stays removed), "kept <version>"
// (the player kept their edited copy over that version), "retired"; and per
// installed file its size, time and hash (FNV-1a 64). A check, per bundle:
// - not installed, and never: installed (switched off - "disabled" in its
//   folder - except a match type);
// - its folder gone: recorded as removed, never installed again;
// - a newer version shipped: an unedited copy is updated (its on / off kept);
//   an edited one is asked about (update - the player's copy to Mods/.backup -
//   or keep theirs, not asked again for that version);
// - files missing from an unedited copy: asked about (fix / not now: asked
//   again next time) - never fixed silently;
// - the player's copy a newer version than the shipped one: left alone.
// The old Mods/.bundled ("name|size|crc<hex>" lines; the PC's "name|size|time")
// is read once, to carry its installs over: a copy of the shipped version that
// still has the bundle's files is the bundle's, any other is taken as edited.
// (It reinstalled a bundle whenever the shipped file changed, over the player's
// edits, and brought a removed one back with a new version.) Every decision
// goes to Mods/.install.log, as the PC's.

package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class BundledRules {
    static final int kNone = 0, kYes = 1, kNo = 2;          // (answers; none: only logged)
    static final int kAskUpdate = 1, kAskFix = 2;

    static final class Question {
        int kind;
        String file, name, have, ship;  // (the bundle's file name, the mod's name, installed / shipped versions)
        int missing;
    }

    interface Host {
        // Installs the .svrmod; its folder, or null when the installed copy is newer (left alone).
        File install(File svrmod) throws IOException;
        int ask(Question q);  // kYes / kNo / kNone (blocking)
    }

    static final class Rec {
        String file, version = "", folder = "", state = "";
        List<long[]> sums = new ArrayList<>();  // (size, time, hash) of files[i]
        List<String> files = new ArrayList<>();  // (rel paths, '/')
    }

    // Bundles no longer shipped: switched off once (the player may switch them back on). hurricane.svrmod (2.0.5):
    // The Hurricane is the playable manager on the M tile already.
    static final String[][] kRetired = { {"hurricane.svrmod", "Superstars/the_hurricane"} };

    final File mods;
    final Host host;
    final Map<String, Rec> recs = new LinkedHashMap<>();  // (by file name, lower case)
    String legacy;  // (the old Mods/.bundled, whole; null if none)
    int added, updated, fixed, kept, removed, asked, failed, carried;
    final StringBuilder bad = new StringBuilder();

    BundledRules(File mods, Host host) {
        this.mods = mods;
        this.host = host;
        mods.mkdirs();
        load();
        legacy = readText(new File(mods, ".bundled"));
        for (String[] r : kRetired) {
            Rec rec = recs.get(r[0].toLowerCase(Locale.ROOT));
            boolean was = rec != null ? rec.state.equals("on") : legacy != null && legacy.contains(r[0] + "|");
            if (!was) continue;
            File folder = new File(mods, r[1]);
            if (folder.isDirectory()) touch(new File(folder, "disabled"));
            if (rec == null) rec = add(r[0]);
            rec.state = "retired";
            log("bundled " + r[0] + ": no longer shipped - switched off");
        }
        save();
    }

    // ---- the record

    Rec add(String file) {
        Rec r = recs.get(file.toLowerCase(Locale.ROOT));
        if (r != null) return r;
        r = new Rec();
        r.file = file;
        recs.put(file.toLowerCase(Locale.ROOT), r);
        return r;
    }

    void load() {
        String text = readText(new File(mods, ".bundled_state"));
        if (text == null) return;
        for (String line : text.split("\n")) {
            if (line.endsWith("\r")) line = line.substring(0, line.length() - 1);
            String[] f = line.split("\t", -1);
            if (f[0].equals("B") && f.length >= 5) {
                Rec r = add(f[1]);
                r.version = f[2];
                r.folder = f[3].replace('\\', '/');
                r.state = f[4];
            } else if (f[0].equals("F") && f.length >= 6) {
                Rec r = recs.get(f[1].toLowerCase(Locale.ROOT));
                if (r == null) continue;
                try {
                    r.sums.add(new long[] {Long.parseUnsignedLong(f[3]), Long.parseUnsignedLong(f[4]), Long.parseUnsignedLong(f[5], 16)});
                    r.files.add(f[2].replace('\\', '/'));
                } catch (NumberFormatException ignored) {
                }
            }
        }
    }

    // (the PC's format: folders and paths with '\', times are the platform's own - another platform's time only
    // makes a file be hashed again)
    void save() {
        StringBuilder b = new StringBuilder("# The bundled mods the launcher installed: their versions, folders and files (don't edit)\n");
        for (Rec r : recs.values()) {
            b.append("B\t").append(r.file).append('\t').append(r.version).append('\t').append(r.folder.replace('/', '\\'))
                .append('\t').append(r.state).append('\n');
            for (int i = 0; i < r.files.size(); i++) {
                long[] s = r.sums.get(i);
                b.append("F\t").append(r.file).append('\t').append(r.files.get(i).replace('/', '\\')).append('\t')
                    .append(Long.toUnsignedString(s[0])).append('\t').append(Long.toUnsignedString(s[1])).append('\t')
                    .append(Long.toHexString(s[2])).append('\n');
            }
        }
        File path = new File(mods, ".bundled_state"), part = new File(mods, ".bundled_state.part");
        try (OutputStream o = new FileOutputStream(part)) {
            o.write(b.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return;
        }
        if (!part.renameTo(path)) {
            path.delete();
            part.renameTo(path);
        }
    }

    // (the mod's own files: not its on / off "disabled" or the game's and launcher's dot files)
    static boolean counts(String name) { return !name.startsWith(".") && !name.equalsIgnoreCase("disabled"); }

    static void walk(File dir, String rel, List<String> out) {
        File[] list = dir.listFiles();
        if (list == null) return;
        for (File f : list) {
            if (!counts(f.getName())) continue;
            String sub = rel.isEmpty() ? f.getName() : rel + "/" + f.getName();
            if (f.isDirectory()) {
                if (!Files.isSymbolicLink(f.toPath())) walk(f, sub, out);
            } else {
                out.add(sub);
            }
        }
    }

    static long hash(InputStream in) throws IOException {
        byte[] buf = new byte[1 << 16];
        long h = 0xcbf29ce484222325L;
        int n;
        while ((n = in.read(buf)) > 0)
            for (int i = 0; i < n; i++) h = (h ^ (buf[i] & 0xFF)) * 0x100000001b3L;
        return h;
    }

    static long hash(File f) {
        try (InputStream in = new FileInputStream(f)) {
            return hash(in);
        } catch (IOException e) {
            return 0;
        }
    }

    static void setFiles(Rec r, File dir) {
        r.files.clear();
        r.sums.clear();
        List<String> all = new ArrayList<>();
        walk(dir, "", all);
        for (String rel : all) {
            File f = new File(dir, rel);
            r.files.add(rel);
            r.sums.add(new long[] {f.length(), f.lastModified(), hash(f)});
        }
    }

    // dir against r's files -> {edited, missing}: a file changed or one added (the Mod Maker's save, the player's
    // own); files gone. The same size and time is taken as unchanged (no rehash at every check); a new time with the
    // same content too (its time updated in r).
    static int[] check(Rec r, File dir) {
        int edited = 0, missing = 0;
        for (int i = 0; i < r.files.size(); i++) {
            File f = new File(dir, r.files.get(i));
            long[] s = r.sums.get(i);
            if (!f.isFile()) {
                missing++;
                continue;
            }
            long size = f.length(), time = f.lastModified();
            if (size == s[0] && time == s[1]) continue;
            if (size == s[0] && hash(f) == s[2]) {
                s[1] = time;  // (touched, not changed)
                continue;
            }
            edited++;
        }
        List<String> now = new ArrayList<>();
        walk(dir, "", now);
        for (String rel : now) {
            boolean known = false;
            for (String k : r.files) if (k.equalsIgnoreCase(rel)) known = true;
            if (!known) edited++;
        }
        return new int[] {edited, missing};
    }

    // ---- a bundle

    // A mod type's folder under Mods (as ModsPage.install places it); null for one the phone doesn't install.
    static String kindFolder(String type) {
        switch (type) {
            case "superstar": return "Superstars";
            case "arena": return "Arenas";
            case "backstage": return "Backstage";
            case "signs": return "Signs";
            case "media": return "Media";
            case "matchtype": return "MatchTypes";
            case "background": return "Backgrounds";
            case "attire": return "Attires";
            default: return null;
        }
    }

    // The .svrmod's manifest.txt (key=value lines) -> its keys.
    static Map<String, String> manifest(ZipFile zip) throws IOException {
        Map<String, String> m = new LinkedHashMap<>();
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
            ZipEntry z = e.nextElement();
            if (!z.getName().replace('\\', '/').equals("manifest.txt")) continue;
            byte[] b;
            try (InputStream in = zip.getInputStream(z)) {
                b = readAll(in);
            }
            for (String line : new String(b, StandardCharsets.UTF_8).replace("\uFEFF", "").split("\r?\n")) {
                int eq = line.indexOf('=');
                if (eq > 0) m.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        }
        return m;
    }

    // Checks the bundle `file` (its name in the release's zip) at `svrmod`, and acts.
    void bundle(String file, File svrmod) {
        Map<String, String> bm;
        try (ZipFile zip = new ZipFile(svrmod)) {
            bm = manifest(zip);
        } catch (IOException e) {
            fail(file, "can't be read (" + e.getMessage() + ")");
            return;
        }
        String type = bm.containsKey("type") ? bm.get("type").toLowerCase(Locale.ROOT) : "arena";
        String id = bm.containsKey("id") ? bm.get("id").replaceAll("[^A-Za-z0-9_\\-]", "_") : "";
        String version = bm.containsKey("version") ? bm.get("version") : "";
        String kind = kindFolder(type);
        if (kind == null || id.isEmpty()) {
            log("bundled " + file + ": a " + type + " mod - not installed on the phone");
            return;
        }
        String folder = kind + "/" + id;
        File dir = new File(mods, folder);
        Rec r = recs.get(file.toLowerCase(Locale.ROOT));
        if (r != null && (r.state.equals("removed") || r.state.equals("retired"))) return;  // (the player's say)
        if (r == null) {
            r = add(file);
            r.folder = folder;
            if (!dir.isDirectory()) {
                if (legacy != null && legacy.contains(file + "|")) {  // (installed before, its folder gone)
                    r.state = "removed";
                    removed++;
                    log("bundled " + file + ": removed by the player - not installed again");
                    save();
                    return;
                }
                if (install(file, svrmod, dir, r, type, version, true, false)) {
                    added++;
                    log("bundled " + file + ": installed (" + folder + ")");
                } else {
                    recs.remove(file.toLowerCase(Locale.ROOT));  // (not recorded: tried again next time)
                }
                save();
                return;
            }
            // installed before this record: the same version as the shipped one with the bundle's files - its record
            // is the bundle's, so the player's changes show as edits; anything else - taken as edited
            String have = manifestVersion(dir);
            r.version = have;
            r.state = "on";
            setFiles(r, dir);
            boolean same = have.equals(version) && sameAsShipped(r, svrmod);
            if (!same)
                for (long[] s : r.sums) {
                    s[2] ^= 1;
                    s[1] = 0;
                }
            carried++;
            log("bundled " + file + ": installed before (version " + (have.isEmpty() ? "?" : have) + ") - carried over "
                + (same ? "as the bundle's" : "as edited"));
            save();
        }
        if (!dir.isDirectory()) {  // (the player removed it)
            r.state = "removed";
            removed++;
            log("bundled " + file + ": removed by the player - not installed again");
            save();
            return;
        }
        int[] em = check(r, dir);
        int edited = em[0], missing = em[1];
        boolean off = new File(dir, "disabled").exists();
        if (Updater.newer(version, r.version)) {
            String from = r.version;
            if (r.state.equals("kept " + version)) return;  // (the player kept theirs over this version)
            if (edited == 0 && missing == 0) {
                if (install(file, svrmod, dir, r, type, version, false, off)) {
                    updated++;
                    log("bundled " + file + ": updated " + (from.isEmpty() ? "?" : from) + " -> " + version + " (unedited)");
                }
                save();
                return;
            }
            Question q = question(kAskUpdate, file, bm, id, r.version, version, missing);
            int a = host.ask(q);
            if (a == kYes) {  // update: the player's copy to Mods/.backup first
                File backup = new File(new File(mods, ".backup"), kind + "-" + id + "-"
                    + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date()));
                backup.getParentFile().mkdirs();
                if (!dir.renameTo(backup)) {
                    log("bundled " + file + ": couldn't back the edited copy up - left as it is");
                    fail(file, null);
                    return;
                }
                if (install(file, svrmod, dir, r, type, version, false, off)) {
                    updated++;
                    log("bundled " + file + ": updated to " + version + ", the edited copy kept in " + mods.getName() + "/.backup/" + backup.getName());
                } else {
                    FileOps.deleteTree(dir);
                    backup.renameTo(dir);
                    log("bundled " + file + ": the update failed - the edited copy put back");
                }
            } else if (a == kNo) {  // keep theirs - not asked again for this version
                r.state = "kept " + version;
                kept++;
                log("bundled " + file + ": the player kept their edited copy over version " + version);
            } else {
                asked++;
            }
            save();
            return;
        }
        if (missing > 0 && edited == 0) {  // a damaged install: a question, never a silent reinstall
            int a = host.ask(question(kAskFix, file, bm, id, r.version, version, missing));
            if (a == kYes) {
                if (install(file, svrmod, dir, r, type, version, false, off)) {
                    fixed++;
                    log("bundled " + file + ": " + missing + " missing file(s) put back (installed again)");
                }
            } else if (a == kNo) {
                log("bundled " + file + ": " + missing + " file(s) missing - not fixed now (asked again next time)");
            } else {
                asked++;
            }
        }
        save();  // (times of touched files)
    }

    Question question(int kind, String file, Map<String, String> bm, String id, String have, String ship, int missing) {
        Question q = new Question();
        q.kind = kind;
        q.file = file;
        q.name = bm.containsKey("name") ? bm.get("name") : id;
        q.have = have;
        q.ship = ship;
        q.missing = missing;
        log(kind == kAskUpdate ? "bundled " + file + ": edited, version " + ship + " shipped (" + have + " installed) - asking"
                               : "bundled " + file + ": files missing - asking to fix it");
        return q;
    }

    // Installs the bundle into dir (kept off when it was; a fresh one off unless a match type), recorded in r.
    boolean install(String file, File svrmod, File dir, Rec r, String type, String version, boolean fresh, boolean off) {
        File got;
        try {
            got = host.install(svrmod);
        } catch (IOException e) {
            fail(file, e.getMessage());
            return false;
        }
        if (got == null) {  // (the installed copy is newer)
            log("bundled " + file + ": the installed copy is newer than " + version + " - left alone");
            return false;
        }
        if ((fresh && !type.equals("matchtype")) || off) touch(new File(dir, "disabled"));
        r.version = version;
        r.state = "on";
        setFiles(r, dir);
        return true;
    }

    // The copy's files against the bundle's (the copy's extra ones aside).
    boolean sameAsShipped(Rec r, File svrmod) {
        try (ZipFile zip = new ZipFile(svrmod)) {
            int n = 0;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                if (z.isDirectory()) continue;
                String rel = z.getName().replace('\\', '/');
                String leaf = rel.substring(rel.lastIndexOf('/') + 1);
                if (leaf.isEmpty() || !counts(leaf)) continue;  // (ModsPage.install leaves dot files out)
                long h;
                try (InputStream in = zip.getInputStream(z)) {
                    h = hash(in);
                }
                boolean found = false;
                for (int i = 0; i < r.files.size() && !found; i++)
                    found = r.files.get(i).equalsIgnoreCase(rel) && r.sums.get(i)[2] == h;
                if (!found) return false;
                n++;
            }
            return n > 0;
        } catch (IOException e) {
            return false;
        }
    }

    void fail(String file, String why) {
        failed++;
        bad.append(bad.length() > 0 ? ", " : "").append(file);
        if (why != null) log("bundled " + file + ": couldn't be installed - " + why);
    }

    // What this pass did, for the status line.
    String summary(boolean stopped) {
        StringBuilder s = new StringBuilder(stopped ? "Stopped: " : "Bundled mods: ");
        s.append(added).append(" new (switched off - match types on), ").append(updated).append(" updated");
        if (fixed > 0) s.append(", ").append(fixed).append(" fixed");
        if (kept > 0) s.append(", ").append(kept).append(" of yours kept");
        if (asked > 0) s.append(", ").append(asked).append(" waiting for an answer");
        if (failed > 0) s.append(", ").append(failed).append(" couldn't be installed (").append(bad).append(")");
        return s.append(stopped ? "; the rest next time." : ".").toString();
    }

    void finish() {
        if (carried > 0) log("bundled mods: " + carried + " installed copies carried over to Mods/.bundled_state as they are");
        save();
    }

    // ---- files

    void log(String text) {
        String line = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date()) + " " + text + "\n";
        try (OutputStream o = new FileOutputStream(new File(mods, ".install.log"), true)) {
            o.write(line.getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    static void touch(File f) {
        try {
            new FileOutputStream(f).close();
        } catch (IOException ignored) {
        }
    }

    static byte[] readAll(InputStream in) throws IOException {
        java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[1 << 14];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return b.toByteArray();
    }

    static String readText(File f) {
        if (!f.isFile()) return null;
        try (InputStream in = new FileInputStream(f)) {
            return new String(readAll(in), StandardCharsets.UTF_8).replace("\uFEFF", "");
        } catch (IOException e) {
            return null;
        }
    }

    // (a folder's manifest version, as ModsPage.manifest reads it)
    static String manifestVersion(File dir) {
        String t = readText(new File(dir, "manifest.txt"));
        if (t == null) return "";
        for (String line : t.split("\r?\n")) {
            int eq = line.indexOf('=');
            if (eq > 0 && line.substring(0, eq).trim().equals("version")) return line.substring(eq + 1).trim();
        }
        return "";
    }
}
