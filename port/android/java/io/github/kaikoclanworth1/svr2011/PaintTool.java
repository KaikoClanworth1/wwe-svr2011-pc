// WWE SmackDown vs. Raw 2011 - the Paint Tool save (Saves/00PaintTool.pt) and
// the port's extra pages (Saves/.paint/pNN_sMM.bin), as the PC launcher reads
// and writes them (launcher/svr2011_launcher.c, the pt_* functions). Byte for
// byte the same: checksums, the colour-mapped TGA (median cut, sorted with a
// port of the MSVC/UCRT qsort so ties split the same way) and the DXT5 DDS.
//
// Layout (big-endian):
//   0        u32 magic 0x02A78E9A, u32 version 3
//   k*S      slot k (S = 0x604CC); offsets below are from k*S:
//     8..51      header (+36 used, +44 / +48 width / height = 256)
//     52         the logo: 256 x 256 pixels, A R G B bytes (the editor's canvas)
//     0x40034    the same as an 8-bit colour-mapped TGA
//     0x50448    the same as a DXT5 DDS (128-byte header, 64 KB, PC little-endian blocks)
//     0x604C8    saved at: u16 year, month, day, hour, minute, second, weekday+1 (UTC)
//     0x604D0    u32 checksum of the slot
//   end-4    u32 magic + version + the 20 slot checksums

package io.github.kaikoclanworth1.svr2011;

import android.graphics.Bitmap;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Arrays;
import java.util.Calendar;
import java.util.TimeZone;

final class PaintTool {
    static final String FILE = "00PaintTool.pt";
    static final String PAGE_DIR = ".paint";
    static final int SLOTS = 20;
    static final int SLOT = 0x604CC;
    static final int BYTES = 8 + SLOTS * SLOT + 4;
    static final int MAGIC = 0x02A78E9A;
    static final int VERSION = 3;
    static final int W = 256;
    static final int CANVAS = 52;
    static final int TGA = 0x40034;
    static final int DDS = 0x50448;
    static final int STAMP = 0x604C8;
    static final int SUM = 0x604D0;
    static final int PAGES = 10;

    private static final byte[] USED_HEADER = bytes(
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00);
    private static final byte[] EMPTY_HEADER = Arrays.copyOf(bytes(
        0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00), 44);
    private static final byte[] TGA_HEADER = bytes(
        0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x08, 0x00, 0x04, 0x01);
    private static final byte[] DDS_HEADER = new byte[128];
    static {
        byte[] h = bytes(0x44, 0x44, 0x53, 0x20, 0x7C, 0x00, 0x00, 0x00, 0x07, 0x10, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00,
                         0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00);
        System.arraycopy(h, 0, DDS_HEADER, 0, h.length);
        DDS_HEADER[76] = 0x20;
        DDS_HEADER[80] = 0x04;
        DDS_HEADER[84] = 0x44; DDS_HEADER[85] = 0x58; DDS_HEADER[86] = 0x54; DDS_HEADER[87] = 0x35;
        DDS_HEADER[108] = 0x02; DDS_HEADER[109] = 0x10;
    }

    private PaintTool() {}

    private static byte[] bytes(int... v) {
        byte[] b = new byte[v.length];
        for (int i = 0; i < v.length; i++) b[i] = (byte) v[i];
        return b;
    }

    private static int u8(byte[] f, int p) { return f[p] & 0xFF; }
    private static int rd32(byte[] f, int p) {
        return (f[p] & 0xFF) << 24 | (f[p + 1] & 0xFF) << 16 | (f[p + 2] & 0xFF) << 8 | (f[p + 3] & 0xFF);
    }
    private static int rd16(byte[] f, int p) { return (f[p] & 0xFF) << 8 | (f[p + 1] & 0xFF); }
    private static void wr32(byte[] f, int p, int v) {
        f[p] = (byte) (v >>> 24); f[p + 1] = (byte) (v >>> 16); f[p + 2] = (byte) (v >>> 8); f[p + 3] = (byte) v;
    }

