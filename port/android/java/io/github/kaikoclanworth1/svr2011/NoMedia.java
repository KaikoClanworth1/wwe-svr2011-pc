// WWE SmackDown vs. Raw 2011 - keeps the game's movies out of the phone's Gallery (suggestion 2a7eb2).
//
// Titantron and entrance videos land in the game folder: downloaded and bundled mods' movie.mp4 (Mods/Superstars/<mod>,
// Mods/.backup/...), the player's movies and their HD copies (Custom Movies, Custom Movies/.hd), the game's own
// (movies, movies/titantron) and the disc's DLC titantrons (UserData/<profile>/.../movies/titantron). Media scanners skip
// a folder holding a ".nomedia" file and everything under it (the legacy scanner looks up the path, MediaProvider hides
// the subtree), so one empty file at the top of each of those four is enough - and it stays when a mod's folder is
// replaced by an update. The game itself opens the files by path, which .nomedia doesn't touch.
//
// Made at the launcher's and the game's start (existing installs get it too) and after a mod install or a movie import
// (either can create its top folder). Only folders that exist get one: the game makes its own folders. Plain Java (no
// android.*): the PC JVM tests it.
package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.io.IOException;

final class NoMedia {
    static final String kName = ".nomedia";
    static final String[] kFolders = {"Custom Movies", "Mods", "movies", "UserData"};

    private NoMedia() {}

    // -> how many .nomedia files were made (0 when all were there, or game is null).
    static int ensure(File game) {
        if (game == null) return 0;
        int made = 0;
        for (String name : kFolders) {
            File dir = new File(game, name);
            if (!dir.isDirectory()) continue;
            File marker = new File(dir, kName);
            if (marker.exists()) continue;
            try {
                if (marker.createNewFile()) made++;
            } catch (IOException | SecurityException ignored) {
                // (a read-only or full card: the movies still play, they only show in the Gallery)
            }
        }
        return made;
    }
}
