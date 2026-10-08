// WWE SmackDown vs. Raw 2011 - custom main menu backgrounds on the phone (as
// the PC launcher's backgrounds.c): a picture becomes a Background mod,
// Mods/Backgrounds/<id>/ with picture.dds (1024 x 512 DXT5, the game's own
// menu pictures' size and format) and manifest.txt (type=Background, id,
// name, logo=0|1). The game shows it after its own 12 (src/backgrounds_page.cpp).
//
// Where the main menu puts the 1024 x 512 picture (measured; see
// backgrounds.c): in the 1280 x 720 frame a texel lands at x = -59.75 +
// 1.8511 tx (tx < 320), x = 532.6 + 1.1471 (tx - 320) (the rest), y = -42.84 +
// 1.3819 ty - the game's own pictures are pre-distorted for it. The picture is
// scaled to cover the visible area (1280 x 665), centred (no squashing; what
// falls outside is cut), and every texel takes the picture's pixel where it
// lands, so it looks right on screen.

package io.github.kaikoclanworth1.svr2011;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

final class Backgrounds {
    private Backgrounds() {}

    static final int kW = 1024, kH = 512;
    static final double kX0 = -59.75, kXs1 = 1.8511, kXs2 = 1.1471, kY0 = -42.84, kYs = 1.3819;
    static final int kSeam = 320;
    static final double kViewW = 1280, kViewH = 664.7;

    static double screenX(int tx) {
        return tx < kSeam ? kX0 + kXs1 * (tx + 0.5) : kX0 + kXs1 * kSeam + kXs2 * (tx + 0.5 - kSeam);
    }

    static double screenY(int ty) { return kY0 + kYs * (ty + 0.5); }

    static File folder() { return new File(new File(InstallActivity.gameFolder(), "Mods"), "Backgrounds"); }

    // Makes the mod from the picture read from `in` (name: shown in the game).
    // Returns its folder.
    static File make(InputStream in, String name, boolean logo) throws IOException {
        Bitmap src = BitmapFactory.decodeStream(in);
        if (src == null || src.getWidth() < 16 || src.getHeight() < 16)
            throw new IOException("it is not a picture Android can read (PNG, JPG, WebP)");
        double scale = Math.max(kViewW / src.getWidth(), kViewH / src.getHeight());
        int sw = Math.max((int) kViewW, (int) Math.round(src.getWidth() * scale));
        int sh = Math.max((int) Math.round(kViewH), (int) Math.round(src.getHeight() * scale));
        Bitmap scaled = Bitmap.createScaledBitmap(src, sw, sh, true);
        if (scaled != src) src.recycle();
        int[] img = new int[sw * sh];
        scaled.getPixels(img, 0, sw, 0, 0, sw, sh);
        scaled.recycle();
        double ox = kViewW / 2 - sw / 2.0, oy = kViewH / 2 - sh / 2.0;
        int[] px = new int[kW * kH];  // ARGB
        for (int y = 0; y < kH; y++) {
            double fy = Math.min(Math.max(screenY(y) - oy - 0.5, 0), sh - 1);
            int y0 = (int) fy, y1 = Math.min(y0 + 1, sh - 1);
            double ay = fy - y0;
            for (int x = 0; x < kW; x++) {
                double fx = Math.min(Math.max(screenX(x) - ox - 0.5, 0), sw - 1);
                int x0 = (int) fx, x1 = Math.min(x0 + 1, sw - 1);
                double ax = fx - x0;
                int p00 = img[y0 * sw + x0], p01 = img[y0 * sw + x1], p10 = img[y1 * sw + x0], p11 = img[y1 * sw + x1];
                int v = 0xFF000000;
                for (int sh8 = 0; sh8 < 24; sh8 += 8) {
                    double c = (((p00 >> sh8) & 0xFF) * (1 - ax) + ((p01 >> sh8) & 0xFF) * ax) * (1 - ay)
                        + (((p10 >> sh8) & 0xFF) * (1 - ax) + ((p11 >> sh8) & 0xFF) * ax) * ay;
                    v |= ((int) (c + 0.5) & 0xFF) << sh8;
                }
                px[y * kW + x] = v;
            }
        }
        byte[] dds = new byte[128 + kW * kH];
        header(dds);
        int[] block = new int[16];
        for (int by = 0; by < kH / 4; by++)
            for (int bx = 0; bx < kW / 4; bx++) {
                for (int i = 0; i < 16; i++) block[i] = px[(by * 4 + i / 4) * kW + bx * 4 + i % 4];
                encode(block, dds, 128 + (by * (kW / 4) + bx) * 16);
            }

        String id = name.toLowerCase().replaceAll("[^a-z0-9_\\-]+", "_").replaceAll("^_+|_+$", "");
        if (id.length() > 40) id = id.substring(0, 40);
        if (id.isEmpty()) id = "background";
        File dir = new File(folder(), id);
        for (int n = 2; dir.exists() && n < 1000; n++) dir = new File(folder(), id + "_" + n);
        if (!dir.mkdirs()) throw new IOException("can't write to the game folder");
        try (OutputStream o = new FileOutputStream(new File(dir, "picture.dds"))) {
            o.write(dds);
        }
        try (OutputStream o = new FileOutputStream(new File(dir, "manifest.txt"))) {
            o.write(("type=Background\nid=" + dir.getName() + "\nname=" + name + "\nversion=1.0\nlogo=" + (logo ? 1 : 0)
                + "\n").getBytes(StandardCharsets.UTF_8));
        }
        return dir;
    }

