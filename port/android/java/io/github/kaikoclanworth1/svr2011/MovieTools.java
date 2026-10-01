// WWE SmackDown vs. Raw 2011 - the Android launcher's native helpers
// (libsvrtools.so: android/jni/svrtools_jni.c): the PC launcher's Bink movie
// reader and writer. Handles are native pointers; 0 = failed (lastError).

package io.github.kaikoclanworth1.svr2011;

final class MovieTools {
    static {
        System.loadLibrary("svrtools");
    }

    private MovieTools() {}

    static native String lastError();

    static native long binkOpen(String path);
    static native int binkWidth(long h);
    static native int binkHeight(long h);
    static native int binkFrames(long h);
    static native long binkFrameTime(long h);  // (100 ns)
    static native boolean binkNext(long h);
    static native void binkBgra(long h, byte[] out);
    static native void binkRewind(long h);
    static native int binkPosition(long h);
    static native void binkClose(long h);

    static native long writerOpen(String path, int frames);
    static native int writerFrame(long h, byte[] bgra);  // 1, 0 out of memory, -1 write failed
    static native boolean writerClose(long h, boolean keep);
}
