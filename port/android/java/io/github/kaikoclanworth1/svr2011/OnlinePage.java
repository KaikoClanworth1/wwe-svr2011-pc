// WWE SmackDown vs. Raw 2011 - the launcher's Online page: play online, the
// server (Default: the port's own, its address not shown; Custom: one the
// player types) and the account on it - sign in, create one, sign out
// (<server>/api/login, /api/register, /api/logout; JSON). Signing in writes
// online_token, online_xuid, online_name and online_enabled for the game.

package io.github.kaikoclanworth1.svr2011;

import android.text.InputType;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

final class OnlinePage {
    static final String kDefaultServer = "https://sho-ti.me/svr";
    static final String kCustomServer = "127.0.0.1:8411";

    private final LauncherActivity a_;
    private Spinner kind_;
    private LinearLayout addressBox_, signedOut_, signedIn_;
    private EditText address_, name_, password_;
    private TextView who_, accountStatus_;
    private Button signIn_, create_, signOut_;
    private boolean busy_;

    OnlinePage(LauncherActivity a) { a_ = a; }

    GameSettings settings() { return a_.settings(); }

    View build() {
        LinearLayout c = a_.column();
        TextView about = a_.text("Community Creations: share Created Superstars, Paint Tool logos, highlight reels "
            + "and more with other players, through a Community Creations server (the original servers closed in "
            + "2014). Online matches are not available yet.", 14, LauncherActivity.kDim);
        about.setPadding(a_.dp(4), a_.dp(6), a_.dp(4), a_.dp(4));
        c.addView(about);

        LinearLayout play = a_.card(c, "Online");
        a_.toggle(play, "Play online", null, "online_enabled", false);

        // Server: Default / Custom (which one: from the address saved).
        LinearLayout server = a_.card(c, "Server");
        kind_ = new Spinner(a_, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> kinds = new ArrayAdapter<String>(a_, android.R.layout.simple_spinner_item,
            new String[] {"Default", "Custom"}) {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(LauncherActivity.kText);
                t.setTextSize(15);
                return t;
            }
        };
        kinds.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        kind_.setAdapter(kinds);
        a_.row(server, "Server", null, kind_);
        addressBox_ = new LinearLayout(a_);
        addressBox_.setOrientation(LinearLayout.VERTICAL);
        address_ = a_.field(kCustomServer, 120, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        a_.row(addressBox_, "Address", "host:port (or the URL) of a Community Creations server", address_);
        View sep = new View(a_);
        sep.setBackgroundColor(LauncherActivity.kLine);
        addressBox_.addView(sep, 0, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 1));
        server.addView(addressBox_);
        // (only the player's own choice is saved: a spinner also reports layouts)
        final boolean[] touched = {false};
        kind_.setOnTouchListener((v, e) -> {
            touched[0] = true;
            return false;
        });
        kind_.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                boolean custom = position == 1;
                addressBox_.setVisibility(custom ? View.VISIBLE : View.GONE);
                if (a_.loading() || !touched[0] || custom == isCustom(server())) return;
                if (custom && address_.getText().toString().trim().isEmpty()) address_.setText(kCustomServer);
                saveServer();
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });
        address_.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override
            public void afterTextChanged(android.text.Editable s) {
                if (!a_.loading() && kind_.getSelectedItemPosition() == 1) saveServer();
            }
        });

        // Account on that server.
        LinearLayout account = a_.card(c, "Account");
        signedIn_ = new LinearLayout(a_);
        signedIn_.setOrientation(LinearLayout.VERTICAL);
        who_ = a_.text("", 16, LauncherActivity.kText);
        who_.setPadding(0, a_.dp(12), 0, a_.dp(12));
        signedIn_.addView(who_);
        account.addView(signedIn_);
        signedOut_ = new LinearLayout(a_);
        signedOut_.setOrientation(LinearLayout.VERTICAL);
        name_ = a_.field("Name", 15, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        a_.row(signedOut_, "Name", "3-15 letters, digits, space, _ . - (shown on your uploads)", name_);
        password_ = a_.field("Password", 64, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        a_.row(signedOut_, "Password", "At least 6 characters", password_);
        account.addView(signedOut_);
        accountStatus_ = a_.text("", 13, LauncherActivity.kDim);
        accountStatus_.setPadding(0, 0, 0, a_.dp(10));
        account.addView(accountStatus_);

        signIn_ = a_.button("Sign in", LauncherActivity.kRed);
        signIn_.setOnClickListener(v -> signIn(false));
        create_ = a_.button("Create account", LauncherActivity.kCard);
        create_.setOnClickListener(v -> signIn(true));
        LinearLayout buttons = a_.pair(signIn_, create_);
        c.addView(buttons, a_.fullWidth(10));
        signOut_ = a_.button("Sign out", LauncherActivity.kCard);
        signOut_.setOnClickListener(v -> signOut());
        c.addView(signOut_, a_.fullWidth(10));

        a_.onRefresh(this::refresh);
        return c;
    }

    String server() { return settings().getString("online_server", kDefaultServer); }

    static boolean isCustom(String server) { return !server.isEmpty() && !server.equals(kDefaultServer); }

    void saveServer() {
        boolean custom = kind_.getSelectedItemPosition() == 1;
        String value = custom ? address_.getText().toString().trim() : kDefaultServer;
        if (custom && value.isEmpty()) return;
        if (value.equals(server())) return;
        settings().setString("online_server", value);
        a_.saved("Server");
        refreshAccount();
    }

    void refresh() {
        String s = server();
        boolean custom = isCustom(s);
        kind_.setSelection(custom ? 1 : 0, false);
        addressBox_.setVisibility(custom ? View.VISIBLE : View.GONE);
        address_.setText(custom ? s : kCustomServer);
        refreshAccount();
    }

    void refreshAccount() {
        boolean in = !settings().getString("online_token", "").isEmpty();
        String name = settings().getString("online_name", "");
        signedIn_.setVisibility(in ? View.VISIBLE : View.GONE);
        signedOut_.setVisibility(in ? View.GONE : View.VISIBLE);
        who_.setText("Signed in as " + name + (isCustom(server()) ? " on " + server() : "") + ".");
        if (!in && name_.getText().length() == 0) name_.setText(name);
        signIn_.setVisibility(in ? View.GONE : View.VISIBLE);
        create_.setVisibility(in ? View.GONE : View.VISIBLE);
        ((View) signIn_.getParent()).setVisibility(in ? View.GONE : View.VISIBLE);
        signOut_.setVisibility(in ? View.VISIBLE : View.GONE);
        // (an account works on the server that made it: sign out to change servers)
        kind_.setEnabled(!in);
        address_.setEnabled(!in);
        boolean enabled = !busy_;
        signIn_.setEnabled(enabled);
        create_.setEnabled(enabled);
        signOut_.setEnabled(enabled);
    }

    static boolean validName(String n) {
        if (n.length() < 3 || n.length() > 15) return false;
        for (char ch : n.toCharArray())
            if (!(Character.isLetterOrDigit(ch) && ch < 128) && ch != ' ' && ch != '_' && ch != '.' && ch != '-')
                return false;
        return true;
    }

    void signIn(boolean create) {
        if (busy_) return;
        String name = name_.getText().toString().trim(), password = password_.getText().toString();
        if (!validName(name)) {
            accountStatus_.setText("Names are 3-15 letters, digits, spaces or _ . -");
            return;
        }
        if (password.length() < 6) {
            accountStatus_.setText("Passwords are at least 6 characters.");
            return;
        }
        busy_ = true;
        accountStatus_.setText(create ? "Creating the account…" : "Signing in…");
        refreshAccount();
        final String base = base(server());
        final Object[] result = new Object[1];
        a_.background(() -> {
            try {
                JSONObject body = new JSONObject();
                body.put("name", name);
                body.put("password", password);
                result[0] = post(base + (create ? "/api/register" : "/api/login"), body.toString(), null);
            } catch (Exception e) {
                result[0] = e;
            }
        }, () -> {
            busy_ = false;
            JSONObject r = result[0] instanceof JSONObject ? (JSONObject) result[0] : null;
            if (r != null && r.optBoolean("ok")) {
                settings().setString("online_token", r.optString("token"));
                settings().setString("online_xuid", r.optString("xuid"));
                settings().setString("online_name", r.optString("name", name));
                settings().setBool("online_enabled", true);
                password_.setText("");
                accountStatus_.setText(settings().save() ? (create ? "Account created. " : "") + "Signed in - "
                    + "online play is on." : "Could not save the settings.");
                a_.refresh();
            } else {
                accountStatus_.setText(error(result[0], create));
                refreshAccount();
            }
        });
    }

    void signOut() {
        if (busy_) return;
        final String token = settings().getString("online_token", "");
        final String base = base(server());
        busy_ = true;
        refreshAccount();
        // (signed out here whatever the server says)
        a_.background(() -> {
            try {
                post(base + "/api/logout", "{}", token);
            } catch (Exception e) {
            }
        }, () -> {
            busy_ = false;
            settings().setString("online_token", "");
            settings().setString("online_xuid", "");
            accountStatus_.setText(settings().save() ? "Signed out." : "Could not save the settings.");
            refreshAccount();
        });
    }

    // A bare host:port is plain http.
    static String base(String server) {
        String b = server.contains("://") ? server : "http://" + server;
        while (b.endsWith("/")) b = b.substring(0, b.length() - 1);
        return b;
    }

    static String error(Object r, boolean create) {
        if (r instanceof JSONObject) {
            String e = ((JSONObject) r).optString("error", "");
            if (!e.isEmpty()) return e;
            int code = ((JSONObject) r).optInt("_status", 0);
            if (code == 403) return create ? "That name is taken." : "Wrong name or password.";
            if (code == 429) return "Too many tries - wait a little and try again.";
            return "The server said no (" + code + ").";
        }
        return "Could not reach the server" + (r instanceof Exception && ((Exception) r).getMessage() != null
            ? ": " + ((Exception) r).getMessage() : ".");
    }

    // POSTs JSON; the answer's JSON (any status), with "_status" added.
    static JSONObject post(String url, String body, String token) throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(url).openConnection();
        try {
            c.setConnectTimeout(10000);
            c.setReadTimeout(20000);
            c.setRequestMethod("POST");
            c.setDoOutput(true);
            c.setRequestProperty("Content-Type", "application/json");
            if (token != null) c.setRequestProperty("Authorization", "Bearer " + token);
            byte[] out = body.getBytes(StandardCharsets.UTF_8);
            c.setFixedLengthStreamingMode(out.length);
            try (OutputStream os = c.getOutputStream()) {
                os.write(out);
            }
            int code = c.getResponseCode();
            InputStream in = code < 400 ? c.getInputStream() : c.getErrorStream();
            ByteArrayOutputStream b = new ByteArrayOutputStream();
            if (in != null) {
                try (InputStream is = in) {
                    FileOps.copy(is, b);
                }
            }
            JSONObject r;
            try {
                r = new JSONObject(b.toString("UTF-8"));
            } catch (org.json.JSONException e) {
                r = new JSONObject();
            }
            r.put("_status", code);
            return r;
        } finally {
            c.disconnect();
        }
    }
}
