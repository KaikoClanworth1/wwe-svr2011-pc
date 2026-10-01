// WWE SmackDown vs. Raw 2011 - custom GPU drivers in the launcher (Settings):
// driver packages (Mesa Turnip and the like: a zip with meta.json - name,
// description, author, driverVersion, libraryName - and the driver's .so)
// unpacked into the app's own storage (libadrenotools can't load a driver
// from shared storage), and the chosen one's .so written to the game's
// gpu_driver setting (src/gpu_driver.cpp loads it; empty: the phone's own).

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.net.Uri;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

final class Drivers {
    static final String kKey = "gpu_driver";

    static final class Driver {
        File dir, library;
        String name, version, author, description;
    }

    private final LauncherActivity a_;
    private final GameSettings settings_;
    private LinearLayout card_;

    Drivers(LauncherActivity a, GameSettings settings) {
        a_ = a;
        settings_ = settings;
    }

    File root() { return new File(a_.getFilesDir(), "drivers"); }

    static Driver read(File dir) {
        try {
            JSONObject m = new JSONObject(new String(Files.readAllBytes(new File(dir, "meta.json").toPath()),
                StandardCharsets.UTF_8));
            Driver d = new Driver();
            d.dir = dir;
            d.name = m.optString("name", dir.getName());
            d.version = m.optString("driverVersion", "");
            d.author = m.optString("author", "");
            d.description = m.optString("description", "");
            d.library = new File(dir, m.getString("libraryName"));
            return d.library.isFile() ? d : null;
        } catch (Exception e) {
            return null;
        }
    }

    List<Driver> installed() {
        List<Driver> list = new ArrayList<>();
        File[] dirs = root().listFiles(File::isDirectory);
        if (dirs == null) return list;
        Arrays.sort(dirs, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File d : dirs) {
            Driver dr = read(d);
            if (dr != null) list.add(dr);
        }
        return list;
    }

    // The Settings card.
    void build(LinearLayout column) {
        card_ = a_.card(column, "Graphics driver");
        Button add = a_.button("Add a driver (.zip)…", LauncherActivity.kCard);
        add.setOnClickListener(v -> add());
        column.addView(add, a_.fullWidth(10));
        TextView note = a_.text("For Adreno GPUs: driver packages such as Mesa Turnip (a zip with meta.json, as "
            + "other emulators use them). A driver that doesn't work falls back to the phone's own. Applies the "
            + "next time the game starts.", 12, LauncherActivity.kDim);
        note.setPadding(a_.dp(4), a_.dp(8), a_.dp(4), 0);
        column.addView(note);
        refresh();
    }

    void refresh() {
        card_.removeAllViews();
        String current = settings_.getString(kKey, "");
        List<RadioButton> radios = new ArrayList<>();
        RadioButton phone = radio("The phone's own driver", null, current.isEmpty() || !new File(current).isFile());
        phone.setOnClickListener(v -> choose(null));
        radios.add(phone);
        card_.addView(phone);
        for (Driver d : installed()) {
            String sub = (d.version.isEmpty() ? "" : d.version) + (d.author.isEmpty() ? "" : " · " + d.author);
            RadioButton r = radio(d.name, sub, d.library.getPath().equals(current));
            r.setOnClickListener(v -> choose(d));
            r.setOnLongClickListener(v -> {
                remove(d);
                return true;
            });
            radios.add(r);
            card_.addView(r);
        }
        if (radios.size() > 1) {
            TextView hint = a_.text("Hold a driver to remove it.", 12, LauncherActivity.kDim);
            hint.setPadding(0, 0, 0, a_.dp(8));
            card_.addView(hint);
        }
    }

    RadioButton radio(String title, String sub, boolean checked) {
        RadioButton r = new RadioButton(a_);
        r.setText(sub == null || sub.isEmpty() ? title : title + "\n" + sub);
        r.setTextColor(LauncherActivity.kText);
        r.setTextSize(15);
        r.setChecked(checked);
        r.setButtonTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        r.setPadding(a_.dp(8), a_.dp(10), 0, a_.dp(10));
        return r;
    }

    void choose(Driver d) {
        settings_.setString(kKey, d == null ? "" : d.library.getPath());
        a_.status(settings_.save() ? (d == null ? "The phone's own driver." : d.name + " chosen.")
                                         + " It applies the next time the game starts."
                                   : "Could not save the settings.");
        refresh();
    }

    void remove(Driver d) {
        a_.confirm("Remove the driver " + d.name + "?", "Remove", () -> {
            if (d.library.getPath().equals(settings_.getString(kKey, ""))) {
                settings_.setString(kKey, "");
                settings_.save();
            }
            FileOps.deleteTree(d.dir);
            a_.status("Removed " + d.name + ".");
            refresh();
        });
    }

    void add() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("application/zip");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            final String[] result = new String[1];
            a_.background(() -> {
                try {
                    result[0] = "Added " + unpack(uri).name + ". Choose it above to use it.";
                } catch (Exception e) {
                    result[0] = "That driver can't be added: " + e.getMessage();
                }
            }, () -> {
                a_.status(result[0]);
                refresh();
            });
        });
    }

    Driver unpack(Uri uri) throws IOException {
        File tmp = new File(a_.getCacheDir(), "driver.zip");
        FileOps.copyIn(a_.getContentResolver(), uri, tmp);
        try (ZipFile zip = new ZipFile(tmp)) {
            ZipEntry metaEntry = null;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                if (z.getName().endsWith("meta.json")) metaEntry = z;
            }
            if (metaEntry == null) throw new IOException("it has no meta.json (not a driver package)");
            String prefix = metaEntry.getName().substring(0, metaEntry.getName().length() - "meta.json".length());
            String metaText;
            try (InputStream in = zip.getInputStream(metaEntry)) {
                java.io.ByteArrayOutputStream b = new java.io.ByteArrayOutputStream();
                FileOps.copy(in, b);
                metaText = b.toString("UTF-8");
            }
            String name, library;
            try {
                JSONObject m = new JSONObject(metaText);
                name = m.optString("name", "driver");
                library = m.getString("libraryName");
            } catch (org.json.JSONException e) {
                throw new IOException("its meta.json can't be read");
            }
            if (zip.getEntry(prefix + library) == null) throw new IOException("it doesn't contain " + library);
            StringBuilder folder = new StringBuilder();
            for (char c : name.toCharArray()) folder.append(Character.isLetterOrDigit(c) || c == '-' || c == '.' ? c : '_');
            File dir = new File(root(), folder.length() == 0 ? "driver" : folder.toString());
            FileOps.deleteTree(dir);
            dir.mkdirs();
            String destPath = dir.getCanonicalPath() + File.separator;
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                ZipEntry z = e.nextElement();
                if (z.isDirectory() || !z.getName().startsWith(prefix)) continue;
                File out = new File(dir, z.getName().substring(prefix.length()));
                if (!out.getCanonicalPath().startsWith(destPath)) throw new IOException("bad path in the package");
                out.getParentFile().mkdirs();
                try (InputStream in = zip.getInputStream(z); OutputStream os = new FileOutputStream(out)) {
                    FileOps.copy(in, os);
                }
            }
            Driver d = read(dir);
            if (d == null) throw new IOException("its meta.json can't be read");
            return d;
        } finally {
            tmp.delete();
        }
    }
}
