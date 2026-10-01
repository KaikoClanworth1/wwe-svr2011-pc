// WWE SmackDown vs. Raw 2011 - the launcher's DLC tab on the phone (as the PC
// launcher's): picked files, folders or .zip archives are searched for this
// game's Xbox 360 content packages (LIVE / PIRS / CON with its title ID);
// downloadable content goes to the game folder's DLC (the game sets it up at
// its next start), title updates are skipped (not needed).

package io.github.kaikoclanworth1.svr2011;

import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class DlcPage {
    static final int kTitleId = 0x5451085D;
    static final int kHeaderSize = 0x491;

    private final LauncherActivity a_;
    private LinearLayout installed_;

    DlcPage(LauncherActivity a) { a_ = a; }

    static File dlcFolder() { return new File(InstallActivity.gameFolder(), "DLC"); }

    View view() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Add downloadable content: pick the DLC package files, a folder with them, or a "
            + ".zip. Packages for this game are copied to the game's DLC folder and set up the next time the game "
            + "starts. Title updates aren't needed and are skipped.", 14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);
        Button files = a_.button("Add files…", LauncherActivity.kRed);
        files.setOnClickListener(v -> pickFiles());
        Button folder = a_.button("Add a folder…", LauncherActivity.kCard);
        folder.setOnClickListener(v -> pickFolder());
        c.addView(a_.pair(files, folder), a_.fullWidth(14));
        installed_ = a_.card(c, "Installed DLC");
        refresh();
        return c;
    }

    // The package kind from its first bytes: 2 DLC, 1 title update, 0 not this game's.
    static int kind(byte[] h, int n) {
        if (n < kHeaderSize) return 0;
        String magic = new String(h, 0, 4, StandardCharsets.US_ASCII);
        if (!magic.equals("LIVE") && !magic.equals("PIRS") && !magic.equals("CON ")) return 0;
        if (be32(h, 0x360) != kTitleId) return 0;
        int type = be32(h, 0x344);
        return type == 0x00000002 ? 2 : type == 0x000B0000 ? 1 : 0;
    }

    static String title(byte[] h) {
        int len = 0;
        while (len + 1 < 0x80 && 0x411 + len + 1 < h.length && (h[0x411 + len] != 0 || h[0x412 + len] != 0)) len += 2;
        return new String(h, 0x411, len, StandardCharsets.UTF_16BE).trim();
    }

    static int be32(byte[] b, int at) {
        return (b[at] & 0xFF) << 24 | (b[at + 1] & 0xFF) << 16 | (b[at + 2] & 0xFF) << 8 | (b[at + 3] & 0xFF);
    }

    static int readFully(InputStream in, byte[] b) throws IOException {
        int n = 0, r;
        while (n < b.length && (r = in.read(b, n, b.length - n)) > 0) n += r;
        return n;
    }

    void refresh() {
        installed_.removeAllViews();
        File[] files = dlcFolder().listFiles(File::isFile);
        if (files == null || files.length == 0) {
            TextView none = a_.text("None yet.", 15, LauncherActivity.kDim);
            none.setPadding(0, a_.dp(14), 0, a_.dp(14));
            installed_.addView(none);
            return;
        }
        Arrays.sort(files, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File f : files) {
            String name = f.getName();
            try (InputStream in = new FileInputStream(f)) {
                byte[] h = new byte[kHeaderSize];
                if (kind(h, readFully(in, h)) == 2 && !title(h).isEmpty()) name = title(h);
            } catch (IOException ignored) {
            }
            TextView t = a_.text(name + "\n" + FileOps.human(f.length()), 15, LauncherActivity.kText);
            t.setPadding(0, a_.dp(10), 0, a_.dp(10));
            installed_.addView(t);
        }
    }

    // What one add found.
    static final class Result {
        final List<String> added = new ArrayList<>();
        int already, updates;
        String error;
    }

    void pickFiles() {
        if (!InstallActivity.installed()) {
            a_.status("Install the game first.");
            return;
        }
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null) return;
            List<Uri> uris = new ArrayList<>();
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++) uris.add(data.getClipData().getItemAt(i).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            run(r -> {
                for (Uri u : uris) addDocument(a_.getContentResolver(), u, FileOps.displayName(a_.getContentResolver(), u), r);
            });
        });
    }

    void pickFolder() {
        if (!InstallActivity.installed()) {
            a_.status("Install the game first.");
            return;
        }
        a_.startForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri tree = data.getData();
            run(r -> addTree(a_.getContentResolver(), tree, FileOps.treeRoot(tree), 0, r));
        });
    }

    interface Work { void run(Result r) throws IOException; }

    void run(Work work) {
        Result r = new Result();
        a_.status("Looking for DLC…");
        a_.background(() -> {
            try {
                work.run(r);
            } catch (Exception e) {
                r.error = e.getMessage();
            }
        }, () -> {
            refresh();
            if (r.error != null) {
                a_.status("Adding DLC failed: " + r.error);
            } else if (r.added.isEmpty() && r.already == 0) {
                a_.status("No DLC for this game was found" + (r.updates > 0 ? " (only title updates, not needed)." : "."));
            } else {
                a_.status(r.added.size() + " DLC package" + (r.added.size() == 1 ? "" : "s") + " installed"
                    + (r.already > 0 ? ", others already there" : "")
                    + (r.updates > 0 ? " (title update skipped: not needed)" : "")
                    + ". They are set up the next time the game starts"
                    + (r.added.isEmpty() ? "." : ": " + String.join(", ", r.added) + "."));
            }
        });
    }

    void addTree(ContentResolver cr, Uri tree, Uri dir, int depth, Result r) throws IOException {
        if (depth > 8) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(dir));
        try (Cursor c = cr.query(children, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE},
                null, null, null)) {
            while (c != null && c.moveToNext()) {
                Uri child = DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0));
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))) addTree(cr, tree, child, depth + 1, r);
                else addDocument(cr, child, c.getString(1), r);
            }
        }
    }

    void addDocument(ContentResolver cr, Uri uri, String name, Result r) throws IOException {
        String lower = name.toLowerCase();
        if (lower.endsWith(".rar") || lower.endsWith(".7z")) {
            throw new IOException(name + ": RAR and 7z archives can't be opened on the phone - extract them (or "
                + "zip them) first");
        }
        try (InputStream raw = cr.openInputStream(uri)) {
            if (raw == null) return;
            InputStream in = new BufferedInputStream(raw, 1 << 16);
            if (lower.endsWith(".zip")) {
                ZipInputStream zip = new ZipInputStream(in);
                ZipEntry e;
                while ((e = zip.getNextEntry()) != null) {
                    if (e.isDirectory()) continue;
                    String n = e.getName();
                    n = n.substring(n.lastIndexOf('/') + 1);
                    addStream(zip, n, e.getSize(), r);
                }
            } else {
                addStream(in, name, FileOps.documentSize(cr, uri), r);
            }
        }
    }

    // One file's bytes: copied to DLC if it's this game's downloadable content.
    void addStream(InputStream in, String name, long size, Result r) throws IOException {
        byte[] h = new byte[kHeaderSize];
        int n = readFully(in, h);
        int k = kind(h, n);
        if (k == 1) r.updates++;
        if (k != 2) return;
        File dest = new File(dlcFolder(), name);
        if (dest.isFile() && size >= 0 && dest.length() == size) {
            r.already++;
            return;
        }
        dest.getParentFile().mkdirs();
        File part = new File(dest.getPath() + ".part");
        try (OutputStream out = new FileOutputStream(part)) {
            out.write(h, 0, n);
            FileOps.copy(in, out);
        }
        if (dest.exists()) dest.delete();
        if (!part.renameTo(dest)) throw new IOException("can't write " + name);
        String t = title(h);
        r.added.add(t.isEmpty() ? name : t);
    }
}
