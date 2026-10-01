// WWE SmackDown vs. Raw 2011 - USER MOVIES maker on the phone: the PC
// launcher's movie_maker.c composition in Java (the big screen on top, the
// strip below; black borders found and cut off; box-filtered scaling), with
// pictures from Android (videos: MediaMetadataRetriever, images:
// BitmapFactory) or Bink movies (MovieTools), and the PC launcher's Bink
// encoder (MovieTools) writing the frames.

package io.github.kaikoclanworth1.svr2011;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;

import java.io.File;
import java.util.Arrays;

final class MovieMaker {
    static final int W = 320, H = 320, TOP_H = 220, BOT_H = H - TOP_H, FPS = 30;
    static final int FIT = 0, FILL = 1, STRETCH = 2;
    static final int PART_TOP = 0, PART_BOTTOM = 1;
    static final int CF_SAMPLES = 24;
    static final int kMaxVideoWidth = 960;  // (decoded frames scaled down to this: the movie is 320 wide)

    interface Progress { void at(int percent); }

    // A picture: 0 black, 1 image, 2 video, 3 Bink movie (a part of it).
    static final class Pic {
        int kind, w, h;
        double par = 1.0;
        byte[] bgra;  // the current picture (B, G, R, A)
        int cx, cy, cw, ch;
        long duration;  // microseconds
        // video
        MediaMetadataRetriever mmr;
        int frameCount, current = -1;
        // Bink
        long bink;
        int by;
        byte[] binkFrame;

        void close() {
            if (mmr != null) {
                try {
                    mmr.release();
                } catch (Exception ignored) {
                }
                mmr = null;
            }
            if (bink != 0) {
                MovieTools.binkClose(bink);
                bink = 0;
            }
        }
    }

    // For content:// sources (picked in the system picker): names and streams.
    static android.content.Context ctx;

    static boolean isContent(String path) { return path.startsWith("content://"); }

    static String nameOf(String path) {
        return isContent(path) ? FileOps.displayName(ctx.getContentResolver(), android.net.Uri.parse(path))
                               : new File(path).getName();
    }

    static boolean isBink(String path) { return nameOf(path).toLowerCase().endsWith(".bik"); }

    static boolean isImage(String path) {
        String p = nameOf(path).toLowerCase();
        for (String e : new String[] {".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp", ".heic", ".heif"}) {
            if (p.endsWith(e)) return true;
        }
        return false;
    }

