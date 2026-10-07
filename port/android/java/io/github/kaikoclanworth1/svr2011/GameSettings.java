// WWE SmackDown vs. Raw 2011 - svr2011.toml in the game folder, as the PC
// launcher reads and writes it: flat "key = value" lines before the first
// [section]; known keys are rewritten in place, everything else is kept.

package io.github.kaikoclanworth1.svr2011;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

final class GameSettings {
    private final File file_;
    private final List<String> lines_ = new ArrayList<>();

    GameSettings(File gameFolder) {
        file_ = new File(gameFolder, "svr2011.toml");
        load();
    }

    void load() {
        lines_.clear();
        if (!file_.isFile()) {
            lines_.add("# WWE SmackDown vs. Raw 2011 - settings (the launcher rewrites this file)");
            return;
        }
        try (InputStream in = new FileInputStream(file_)) {
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[8192];
            int n;
            while ((n = in.read(buffer)) > 0) bytes.write(buffer, 0, n);  // (readAllBytes: API 33+)
            String text = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
            if (text.startsWith("﻿")) text = text.substring(1);
            for (String line : text.split("\r?\n", -1)) lines_.add(line);
            while (!lines_.isEmpty() && lines_.get(lines_.size() - 1).isEmpty()) lines_.remove(lines_.size() - 1);
        } catch (IOException e) {
            lines_.add("# WWE SmackDown vs. Raw 2011 - settings (the launcher rewrites this file)");
        }
    }

    // The index of key's line (top level only), or -1.
    private int find(String key) {
        for (int i = 0; i < lines_.size(); i++) {
            String s = lines_.get(i).trim();
            if (s.startsWith("[")) return -1;
            int eq = s.indexOf('=');
            if (eq > 0 && !s.startsWith("#") && s.substring(0, eq).trim().equals(key)) return i;
        }
        return -1;
    }

    // The raw value (quotes removed), or null.
    String get(String key) {
        int i = find(key);
        if (i < 0) return null;
        String s = lines_.get(i);
        String v = s.substring(s.indexOf('=') + 1).trim();
        int hash = v.startsWith("\"") ? -1 : v.indexOf('#');
        if (hash >= 0) v = v.substring(0, hash).trim();
        if (v.length() >= 2 && v.startsWith("\"") && v.endsWith("\"")) {
            v = v.substring(1, v.length() - 1).replace("\\\"", "\"").replace("\\\\", "\\");
        }
        return v;
    }

    boolean getBool(String key, boolean fallback) {
        String v = get(key);
        return v == null ? fallback : v.equals("true");
    }

    int getInt(String key, int fallback) {
        String v = get(key);
        try {
            return v == null ? fallback : Integer.parseInt(v);
        } catch (NumberFormatException e) {
            return fallback;
        }
    }

    String getString(String key, String fallback) {
        String v = get(key);
        return v == null ? fallback : v;
    }

    // Sets a value as written (true/false, a number, or an already quoted string).
    private void setRaw(String key, String value) {
        String line = key + " = " + value;
        int i = find(key);
        if (i >= 0) {
            lines_.set(i, line);
            return;
        }
        int at = lines_.size();
        for (int k = 0; k < lines_.size(); k++) {
            if (lines_.get(k).trim().startsWith("[")) {
                at = k;
                break;
            }
        }
        lines_.add(at, line);
    }

    void setBool(String key, boolean value) { setRaw(key, value ? "true" : "false"); }

    void setInt(String key, int value) { setRaw(key, Integer.toString(value)); }

    void setDouble(String key, double value) { setRaw(key, Double.toString(value)); }

    void setString(String key, String value) {
        setRaw(key, "\"" + value.replace("\\", "\\\\").replace("\"", "\\\"") + "\"");
    }

    // Written through a temporary file, so a failed write keeps the old one.
    boolean save() {
        StringBuilder sb = new StringBuilder();
        for (String line : lines_) sb.append(line).append('\n');
        File tmp = new File(file_.getPath() + ".tmp");
        try (OutputStream out = new FileOutputStream(tmp)) {
            out.write(sb.toString().getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            return false;
        }
        return tmp.renameTo(file_) || (file_.delete() && tmp.renameTo(file_));
    }
}