    static void put32(byte[] d, int at, int v) {
        d[at] = (byte) v; d[at + 1] = (byte) (v >> 8); d[at + 2] = (byte) (v >> 16); d[at + 3] = (byte) (v >> 24);
    }

    // A DXT5 DDS header for 1024 x 512, one level.
    static void header(byte[] d) {
        d[0] = 'D'; d[1] = 'D'; d[2] = 'S'; d[3] = ' ';
        put32(d, 4, 124);
        put32(d, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
        put32(d, 12, kH);
        put32(d, 16, kW);
        put32(d, 20, kW * kH);
        put32(d, 76, 32);
        put32(d, 80, 0x4);
        d[84] = 'D'; d[85] = 'X'; d[86] = 'T'; d[87] = '5';
        put32(d, 108, 0x1000);
    }

    static int to565(int r, int g, int b) {
        return ((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255);
    }

    // One opaque 4 x 4 block -> 16 bytes of DXT5 (as backgrounds.c: the
    // colours' bounding box, inset, its diagonal turned to follow them).
    static void encode(int[] px, byte[] o, int at) {
        int[] mn = {255, 255, 255}, mx = {0, 0, 0}, mean = {0, 0, 0};
        for (int p : px) {
            int[] c = {(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF};
            for (int k = 0; k < 3; k++) {
                mean[k] += c[k];
                mn[k] = Math.min(mn[k], c[k]);
                mx[k] = Math.max(mx[k], c[k]);
            }
        }
        for (int k = 0; k < 3; k++) mean[k] /= 16;
        long covRg = 0, covBg = 0;
        for (int p : px) {
            int r = ((p >> 16) & 0xFF) - mean[0], g = ((p >> 8) & 0xFF) - mean[1], b = (p & 0xFF) - mean[2];
            covRg += r * g;
            covBg += b * g;
        }
        int[] c0 = new int[3], c1 = new int[3];
        for (int k = 0; k < 3; k++) {
            int inset = (mx[k] - mn[k]) / 16;
            c0[k] = mx[k] - inset;
            c1[k] = mn[k] + inset;
        }
        if (covRg < 0) { int t = c0[0]; c0[0] = c1[0]; c1[0] = t; }
        if (covBg < 0) { int t = c0[2]; c0[2] = c1[2]; c1[2] = t; }
        int e0 = to565(c0[0], c0[1], c0[2]), e1 = to565(c1[0], c1[1], c1[2]);
        if (e0 < e1) { int t = e0; e0 = e1; e1 = t; }
        o[at] = (byte) 255; o[at + 1] = (byte) 255;
        for (int i = 2; i < 8; i++) o[at + i] = 0;
        o[at + 8] = (byte) e0; o[at + 9] = (byte) (e0 >> 8); o[at + 10] = (byte) e1; o[at + 11] = (byte) (e1 >> 8);
        if (e0 == e1) {
            for (int i = 12; i < 16; i++) o[at + i] = 0;
            return;
        }
        int[][] pal = new int[4][3];
        pal[0][0] = ((e0 >> 11) & 31) * 255 / 31; pal[0][1] = ((e0 >> 5) & 63) * 255 / 63; pal[0][2] = (e0 & 31) * 255 / 31;
        pal[1][0] = ((e1 >> 11) & 31) * 255 / 31; pal[1][1] = ((e1 >> 5) & 63) * 255 / 63; pal[1][2] = (e1 & 31) * 255 / 31;
        for (int k = 0; k < 3; k++) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        }
        int bits = 0;
        for (int i = 0; i < 16; i++) {
            int r = (px[i] >> 16) & 0xFF, g = (px[i] >> 8) & 0xFF, b = px[i] & 0xFF, best = 0, bd = Integer.MAX_VALUE;
            for (int k = 0; k < 4; k++) {
                int dr = r - pal[k][0], dg = g - pal[k][1], db = b - pal[k][2];
                int dd = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
                if (dd < bd) { bd = dd; best = k; }
            }
            bits |= best << (2 * i);
        }
        put32(o, at + 12, bits);
    }
}
