// WWE SmackDown vs. Raw 2011 - the game (SDL's activity running the port's
// native code; InstallActivity starts it once the game files are in place).

package io.github.kaikoclanworth1.svr2011;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    // While the game runs (the launcher's Saves / Paint Tool tabs wait: it keeps its saves open).
    static volatile boolean running;

    @Override
    protected void onDestroy() {
        running = false;
        super.onDestroy();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        running = true;
        // The native side's "program folder" (settings, saves, logs, disc
        // files): rex::filesystem::GetExecutableFolder() reads it. Set before
        // SDLActivity loads the libraries and starts the game's main.
        try {
            Os.setenv("REX_APP_FOLDER", InstallActivity.gameFolder().getAbsolutePath(), true);
            // Custom GPU drivers (src/gpu_driver.cpp): libadrenotools' hooks are with the app's libraries.
            Os.setenv("SVR2011_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("SVR2011_CACHE_DIR", getCacheDir().getAbsolutePath(), true);
            // Automated tests over USB debugging (tools/phone_session.ps1):
            // am start ... --es SVR2011_INPUT_FILE <file> --es SVR2011_USER_DATA <folder>
            // become the environment the game reads, as on the PC.
            Bundle extras = getIntent().getExtras();
            if (extras != null) {
                for (String key : extras.keySet()) {
                    Object value = extras.get(key);
                    if (key.startsWith("SVR2011_") && value != null) Os.setenv(key, value.toString(), true);
                }
            }
        } catch (ErrnoException e) {
            Log.e("SvR2011", "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
    }

    // Settings on the command line (tests: --es args "--audio_mute=true ...").
    @Override
    protected String[] getArguments() {
        Bundle extras = getIntent().getExtras();
        String args = extras != null ? extras.getString("args") : null;
        return args == null || args.trim().isEmpty() ? new String[0] : args.trim().split("\\s+");
    }

    @Override
    protected String[] getLibraries() {
        // Dependencies first: libmain.so (the game, with SDL_main) needs the
        // runtime (with SDL); the GPU plugin is loaded by name at startup.
        return new String[] { "c++_shared", "rexruntime", "rexgpu-xenos", "main" };
    }
}
