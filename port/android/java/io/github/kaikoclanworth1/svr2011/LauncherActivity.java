// WWE SmackDown vs. Raw 2011 - the launcher on the phone: the PC launcher's
// tabs for a touch screen (InstallActivity opens it once the game is
// installed). Play starts the game as the PC launcher does; Settings and
// Online read and write the game folder's svr2011.toml (the same keys as the
// PC launcher and the game's GRAPHICS page), each change saved at once.

package io.github.kaikoclanworth1.svr2011;

import android.app.Activity;
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
    private final List<Button> tabs_ = new ArrayList<>();
    private final List<View> pages_ = new ArrayList<>();
    private TextView status_;
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
        select(0);
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
    }

    String versionName() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.versionName;
        } catch (Exception e) {
            return "?";
        }
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

        TextView tips = text("Play with a controller or the on-screen touch controls (Settings → Touch "
            + "controls). Display options are also in game: MY WWE → Options → Graphics.", 13, kDim);
        tips.setPadding(dp(4), dp(14), dp(4), 0);
        c.addView(tips);
        return c;
    }

    void play() {
        if (!InstallActivity.installed()) {
            status_.setText("The game isn't installed.");
            startActivity(new Intent(this, InstallActivity.class));
            return;
        }
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
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                // (a spinner also reports its first layout: only a real change is saved)
                if (loading_ || values[position].equals(settings_.getString(key, fallback))) return;
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
        choice(display, "Renderer", "Native is the fastest", "native_renderer",
            new String[] {"Native", "Emulated"}, new String[] {"main", "off"}, true, "main");
        choice(display, "Language", "The game's text (the commentary stays English); at the next start",
            "user_language", new String[] {"English", "Français", "Deutsch", "Español", "Italiano"},
            new String[] {"1", "4", "3", "5", "6"}, false, "1");

        LinearLayout quality = card(c, "Quality");
        choice(quality, "Render resolution", "The most the game renders at; lower is faster", "native_max_scale",
            new String[] {"Auto", "720p (Xbox 360)", "1440p", "2160p"}, new String[] {"4", "1", "2", "3"}, false, "4");
        toggle(quality, "Anti-aliasing", "Smoother edges, slower", "native_2x_msaa", true);
        toggle(quality, "Shadows & effects: high", "At the render resolution (off: the console's, faster)",
            "native_scale_effects", false);
        toggle(quality, "Entrances at 60 fps", "Off: 30 fps as on the Xbox 360 (half the work)", "unlock_30fps", true);
        toggle(quality, "Wide screens", "Matches fill the screen's width (menus stay 16:9)", "native_widescreen", true);
        toggle(quality, "Prepare graphics", "Builds the graphics ahead in the menus: no stutter in matches",
            "native_prepare_pipelines", true);

        LinearLayout audio = card(c, "Audio");
        toggle(audio, "Mute", null, "audio_mute", false);

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

    static final String kDefaultServer = "127.0.0.1:8411";

    View onlinePage() {
        LinearLayout c = column();
        TextView about = text("Community Creations: share Created Superstars, Paint Tool logos, highlight reels and "
            + "more with other players, through a Community Creations server (the original servers closed in "
            + "2014). Online matches are not available yet.", 14, kDim);
        about.setPadding(dp(4), dp(6), dp(4), dp(4));
        c.addView(about);

        LinearLayout play = card(c, "Online");
        toggle(play, "Play online", null, "online_enabled", false);

        LinearLayout profile = card(c, "Your profile");
        EditText name = field("User", 15, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        textSetting(name, "online_name", "Online name", "");
        row(profile, "Online name", "Up to 15 characters, shown on your profile and uploads", name);

        LinearLayout server = card(c, "Server");
        EditText address = field(kDefaultServer, 120, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        textSetting(address, "online_server", "Server", kDefaultServer);
        row(server, "Address", "host:port of the Community Creations server", address);
        Button reset = button("Default server", kCard);
        reset.setOnClickListener(v -> address.setText(kDefaultServer));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(14);
        c.addView(reset, lp);
        return c;
    }

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