    // Bitmap -> B, G, R, A bytes.
    static void toBgra(Bitmap b, byte[] out) {
        int w = b.getWidth(), h = b.getHeight();
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);
        for (int i = 0; i < px.length; i++) {
            int c = px[i];
            out[i * 4] = (byte) c;
            out[i * 4 + 1] = (byte) (c >> 8);
            out[i * 4 + 2] = (byte) (c >> 16);
            out[i * 4 + 3] = (byte) 255;
        }
    }

    static Bitmap shrink(Bitmap b) {
        if (b == null || b.getWidth() <= kMaxVideoWidth) return b;
        int h = Math.max(1, Math.round(b.getHeight() * (float) kMaxVideoWidth / b.getWidth()));
        Bitmap s = Bitmap.createScaledBitmap(b, kMaxVideoWidth, h, true);
        if (s != b) b.recycle();
        return s;
    }

    // Opens a picture ("" or null: black). Throws with a message the player can read.
    static Pic open(String path, int part) throws Exception {
        Pic p = new Pic();
        if (path == null || path.isEmpty()) return p;
        if (!isContent(path) && !new File(path).isFile()) throw new Exception("Can't find " + path);
        if (isBink(path) && isContent(path)) {
            // (the Bink reader opens files: a copy in the cache)
            File copy = new File(ctx.getCacheDir(), "movie_" + (part == PART_TOP ? "top" : "strip") + ".bik");
            FileOps.copyIn(ctx.getContentResolver(), android.net.Uri.parse(path), copy);
            path = copy.getPath();
        }
        if (isBink(path)) {
            p.bink = MovieTools.binkOpen(path);
            if (p.bink == 0) throw new Exception(MovieTools.lastError());
            int bw = MovieTools.binkWidth(p.bink), bh = MovieTools.binkHeight(p.bink);
            p.binkFrame = new byte[bw * bh * 4];
            p.w = bw;
            p.h = bh;
            if (bw == W && bh == H) {
                p.by = part == PART_BOTTOM ? TOP_H : 0;
                p.h = part == PART_BOTTOM ? BOT_H : TOP_H;
                if (part == PART_TOP) p.par = (16.0 / 9.0) * TOP_H / W;
            }
            p.bgra = new byte[p.w * p.h * 4];
            p.duration = MovieTools.binkFrameTime(p.bink) * MovieTools.binkFrames(p.bink) / 10;
            p.kind = 3;
        } else if (isImage(path)) {
            Bitmap b;
            if (isContent(path)) {
                try (java.io.InputStream in = ctx.getContentResolver().openInputStream(android.net.Uri.parse(path))) {
                    b = BitmapFactory.decodeStream(in);
                }
            } else {
                b = BitmapFactory.decodeFile(path);
            }
            if (b == null) throw new Exception("Can't read the picture " + nameOf(path));
            b = shrink(b);
            p.w = b.getWidth();
            p.h = b.getHeight();
            p.bgra = new byte[p.w * p.h * 4];
            toBgra(b, p.bgra);
            p.kind = 1;
        } else {
            MediaMetadataRetriever m = new MediaMetadataRetriever();
            try {
                if (isContent(path)) m.setDataSource(ctx, android.net.Uri.parse(path));
                else m.setDataSource(path);
            } catch (Exception e) {
                m.release();
                throw new Exception("Can't read the video " + nameOf(path));
            }
            p.mmr = m;
            String d = m.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION);
            String n = m.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_FRAME_COUNT);
            p.duration = d == null ? 0 : Long.parseLong(d) * 1000;
            p.frameCount = n == null ? 0 : Integer.parseInt(n);
            Bitmap first = frame(p, 0);
            if (first == null) {
                p.close();
                throw new Exception("Can't decode the video " + nameOf(path));
            }
            p.w = first.getWidth();
            p.h = first.getHeight();
            p.bgra = new byte[p.w * p.h * 4];
            first.recycle();
            p.kind = 2;
        }
        // A superstar's strip is used as it is; anything else loses its black bars.
        if (p.kind == 3 && part == PART_BOTTOM) {
            p.cw = p.w;
            p.ch = p.h;
        } else {
            contentFind(p);
        }
        return p;
    }

    static Bitmap frame(Pic p, int index) {
        try {
            Bitmap b = p.frameCount > 0 ? p.mmr.getFrameAtIndex(Math.min(index, p.frameCount - 1))
                                        : p.mmr.getFrameAtTime(index * 1000000L / FPS, MediaMetadataRetriever.OPTION_CLOSEST);
            return shrink(b);
        } catch (Exception e) {
            return null;
        }
    }

    static void binkNext(Pic p) {
        if (!MovieTools.binkNext(p.bink)) return;
        MovieTools.binkBgra(p.bink, p.binkFrame);
        System.arraycopy(p.binkFrame, p.by * p.w * 4, p.bgra, 0, p.w * p.h * 4);
    }

    // Brings the picture to time t (microseconds); an ended video keeps its
    // last frame, or starts again if `loop`.
    static void seek(Pic p, long t, boolean loop) {
        if (p.kind == 3) {
            long ft = MovieTools.binkFrameTime(p.bink) / 10;
            int n = MovieTools.binkFrames(p.bink);
            long want = ft > 0 ? t / ft : 0;
            if (loop) want %= n;
            else if (want >= n) want = n - 1;
            if (want < MovieTools.binkPosition(p.bink) - 1) MovieTools.binkRewind(p.bink);
            while (MovieTools.binkPosition(p.bink) <= want) {
                int before = MovieTools.binkPosition(p.bink);
                binkNext(p);
                if (MovieTools.binkPosition(p.bink) == before) break;
            }
            return;
        }
        if (p.kind != 2) return;
        long tt = t;
        if (p.duration > 0) {
            if (loop) tt %= p.duration;
            else if (tt >= p.duration) tt = p.duration - 1;
        }
        int index = p.frameCount > 0 && p.duration > 0 ? (int) (tt * p.frameCount / p.duration) : (int) (tt * FPS / 1000000);
        if (index == p.current) return;
        Bitmap b = frame(p, index);
        if (b == null) return;
        if (b.getWidth() == p.w && b.getHeight() == p.h) toBgra(b, p.bgra);
        b.recycle();
        p.current = index;
    }

    // The parts of the picture that are not black (content_box in movie_maker.c).
    static int[] contentBox(Pic p) {
        int step = 2, x0 = p.w, y0 = p.h, x1 = 0, y1 = 0;
        for (int y = 0; y < p.h; y += step) {
            int n = 0;
            for (int x = 0; x < p.w; x += step) {
                int s = (y * p.w + x) * 4;
                if ((p.bgra[s] & 0xFF) + (p.bgra[s + 1] & 0xFF) + (p.bgra[s + 2] & 0xFF) > 3 * 32) n++;
            }
            if (n * step * 50 > p.w) {
                if (y < y0) y0 = y;
                if (y + step > y1) y1 = y + step;
            }
        }
        for (int x = 0; x < p.w; x += step) {
            int n = 0;
            for (int y = 0; y < p.h; y += step) {
                int s = (y * p.w + x) * 4;
                if ((p.bgra[s] & 0xFF) + (p.bgra[s + 1] & 0xFF) + (p.bgra[s + 2] & 0xFF) > 3 * 32) n++;
            }
            if (n * step * 50 > p.h) {
                if (x < x0) x0 = x;
                if (x + step > x1) x1 = x + step;
            }
        }
        return x1 > x0 && y1 > y0 ? new int[] {x0, y0, x1, y1} : null;
    }

    // Black borders that are part of the picture, measured across it (content_find).
    static void contentFind(Pic p) {
        int[] bx0 = new int[CF_SAMPLES], by0 = new int[CF_SAMPLES], bx1 = new int[CF_SAMPLES], by1 = new int[CF_SAMPLES];
        int n = 0;
        p.cx = 0;
        p.cy = 0;
        p.cw = p.w;
        p.ch = p.h;
        if (p.kind == 1) {
            int[] b = contentBox(p);
            if (b != null) {
                bx0[0] = b[0]; by0[0] = b[1]; bx1[0] = b[2]; by1[0] = b[3];
                n = 1;
            }
        } else if (p.kind == 2 && p.duration > 0) {
            for (int k = 1; k <= CF_SAMPLES; k++) {
                long want = p.duration * k / (CF_SAMPLES + 2);
                seek(p, want, false);
                int[] b = contentBox(p);
                if (b != null) {
                    bx0[n] = b[0]; by0[n] = b[1]; bx1[n] = b[2]; by1[n] = b[3];
                    n++;
                }
            }
            p.current = -1;
            Arrays.fill(p.bgra, (byte) 0);
        } else if (p.kind == 3) {
            int frames = Math.min(MovieTools.binkFrames(p.bink), 600);
            int every = Math.max(1, frames / CF_SAMPLES);
            for (int k = 0; k < frames && n < CF_SAMPLES; k++) {
                int before = MovieTools.binkPosition(p.bink);
                binkNext(p);
                if (MovieTools.binkPosition(p.bink) == before) break;
                if (k % every == every / 2) {
                    int[] b = contentBox(p);
                    if (b != null) {
                        bx0[n] = b[0]; by0[n] = b[1]; bx1[n] = b[2]; by1[n] = b[3];
                        n++;
                    }
                }
            }
            MovieTools.binkRewind(p.bink);
            Arrays.fill(p.bgra, (byte) 0);
        }
        if (n == 0) return;
        Arrays.sort(bx0, 0, n);
        Arrays.sort(by0, 0, n);
        Arrays.sort(bx1, 0, n);
        Arrays.sort(by1, 0, n);
        int x0 = bx0[n / 4], y0 = by0[n / 4], x1 = bx1[n - 1 - n / 4], y1 = by1[n - 1 - n / 4];
        if (x1 > p.w) x1 = p.w;
        if (y1 > p.h) y1 = p.h;
        if (x1 - x0 < p.w / 4 || y1 - y0 < p.h / 4) return;
        if (x0 * 1000 < p.w * 15) x0 = 0;
        if ((p.w - x1) * 1000 < p.w * 15) x1 = p.w;
        if (y0 * 1000 < p.h * 15) y0 = 0;
        if ((p.h - y1) * 1000 < p.h * 15) y1 = p.h;
        p.cx = x0;
        p.cy = y0;
        p.cw = x1 - x0;
        p.ch = y1 - y0;
    }

    // Box-filtered copy of a source rectangle into a destination rectangle (pic_draw).
    static void draw(Pic p, double sx, double sy, double sw, double sh, byte[] dst, int dstW, int dx, int dy, int dw, int dh) {
        if (p.bgra == null || dw <= 0 || dh <= 0) return;
        for (int y = 0; y < dh; y++) {
            double y0 = sy + sh * y / dh, y1 = sy + sh * (y + 1) / dh;
            int iy0 = (int) y0, iy1 = (int) (y1 + 0.999);
            if (iy0 < 0) iy0 = 0;
            if (iy1 > p.h) iy1 = p.h;
            if (iy1 <= iy0) iy1 = iy0 + 1;
            for (int x = 0; x < dw; x++) {
                double x0 = sx + sw * x / dw, x1 = sx + sw * (x + 1) / dw;
                int ix0 = (int) x0, ix1 = (int) (x1 + 0.999), n = 0;
                long a0 = 0, a1 = 0, a2 = 0;
                int o = ((dy + y) * dstW + dx + x) * 4;
                if (ix0 < 0) ix0 = 0;
                if (ix1 > p.w) ix1 = p.w;
                if (ix1 <= ix0) ix1 = ix0 + 1;
                for (int j = iy0; j < iy1 && j < p.h; j++) {
                    for (int i = ix0; i < ix1 && i < p.w; i++) {
                        int s = (j * p.w + i) * 4;
                        a0 += p.bgra[s] & 0xFF;
                        a1 += p.bgra[s + 1] & 0xFF;
                        a2 += p.bgra[s + 2] & 0xFF;
                        n++;
                    }
                }
                if (n > 0) {
                    dst[o] = (byte) (a0 / n);
                    dst[o + 1] = (byte) (a1 / n);
                    dst[o + 2] = (byte) (a2 / n);
                }
                dst[o + 3] = (byte) 255;
            }
        }
    }

    // The big screen: shown at 16:9, stored as 320 x 220.
    static void drawTop(Pic p, int fit, byte[] frame) {
        double screen = 16.0 / 9.0, sx = p.cx, sy = p.cy, sw = p.cw, sh = p.ch, aspect;
        int dx = 0, dy = 0, dw = W, dh = TOP_H;
        if (p.bgra == null) return;
        aspect = sw * p.par / sh;
        if (fit == FILL) {
            if (aspect > screen) {
                sw = sh * screen / p.par;
                sx = p.cx + (p.cw - sw) / 2;
            } else {
                sh = sw * p.par / screen;
                sy = p.cy + (p.ch - sh) / 2;
            }
        } else if (fit == FIT) {
            if (aspect > screen) {
                dh = (int) (TOP_H * screen / aspect + 0.5);
                dy = (TOP_H - dh) / 2;
            } else {
                dw = (int) (W * aspect / screen + 0.5);
                dx = (W - dw) / 2;
            }
        }
        draw(p, sx, sy, sw, sh, frame, W, dx, dy, dw, dh);
    }

    // The strip: 320 x 100, filled (cropped to its shape).
    static void drawBottom(Pic p, byte[] frame) {
        double strip = (double) W / BOT_H, sx = p.cx, sy = p.cy, sw = p.cw, sh = p.ch, aspect;
        if (p.bgra == null) return;
        aspect = sw * p.par / sh;
        if (aspect > strip) {
            sw = sh * strip / p.par;
            sx = p.cx + (p.cw - sw) / 2;
        } else {
            sh = sw * p.par / strip;
            sy = p.cy + (p.ch - sh) / 2;
        }
        draw(p, sx, sy, sw, sh, frame, W, 0, TOP_H, W, BOT_H);
    }

    static void compose(Pic top, Pic bottom, int fit, long t, byte[] frame) {
        for (int i = 0; i < W * H; i++) {
            frame[i * 4] = frame[i * 4 + 1] = frame[i * 4 + 2] = 0;
            frame[i * 4 + 3] = (byte) 255;
        }
        seek(top, t, false);
        seek(bottom, t, true);
        drawTop(top, fit, frame);
        drawBottom(bottom, frame);
    }

    static Bitmap toBitmap(byte[] bgra) {
        int[] px = new int[W * H];
        for (int i = 0; i < px.length; i++) {
            px[i] = 0xFF000000 | (bgra[i * 4 + 2] & 0xFF) << 16 | (bgra[i * 4 + 1] & 0xFF) << 8 | (bgra[i * 4] & 0xFF);
        }
        return Bitmap.createBitmap(px, W, H, Bitmap.Config.ARGB_8888);
    }

    // The movie's frame at `seconds` (the preview).
    static Bitmap preview(String video, String bottom, int fit, double seconds) throws Exception {
        Pic top = open(video, PART_TOP), bot = null;
        try {
            bot = open(bottom, PART_BOTTOM);
            byte[] frame = new byte[W * H * 4];
            compose(top, bot, fit, (long) (seconds * 1e6), frame);
            return toBitmap(frame);
        } finally {
            top.close();
            if (bot != null) bot.close();
        }
    }

    // Writes `out` (through out.part). Returns the frames written; throws with a readable message.
    static int make(String video, String bottom, int fit, int maxSeconds, File out, Progress progress,
                    java.util.concurrent.atomic.AtomicBoolean cancel) throws Exception {
        Pic top = open(video, PART_TOP), bot = null;
        long writer = 0;
        File part = new File(out.getPath() + ".part");
        try {
            if (top.kind == 0) throw new Exception("Choose the video.");
            bot = open(bottom, PART_BOTTOM);
            long length = (top.kind == 2 || top.kind == 3) && top.duration > 0 ? top.duration : 10_000_000L;
            if (maxSeconds > 0 && length > maxSeconds * 1_000_000L) length = maxSeconds * 1_000_000L;
            int n = (int) Math.max(1, (length * FPS + 999_999) / 1_000_000);
            out.getParentFile().mkdirs();
            writer = MovieTools.writerOpen(part.getPath(), n);
            if (writer == 0) throw new Exception("Could not create the movie file.");
            byte[] frame = new byte[W * H * 4];
            for (int i = 0; i < n; i++) {
                if (cancel.get()) throw new Exception("Stopped.");
                compose(top, bot, fit, (long) i * 1_000_000L / FPS, frame);
                int r = MovieTools.writerFrame(writer, frame);
                if (r == 0) throw new Exception("Out of memory.");
                if (r < 0) throw new Exception("Could not write the movie (storage full?).");
                progress.at((i + 1) * 100 / n);
            }
            boolean ok = MovieTools.writerClose(writer, true);
            writer = 0;
            if (!ok) throw new Exception("Could not write the movie (storage full?).");
            top.close();
            if (out.exists() && !out.delete()) throw new Exception("Could not replace " + out.getName() + ".");
            if (!part.renameTo(out)) throw new Exception("Could not save the movie file.");
            return n;
        } finally {
            if (writer != 0) MovieTools.writerClose(writer, false);
            part.delete();
            top.close();
            if (bot != null) bot.close();
        }
    }
}
