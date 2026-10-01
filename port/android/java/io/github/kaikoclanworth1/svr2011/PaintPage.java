// WWE SmackDown vs. Raw 2011 - the launcher's Paint Tool tab on the phone (as
// the PC launcher's): the 10 pages of 20 logos (PaintTool: page 1 the game's
// 00PaintTool.pt, pages 2-10 Saves/.paint), with export / import of PNG
// images and delete. Each change backs the saves up first.

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.GridLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;

final class PaintPage {
    static final int kPages = 10, kSlots = 20;

    private final LauncherActivity a_;
    private GridLayout grid_;
    private TextView pageLabel_;
    private int page_ = 1, slot_ = -1;
    private byte[] data_;

    PaintPage(LauncherActivity a) { a_ = a; }

    View view() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Your Paint Tool logos: 10 pages of 20. Tap one to select it, then export it as a "
            + "PNG, replace it with your own image, or delete it.", 14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(8));
        c.addView(about);

        LinearLayout nav = new LinearLayout(a_);
        nav.setGravity(Gravity.CENTER_VERTICAL);
        Button prev = a_.button("◀", LauncherActivity.kCard);
        prev.setOnClickListener(v -> turn(-1));
        Button next = a_.button("▶", LauncherActivity.kCard);
        next.setOnClickListener(v -> turn(1));
        pageLabel_ = a_.text("", 17, LauncherActivity.kText);
        pageLabel_.setGravity(Gravity.CENTER);
        nav.addView(prev, new LinearLayout.LayoutParams(a_.dp(64), ViewGroup.LayoutParams.WRAP_CONTENT));
        nav.addView(pageLabel_, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        nav.addView(next, new LinearLayout.LayoutParams(a_.dp(64), ViewGroup.LayoutParams.WRAP_CONTENT));
        c.addView(nav);

        grid_ = new GridLayout(a_);
        grid_.setColumnCount(5);
        grid_.setPadding(0, a_.dp(10), 0, a_.dp(4));
        c.addView(grid_, a_.fullWidth(4));

        Button export = a_.button("Export PNG…", LauncherActivity.kCard);
        export.setOnClickListener(v -> exportOne());
        Button imp = a_.button("Import image…", LauncherActivity.kRed);
        imp.setOnClickListener(v -> importImage());
        c.addView(a_.pair(imp, export), a_.fullWidth(12));
        Button delete = a_.button("Delete logo", LauncherActivity.kCard);
        delete.setOnClickListener(v -> deleteLogo());
        Button all = a_.button("Export all…", LauncherActivity.kCard);
        all.setOnClickListener(v -> exportAll());
        c.addView(a_.pair(delete, all), a_.fullWidth(10));
        load();
        return c;
    }

    void turn(int d) {
        page_ = (page_ - 1 + d + kPages) % kPages + 1;
        slot_ = -1;
        load();
    }

    void load() {
        pageLabel_.setText("Page " + page_ + " of " + kPages);
        data_ = PaintTool.readPage(FileOps.saves(), page_);
        grid_.removeAllViews();
        if (data_ == null) {
            TextView none = a_.text("There is no Paint Tool save yet: open CREATE MODES > CREATE A SUPERSTAR > "
                + "PAINT TOOL in the game once.", 15, LauncherActivity.kDim);
            none.setPadding(a_.dp(4), a_.dp(12), a_.dp(4), a_.dp(12));
            GridLayout.LayoutParams lp = new GridLayout.LayoutParams();
            lp.columnSpec = GridLayout.spec(0, 5, 1f);
            grid_.addView(none, lp);
            return;
        }
        int cell = (a_.getResources().getDisplayMetrics().widthPixels - a_.dp(32) - a_.dp(30)) / 5;
        for (int k = 0; k < kSlots; k++) {
            final int slot = k;
            FrameLayout f = new FrameLayout(a_);
            GradientDrawable bg = new GradientDrawable();
            bg.setColor(Color.WHITE);
            bg.setCornerRadius(a_.dp(6));
            if (k == slot_) bg.setStroke(a_.dp(3), LauncherActivity.kRed);
            f.setBackground(bg);
            f.setPadding(a_.dp(3), a_.dp(3), a_.dp(3), a_.dp(3));
            ImageView img = new ImageView(a_);
            if (PaintTool.slotUsed(data_, k)) {
                img.setImageBitmap(Bitmap.createBitmap(PaintTool.getArgb(data_, k), 256, 256, Bitmap.Config.ARGB_8888));
            }
            img.setScaleType(ImageView.ScaleType.FIT_CENTER);
            f.addView(img, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            TextView n = a_.text(Integer.toString(k + 1), 11, 0xFF707078);
            f.addView(n, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.START));
            f.setOnClickListener(v -> {
                slot_ = slot;
                load();
            });
            GridLayout.LayoutParams lp = new GridLayout.LayoutParams();
            lp.width = cell;
            lp.height = cell;
            lp.setMargins(a_.dp(3), a_.dp(3), a_.dp(3), a_.dp(3));
            grid_.addView(f, lp);
        }
    }

    String logoName(int slot) {
        return (page_ == 1 ? "" : "Page " + page_ + " ") + "logo " + (slot + 1);
    }

    boolean needUsed() {
        if (data_ == null) return false;
        if (slot_ < 0) {
            a_.status("Choose a logo in the grid first.");
            return false;
        }
        if (!PaintTool.slotUsed(data_, slot_)) {
            a_.status("Logo " + (slot_ + 1) + " is empty.");
            return false;
        }
        return true;
    }

    static byte[] png(int[] argb) {
        Bitmap b = Bitmap.createBitmap(argb, 256, 256, Bitmap.Config.ARGB_8888);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        b.compress(Bitmap.CompressFormat.PNG, 100, out);
        return out.toByteArray();
    }

    void exportOne() {
        if (!needUsed()) return;
        int slot = slot_;
        String name = Character.toUpperCase(logoName(slot).charAt(0)) + logoName(slot).substring(1) + ".png";
        Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        save.addCategory(Intent.CATEGORY_OPENABLE);
        save.setType("image/png");
        save.putExtra(Intent.EXTRA_TITLE, name);
        a_.startForResult(save, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            try (java.io.OutputStream out = a_.getContentResolver().openOutputStream(data.getData())) {
                out.write(png(PaintTool.getArgb(data_, slot)));
                a_.status("Exported " + name + ".");
            } catch (Exception e) {
                a_.status("Exporting failed: " + e.getMessage());
            }
        });
    }

    void exportAll() {
        a_.startForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Uri tree = data.getData();
            final String[] result = new String[1];
            a_.status("Exporting…");
            a_.background(() -> {
                int n = 0;
                try {
                    Uri root = FileOps.treeRoot(tree);
                    for (int p = 1; p <= kPages; p++) {
                        byte[] d = PaintTool.readPage(FileOps.saves(), p);
                        if (d == null) continue;
                        for (int k = 0; k < kSlots; k++) {
                            if (!PaintTool.slotUsed(d, k)) continue;
                            String name = (p == 1 ? "Logo " : "Page " + p + " logo ") + (k + 1) + ".png";
                            FileOps.writeOut(a_.getContentResolver(), tree, root, name, "image/png", png(PaintTool.getArgb(d, k)));
                            n++;
                        }
                    }
                    result[0] = n == 0 ? "There are no logos to export." : "Exported " + n + " logos.";
                } catch (Exception e) {
                    result[0] = "Exporting failed: " + e.getMessage();
                }
            }, () -> a_.status(result[0]));
        });
    }

    // A change to this page: game closed, saves backed up, written.
    interface Change { void apply(byte[] page); }

    void change(String done, Change c) {
        if (!a_.gameClosed() || data_ == null) return;
        final String[] result = new String[1];
        final int page = page_;
        a_.background(() -> {
            try {
                FileOps.backup(" (before Paint Tool change)");
                byte[] d = PaintTool.readPage(FileOps.saves(), page);
                if (d == null) throw new IOException("the Paint Tool save can't be read");
                c.apply(d);
                if (!PaintTool.writePage(FileOps.saves(), page, d)) throw new IOException("the Paint Tool save can't be written");
                result[0] = done;
            } catch (IOException e) {
                result[0] = "The change failed: " + e.getMessage();
            }
        }, () -> {
            a_.status(result[0]);
            load();
        });
    }

    void importImage() {
        if (data_ == null) {
            a_.status("There is no Paint Tool save yet: open the Paint Tool in the game once.");
            return;
        }
        if (slot_ < 0) {
            a_.status("Choose a logo in the grid first.");
            return;
        }
        int slot = slot_;
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("image/*");
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            Bitmap bmp;
            try (InputStream in = a_.getContentResolver().openInputStream(data.getData())) {
                bmp = BitmapFactory.decodeStream(in);
            } catch (Exception e) {
                bmp = null;
            }
            if (bmp == null) {
                a_.status("That image can't be read.");
                return;
            }
            int[] argb = PaintTool.fitImage(bmp);
            Runnable go = () -> change("Logo " + (slot + 1) + " replaced.", d -> PaintTool.put(d, slot, argb));
            if (PaintTool.slotUsed(data_, slot)) a_.confirm("Replace logo " + (slot + 1) + "?", "Replace", go);
            else go.run();
        });
    }

    void deleteLogo() {
        if (!needUsed()) return;
        int slot = slot_;
        a_.confirm("Delete logo " + (slot + 1) + "? (The saves are backed up first.)", "Delete",
            () -> change("Logo " + (slot + 1) + " deleted.", d -> PaintTool.clear(d, slot)));
    }
}
