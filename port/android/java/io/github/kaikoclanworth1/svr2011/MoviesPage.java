// WWE SmackDown vs. Raw 2011 - the launcher's Movies tab on the phone (as the
// PC launcher's): USER MOVIES for Create An Entrance, made from a video or a
// picture (and a strip below it: a superstar's, another picture or video, or
// black) into the game folder's Custom Movies (MovieMaker).

package io.github.kaikoclanworth1.svr2011;

import android.content.Intent;
import android.graphics.Bitmap;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

final class MoviesPage {
    // The superstars' titantron movies (movies/titantron/<id>.bik): their strips.
    static final String[][] kStars = {
        {"297", "ALICIA FOX"},
        {"160", "BATISTA"},
        {"224", "BETH PHOENIX"},
        {"125", "BIG SHOW"},
        {"294", "BRIE BELLA"},
        {"137", "CHAVO GUERRERO"},
        {"104", "CHRIS JERICHO"},
        {"276", "CHRISTIAN"},
        {"902", "CHRISTIAN OLD"},
        {"131", "CM PUNK"},
        {"205", "CODY RHODES"},
        {"424", "CRYME TYME"},
        {"281", "DAVID HART SMITH"},
        {"407", "D-GENERATION X"},
        {"275", "DOLPH ZIGGLER"},
        {"278", "DREW McINTYRE"},
        {"110", "EDGE"},
        {"423", "EDGE & CHRISTIAN"},
        {"901", "EDGE OLD"},
        {"265", "EVAN BOURNE"},
        {"292", "EVE"},
        {"268", "EZEKIEL JACKSON"},
        {"158", "FINLAY"},
        {"293", "GAIL KIM"},
        {"277", "GOLDUST"},
        {"401", "THE HART DYNASTY"},
        {"267", "JACK SWAGGER"},
        {"132", "JAKE ROBERTS"},
        {"122", "JIMMY SNUKA"},
        {"139", "JOHN CENA"},
        {"175", "JOHN MORRISON"},
        {"165", "JTG"},
        {"107", "KANE"},
        {"903", "MASKED KANE"},
        {"164", "KELLY KELLY"},
        {"176", "KOFI KINGSTON"},
        {"321", "LUKE GALLOWS"},
        {"179", "MARK HENRY"},
        {"291", "MARYSE"},
        {"112", "MATT HARDY"},
        {"174", "MELINA"},
        {"182", "MICHELLE McCOOL"},
        {"143", "MICKIE JAMES"},
        {"263", "MIKE KNOX"},
        {"218", "THE MIZ"},
        {"115", "MR. McMAHON"},
        {"135", "MVP"},
        {"295", "NIKKI BELLA"},
        {"271", "PRIMO"},
        {"161", "RANDY ORTON"},
        {"123", "REY MYSTERIO"},
        {"305", "RICKY STEAMBOAT"},
        {"306", "ROB VAN DAM"},
        {"100", "THE ROCK"},
        {"272", "R-TRUTH"},
        {"201", "SANTINO MARELLA"},
        {"178", "SHAD GASPARD"},
        {"145", "SHAWN MICHAELS"},
        {"279", "SHEAMUS"},
        {"208", "SHELTON BENJAMIN"},
        {"422", "SHOWMIZ"},
        {"101", "STONE COLD"},
        {"131", "THE STRAIGHT EDGE SOCIETY"},
        {"261", "TED DIBIASE"},
        {"169", "TERRY FUNK"},
        {"186", "THEODORE LONG"},
        {"102", "TRIPLE H"},
        {"103", "UNDERTAKER"},
        {"284", "VANCE ARCHER"},
        {"262", "VLADIMIR KOZLOV"},
        {"117", "WILLIAM REGAL"},
        {"283", "YOSHI TATSU"},
        {"219", "ZACK RYDER"},
        {"50", "CHRIS MASTERS"},
        {"51", "LEX LUGER"},
        {"52", "BRITISH BULLDOG"},
        {"53", "JUSTIN GABRIEL"},
        {"54", "DAVID OTUNGA"},
        {"55", "WADE BARRETT"},
        {"56", "LAYLA"},
        {"255", "WWE LOGO"},
        {"256", "LEGENDS LOGO"}
    };

    private final LauncherActivity a_;
    private String video_, strip_;
    private TextView videoLabel_, stripLabel_;
    private EditText name_;
    private Spinner fit_, length_;
    private ImageView preview_;
    private ProgressBar progress_;
    private LinearLayout list_;
    private final AtomicBoolean cancel_ = new AtomicBoolean();
    private boolean busy_;

    MoviesPage(LauncherActivity a) {
        a_ = a;
        MovieMaker.ctx = a.getApplicationContext();
    }

