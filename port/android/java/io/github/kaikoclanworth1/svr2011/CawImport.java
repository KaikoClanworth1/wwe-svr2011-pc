// WWE SmackDown vs. Raw 2011 - bringing someone else's Created Superstars into
// your saves on the phone, as the PC launcher's Import Superstar
// (launcher/caw_import.c, launcher/stfs.c - see there for the formats):
//  - each Created Superstar (.cas, or an Xbox 360 package of one) goes into the
//    first free of the 50 slots: the .cas with its slot number changed (the
//    checksum moved by that byte sum's change), its record in SaveData.dat (from
//    a picked main save when it has the Superstar's record, else the .cas's copy
//    with a fresh save part), and its header;
//  - its Paint Tool logos go into free Paint Tool slots (any of the 10 pages),
//    as their exact palette + pixels, leaving out ones the Paint Tool has.
// The picker gives files, not their folder, so the main save the records come
// from (SaveData.dat, or the 360 package of it) and the port's extra logo files
// (Saves/.logos/<hash>.bin) are picked along with the .cas files.

package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

final class CawImport {
    static final int CAS_SIZE = 1347532;
    private static final int REC_IN_CAS = 0x1483C4, REC_BASE = 0x13FB8, REC_SIZE = 0x6BC, MAX_CAW = 50;
    private static final int SD_SUM_FROM = 0x1C, SD_SUM_TO = 0x812DC;
    private static final int CACHE0 = 20, CACHE_SIZE = 186580, ATTIRES = 4;
    private static final int LOW_USED = 12168, HIGH_USED = 12178, PALETTES = 12180, HIGH_PIXELS = 14228;
    private static final int LOW_PIXELS = 22420, HIGH_IDS = 145300, LOW_IDS = 186260, EXT_OFFSET = 160000;
    private static final int EXT_MAGIC = 0x584C4731, MAX_HIGH = 10, LOGO_BIN = 1024 + 65536;
    private static final int TITLE_ID = 0x5451085D;
    private static final int[][] TAIL = {  // the save's own part of a record (offset, byte)
        {0x54A, 0x02}, {0x54C, 0x01}, {0x667, 0x4D}, {0x66C, 0x01}, {0x66D, 0x03}, {0x676, 0xC7}, {0x677, 0x03},
        {0x678, 0x58}, {0x679, 0x05}, {0x683, 0x6A}, {0x689, 0x0E}, {0x68C, 0x01}, {0x690, 0x01}};

    /** A picked file: its name and bytes. */
    static final class Picked {
        final String name;
        final byte[] data;
        Picked(String name, byte[] data) { this.name = name; this.data = data; }
    }

    private static final class Logo {
        final byte[] palette = new byte[1024], pixels = new byte[65536];
        int id;
    }

    private CawImport() {}

    private static int be32(byte[] d, int o) {
        return (d[o] & 0xFF) << 24 | (d[o + 1] & 0xFF) << 16 | (d[o + 2] & 0xFF) << 8 | (d[o + 3] & 0xFF);
    }
    private static int le32(byte[] d, int o) {
        return (d[o] & 0xFF) | (d[o + 1] & 0xFF) << 8 | (d[o + 2] & 0xFF) << 16 | (d[o + 3] & 0xFF) << 24;
    }
    private static void wbe32(byte[] d, int o, int v) {
        d[o] = (byte) (v >>> 24); d[o + 1] = (byte) (v >>> 16); d[o + 2] = (byte) (v >>> 8); d[o + 3] = (byte) v;
    }
    private static int le24(byte[] d, int o) { return (d[o] & 0xFF) | (d[o + 1] & 0xFF) << 8 | (d[o + 2] & 0xFF) << 16; }
    private static int byteSum(byte[] d, int from, int to) {
        int s = 0;
        for (int i = from; i < to; i++) s += d[i] & 0xFF;
        return s;
    }

    // ── Xbox 360 packages (STFS) ──

