// WWE SmackDown vs. Raw 2011 - which GPU the phone has (the launcher's
// driver advice and Mali alpha mode, Drivers.java): OpenGL ES's renderer
// name ("Adreno (TM) 710", "Mali-G610"), from a throwaway 1x1 context - the
// launcher has no Vulkan of its own. Kept per system build (a ROM update can
// change the driver's name).

package io.github.kaikoclanworth1.svr2011;

import android.content.Context;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;

import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class GpuInfo {
    private static String renderer_;

    // The GPU's name, or "" if it can't be told.
    static synchronized String renderer(Context c) {
        if (renderer_ != null) return renderer_;
        SharedPreferences prefs = c.getSharedPreferences("gpu", Context.MODE_PRIVATE);
        if (Build.FINGERPRINT.equals(prefs.getString("fingerprint", null))) {
            renderer_ = prefs.getString("renderer", "");
            return renderer_;
        }
        renderer_ = query();
        prefs.edit().putString("fingerprint", Build.FINGERPRINT).putString("renderer", renderer_).apply();
        return renderer_;
    }

    // As known so far (ProblemReport: no Context there), or "".
    static synchronized String known() { return renderer_ == null ? "" : renderer_; }

    // Adreno's model number (710 for "Adreno (TM) 710"), or 0.
    static int adrenoModel(String renderer) {
        Matcher m = Pattern.compile("Adreno[^0-9]*([0-9]{3})").matcher(renderer);
        return m.find() ? Integer.parseInt(m.group(1)) : 0;
    }

    static boolean isMali(String renderer) {
        String r = renderer.toLowerCase(Locale.ROOT);
        return r.contains("mali") || r.contains("immortalis");
    }

    private static String query() {
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (display == EGL14.EGL_NO_DISPLAY) return "";
        int[] version = new int[2];
        if (!EGL14.eglInitialize(display, version, 0, version, 1)) return "";
        EGLContext context = EGL14.EGL_NO_CONTEXT;
        EGLSurface surface = EGL14.EGL_NO_SURFACE;
        try {
            int[] attribs = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE,
                             EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE};
            EGLConfig[] configs = new EGLConfig[1];
            int[] count = new int[1];
            if (!EGL14.eglChooseConfig(display, attribs, 0, configs, 0, 1, count, 0) || count[0] == 0) return "";
            context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                                             new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
            surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                                                    new int[] {EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE}, 0);
            if (context == EGL14.EGL_NO_CONTEXT || surface == EGL14.EGL_NO_SURFACE
                || !EGL14.eglMakeCurrent(display, surface, surface, context))
                return "";
            String r = GLES20.glGetString(GLES20.GL_RENDERER);
            return r == null ? "" : r.trim();
        } catch (RuntimeException e) {
            return "";
        } finally {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface);
            if (context != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(display, context);
            // (no eglTerminate: the display is the app's; the UI draws with it too)
        }
    }
}