    static File folder() { return new File(InstallActivity.gameFolder(), "Custom Movies"); }

    View view() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Make an entrance movie from your own video or picture. In the game: CREATE AN "
            + "ENTRANCE → FINALIZE → MOVIE, after NONE.", 14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);

        LinearLayout src = a_.card(c, "Big screen");
        videoLabel_ = a_.text("No video chosen", 14, LauncherActivity.kDim);
        Button pickVideo = a_.button("Choose…", LauncherActivity.kRed);
        pickVideo.setOnClickListener(v -> pick(true));
        a_.row(src, "Video or picture", null, pickVideo);
        src.addView(videoLabel_);
        fit_ = spinner(new String[] {"Fill the screen", "Whole picture (bars)", "Stretch"}, 0);
        a_.row(src, "Fit", null, fit_);

        LinearLayout strip = a_.card(c, "Strip below (stage and ramp screens)");
        stripLabel_ = a_.text("Black", 14, LauncherActivity.kDim);
        Button star = a_.button("Superstar…", LauncherActivity.kCard);
        star.setOnClickListener(v -> pickStar());
        Button other = a_.button("Picture / video…", LauncherActivity.kCard);
        other.setOnClickListener(v -> pick(false));
        Button none = a_.button("None (black)", LauncherActivity.kCard);
        none.setOnClickListener(v -> {
            strip_ = null;
            stripLabel_.setText("Black");
            preview();
        });
        strip.addView(stripLabel_);
        strip.addView(a_.pair(star, other), a_.fullWidth(8));
        strip.addView(none, a_.fullWidth(8));
        TextView pad = a_.text("", 4, 0);
        strip.addView(pad);

        LinearLayout out = a_.card(c, "Movie");
        name_ = new EditText(a_);
        name_.setHint("Name");
        name_.setHintTextColor(LauncherActivity.kDim);
        name_.setTextColor(LauncherActivity.kText);
        name_.setSingleLine(true);
        name_.setMinWidth(a_.dp(160));
        name_.setGravity(Gravity.END);
        a_.row(out, "Name", "Letters, digits, space, - and _ (32 at most)", name_);
        length_ = spinner(new String[] {"Whole video (3 min at most)", "30 seconds", "60 seconds", "90 seconds"}, 1);
        a_.row(out, "Length", null, length_);

        preview_ = new ImageView(a_);
        preview_.setScaleType(ImageView.ScaleType.FIT_CENTER);
        preview_.setBackgroundColor(0xFF000000);
        c.addView(preview_, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, a_.dp(220)));
        Button prev = a_.button("Preview", LauncherActivity.kCard);
        prev.setOnClickListener(v -> preview());
        Button make = a_.button("Make movie", LauncherActivity.kRed);
        make.setOnClickListener(v -> make());
        c.addView(a_.pair(prev, make), a_.fullWidth(10));
        progress_ = new ProgressBar(a_, null, android.R.attr.progressBarStyleHorizontal);
        progress_.setMax(100);
        progress_.setVisibility(View.GONE);
        progress_.setProgressTintList(android.content.res.ColorStateList.valueOf(LauncherActivity.kRed));
        c.addView(progress_, a_.fullWidth(10));
        Button stop = a_.button("Stop", LauncherActivity.kCard);
        stop.setOnClickListener(v -> cancel_.set(true));
        c.addView(stop, a_.fullWidth(8));

        list_ = a_.card(c, "Your movies");
        refresh();
        return c;
    }

    Spinner spinner(String[] labels, int selected) {
        Spinner sp = new Spinner(a_, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> ad = new ArrayAdapter<String>(a_, android.R.layout.simple_spinner_item, labels) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(LauncherActivity.kText);
                return t;
            }
        };
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        sp.setSelection(selected);
        return sp;
    }

    int fit() {
        int i = fit_.getSelectedItemPosition();
        return i == 0 ? MovieMaker.FILL : i == 1 ? MovieMaker.FIT : MovieMaker.STRETCH;
    }

    int maxSeconds() {
        int i = length_.getSelectedItemPosition();
        return i == 0 ? 180 : i * 30;
    }

    void pick(boolean top) {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"video/*", "image/*", "application/octet-stream"});
        a_.startForResult(pick, (code, data) -> {
            if (code != android.app.Activity.RESULT_OK || data == null || data.getData() == null) return;
            String s = data.getData().toString();
            String name = MovieMaker.nameOf(s);
            if (top) {
                video_ = s;
                videoLabel_.setText(name);
                if (name_.getText().toString().trim().isEmpty()) {
                    String base = name.contains(".") ? name.substring(0, name.lastIndexOf('.')) : name;
                    name_.setText(cleanName(base));
                }
            } else {
                strip_ = s;
                stripLabel_.setText(name);
            }
            preview();
        });
    }

    // Where a superstar's movie is: the game folder, or the installed DLC.
    static File starMovie(String id) {
        File f = new File(InstallActivity.gameFolder(), "movies/titantron/" + id + ".bik");
        if (f.isFile()) return f;
        File dlc = new File(InstallActivity.gameFolder(), "UserData/0000000000000000/5451085D/00000002");
        File[] dirs = dlc.listFiles(File::isDirectory);
        if (dirs != null) {
            for (File d : dirs) {
                File g = new File(d, "movies/titantron/" + id + ".bik");
                if (g.isFile()) return g;
            }
        }
        return null;
    }

    void pickStar() {
        List<String> names = new ArrayList<>();
        List<File> files = new ArrayList<>();
        for (String[] s : kStars) {
            File f = starMovie(s[0]);
            if (f != null) {
                names.add(s[1]);
                files.add(f);
            }
        }
        if (names.isEmpty()) {
            a_.status("The superstars' movies aren't in the game folder (install the game first).");
            return;
        }
        new android.app.AlertDialog.Builder(a_, android.app.AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("A superstar's strip")
            .setItems(names.toArray(new String[0]), (d, which) -> {
                strip_ = files.get(which).getPath();
                stripLabel_.setText(names.get(which) + " (superstar's strip)");
                preview();
            })
            .setNegativeButton("Cancel", null)
            .show();
    }

    static String cleanName(String s) {
        StringBuilder b = new StringBuilder();
        for (char ch : s.toCharArray()) {
            if (Character.isLetterOrDigit(ch) || ch == ' ' || ch == '-' || ch == '_') b.append(ch);
            if (b.length() == 32) break;
        }
        return b.toString().trim();
    }

    void preview() {
        if (video_ == null || busy_) return;
        String v = video_, s = strip_;
        int fit = fit();
        final Bitmap[] out = new Bitmap[1];
        final String[] err = new String[1];
        a_.status("Making the preview…");
        a_.background(() -> {
            try {
                out[0] = MovieMaker.preview(v, s, fit, 2.0);
            } catch (Exception e) {
                err[0] = e.getMessage();
            }
        }, () -> {
            if (out[0] != null) {
                preview_.setImageBitmap(out[0]);
                a_.status("Preview: the movie at 2 s (the game shows the top at 16:9).");
            } else {
                a_.status(err[0]);
            }
        });
    }

    void make() {
        if (busy_) return;
        if (video_ == null) {
            a_.status("Choose the video first.");
            return;
        }
        String name = cleanName(name_.getText().toString());
        if (name.isEmpty()) {
            a_.status("Give the movie a name.");
            return;
        }
        File out = new File(folder(), name + ".bik");
        Runnable go = () -> run(out);
        if (out.exists()) a_.confirm(out.getName() + " is already there. Replace it?", "Replace", go);
        else go.run();
    }

    void run(File out) {
        busy_ = true;
        cancel_.set(false);
        progress_.setProgress(0);
        progress_.setVisibility(View.VISIBLE);
        String v = video_, s = strip_;
        int fit = fit(), max = maxSeconds();
        final String[] result = new String[1];
        a_.status("Making " + out.getName() + "…");
        a_.background(() -> {
            try {
                int n = MovieMaker.make(v, s, fit, max, out,
                    p -> a_.runOnUiThread(() -> progress_.setProgress(p)), cancel_);
                result[0] = "Made " + out.getName() + " (" + (n / MovieMaker.FPS) + " s). In the game: CREATE AN "
                    + "ENTRANCE → FINALIZE → MOVIE, after NONE.";
            } catch (Exception e) {
                result[0] = e.getMessage();
            }
        }, () -> {
            busy_ = false;
            progress_.setVisibility(View.GONE);
            a_.status(result[0]);
            refresh();
        });
    }

    void refresh() {
        list_.removeAllViews();
        File[] movies = folder().listFiles((d, n) -> n.toLowerCase().endsWith(".bik"));
        if (movies == null || movies.length == 0) {
            TextView none = a_.text("None yet.", 15, LauncherActivity.kDim);
            none.setPadding(0, a_.dp(14), 0, a_.dp(14));
            list_.addView(none);
            return;
        }
        Arrays.sort(movies, (x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        for (File m : movies) {
            Button del = a_.button("Delete", LauncherActivity.kBackground);
            del.setTextSize(13);
            del.setMinHeight(a_.dp(40));
            del.setOnClickListener(v -> a_.confirm("Delete " + m.getName() + "?", "Delete", () -> {
                a_.status(m.delete() ? "Deleted " + m.getName() + "." : "Could not delete " + m.getName() + ".");
                refresh();
            }));
            a_.row(list_, m.getName(), FileOps.human(m.length()), del);
        }
    }
}