    /** A 360 saved-game package's save file, or null if `d` isn't one of the game's. */
    static byte[] unpackage(byte[] d) {
        if (d.length < 0x1000) return null;
        String magic = new String(d, 0, 4, StandardCharsets.ISO_8859_1);
        if (!magic.equals("CON ") && !magic.equals("LIVE") && !magic.equals("PIRS")) return null;
        if (be32(d, 0x360) != TITLE_ID || be32(d, 0x344) != 1) return null;
        final int headerSize = be32(d, 0x340), sep = d[0x379 + 2] & 0xFF;
        final int ftCount = (d[0x379 + 3] & 0xFF) | (d[0x379 + 4] & 0xFF) << 8, ftBlock = le24(d, 0x379 + 5);
        final int allocated = be32(d, 0x379 + 0x1C);
        final int sex = (~sep) & 1, step0 = sex != 0 ? 0xAC : 0xAB, step1 = sex != 0 ? 0x723A : 0x718F;
        final int first = (headerSize + 0xFFF) & 0xFFFFF000;
        final int top = allocated <= 0xAA ? 0 : allocated <= 0x70E4 ? 1 : 2;
        if (top == 2 || ftCount == 0 || ftCount > 64) return null;
        Stfs k = new Stfs(d, sex, step0, step1, first, top, sep);
        byte[] table = k.read(ftBlock, ftCount, false);
        if (table == null) return null;
        for (int i = 0; i + 0x40 <= table.length; i += 0x40) {
            int flags = table[i + 0x28] & 0xFF;
            if ((flags & 0x3F) == 0 || (flags & 0x80) != 0) continue;
            int blocks = le24(table, i + 0x29), start = le24(table, i + 0x2F), size = be32(table, i + 0x34);
            if (blocks == 0 || size < 0 || size > blocks * 0x1000) return null;
            byte[] data = k.read(start, blocks, (flags & 0x40) != 0);
            return data == null ? null : Arrays.copyOf(data, size);
        }
        return null;
    }

    private static final class Stfs {
        final byte[] d;
        final int sex, step0, step1, first, top, sep;
        Stfs(byte[] d, int sex, int step0, int step1, int first, int top, int sep) {
            this.d = d; this.sex = sex; this.step0 = step0; this.step1 = step1; this.first = first; this.top = top; this.sep = sep;
        }
        long dataBlock(long b) {
            long r = (((b + 0xAA) / 0xAA) << sex) + b;
            if (b < 0xAA) return r;
            if (b < 0x70E4) return r + (((b + 0x70E4) / 0x70E4) << sex);
            return (1L << sex) + r + (((b + 0x70E4) / 0x70E4) << sex);
        }
        long next(long b) {
            long l0 = 0;
            if (b >= 0xAA) {
                l0 = (b / 0xAA) * step0 + (((b / 0x70E4) + 1) << sex);
                if (b / 0x70E4 != 0) l0 += 1L << sex;
            }
            long at = (l0 << 12) + first + (b % 0xAA) * 0x18;
            if (top == 0) {
                at += (long) (sep & 2) << 0xB;
            } else {
                long l1 = b < 0x70E4 ? step0 : (1L << sex) + (b / 0x70E4) * step1;
                long t = (l1 << 12) + first + ((long) (sep & 2) << 0xB) + (b / 0xAA) * 0x18 + 0x14;
                if (t >= d.length) return -1;
                at += (long) (d[(int) t] & 0x40) << 6;
            }
            if (at + 0x18 > d.length) return -1;
            int a = (int) at;
            return (d[a + 0x15] & 0xFF) << 16 | (d[a + 0x16] & 0xFF) << 8 | (d[a + 0x17] & 0xFF);
        }
        byte[] read(long start, int count, boolean inARow) {
            byte[] out = new byte[count * 0x1000];
            long b = start;
            for (int i = 0; i < count; i++) {
                long at = (dataBlock(b) << 12) + first;
                if (b < 0 || at + 0x1000 > d.length) return null;
                System.arraycopy(d, (int) at, out, i * 0x1000, 0x1000);
                b = inARow ? b + 1 : next(b);
            }
            return out;
        }
    }

    // ── the import ──

    static boolean isCaw(byte[] d) {
        return d.length == CAS_SIZE && be32(d, 0) == 0x4B && be32(d, 4) == 0x18 && be32(d, 8) == 7;
    }
    static boolean isMainSave(byte[] d) {
        return d.length >= SD_SUM_TO && be32(d, 8) == 0x1C && be32(d, 12) == 0x4B && be32(d, 16) == 0x18
            && be32(d, 20) == 3;
    }

    private static byte[] readFile(File f) {
        if (!f.isFile() || f.length() > 64L * 1024 * 1024) return null;
        byte[] b = new byte[(int) f.length()];
        try (InputStream in = new FileInputStream(f)) {
            int got = 0;
            while (got < b.length) {
                int r = in.read(b, got, b.length - got);
                if (r < 0) return null;
                got += r;
            }
        } catch (IOException e) {
            return null;
        }
        return b;
    }

    private static void writeFile(File f, byte[] b) throws IOException {
        File tmp = new File(f.getPath() + ".new");
        try (FileOutputStream o = new FileOutputStream(tmp)) {
            o.write(b);
            o.getFD().sync();
        }
        try {
            java.nio.file.Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException e) {
            tmp.delete();
            throw new IOException("could not write " + f.getName());
        }
    }

