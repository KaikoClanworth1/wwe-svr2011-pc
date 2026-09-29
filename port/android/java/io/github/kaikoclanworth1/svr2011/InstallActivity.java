// WWE SmackDown vs. Raw 2011 - first run on the phone.
//
// The PC launcher's "Create APK Package" makes the APK and a zip of the
// installed game. This activity gets all-files access (the game folder is
// in shared storage, so the files stay reachable from a PC over USB), finds
// that zip in the games or Download folder and extracts it into
// games/<kFolderName>, then starts the game.

package io.github.kaikoclanworth1.svr2011;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public class InstallActivity extends Activity {
    static final String kFolderName = "WWE SmackDown vs. Raw 2011";
    static final String kPackageName = "SvR2011-Game.zip";
    static final String kTag = "SvR2011";

    private TextView text_;
    private ProgressBar progress_;
    private Button button_;
    private Thread worker_;

    // <shared storage>/games/WWE SmackDown vs. Raw 2011
    static File gameFolder() {
        return new File(new File(Environment.getExternalStorageDirectory(), "games"), kFolderName);
    }

    static boolean installed() {
        return new File(gameFolder(), "default.xex").isFile();
    }

    // The package the launcher made, where the player may have copied it.
    static File findPackage() {
        File root = Environment.getExternalStorageDirectory();
        File[] places = {
            new File(root, "games"),
            new File(root, Environment.DIRECTORY_DOWNLOADS),
            gameFolder(),
            root,
        };
        for (File dir : places) {
            File zip = new File(dir, kPackageName);
            if (zip.isFile()) return zip;
        }
        return null;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        layout.setBackgroundColor(Color.BLACK);
        layout.setPadding(64, 64, 64, 64);
        text_ = new TextView(this);
        text_.setTextColor(Color.WHITE);
        text_.setTextSize(20);
        text_.setGravity(Gravity.CENTER);
        progress_ = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress_.setMax(1000);
        progress_.setVisibility(ProgressBar.GONE);
        button_ = new Button(this);
        button_.setVisibility(Button.GONE);
        layout.addView(text_);
        layout.addView(progress_, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        layout.addView(button_);
        setContentView(layout);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (worker_ == null) next();
    }

    // The next first-run step, or the game.
    private void next() {
        if (!Environment.isExternalStorageManager()) {
            show("SvR 2011 keeps the game in the phone's games folder:\n"
                + "games/" + kFolderName + "\n\nAllow access to all files on the next screen.",
                "Allow access", () -> startActivity(new Intent(
                    Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()))));
            return;
        }
        if (installed()) {
            startActivity(new Intent(this, GameActivity.class));
            finish();
            return;
        }
        File zip = findPackage();
        if (zip == null) {
            show("The game isn't installed yet.\n\n"
                + "On your PC, open the SvR 2011 launcher and use \"Create APK Package\".\n"
                + "Copy " + kPackageName + " to this phone's games or Download folder, then tap Check again.",
                "Check again", this::next);
            return;
        }
        install(zip);
    }

    private void show(String message, String action, Runnable onClick) {
        text_.setText(message);
        progress_.setVisibility(ProgressBar.GONE);
        if (action != null) {
            button_.setText(action);
            button_.setOnClickListener(v -> onClick.run());
            button_.setVisibility(Button.VISIBLE);
        } else {
            button_.setVisibility(Button.GONE);
        }
    }

    private void install(File zip) {
        show("Installing the game from\n" + zip.getAbsolutePath() + " ...", null, null);
        progress_.setProgress(0);
        progress_.setVisibility(ProgressBar.VISIBLE);
        worker_ = new Thread(() -> {
            String error = extract(zip, gameFolder());
            runOnUiThread(() -> {
                worker_ = null;
                if (error != null) {
                    show("Installing failed:\n" + error, "Try again", this::next);
                } else {
                    show("Installed to games/" + kFolderName + ".\n\n"
                        + "You can delete " + zip.getName() + " now to free up space.",
                        "Play", this::next);
                }
            });
        }, "install");
        worker_.start();
    }

    // Extracts every entry of zip into dest (default.xex last, so an
    // interrupted install is redone). Returns null or the error.
    private String extract(File zip, File dest) {
        try (ZipFile file = new ZipFile(zip)) {
            long total = 0, done = 0;
            for (Enumeration<? extends ZipEntry> e = file.entries(); e.hasMoreElements();) {
                total += Math.max(0, e.nextElement().getSize());
            }
            String destPath = dest.getCanonicalPath() + File.separator;
            ZipEntry xex = null;
            byte[] buffer = new byte[1 << 20];
            for (Enumeration<? extends ZipEntry> e = file.entries(); e.hasMoreElements();) {
                ZipEntry entry = e.nextElement();
                if (entry.getName().equals("default.xex")) {
                    xex = entry;
                    continue;
                }
                done = extractEntry(file, entry, dest, destPath, buffer, done, total);
            }
            if (xex == null) return "the package has no default.xex";
            extractEntry(file, xex, dest, destPath, buffer, done, total);
            return null;
        } catch (IOException e) {
            Log.e(kTag, "install", e);
            return e.toString();
        }
    }

    private long extractEntry(ZipFile file, ZipEntry entry, File dest, String destPath,
                              byte[] buffer, long done, long total) throws IOException {
        File out = new File(dest, entry.getName());
        if (!out.getCanonicalPath().startsWith(destPath)) {
            throw new IOException("bad path in package: " + entry.getName());
        }
        if (entry.isDirectory()) {
            out.mkdirs();
            return done;
        }
        out.getParentFile().mkdirs();
        try (InputStream in = file.getInputStream(entry);
             OutputStream os = new FileOutputStream(out)) {
            int n;
            long last = 0;
            while ((n = in.read(buffer)) > 0) {
                os.write(buffer, 0, n);
                done += n;
                if (done - last > (16 << 20)) {
                    last = done;
                    final int p = total > 0 ? (int) (done * 1000 / total) : 0;
                    runOnUiThread(() -> progress_.setProgress(p));
                }
            }
        }
        return done;
    }
}