    private static int slot(int k) { return k * SLOT; }

    // ── checksums ──

    private static final int[] WORDS = { 8, 12, 16, 28, 32, 36, 40, 44, 48 };
    private static final int[] TB = { 0, 1, 2, 7, 16, 17 };
    private static final int[] TH = { 3, 5, 8, 10, 12, 14, 18 };

    /** The game's slot checksum (sub_827B3118): every field added as the type the game reads it as. */
    static int slotSum(byte[] f, int k) {
        final int b = slot(k), t = b + TGA, d = b + DDS, e = b + STAMP;
        int s = 0, i;
        for (i = 20; i < 28; i++)
            s += f[b + i];                                  // signed bytes
        for (i = 0; i < 9; i++)
            s += rd32(f, b + WORDS[i]);
        for (i = 0; i < W * W; i++)
            s += rd32(f, b + CANVAS + 4 * i);
        for (i = 0; i < 6; i++)
            s += u8(f, t + TB[i]);
        for (i = 0; i < 7; i++)
            s += rd16(f, t + TH[i]);
        for (i = 0; i < 256; i++)
            s += rd32(f, t + 20 + 4 * i);
        for (i = 0; i < W * W; i++)
            s += u8(f, t + 0x414 + i);
        for (i = 0; i < 32; i++)
            s += rd32(f, d + 4 * i);
        for (int j = 0; j < 2048; j++) {
            int q = d + 128 + 32 * j;
            s += rd16(f, q) + rd16(f, q + 2) + rd32(f, q + 4) + rd16(f, q + 8) + rd16(f, q + 10) + rd32(f, q + 12)
               + rd16(f, q + 16) + rd16(f, q + 18) + rd32(f, q + 20) + rd16(f, q + 24) + rd16(f, q + 26) + rd32(f, q + 28);
        }
        s += rd16(f, e);
        for (i = 2; i < 8; i++)
            s += u8(f, e + i);
        return s;
    }

    /** Recomputes every checksum (the game's sub_827B35C8). */
    static void fixSums(byte[] f) {
        int total = rd32(f, 0) + rd32(f, 4);
        for (int k = 0; k < SLOTS; k++) {
            int s = slotSum(f, k);
            wr32(f, slot(k) + SUM, s);
            total += s;
        }
        wr32(f, BYTES - 4, total);
    }

    static boolean valid(byte[] f) {
        if (f == null || f.length < BYTES || rd32(f, 0) != MAGIC || rd32(f, 4) != VERSION)
            return false;
        int total = rd32(f, 0) + rd32(f, 4);
        for (int k = 0; k < SLOTS; k++)
            total += rd32(f, slot(k) + SUM);
        return total == rd32(f, BYTES - 4);
    }

    static boolean slotUsed(byte[] f, int k) { return rd32(f, slot(k) + 36) != 0; }

    // ── files ──

    /** Page `page` (2..10, 1-based) slot k's (0-based) file. */
    static File pageFile(File savesDir, int page, int k) {
        return new File(new File(savesDir, PAGE_DIR), String.format("p%02d_s%02d.bin", page, k + 1));
    }

    /** Reads up to `n` bytes into b[off..]; returns how many were read (like fread). */
    private static int readFully(InputStream in, byte[] b, int off, int n) throws IOException {
        int got = 0;
        while (got < n) {
            int r = in.read(b, off + got, n - got);
            if (r < 0) break;
            got += r;
        }
        return got;
    }

    private static byte[] readWhole(File path) {
        if (!path.isFile())
            return null;
        byte[] buf = new byte[BYTES + 1];
        int got;
        try (InputStream in = new FileInputStream(path)) {
            got = readFully(in, buf, 0, BYTES + 1);
        } catch (IOException e) {
            return null;
        }
        if (got != BYTES)
            return null;
        byte[] f = Arrays.copyOf(buf, BYTES);
        return valid(f) ? f : null;
    }