    /**
     * Imports the picked files into the saves folder `saves`; returns a summary for the
     * status line. Throws if nothing could be done (no main save there, ...).
     */
    static String run(File saves, List<Picked> picked) throws IOException {
        List<byte[]> caws = new ArrayList<>(), mains = new ArrayList<>();
        File logosDir = new File(saves, ".logos");
        for (Picked p : picked) {
            byte[] d = p.data, inner = unpackage(d);
            if (inner != null) d = inner;
            if (isCaw(d)) caws.add(d);
            else if (isMainSave(d)) mains.add(d);
            else if (d.length == LOGO_BIN && p.name.matches("(?i)[0-9a-f]{16}\\.bin")) {
                if (!logosDir.isDirectory()) logosDir.mkdirs();
                File to = new File(logosDir, p.name.toUpperCase().replace(".BIN", ".bin"));
                if (!to.exists()) writeFile(to, d);
            }
        }
        if (caws.isEmpty())
            throw new IOException("none of the picked files is a Created Superstar (.cas).");
        File sdFile = new File(saves, "SaveData.dat");
        byte[] sd = readFile(sdFile);
        if (sd == null || sd.length < SD_SUM_TO || be32(sd, 0x18) != byteSum(sd, SD_SUM_FROM, SD_SUM_TO))
            throw new IOException("there is no main save (SaveData.dat) the game accepts yet: start the game once first.");
        List<Logo> logos = new ArrayList<>();
        List<String> names = new ArrayList<>();
        int full = 0;
        for (byte[] cas : caws) {
            int slot = -1;
            for (int k = 0; k < MAX_CAW && slot < 0; k++) {
                boolean used = false;
                for (int i = 0; i < REC_SIZE && !used; i++) used = sd[REC_BASE + k * REC_SIZE + i] != 0;
                if (!used && !new File(saves, String.format("%02dCreateSuperStar.cas", k)).exists()) slot = k;
            }
            if (slot < 0) {
                full++;
                continue;
            }
            // the record: from a picked main save that has this Superstar's, else the .cas's copy
            byte[] rec = null;
            for (byte[] m : mains)
                for (int k = 0; k < MAX_CAW && rec == null; k++)
                    if (be32(m, REC_BASE + k * REC_SIZE + 0x65C) == be32(cas, 12))
                        rec = Arrays.copyOfRange(m, REC_BASE + k * REC_SIZE, REC_BASE + (k + 1) * REC_SIZE);
            if (rec == null) {
                rec = new byte[REC_SIZE];
                System.arraycopy(cas, REC_IN_CAS, rec, 0, 0x548);
                for (int[] t : TAIL) rec[t[0]] = (byte) t[1];
            }
            int at = REC_IN_CAS + 0x20, old = (cas[at] & 0xFF) + (cas[at + 1] & 0xFF);
            cas[at] = (byte) (slot >> 8);
            cas[at + 1] = (byte) slot;
            int delta = (cas[at] & 0xFF) + (cas[at + 1] & 0xFF) - old;
            for (int o : new int[] {12, 0x148FC8, 0x148DBC}) wbe32(cas, o, be32(cas, o) + delta);
            rec[0x20] = (byte) (slot >> 8);
            rec[0x21] = (byte) slot;
            System.arraycopy(cas, 12, rec, 0x65C, 4);
            Arrays.fill(rec, 0x69C, REC_SIZE, (byte) 0);
            StringBuilder name = new StringBuilder();
            for (int i = 0; i < 31 && (rec[0x22 + i] & 0xFF) >= 0x20 && (rec[0x22 + i] & 0xFF) < 0x7F; i++)
                name.append((char) rec[0x22 + i]);
            String file = String.format("%02dCreateSuperStar.cas", slot);
            writeFile(new File(saves, file), cas);
            byte[] h = new byte[328];
            h[3] = 1;
            h[7] = 1;
            String disp = String.format("%02d.CREATED SUPERSTAR", slot + 1);
            for (int i = 0; i < disp.length(); i++) h[8 + 2 * i + 1] = (byte) disp.charAt(i);
            for (int i = 0; i < file.length(); i++) h[264 + i] = (byte) file.charAt(i);
            h[320] = 0x54; h[321] = 0x51; h[322] = 0x08; h[323] = 0x5D;
            File info = new File(saves, ".info");
            if (!info.isDirectory()) info.mkdirs();
            writeFile(new File(info, file + ".header"), h);
            System.arraycopy(rec, 0, sd, REC_BASE + slot * REC_SIZE, REC_SIZE);
            wbe32(sd, 0x18, byteSum(sd, SD_SUM_FROM, SD_SUM_TO));
            writeFile(sdFile, sd);
            collectLogos(cas, logosDir, logos);
            names.add(name + " (slot " + (slot + 1) + ")");
        }
        int[] r = logosToPaint(saves, logos);
        StringBuilder s = new StringBuilder("Imported " + names.size() + " Created Superstar"
            + (names.size() == 1 ? "" : "s") + (names.isEmpty() ? "" : ": " + String.join(", ", names)) + ". Logos: "
            + r[0] + " added to the Paint Tool");
        if (r[1] > 0) s.append(", some you had already");
        if (r[2] > 0) s.append(", some didn't fit (the Paint Tool is full, or not made yet)");
        s.append('.');
        if (full > 0) s.append(" ").append(full).append(" didn't fit: all ").append(MAX_CAW).append(" slots are in use.");
        return s.toString();
    }

