// WWE SmackDown vs. Raw 2011 - the game (SDL's activity running the port's
// native code; InstallActivity starts it once the game files are in place).

package io.github.kaikoclanworth1.svr2011;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The native side's "program folder" (settings, saves, logs, disc
        // files): rex::filesystem::GetExecutableFolder() reads it. Set before
        // SDLActivity loads the libraries and starts the game's main.
        try {
            Os.setenv("REX_APP_FOLDER", InstallActivity.gameFolder().getAbsolutePath(), true);
        } catch (ErrnoException e) {
            Log.e("SvR2011", "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        // Dependencies first: libmain.so (the game, with SDL_main) needs the
        // runtime (with SDL); the GPU plugin is loaded by name at startup.
        return new String[] { "c++_shared", "rexruntime", "rexgpu-xenos", "main" };
    }
}
