// WWE SmackDown vs. Raw 2011 - the game's https requests on Android (the
// Community Creations server, src/online_net.cpp): Android's own TLS, which
// the native side doesn't have. Called over JNI from the relay's threads.

package io.github.kaikoclanworth1.svr2011;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

public final class NetBridge {
    private NetBridge() {}

    // headers: name, value, name, value... The answer as an HTTP/1.1 response
    // ("HTTP/1.1 <status>", its headers, a blank line, the body) for the native
    // side's parser, or null if the server didn't answer.
    public static byte[] request(String method, String url, String[] headers, byte[] body) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(10000);
            c.setReadTimeout(60000);
            c.setUseCaches(false);
            c.setInstanceFollowRedirects(false);
            c.setRequestMethod(method);
            for (int i = 0; i + 1 < headers.length; i += 2) c.setRequestProperty(headers[i], headers[i + 1]);
            if (body != null && body.length > 0) {
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                try (OutputStream out = c.getOutputStream()) {
                    out.write(body);
                }
            }
            final int status = c.getResponseCode();
            StringBuilder head = new StringBuilder("HTTP/1.1 ").append(status).append(" \r\n");
            for (Map.Entry<String, List<String>> h : c.getHeaderFields().entrySet()) {
                if (h.getKey() == null) continue;  // (the status line)
                for (String v : h.getValue()) head.append(h.getKey()).append(": ").append(v).append("\r\n");
            }
            head.append("\r\n");
            ByteArrayOutputStream answer = new ByteArrayOutputStream();
            answer.write(head.toString().getBytes(StandardCharsets.UTF_8));
            InputStream in = status >= 400 ? c.getErrorStream() : c.getInputStream();
            if (in != null) {
                try (InputStream s = in) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = s.read(buf)) > 0 && answer.size() < (32 << 20)) answer.write(buf, 0, n);
                }
            }
            return answer.toByteArray();
        } catch (Exception e) {
            android.util.Log.w("SvR2011", "online: " + method + " " + url + ": " + e);
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