    private static void addLogo(List<Logo> list, byte[] src, int palette, int pixels, int size, int id) {
        Logo l = new Logo();
        System.arraycopy(src, palette, l.palette, 0, 1024);
        if (size == 256) {
            System.arraycopy(src, pixels, l.pixels, 0, 65536);
        } else {
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++) l.pixels[y * 256 + x] = src[pixels + (y / 2) * 128 + x / 2];
        }
        l.id = id;
        for (Logo o : list)
            if (Arrays.equals(o.palette, l.palette) && Arrays.equals(o.pixels, l.pixels)) return;
        list.add(l);
    }

    private static void collectLogos(byte[] cas, File logosDir, List<Logo> list) {
        for (int a = 0; a < ATTIRES; a++) {
            final int c = CACHE0 + a * CACHE_SIZE, e = c + EXT_OFFSET;
            final boolean high = cas[c + HIGH_USED] != 0 || cas[c + HIGH_USED + 1] != 0;
            if (high) {
                for (int i = 0; i < 2; i++)
                    if (cas[c + HIGH_USED + i] != 0)
                        addLogo(list, cas, c + PALETTES + 1024 * i, c + HIGH_PIXELS + 65536 * i, 256, be32(cas, c + HIGH_IDS + 4 * i));
            } else {
                for (int i = 0; i < 10; i++)
                    if (cas[c + LOW_USED + i] != 0)
                        addLogo(list, cas, c + PALETTES + 1024 * i, c + LOW_PIXELS + 16384 * i, 128, be32(cas, c + LOW_IDS + 4 * i));
            }
            if (!high || le32(cas, e) != EXT_MAGIC) continue;
            for (int i = 2; i < MAX_HIGH; i++) {
                if (cas[e + 8 + i] == 0) continue;
                long hash = 0;
                for (int b = 7; b >= 0; b--) hash = hash << 8 | (cas[e + 24 + 8 * i + b] & 0xFF);
                byte[] bin = readFile(new File(logosDir, String.format("%016X.bin", hash)));
                if (bin != null && bin.length == LOGO_BIN) addLogo(list, bin, 0, 1024, 256, le32(cas, e + 104 + 4 * i));
            }
        }
    }

    /** {added, had already, didn't fit} */
    private static int[] logosToPaint(File saves, List<Logo> logos) {
        int[] r = new int[3];
        if (logos.isEmpty()) return r;
        byte[][] pages = new byte[PaintTool.PAGES][];
        boolean[] dirty = new boolean[PaintTool.PAGES];
        for (int p = 0; p < PaintTool.PAGES; p++) pages[p] = PaintTool.readPage(saves, p + 1);
        if (pages[0] == null) {
            r[2] = logos.size();
            return r;
        }
        for (Logo l : logos) {
            boolean found = false, placed = false;
            for (int p = 0; p < PaintTool.PAGES && !found; p++)
                for (int k = 0; pages[p] != null && k < PaintTool.SLOTS && !found; k++) {
                    if (!PaintTool.slotUsed(pages[p], k)) continue;
                    int t = PaintTool.tgaOffset(k);
                    found = (l.id != 0 && PaintTool.storedSum(pages[p], k) == l.id)
                        || (Arrays.equals(Arrays.copyOfRange(pages[p], t + 20, t + 20 + 1024), l.palette)
                            && Arrays.equals(Arrays.copyOfRange(pages[p], t + 0x414, t + 0x414 + 65536), l.pixels));
                }
            if (found) {
                r[1]++;
                continue;
            }
            for (int p = 0; p < PaintTool.PAGES && !placed; p++)
                for (int k = 0; pages[p] != null && k < PaintTool.SLOTS && !placed; k++)
                    if (!PaintTool.slotUsed(pages[p], k)) {
                        PaintTool.putIndexed(pages[p], k, l.palette, l.pixels);
                        dirty[p] = placed = true;
                    }
            if (placed) r[0]++;
            else r[2]++;
        }
        for (int p = 0; p < PaintTool.PAGES; p++)
            if (dirty[p]) PaintTool.writePage(saves, p + 1, pages[p]);
        return r;
    }
}
