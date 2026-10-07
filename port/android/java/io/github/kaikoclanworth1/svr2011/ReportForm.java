// WWE SmackDown vs. Raw 2011 - the launcher's Report a problem, as the PC
// launcher's (launcher/svr2011_launcher.c rep_page1 / rep_page2): it asks what
// kind of problem, how often, what happened and what came just before - that
// goes at the top of the zip's report.txt (ProblemReport) - then says how to
// post it: a new thread in the Discord's bug-svr11 forum, with Share for the
// zip and the description already copied.
package io.github.kaikoclanworth1.svr2011;

import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.File;

final class ReportForm {
    static final String kDiscord = "https://discord.com/invite/QhGSuDKHwk";
    static final String kForum = "bug-svr11";
    static final String[] kKinds = {"The game crashed (closed by itself)", "The game froze or hung",
        "Graphics look wrong", "Sound or music", "Controller or keyboard", "Online", "Mods",
        "Saves or created content", "The launcher", "Something else"};
    static final String[] kAgain = {"Every time", "Sometimes", "It happened once"};

    // Page 1: the questions.
    static void show(LauncherActivity a) {
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text("Tell us what went wrong. The report also takes your newest game logs, crash reports and "
            + "settings (your online password and token are left out). If you can, run the game and make it happen "
            + "once more first: the newest log is the one that counts.", 14, LauncherActivity.kDim));
        Spinner kind = spinner(a, kKinds);
        Spinner again = spinner(a, kAgain);
        EditText what = box(a, 4);
        EditText before = box(a, 3);
        label(a, v, "What kind of problem?");
        v.addView(kind);
        label(a, v, "Does it happen again?");
        v.addView(again);
        label(a, v, "What happened?");
        v.addView(what);
        label(a, v, "What were you doing just before? (menu or mode, match type, superstars, mods)");
        v.addView(before);
        TextView error = a.text("", 14, LauncherActivity.kRed);
        error.setVisibility(View.GONE);
        v.addView(error);
        ScrollView scroll = new ScrollView(a);
        scroll.addView(v);
        AlertDialog d = new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Report a problem")
            .setView(scroll)
            .setPositiveButton("Create report", null)
            .setNegativeButton("Cancel", null)
            .create();
        d.setOnShowListener(x -> d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> {
            String happened = what.getText().toString().trim();
            if (happened.isEmpty()) {
                error.setText("Please say what happened first.");
                error.setVisibility(View.VISIBLE);
                what.requestFocus();
                return;
            }
            String just = before.getText().toString().trim();
            String description = "SvR 2011 PC port " + a.versionName() + " (Android)\n"
                + "Problem: " + kKinds[kind.getSelectedItemPosition()] + "\n"
                + "Happens: " + kAgain[again.getSelectedItemPosition()] + "\n\n"
                + "What happened:\n" + happened + "\n\n"
                + "Just before:\n" + (just.isEmpty() ? "(not said)" : just) + "\n";
            d.dismiss();
            final ProblemReport.Result[] result = new ProblemReport.Result[1];
            final String[] failed = new String[1];
            a.background(() -> {
                try {
                    result[0] = ProblemReport.make(InstallActivity.gameFolder(), a.versionName(), description);
                } catch (Exception e) {
                    failed[0] = e.getMessage();
                }
            }, () -> {
                if (result[0] == null) a.status("The report couldn't be made: " + failed[0]);
                else ready(a, result[0], description);
            });
        }));
        d.show();
    }

    // Page 2: the report is ready - how to post it.
    static void ready(LauncherActivity a, ProblemReport.Result r, String description) {
        copy(a, description);
        a.status("Report saved: Download/" + r.zip.getName());
        LinearLayout v = new LinearLayout(a);
        v.setOrientation(LinearLayout.VERTICAL);
        v.setPadding(a.dp(20), a.dp(8), a.dp(20), a.dp(4));
        v.addView(a.text("Download/" + r.zip.getName() + "\n\n"
            + "Now post it on our Discord so we can help:\n\n"
            + "1.  Open the Discord (button below) and go to the  " + kForum + "  forum.\n\n"
            + "2.  Start a NEW post (a thread of your own) with a short title, e.g. "
            + "\"Crash when opening Create A Moveset\".\n\n"
            + "3.  Paste your description (it is already copied) and attach the report file: \"Share\" sends the "
            + "zip to Discord.\n\n"
            + "One problem per thread, please, and not in the other channels: the forum is where we keep track of "
            + "them.", 14, LauncherActivity.kText));
        TextView note = a.text("Description copied - paste it into your Discord post.", 13, LauncherActivity.kDim);
        note.setPadding(0, a.dp(10), 0, 0);
        Button discord = a.button("Open Discord", LauncherActivity.kRed);
        discord.setOnClickListener(b -> {
            try {
                a.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(kDiscord)));
            } catch (Exception e) {
                note.setText("No app can open " + kDiscord);
            }
        });
        Button share = a.button("Share", LauncherActivity.kBackground);
        share.setOnClickListener(b -> share(a, r.zip, note));
        Button copy = a.button("Copy description", LauncherActivity.kBackground);
        copy.setOnClickListener(b -> {
            copy(a, description);
            note.setText("Description copied - paste it into your Discord post.");
        });
        v.addView(discord, a.fullWidth(16));
        v.addView(share, a.fullWidth(8));
        v.addView(copy, a.fullWidth(8));
        v.addView(note);
        ScrollView scroll = new ScrollView(a);
        scroll.addView(v);
        new AlertDialog.Builder(a, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
            .setTitle("Your report is ready")
            .setView(scroll)
            .setNegativeButton("Close", null)
            .show();
    }

    // The zip to another app (Discord): a content URI from the media store
    // (the app has no file provider).
    static void share(LauncherActivity a, File zip, TextView note) {
        MediaScannerConnection.scanFile(a, new String[] {zip.getPath()}, new String[] {"application/zip"},
            (path, uri) -> a.runOnUiThread(() -> {
                if (uri == null) {
                    note.setText("Couldn't share it: attach Download/" + zip.getName() + " in Discord instead.");
                    return;
                }
                Intent s = new Intent(Intent.ACTION_SEND);
                s.setType("application/zip");
                s.putExtra(Intent.EXTRA_STREAM, uri);
                s.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
                try {
                    a.startActivity(Intent.createChooser(s, "Share the report"));
                } catch (Exception e) {
                    note.setText("Couldn't share it: attach Download/" + zip.getName() + " in Discord instead.");
                }
            }));
    }

    static void copy(Context c, String text) {
        ClipboardManager cm = (ClipboardManager) c.getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("SvR 2011 problem report", text));
    }

    static void label(LauncherActivity a, LinearLayout v, String s) {
        TextView t = a.text(s, 14, LauncherActivity.kText);
        t.setPadding(0, a.dp(14), 0, a.dp(2));
        v.addView(t);
    }

    static Spinner spinner(LauncherActivity a, String[] items) {
        Spinner sp = new Spinner(a, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> ad = new ArrayAdapter<>(a, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        return sp;
    }

    static EditText box(LauncherActivity a, int lines) {
        EditText e = new EditText(a);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE
            | InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        e.setMinLines(lines);
        e.setGravity(Gravity.TOP | Gravity.START);
        e.setTextColor(LauncherActivity.kText);
        return e;
    }
}
