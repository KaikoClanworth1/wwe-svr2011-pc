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
        InstallActivity.init(this);
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
        PreferSixtyHz();
    }

    // The game runs at 60 fps; on a 120 Hz screen its frames land on 2 or 3
    // of the screen's refreshes in turn (8 / 25 ms: a steady judder). Asks for
    // the screen's 60 Hz mode (same size) while the game is in front.
    private void PreferSixtyHz() {
        try {
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode();
            android.view.Display.Mode best = null;
            for (android.view.Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() != current.getPhysicalWidth() ||
                    m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
                if (Math.abs(m.getRefreshRate() - 60f) < 1f) best = m;
            }
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            if (best != null) lp.preferredDisplayModeId = best.getModeId();
            lp.preferredRefreshRate = 60f;
            getWindow().setAttributes(lp);
            Log.i("SvR2011", "display: asked for 60 Hz (mode " + (best != null ? best.getModeId() : -1) + ")");
        } catch (Exception e) {
            Log.w("SvR2011", "display: 60 Hz request failed", e);
        }
    }

    // The game's surface votes for 60 fps itself (a fixed-rate source): the
    // compositor then runs the screen at 60 Hz (the window's preference
    // alone left the Fold's cover screen at 120 Hz).
    @Override
    protected org.libsdl.app.SDLSurface createSDLSurface(android.content.Context context) {
        org.libsdl.app.SDLSurface surface = super.createSDLSurface(context);
        surface.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
            @Override public void surfaceCreated(android.view.SurfaceHolder holder) { Sixty(holder); }
            @Override public void surfaceChanged(android.view.SurfaceHolder holder, int f, int w, int h) { Sixty(holder); }
            @Override public void surfaceDestroyed(android.view.SurfaceHolder holder) {}
        });
        return surface;
    }

    private static void Sixty(android.view.SurfaceHolder holder) {
        if (android.os.Build.VERSION.SDK_INT < 31) return;  // (Android 10 / 11: the display mode above only)
        try {
            holder.getSurface().setFrameRate(60f, android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                                             android.view.Surface.CHANGE_FRAME_RATE_ALWAYS);
        } catch (Exception e) {
            Log.w("SvR2011", "display: surface frame rate failed", e);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) PreferSixtyHz();  // (the Fold: the other screen has its own modes)
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
