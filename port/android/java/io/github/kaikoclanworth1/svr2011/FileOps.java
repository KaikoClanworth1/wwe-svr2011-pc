// WWE SmackDown vs. Raw 2011 - file helpers for the launcher's pages: copies,
// moves, backups (the PC launcher's SaveBackups folders), and files in and
// out through the system picker (content:// documents and folder trees).

package io.github.kaikoclanworth1.svr2011;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

final class FileOps {
    private FileOps() {}

    static File saves() { return new File(InstallActivity.gameFolder(), "Saves"); }

    static File backups() { return new File(InstallActivity.gameFolder(), "SaveBackups"); }

    static void copy(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[1 << 20];
        int n;
        while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
    }

    static void copyFile(File from, File to) throws IOException {
        to.getParentFile().mkdirs();
        File part = new File(to.getPath() + ".part");
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(part)) {
            copy(in, out);
        }
        if (to.exists() && !to.delete()) throw new IOException("can't replace " + to.getName());
        if (!part.renameTo(to)) throw new IOException("can't write " + to.getName());
        to.setLastModified(from.lastModified());
    }

    // Copies a folder's contents (skipping .mount: the game's scratch folder).
    static void copyTree(File from, File to) throws IOException {
        to.mkdirs();
        File[] list = from.listFiles();
        if (list == null) return;
        for (File f : list) {
            if (f.getName().equals(".mount")) continue;
            File t = new File(to, f.getName());
            if (f.isDirectory()) copyTree(f, t);
            else copyFile(f, t);
        }
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File c : list) deleteTree(c);
        f.delete();
    }

    // Moves a file or folder (rename; copy + delete across file systems).
    static void move(File from, File to) throws IOException {
        to.getParentFile().mkdirs();
        if (from.renameTo(to)) return;
        if (from.isDirectory()) copyTree(from, to);
        else copyFile(from, to);
        deleteTree(from);
    }

    static String stamp() {
        return new SimpleDateFormat("yyyy-MM-dd HH-mm-ss", Locale.US).format(new Date());
    }

    // A copy of the whole Saves folder in SaveBackups/<date><suffix>, as the
    // PC launcher makes them. Returns the folder, or null if there was nothing.
    static File backup(String suffix) throws IOException {
        File src = saves();
        if (!src.isDirectory() || countSaves(src) == 0) return null;
        File dest = new File(backups(), stamp() + suffix);
        for (int i = 2; dest.exists(); i++) dest = new File(backups(), stamp() + suffix + " " + i);
        copyTree(src, dest);
        return dest;
    }

    // A save entry: not hidden (the dot folders), not desktop.ini.
    static boolean isSave(File f) {
        String n = f.getName();
        return !n.startsWith(".") && !n.equalsIgnoreCase("desktop.ini") && !n.endsWith(".part");
    }

    static int countSaves(File dir) {
        File[] list = dir.listFiles();
        int n = 0;
        if (list != null) for (File f : list) if (isSave(f)) n++;
        return n;
    }

    static long size(File f) {
        if (!f.isDirectory()) return f.length();
        long s = 0;
        File[] list = f.listFiles();
        if (list != null) for (File c : list) s += size(c);
        return s;
    }

    static String human(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return String.format(Locale.US, "%.0f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format(Locale.US, "%.1f MB", bytes / 1048576.0);
        return String.format(Locale.US, "%.2f GB", bytes / 1073741824.0);
    }

    // ── picked documents ─────────────────────────────────────────────────

    static String displayName(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        String p = uri.getLastPathSegment();
        return p == null ? "file" : p.substring(p.lastIndexOf('/') + 1);
    }

    static long documentSize(ContentResolver cr, Uri uri) {
        try (Cursor c = cr.query(uri, new String[] {OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getLong(0);
        } catch (Exception ignored) {
        }
        return -1;
    }

    static void copyIn(ContentResolver cr, Uri uri, File to) throws IOException {
        to.getParentFile().mkdirs();
        File part = new File(to.getPath() + ".part");
        try (InputStream in = cr.openInputStream(uri); OutputStream out = new FileOutputStream(part)) {
            if (in == null) throw new IOException("can't read " + uri);
            copy(in, out);
        }
        if (to.exists() && !to.delete()) throw new IOException("can't replace " + to.getName());
        if (!part.renameTo(to)) throw new IOException("can't write " + to.getName());
    }

    // A folder tree the player picked (ACTION_OPEN_DOCUMENT_TREE): its root document.
    static Uri treeRoot(Uri tree) {
        return DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
    }

    // The child of a tree folder with that name, or null.
    static Uri findChild(ContentResolver cr, Uri tree, Uri parentDoc, String name) {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree,
            DocumentsContract.getDocumentId(parentDoc));
        try (Cursor c = cr.query(children, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null)) {
            while (c != null && c.moveToNext()) {
                if (name.equals(c.getString(1))) return DocumentsContract.buildDocumentUriUsingTree(tree, c.getString(0));
            }
        } catch (Exception ignored) {
        }
        return null;
    }

    // Copies a file or folder into a picked tree folder. false if `name` is
    // already there (skipped, as the PC launcher does).
    static boolean copyOut(ContentResolver cr, Uri tree, Uri parentDoc, File from) throws IOException {
        if (findChild(cr, tree, parentDoc, from.getName()) != null) return false;
        if (from.isDirectory()) {
            Uri dir = DocumentsContract.createDocument(cr, parentDoc, DocumentsContract.Document.MIME_TYPE_DIR,
                from.getName());
            if (dir == null) throw new IOException("can't create " + from.getName());
            File[] list = from.listFiles();
            if (list != null) for (File f : list) copyOut(cr, tree, dir, f);
            return true;
        }
        Uri doc = DocumentsContract.createDocument(cr, parentDoc, "application/octet-stream", from.getName());
        if (doc == null) throw new IOException("can't create " + from.getName());
        try (InputStream in = new FileInputStream(from); OutputStream out = cr.openOutputStream(doc)) {
            if (out == null) throw new IOException("can't write " + from.getName());
            copy(in, out);
        }
        return true;
    }

    // Writes bytes as a new document in a picked tree (replacing a same-named one).
    static void writeOut(ContentResolver cr, Uri tree, Uri parentDoc, String name, String mime, byte[] data)
        throws IOException {
        Uri old = findChild(cr, tree, parentDoc, name);
        if (old != null) DocumentsContract.deleteDocument(cr, old);
        Uri doc = DocumentsContract.createDocument(cr, parentDoc, mime, name);
        if (doc == null) throw new IOException("can't create " + name);
        try (OutputStream out = cr.openOutputStream(doc)) {
            if (out == null) throw new IOException("can't write " + name);
            out.write(data);
        }
    }
}
