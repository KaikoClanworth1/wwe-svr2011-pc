// WWE SmackDown vs. Raw 2011 - the native renderer's shaders from the APK:
// assets/native_shaders (the Vulkan shader pack and the known pipelines list,
// tools/build_apk.py) copied into the game folder's native_shaders when the
// APK is newer than what's there (an install from the disc image on the
// phone has none; an APK update brings new ones).

package io.github.kaikoclanworth1.svr2011;

import android.content.Context;
import android.content.pm.PackageInfo;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

final class Shaders {
    private Shaders() {}

    static final String[] kFiles = {"shaders.spv.pak", "pipelines.list"};

    // This APK's build: its version code and when it was installed.
    static String stamp(Context c) {
        try {
            PackageInfo p = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return p.getLongVersionCode() + " " + p.lastUpdateTime;
        } catch (Exception e) {
            return "?";
        }
    }

    // Copies the shaders if the folder's are from another APK. Returns false on an error.
    static boolean install(Context c) {
        File dir = new File(InstallActivity.gameFolder(), "native_shaders");
        File mark = new File(dir, ".apk");
        String stamp = stamp(c);
        try {
            if (mark.isFile() && new String(Files.readAllBytes(mark.toPath()), StandardCharsets.UTF_8).equals(stamp)) {
                return true;
            }
        } catch (IOException ignored) {
        }
        dir.mkdirs();
        try {
            for (String name : kFiles) {
                File out = new File(dir, name), part = new File(dir, name + ".part");
                try (InputStream in = c.getAssets().open("native_shaders/" + name);
                     OutputStream os = new FileOutputStream(part)) {
                    FileOps.copy(in, os);
                } catch (java.io.FileNotFoundException e) {
                    part.delete();
                    continue;  // (an APK built without shaders)
                }
                if (out.exists()) out.delete();
                if (!part.renameTo(out)) return false;
            }
            try (OutputStream os = new FileOutputStream(mark)) {
                os.write(stamp.getBytes(StandardCharsets.UTF_8));
            }
            return true;
        } catch (IOException e) {
            return false;
        }
    }
}
