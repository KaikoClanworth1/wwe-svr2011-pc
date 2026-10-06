// WWE SmackDown vs. Raw 2011 - the launcher on the phone: the PC launcher's
// tabs for a touch screen (InstallActivity opens it once the game is
// installed). Play starts the game as the PC launcher does; Settings and
// Online read and write the game folder's svr2011.toml (the same keys as the
// PC launcher and the game's GRAPHICS page), each change saved at once.

package io.github.kaikoclanworth1.svr2011;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

public class LauncherActivity extends Activity {
    static final int kRed = 0xFFC8102E, kBackground = 0xFF101014, kCard = 0xFF1C1C22, kText = 0xFFF2F2F2,
        kDim = 0xFF9A9AA4, kLine = 0xFF2C2C34;

    private GameSettings settings_;
    private Drivers drivers_;
    private final List<Button> tabs_ = new ArrayList<>();
    private final List<View> pages_ = new ArrayList<>();
    private final List<Runnable> onShow_ = new ArrayList<>();
    private final List<String> names_ = new ArrayList<>();
    TextView status_;
    private boolean loading_;  // (filling the controls: no saves)

    int dp(float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().setStatusBarColor(kBackground);
        getWindow().setNavigationBarColor(kBackground);
        settings_ = new GameSettings(InstallActivity.gameFolder());

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(kBackground);

        // Header: the title with the red bar, as the PC launcher's.
        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setPadding(dp(16), dp(14), dp(16), dp(10));
        View bar = new View(this);
        bar.setBackgroundColor(kRed);
        header.addView(bar, new LinearLayout.LayoutParams(dp(6), ViewGroup.LayoutParams.MATCH_PARENT));
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding(dp(12), 0, 0, 0);
        TextView title = text("WWE SmackDown vs. Raw 2011", 22, kText);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        titles.addView(title);
        titles.addView(text("Launcher — version " + versionName(), 13, kDim));
        header.addView(titles);
        root.addView(header);

        // Tabs (scroll sideways on a narrow screen).
        HorizontalScrollView tabScroll = new HorizontalScrollView(this);
        tabScroll.setHorizontalScrollBarEnabled(false);
        LinearLayout tabRow = new LinearLayout(this);
        tabRow.setPadding(dp(8), 0, dp(8), 0);
        tabScroll.addView(tabRow);
        root.addView(tabScroll);
        View line = new View(this);
        line.setBackgroundColor(kLine);
        root.addView(line, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)));

        FrameLayout content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        status_ = text("", 13, kDim);
        status_.setPadding(dp(16), dp(6), dp(16), dp(10));
        root.addView(status_);

        addPage(tabRow, content, "Play", playPage());
        addPage(tabRow, content, "Settings", settingsPage());
        addPage(tabRow, content, "Online", onlinePage());
        SavesPage saves = new SavesPage(this);
        addPage(tabRow, content, "Saves", saves.view(), saves::refresh);
        PaintPage paint = new PaintPage(this);
        addPage(tabRow, content, "Paint Tool", paint.view(), paint::load);
        DlcPage dlc = new DlcPage(this);
        addPage(tabRow, content, "DLC", dlc.view(), dlc::refresh);
        ModsPage mods = new ModsPage(this);
        addPage(tabRow, content, "Mods", mods.view(), mods::refresh);
        MoviesPage movies = new MoviesPage(this);
        addPage(tabRow, content, "Movies", movies.view(), movies::refresh);
        InstallPage install = new InstallPage(this);
        addPage(tabRow, content, "Install", install.view(), install::refresh);
        String tab = getIntent().getStringExtra("tab");
        select(Math.max(0, tab == null ? 0 : names_.indexOf(tab)));
        // The shaders from this APK into the game folder (when it's newer).
        if (InstallActivity.installed()) background(() -> Shaders.install(this), null);
        if (getPreferences(MODE_PRIVATE).getBoolean("check_updates", true)) checkUpdates(false);
        // Edge to edge (Android 15+): keep clear of the status and navigation bars
        // and the camera cut-out.
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets bars = insets.getInsets(android.view.WindowInsets.Type.systemBars()
                | android.view.WindowInsets.Type.displayCutout());
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            return android.view.WindowInsets.CONSUMED;
        });
        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // (the game's GRAPHICS page may have changed settings meanwhile)
        settings_.load();
        refresh();
        if (drivers_ != null) {
            drivers_.checkCrash();
            drivers_.offerBuiltIn();  // (Adreno 710/720/722: once)
        }
    }

    String versionName() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    // ── shared by the pages ──────────────────────────────────────────────

    void status(String s) { status_.setText(s); }

    // Something that changes the saves: not while the game has them open.
    boolean gameClosed() {
        if (!GameActivity.running) return true;
        status("Close the game first: it keeps its save files open while it runs.");
        return false;
    }

    void confirm(String message, String action, Runnable onYes) {
        new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setMessage(message)
            .setPositiveButton(action, (d, w) -> onYes.run())
            .setNegativeButton("Cancel", null)
            .show();
    }

    // The system file picker and other activities' results, by request code.
    interface Result { void run(int resultCode, Intent data); }
    private final java.util.Map<Integer, Result> results_ = new java.util.HashMap<>();
    private int nextRequest_ = 1000;

    void startForResult(Intent intent, Result onResult) {
        int code = nextRequest_++;
        results_.put(code, onResult);
        try {
            startActivityForResult(intent, code);
        } catch (Exception e) {
            results_.remove(code);
            status("Could not open the file picker: " + e.getMessage());
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        Result r = results_.remove(requestCode);
        if (r != null) r.run(resultCode, data);
        else super.onActivityResult(requestCode, resultCode, data);
    }

    // Work off the UI thread, then `done` on it.
    void background(Runnable work, Runnable done) {
        new Thread(() -> {
            work.run();
            if (done != null) runOnUiThread(done);
        }, "launcher").start();
    }

    LinearLayout.LayoutParams fullWidth(int topMargin) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(topMargin);
        return lp;
    }

    // Two buttons side by side.
    LinearLayout pair(Button a, Button b) {
        LinearLayout r = new LinearLayout(this);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        lp.rightMargin = dp(6);
        r.addView(a, lp);
        LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1);
        lp2.leftMargin = dp(6);
        r.addView(b, lp2);
        return r;
    }

    // ── building blocks ──────────────────────────────────────────────────

    TextView text(String s, float sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        return t;
    }

    GradientDrawable rounded(int color, float radius) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(color);
        d.setCornerRadius(dp(radius));
        return d;
    }

    Button button(String label, int color) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(16);
        b.setBackground(rounded(color, 10));
        b.setMinHeight(dp(52));
        return b;
    }

    // A page: a scrolling column of cards.
    LinearLayout column() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(12), dp(16), dp(16));
        return c;
    }

    LinearLayout card(LinearLayout column, String heading) {
        TextView h = text(heading.toUpperCase(), 12, kDim);
        h.setTypeface(Typeface.DEFAULT_BOLD);
        h.setLetterSpacing(0.08f);
        h.setPadding(dp(4), dp(10), 0, dp(6));
        column.addView(h);
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setBackground(rounded(kCard, 12));
        c.setPadding(dp(14), dp(4), dp(14), dp(4));
        column.addView(c);
        return c;
    }

    // A row: the label (and a hint under it) with a control at the right.
    void row(LinearLayout card, String label, String hint, View control) {
        LinearLayout r = new LinearLayout(this);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setMinimumHeight(dp(56));
        r.setPadding(0, dp(6), 0, dp(6));
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.addView(text(label, 16, kText));
        if (hint != null) texts.addView(text(hint, 12, kDim));
        r.addView(texts, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        r.addView(control);
        if (card.getChildCount() > 0) {
            View sep = new View(this);
            sep.setBackgroundColor(kLine);
            card.addView(sep, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));
        }
        card.addView(r);
    }

    void addPage(LinearLayout tabRow, FrameLayout content, String name, View page) {
        addPage(tabRow, content, name, page, null);
    }

    void addPage(LinearLayout tabRow, FrameLayout content, String name, View page, Runnable onShow) {
        names_.add(name);
        onShow_.add(onShow);
        final int index = tabs_.size();
        Button tab = new Button(this);
        tab.setText(name);
        tab.setAllCaps(false);
        tab.setTextSize(15);
        tab.setBackgroundColor(Color.TRANSPARENT);
        tab.setMinHeight(dp(48));
        tab.setPadding(dp(16), 0, dp(16), 0);
        tab.setOnClickListener(v -> select(index));
        tabRow.addView(tab);
        tabs_.add(tab);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        content.addView(scroll);
        pages_.add(scroll);
    }

    void select(int index) {
        for (int i = 0; i < tabs_.size(); i++) {
            boolean on = i == index;
            tabs_.get(i).setTextColor(on ? Color.WHITE : kDim);
            tabs_.get(i).setTypeface(on ? Typeface.DEFAULT_BOLD : Typeface.DEFAULT);
            tabs_.get(i).setBackground(on ? underline() : null);
            pages_.get(i).setVisibility(on ? View.VISIBLE : View.GONE);
        }
        // The chosen tab centred in the tab bar (its neighbours show there are more).
        Button tab = tabs_.get(index);
        HorizontalScrollView bar = (HorizontalScrollView) tab.getParent().getParent();
        bar.post(() -> bar.smoothScrollTo(tab.getLeft() + tab.getWidth() / 2 - bar.getWidth() / 2, 0));
        if (onShow_.get(index) != null) onShow_.get(index).run();
    }

    android.graphics.drawable.Drawable underline() {
        GradientDrawable d = new GradientDrawable();
        d.setColor(Color.TRANSPARENT);
        android.graphics.drawable.LayerDrawable l = new android.graphics.drawable.LayerDrawable(
            new android.graphics.drawable.Drawable[] {d, new android.graphics.drawable.ColorDrawable(kRed)});
        l.setLayerInset(1, dp(12), dp(44), dp(12), 0);
        return l;
    }

    void saved(String what) {
        if (loading_) return;
        status_.setText(settings_.save() ? what + " saved." : "Could not save the settings.");
    }

    // ── Play ─────────────────────────────────────────────────────────────

    View playPage() {
        LinearLayout c = column();
        LinearLayout about = card(c, "Game");
        TextView desc = text("The PC port of the 2010 Xbox 360 game, statically recompiled from the original "
            + "game code — on your phone.", 15, kText);
        desc.setPadding(0, dp(10), 0, dp(6));
        about.addView(desc);
        TextView folder = text("Game folder: games/" + InstallActivity.kFolderName, 13, kDim);
        folder.setPadding(0, 0, 0, dp(10));
        about.addView(folder);

        Button play = button("Play", kRed);
        play.setTextSize(22);
        play.setTypeface(Typeface.DEFAULT_BOLD);
        play.setMinHeight(dp(72));
        play.setOnClickListener(v -> play());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(18);
        c.addView(play, lp);

        // Report a problem: a zip of the logs, crash reports and settings in Download.
        Button report = button("Report a problem…", kBackground);
        report.setOnClickListener(v -> {
            final String[] result = new String[1];
            background(() -> {
                try {
                    ProblemReport.Result r = ProblemReport.make(InstallActivity.gameFolder(), versionName());
                    result[0] = "Saved Download/" + r.zip.getName() + " (" + r.logs + " game log"
                        + (r.logs == 1 ? "" : "s") + ", " + r.crashes + " crash report" + (r.crashes == 1 ? "" : "s")
                        + "; your online password and token are left out). Send it with a short description of "
                        + "what happened.";
                } catch (Exception e) {
                    result[0] = "The report couldn't be made: " + e.getMessage();
                }
            }, () -> status(result[0]));
        });
        LinearLayout.LayoutParams rp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        rp.topMargin = dp(10);
        c.addView(report, rp);

        TextView tips = text("Play with a controller or the on-screen touch controls (Settings → Touch "
            + "controls). Display options are also in game: MY WWE → Options → Graphics.", 13, kDim);
        tips.setPadding(dp(4), dp(14), dp(4), 0);
        c.addView(tips);

        // Updates (as the PC launcher's Play tab).
        LinearLayout up = card(c, "Updates");
        updateText_ = text("Version " + versionName() + ".", 15, kText);
        updateText_.setPadding(0, dp(12), 0, dp(6));
        up.addView(updateText_);
        updateProgress_ = new android.widget.ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        updateProgress_.setMax(1000);
        updateProgress_.setVisibility(View.GONE);
        updateProgress_.setProgressTintList(android.content.res.ColorStateList.valueOf(kRed));
        up.addView(updateProgress_);
        Button check = button("Check for updates", kBackground);
        check.setOnClickListener(v -> checkUpdates(true));
        updateButton_ = button("Download and install", kRed);
        updateButton_.setVisibility(View.GONE);
        updateButton_.setOnClickListener(v -> applyUpdate());
        up.addView(check, fullWidth(6));
        up.addView(updateButton_, fullWidth(8));
        Switch auto = new Switch(this);
        auto.setChecked(getPreferences(MODE_PRIVATE).getBoolean("check_updates", true));
        auto.setOnCheckedChangeListener((b, on) -> getPreferences(MODE_PRIVATE).edit().putBoolean("check_updates", on).apply());
        int[][] states = {{android.R.attr.state_checked}, {}};
        auto.setThumbTintList(new android.content.res.ColorStateList(states, new int[] {kRed, 0xFFB0B0B8}));
        auto.setTrackTintList(new android.content.res.ColorStateList(states, new int[] {0x88C8102E, 0xFF4A4A54}));
        row(up, "Check when the launcher opens", null, auto);
        return c;
    }

    // ── updates ──

    private TextView updateText_;
    private Button updateButton_;
    private android.widget.ProgressBar updateProgress_;
    private Updater.Release release_;
    private final java.util.concurrent.atomic.AtomicBoolean updateCancel_ = new java.util.concurrent.atomic.AtomicBoolean();
    private boolean updating_;

    void checkUpdates(boolean asked) {
        updateText_.setText("Checking for updates\u2026");
        final Updater.Release[] rel = new Updater.Release[1];
        final String[] err = new String[1];
        background(() -> {
            try {
                rel[0] = Updater.latest();
            } catch (Exception e) {
                err[0] = e.getMessage();
            }
        }, () -> {
            String mine = versionName();
            if (err[0] != null) {
                updateText_.setText("Version " + mine + ". Couldn't check for updates: " + err[0] + ".");
            } else if (rel[0] == null || !Updater.newer(rel[0].version, mine) || rel[0].assetUrl == null) {
                updateText_.setText("Version " + mine + " \u2014 the newest.");
                updateButton_.setVisibility(View.GONE);
            } else {
                release_ = rel[0];
                updateText_.setText("Version " + rel[0].version + " is out (you have " + mine + ")"
                    + (rel[0].prerelease ? ", a pre-release" : "") + ".");
                updateButton_.setVisibility(View.VISIBLE);
                if (!asked) status("An update is out: Play tab \u2192 Download and install.");
            }
        });
    }

    void applyUpdate() {
        if (release_ == null || updating_) return;
        Updater.Release rel = release_;
        String size = rel.assetSize > 0 ? " (" + FileOps.human(rel.assetSize) + ")" : "";
        confirm("Download version " + rel.version + size + " and install it? Android asks you to confirm; your "
            + "saves and settings stay.", "Download", () -> {
            updating_ = true;
            updateCancel_.set(false);
            updateProgress_.setProgress(0);
            updateProgress_.setVisibility(View.VISIBLE);
            final String[] err = new String[1];
            background(() -> {
                try {
                    java.io.File apk = Updater.download(this, rel, (done, total) -> runOnUiThread(() -> {
                        if (total > 0) updateProgress_.setProgress((int) (done * 1000 / total));
                        updateText_.setText("Downloading\u2026 " + FileOps.human(done)
                            + (total > 0 ? " of " + FileOps.human(total) : ""));
                    }), updateCancel_);
                    Updater.install(this, apk);
                } catch (Exception e) {
                    err[0] = e.getMessage();
                }
            }, () -> {
                updating_ = false;
                updateProgress_.setVisibility(View.GONE);
                updateText_.setText(err[0] != null ? "The update failed: " + err[0] + "." : "Installing version "
                    + rel.version + "\u2026");
            });
        });
    }

    // The package installer's answers (Updater.install).
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (!Updater.kInstallAction.equals(intent.getAction())) return;
        int st = intent.getIntExtra(android.content.pm.PackageInstaller.EXTRA_STATUS, -999);
        if (st == android.content.pm.PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) startActivity(confirm);
        } else if (st != android.content.pm.PackageInstaller.STATUS_SUCCESS) {
            String msg = intent.getStringExtra(android.content.pm.PackageInstaller.EXTRA_STATUS_MESSAGE);
            updateText_.setText("The update wasn't installed" + (msg != null ? ": " + msg : "") + ".");
        }
    }

    void play() {
        if (!InstallActivity.installed()) {
            status_.setText("The game isn't installed.");
            startActivity(new Intent(this, InstallActivity.class));
            return;
        }
        if (drivers_ != null && !drivers_.readyToPlay(this::play)) return;  // (Mali: alpha mode first)
        settings_.save();
        startActivity(new Intent(this, GameActivity.class));
    }

    // ── Settings ─────────────────────────────────────────────────────────

    interface Refresh { void run(); }
    private final List<Refresh> refreshers_ = new ArrayList<>();

    void refresh() {
        loading_ = true;
        for (Refresh r : refreshers_) r.run();
        loading_ = false;
    }

    // An on/off setting.
    void toggle(LinearLayout card, String label, String hint, String key, boolean fallback) {
        Switch s = new Switch(this);
        // (the theme's own colours left an "on" switch nearly invisible on the dark cards)
        int[][] states = {{android.R.attr.state_checked}, {}};
        s.setThumbTintList(new android.content.res.ColorStateList(states, new int[] {kRed, 0xFFB0B0B8}));
        s.setTrackTintList(new android.content.res.ColorStateList(states, new int[] {0x88C8102E, 0xFF4A4A54}));
        s.setOnCheckedChangeListener((b, on) -> {
            if (loading_ || on == settings_.getBool(key, fallback)) return;
            settings_.setBool(key, on);
            saved(label);
        });
        refreshers_.add(() -> s.setChecked(settings_.getBool(key, fallback)));
        row(card, label, hint, s);
    }

    // A choice between labelled values (strings saved quoted, or numbers).
    void choice(LinearLayout card, String label, String hint, String key, String[] labels, String[] values,
                boolean quoted, String fallback) {
        Spinner sp = new Spinner(this, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> a = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, labels) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(kText);
                t.setTextSize(15);
                return t;
            }
        };
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(a);
        // (a spinner also reports layouts - its first, the launcher coming back in another
        // orientation: only the player's own choice is saved)
        final boolean[] touched = {false};
        sp.setOnTouchListener((v, e) -> {
            touched[0] = true;
            return false;
        });
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (loading_ || !touched[0] || values[position].equals(settings_.getString(key, fallback))) return;
                if (quoted) settings_.setString(key, values[position]);
                else settings_.setInt(key, Integer.parseInt(values[position]));
                saved(label);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        refreshers_.add(() -> {
            String v = settings_.getString(key, fallback);
            int index = 0;
            for (int i = 0; i < values.length; i++) if (values[i].equals(v)) index = i;
            sp.setSelection(index, false);
        });
        row(card, label, hint, sp);
    }

    View settingsPage() {
        LinearLayout c = column();
        LinearLayout display = card(c, "Display");
        toggle(display, "Touch controls", "The on-screen controller (EDIT in game rearranges it)", "touch_controls", true);
        toggle(display, "Show FPS", "The frame rate at the top of the screen", "show_fps", true);
        toggle(display, "VSync", "No tearing; waits for the screen's refresh", "vsync", true);
        // (no Renderer choice: the game draws only natively; an old "off" becomes "main")
        if ("off".equals(settings_.getString("native_renderer", "main"))) {
            settings_.setString("native_renderer", "main");
            settings_.save();
        }
        choice(display, "Language", "The game's text (the commentary stays English); at the next start",
            "user_language", new String[] {"English", "Français", "Deutsch", "Español", "Italiano"},
            new String[] {"1", "4", "3", "5", "6"}, false, "1");

        LinearLayout quality = card(c, "Quality");
        choice(quality, "Render resolution", "The most the game renders at; lower is faster", "native_max_scale",
            new String[] {"Auto", "720p (Xbox 360)", "1440p", "2160p"}, new String[] {"4", "1", "2", "3"}, false, "4");
        toggle(quality, "Anti-aliasing", "Smoother edges, slower", "native_2x_msaa", true);
        choice(quality, "Textures", "Lower: big textures at half / a quarter size - less memory, faster loading",
            "native_texture_quality", new String[] {"High", "Medium", "Low"},
            new String[] {"high", "medium", "low"}, true, "high");
        toggle(quality, "Shadows & effects: high", "At the render resolution (off: the console's, faster)",
            "native_scale_effects", false);
        toggle(quality, "Entrances at 60 fps", "Off: 30 fps as on the Xbox 360 (half the work)", "unlock_30fps", true);
        toggle(quality, "Wide screens", "Matches fill the screen's width (menus stay 16:9)", "native_widescreen", true);
        toggle(quality, "Prepare graphics", "Builds the graphics ahead in the menus: no stutter in matches",
            "native_prepare_pipelines", true);

        LinearLayout audio = card(c, "Audio");
        toggle(audio, "Mute", null, "audio_mute", false);

        // (src/dlc.cpp: off takes the Fan Axxess unlock keys out of the installed DLC catalogs)
        LinearLayout game = card(c, "Game");
        toggle(game, "Everything unlocked from the start", "Off: unlock as you play; DLC stays. At the next start",
            "unlock_everything", true);

        drivers_ = new Drivers(this, settings_);
        drivers_.build(c);

        Button defaults = button("Restore defaults", kCard);
        defaults.setOnClickListener(v -> {
            settings_.setBool("touch_controls", true);
            settings_.setBool("show_fps", true);
            settings_.setBool("vsync", true);
            settings_.setString("native_renderer", "main");
            settings_.setInt("user_language", 1);
            settings_.setInt("native_max_scale", 4);
            settings_.setBool("native_2x_msaa", true);
            settings_.setBool("native_scale_effects", false);
            settings_.setBool("unlock_30fps", true);
            settings_.setBool("native_widescreen", true);
            settings_.setBool("native_prepare_pipelines", true);
            settings_.setBool("audio_mute", false);
            settings_.setBool("unlock_everything", true);
            settings_.setString("native_texture_quality", "high");
            saved("Defaults");
            refresh();
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(18);
        c.addView(defaults, lp);
        return c;
    }

    // ── Online ───────────────────────────────────────────────────────────

    View onlinePage() { return new OnlinePage(this).build(); }

    // (for the pages in their own classes)
    GameSettings settings() { return settings_; }

    boolean loading() { return loading_; }

    void onRefresh(Refresh r) { refreshers_.add(r); }

    EditText field(String hint, int max, int type) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setHintTextColor(kDim);
        e.setTextColor(kText);
        e.setSingleLine(true);
        e.setInputType(type);
        e.setFilters(new InputFilter[] {new InputFilter.LengthFilter(max)});
        e.setMinWidth(dp(150));
        e.setGravity(Gravity.END);
        return e;
    }

    void textSetting(EditText e, String key, String label, String fallback) {
        refreshers_.add(() -> e.setText(settings_.getString(key, fallback)));
        e.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override
            public void afterTextChanged(Editable s) {
                if (loading_) return;
                settings_.setString(key, s.toString().trim());
                saved(label);
            }
        });
    }
}