    /**
     * Page `page` (1-based, 1..10) as a whole Paint Tool file: page 1 is 00PaintTool.pt
     * (null when missing or not valid), pages 2..10 are built from Saves/.paint/pNN_sMM.bin
     * (they need 00PaintTool.pt to exist, as on PC).
     */
    static byte[] readPage(File savesDir, int page) {
        File pt = new File(savesDir, FILE);
        if (page < 1 || page > PAGES)
            return null;
        if (page == 1 || !pt.exists())
            return readWhole(pt);
        byte[] buf = new byte[BYTES];
        wr32(buf, 0, MAGIC);
        wr32(buf, 4, VERSION);
        for (int k = 0; k < SLOTS; k++) {
            int got = 0;
            File file = pageFile(savesDir, page, k);
            if (file.isFile()) {
                try (InputStream in = new FileInputStream(file)) {
                    got = readFully(in, buf, slot(k) + 8, SLOT);
                } catch (IOException e) {
                    // got stays as read so far, as fread would
                }
            }
            if (got != SLOT || !slotUsed(buf, k))
                clear(buf, k);
        }
        fixSums(buf);
        return buf;
    }

    /** Renames `tmp` over `path` (replacing it, on any file system). */
    private static boolean replace(File tmp, File path) {
        try {
            java.nio.file.Files.move(tmp.toPath(), path.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            return true;
        } catch (IOException e) {
            return false;
        }
    }

    private static boolean writeFile(File path, byte[] b, int off, int n) {
        File tmp = new File(path.getPath() + ".new");
        boolean ok;
        try (FileOutputStream o = new FileOutputStream(tmp)) {
            o.write(b, off, n);
            o.getFD().sync();
            ok = true;
        } catch (IOException e) {
            ok = false;
        }
        if (!ok || !replace(tmp, path)) {
            tmp.delete();
            return false;
        }
        return true;
    }

    /**
     * Writes page `page` (1-based): page 1 as 00PaintTool.pt, pages 2..10 one file per used
     * slot (empty slots' files removed). Checksums are fixed first.
     */
    static boolean writePage(File savesDir, int page, byte[] f) {
        if (page < 1 || page > PAGES || f == null || f.length < BYTES)
            return false;
        fixSums(f);
        if (page == 1)
            return writeFile(new File(savesDir, FILE), f, 0, BYTES);
        File dir = new File(savesDir, PAGE_DIR);
        if (!dir.isDirectory() && !dir.mkdirs())
            return false;
        boolean ok = true;
        for (int k = 0; k < SLOTS; k++) {
            File file = pageFile(savesDir, page, k);
            if (!slotUsed(f, k)) {
                file.delete();
                continue;
            }
            File tmp = new File(file.getPath() + ".new");
            FileOutputStream o;
            try {
                o = new FileOutputStream(tmp);
            } catch (IOException e) {
                ok = false;
                continue;
            }
            try {
                o.write(f, slot(k) + 8, SLOT);
                o.getFD().sync();
            } catch (IOException e) {
                ok = false;
            }
            try {
                o.close();
            } catch (IOException e) {
                ok = false;
            }
            if (!ok || !replace(tmp, file)) {
                tmp.delete();
                ok = false;
            }
        }
        return ok;
    }

    // ── the slot's logo ──

    /** Slot k's logo as 65536 ARGB ints ((a<<24)|(r<<16)|(g<<8)|b, unpremultiplied). */
    static int[] getArgb(byte[] f, int k) {
        int[] out = new int[W * W];
        int c = slot(k) + CANVAS;
        for (int i = 0; i < W * W; i++, c += 4)
            out[i] = rd32(f, c);
        return out;
    }

    /** Puts a logo (256 x 256 ARGB ints) in slot k, as the game would have saved it. */
    static void put(byte[] f, int k, int[] argb) {
        final int b = slot(k);
        System.arraycopy(USED_HEADER, 0, f, b + 8, 44);
        for (int i = 0; i < W * W; i++)
            wr32(f, b + CANVAS + 4 * i, argb[i]);
        makeTga(f, b + TGA, argb);
        makeDds(f, b + DDS, argb);
        Calendar st = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        int year = st.get(Calendar.YEAR);
        f[b + STAMP] = (byte) (year >> 8); f[b + STAMP + 1] = (byte) year;
        f[b + STAMP + 2] = (byte) (st.get(Calendar.MONTH) + 1); f[b + STAMP + 3] = (byte) st.get(Calendar.DAY_OF_MONTH);
        f[b + STAMP + 4] = (byte) st.get(Calendar.HOUR_OF_DAY); f[b + STAMP + 5] = (byte) st.get(Calendar.MINUTE);
        f[b + STAMP + 6] = (byte) st.get(Calendar.SECOND);
        f[b + STAMP + 7] = (byte) st.get(Calendar.DAY_OF_WEEK);  // Sunday = 1, as wDayOfWeek + 1
    }

    /**
     * Puts a logo given as a palette (256 entries, A R G B bytes) and 256 x 256 8-bit pixels -
     * as a Created Superstar keeps it - in slot k: the slot's 8-bit picture is those exact bytes
     * (the PC launcher's pt_put_indexed).
     */
    static void putIndexed(byte[] f, int k, byte[] palette, byte[] pixels) {
        final int b = slot(k);
        int[] argb = new int[W * W];
        System.arraycopy(USED_HEADER, 0, f, b + 8, 44);
        for (int i = 0; i < W * W; i++) {
            argb[i] = rd32(palette, 4 * (pixels[i] & 0xFF));
            wr32(f, b + CANVAS + 4 * i, argb[i]);
        }
        System.arraycopy(TGA_HEADER, 0, f, b + TGA, 20);
        System.arraycopy(palette, 0, f, b + TGA + 20, 1024);
        System.arraycopy(pixels, 0, f, b + TGA + 0x414, W * W);
        makeDds(f, b + DDS, argb);
        Calendar st = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        int year = st.get(Calendar.YEAR);
        f[b + STAMP] = (byte) (year >> 8); f[b + STAMP + 1] = (byte) year;
        f[b + STAMP + 2] = (byte) (st.get(Calendar.MONTH) + 1); f[b + STAMP + 3] = (byte) st.get(Calendar.DAY_OF_MONTH);
        f[b + STAMP + 4] = (byte) st.get(Calendar.HOUR_OF_DAY); f[b + STAMP + 5] = (byte) st.get(Calendar.MINUTE);
        f[b + STAMP + 6] = (byte) st.get(Calendar.SECOND);
        f[b + STAMP + 7] = (byte) st.get(Calendar.DAY_OF_WEEK);
    }

    /** Slot k's 8-bit picture (palette at +20, pixels at +0x414 of the returned offset). */
    static int tgaOffset(int k) { return slot(k) + TGA; }

    /** Slot k's checksum as stored (the game's logo id for a Created Superstar's copy). */
    static int storedSum(byte[] f, int k) { return rd32(f, slot(k) + SUM); }

    /** Empties slot k (everything from +8 up to the time stamp, then the empty header). */
    static void clear(byte[] f, int k) {
        final int b = slot(k);
        Arrays.fill(f, b + 8, b + STAMP, (byte) 0);
        System.arraycopy(EMPTY_HEADER, 0, f, b + 8, 44);
    }

    // ── 256-colour palette by median cut (TGA) ──

    /** The colour list being cut: argb + pixel count, compared by one channel or the whole argb. */
    private static final class Colors {
        final int[] argb, count;
        int ch;          // channel shift for cmpChannel
        boolean whole;   // compare the whole argb (unsigned)

        Colors(int n) { argb = new int[n]; count = new int[n]; }

        int cmp(int a, int b) {
            if (whole)
                return Integer.compareUnsigned(argb[a], argb[b]) < 0 ? -1 : (argb[a] != argb[b] ? 1 : 0);
            return ((argb[a] >>> ch) & 0xFF) - ((argb[b] >>> ch) & 0xFF);
        }

        void swap(int a, int b) {
            if (a != b) {
                int t = argb[a]; argb[a] = argb[b]; argb[b] = t;
                t = count[a]; count[a] = count[b]; count[b] = t;
            }
        }
    }

    /** The UCRT qsort (CUTOFF 8, median of three, selection short sort) over c[base..base+num). */
    private static void qsort(Colors c, int base, int num) {
        final int cutoff = 8;
        int[] lostk = new int[64], histk = new int[64];
        int stkptr = 0;
        if (num < 2)
            return;
        int lo = base, hi = base + num - 1;
        for (;;) {
            int size = hi - lo + 1;
            if (size <= cutoff) {
                shortsort(c, lo, hi);
            } else {
                int mid = lo + size / 2;
                if (c.cmp(lo, mid) > 0) c.swap(lo, mid);
                if (c.cmp(lo, hi) > 0) c.swap(lo, hi);
                if (c.cmp(mid, hi) > 0) c.swap(mid, hi);
                int loguy = lo, higuy = hi;
                for (;;) {
                    if (mid > loguy) {
                        do { loguy++; } while (loguy < mid && c.cmp(loguy, mid) <= 0);
                    }
                    if (mid <= loguy) {
                        do { loguy++; } while (loguy <= hi && c.cmp(loguy, mid) <= 0);
                    }
                    do { higuy--; } while (higuy > mid && c.cmp(higuy, mid) > 0);
                    if (higuy < loguy)
                        break;
                    c.swap(loguy, higuy);
                    if (mid == higuy)
                        mid = loguy;
                }
                higuy++;
                if (mid < higuy) {
                    do { higuy--; } while (higuy > mid && c.cmp(higuy, mid) == 0);
                }
                if (mid >= higuy) {
                    do { higuy--; } while (higuy > lo && c.cmp(higuy, mid) == 0);
                }
                if (higuy - lo >= hi - loguy) {
                    if (lo < higuy) { lostk[stkptr] = lo; histk[stkptr] = higuy; ++stkptr; }
                    if (loguy < hi) { lo = loguy; continue; }
                } else {
                    if (loguy < hi) { lostk[stkptr] = loguy; histk[stkptr] = hi; ++stkptr; }
                    if (lo < higuy) { hi = higuy; continue; }
                }
            }
            --stkptr;
            if (stkptr < 0)
                return;
            lo = lostk[stkptr];
            hi = histk[stkptr];
        }
    }

    private static void shortsort(Colors c, int lo, int hi) {
        while (hi > lo) {
            int max = lo;
            for (int p = lo + 1; p <= hi; p++)
                if (c.cmp(p, max) > 0)
                    max = p;
            c.swap(max, hi);
            hi--;
        }
    }

    private static void makeTga(byte[] f, int t, int[] argb) {
        final int npx = W * W;
        Colors c = new Colors(npx);
        int[] boxLo = new int[256], boxHi = new int[256];
        int[] pal = new int[256];
        int n = 0, nb = 1, i, j;
        for (i = 0; i < npx; i++) { c.argb[i] = argb[i]; c.count[i] = 1; }
        c.whole = true;
        qsort(c, 0, npx);
        c.whole = false;
        for (i = 0; i < npx; i++) {                          // unique colours with counts
            if (n > 0 && c.argb[n - 1] == c.argb[i]) c.count[n - 1]++;
            else { c.argb[n] = c.argb[i]; c.count[n] = c.count[i]; n++; }
        }
        boxLo[0] = 0; boxHi[0] = n;
        while (nb < 256) {                                    // split the widest box
            int best = -1, bestw = 0, bestch = 0;
            for (i = 0; i < nb; i++) {
                if (boxHi[i] - boxLo[i] < 2) continue;
                for (j = 0; j < 32; j += 8) {
                    int lo = 255, hi = 0;
                    for (int k = boxLo[i]; k < boxHi[i]; k++) {
                        int v = (c.argb[k] >>> j) & 0xFF;
                        if (v < lo) lo = v;
                        if (v > hi) hi = v;
                    }
                    if (hi - lo > bestw) { bestw = hi - lo; best = i; bestch = j; }
                }
            }
            if (best < 0) break;
            c.ch = bestch;
            qsort(c, boxLo[best], boxHi[best] - boxLo[best]);
            {   // at the median by pixel count
                long total = 0, acc = 0;
                int mid = -1;
                for (int k = boxLo[best]; k < boxHi[best]; k++) total += c.count[k];
                for (int k = boxLo[best]; k < boxHi[best] - 1; k++) {
                    acc += c.count[k];
                    if (acc * 2 >= total) { mid = k + 1; break; }
                }
                if (mid < 0) mid = boxHi[best] - 1;
                boxLo[nb] = mid; boxHi[nb] = boxHi[best]; boxHi[best] = mid; nb++;
            }
        }
        for (i = 0; i < nb; i++) {                            // each box's average
            long[] s = new long[4];
            long w = 0;
            for (int k = boxLo[i]; k < boxHi[i]; k++) {
                for (j = 0; j < 4; j++) s[j] += (long) ((c.argb[k] >>> (8 * j)) & 0xFF) * c.count[k];
                w += c.count[k];
            }
            if (w != 0)
                pal[i] = (int) ((s[3] / w) << 24 | (s[2] / w) << 16 | (s[1] / w) << 8 | (s[0] / w));
        }
        System.arraycopy(TGA_HEADER, 0, f, t, 20);
        for (i = 0; i < 256; i++) wr32(f, t + 20 + 4 * i, pal[i]);
        for (i = 0; i < npx; i++) {                           // nearest palette entry
            int p = argb[i];
            int best = 0;
            long bd = 0xFFFFFFFFL;
            for (j = 0; j < nb; j++) {
                int q = pal[j];
                int da = (p >>> 24) - (q >>> 24), dr = ((p >>> 16) & 0xFF) - ((q >>> 16) & 0xFF),
                    dg = ((p >>> 8) & 0xFF) - ((q >>> 8) & 0xFF), db = (p & 0xFF) - (q & 0xFF);
                long dd = (da * da + dr * dr + dg * dg + db * db) & 0xFFFFFFFFL;
                if (dd < bd) { bd = dd; best = j; if (dd == 0) break; }
            }
            f[t + 0x414 + i] = (byte) best;
        }
    }

    // ── DXT5 (DDS) ──

    private static int to565(int r, int g, int b) {
        return (((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255)) & 0xFFFF;
    }

    private static void makeDds(byte[] f, int d, int[] argb) {
        int o = d + 128;
        int[] px = new int[16];
        int[] apal = new int[8];
        int[] cr = new int[4], cg = new int[4], cb = new int[4];
        System.arraycopy(DDS_HEADER, 0, f, d, 128);
        for (int by = 0; by < W / 4; by++)
            for (int bx = 0; bx < W / 4; bx++, o += 16) {
                int amin = 255, amax = 0, i;
                long abits = 0;
                int cbits = 0;
                int c0, c1;
                for (i = 0; i < 16; i++) {
                    int p = argb[(by * 4 + i / 4) * W + bx * 4 + i % 4];
                    int a = p >>> 24;
                    px[i] = p;
                    if (a < amin) amin = a;
                    if (a > amax) amax = a;
                }
                // alpha: 8 steps between max and min
                apal[0] = amax; apal[1] = amin;
                for (i = 1; i < 7; i++) apal[i + 1] = ((7 - i) * amax + i * amin) / 7;
                for (i = 0; i < 16; i++) {
                    int a = px[i] >>> 24, best = 0, bd = 1 << 30;
                    for (int k = 0; k < 8; k++) { int dd = Math.abs(a - apal[k]); if (dd < bd) { bd = dd; best = k; } }
                    abits |= (long) best << (3 * i);
                }
                f[o] = (byte) amax; f[o + 1] = (byte) amin;
                for (i = 0; i < 6; i++) f[o + 2 + i] = (byte) (abits >>> (8 * i));
                // colour: the block's two most different pixels, four-colour mode
                {
                    int best = -1, pa = 0, pb = 0;
                    for (int a2 = 0; a2 < 16; a2++)
                        for (int b2 = a2 + 1; b2 < 16; b2++) {
                            int dr = ((px[a2] >>> 16) & 0xFF) - ((px[b2] >>> 16) & 0xFF),
                                dg = ((px[a2] >>> 8) & 0xFF) - ((px[b2] >>> 8) & 0xFF),
                                db = (px[a2] & 0xFF) - (px[b2] & 0xFF), dd = dr * dr + dg * dg + db * db;
                            if (dd > best) { best = dd; pa = a2; pb = b2; }
                        }
                    c0 = to565((px[pa] >>> 16) & 0xFF, (px[pa] >>> 8) & 0xFF, px[pa] & 0xFF);
                    c1 = to565((px[pb] >>> 16) & 0xFF, (px[pb] >>> 8) & 0xFF, px[pb] & 0xFF);
                }
                if (c0 < c1) { int t = c0; c0 = c1; c1 = t; }
                if (c0 != c1) {
                    cr[0] = ((c0 >> 11) & 31) * 255 / 31; cg[0] = ((c0 >> 5) & 63) * 255 / 63; cb[0] = (c0 & 31) * 255 / 31;
                    cr[1] = ((c1 >> 11) & 31) * 255 / 31; cg[1] = ((c1 >> 5) & 63) * 255 / 63; cb[1] = (c1 & 31) * 255 / 31;
                    cr[2] = (2 * cr[0] + cr[1]) / 3; cg[2] = (2 * cg[0] + cg[1]) / 3; cb[2] = (2 * cb[0] + cb[1]) / 3;
                    cr[3] = (cr[0] + 2 * cr[1]) / 3; cg[3] = (cg[0] + 2 * cg[1]) / 3; cb[3] = (cb[0] + 2 * cb[1]) / 3;
                    for (i = 0; i < 16; i++) {
                        int r = (px[i] >>> 16) & 0xFF, g = (px[i] >>> 8) & 0xFF, b = px[i] & 0xFF, best = 0, bd = 1 << 30;
                        for (int k = 0; k < 4; k++) {
                            int dd = (r - cr[k]) * (r - cr[k]) + (g - cg[k]) * (g - cg[k]) + (b - cb[k]) * (b - cb[k]);
                            if (dd < bd) { bd = dd; best = k; }
                        }
                        cbits |= best << (2 * i);
                    }
                }
                f[o + 8] = (byte) c0; f[o + 9] = (byte) (c0 >> 8); f[o + 10] = (byte) c1; f[o + 11] = (byte) (c1 >> 8);
                for (i = 0; i < 4; i++) f[o + 12 + i] = (byte) (cbits >>> (8 * i));
            }
    }

    // ── images ──

    /**
     * Fits an image into 256 x 256 (aspect kept, centred, transparent around it), filtered
     * scaling, as 65536 ARGB ints for put().
     */
    static int[] fitImage(Bitmap src) {
        int w = src.getWidth(), h = src.getHeight(), sw, sh;
        int[] out = new int[W * W];
        if (w <= 0 || h <= 0)
            return out;
        if (w >= h) { sw = W; sh = (int) (((long) h * W + w / 2) / w); }
        else        { sh = W; sw = (int) (((long) w * W + h / 2) / h); }
        if (sw == 0) sw = 1;
        if (sh == 0) sh = 1;
        Bitmap s = src;
        if (s.getConfig() != Bitmap.Config.ARGB_8888) {
            Bitmap c = s.copy(Bitmap.Config.ARGB_8888, false);
            if (c != null) s = c;
        }
        Bitmap scaled = (sw != w || sh != h) ? Bitmap.createScaledBitmap(s, sw, sh, true) : s;
        int x0 = (W - sw) / 2, y0 = (W - sh) / 2;
        scaled.getPixels(out, y0 * W + x0, W, 0, 0, sw, sh);
        if (scaled != s && scaled != src) scaled.recycle();
        if (s != src) s.recycle();
        return out;
    }
}
