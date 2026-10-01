"""Builds the Android app (out/android/SvR2011.apk) without Gradle.

  python tools/build_apk.py [--skip-native]

The native code: the port's CMake project cross-compiled with the NDK
(out/build/Android: libmain.so, with the SDK's librexruntime.so and
librexgpu-xenos.so). The Java side: android/ (InstallActivity, GameActivity)
and SDL's own activity classes from the SDK's SDL. Tools from the Android SDK
(D:\\Android by default; ANDROID_HOME / JAVA_HOME override): aapt2, javac, d8,
zipalign, apksigner. The signing key (android/svr2011.keystore, made on the
first build) must stay the same for updates to install over the app.

The APK holds no game data: the launcher's "Create APK Package" puts it beside
a zip of the installed game, which the app extracts on the phone.
"""

import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

PORT = Path(__file__).resolve().parent.parent
ROOT = PORT.parent
SDK_DIR = ROOT / "recomp" / "rexglue-sdk"
ANDROID = Path(os.environ.get("ANDROID_HOME", r"D:\Android\sdk"))
JAVA = Path(os.environ.get("JAVA_HOME", r"D:\Android\jdk17"))
NDK = ANDROID / "ndk" / "30.0.16248370"
BUILD_TOOLS = ANDROID / "build-tools" / "36.1.0"
PLATFORM_JAR = ANDROID / "platforms" / "android-36" / "android.jar"
LLVM = NDK / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64"
NATIVE = PORT / "out" / "build" / "Android"
OUT = PORT / "out" / "android"
KEYSTORE = PORT / "android" / "svr2011.keystore"
KEY_PASS = "svr2011"  # (not a secret: it only has to be the same key each build)


def run(*args, **kw):
    print(">", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kw)


def build_native():
    env = dict(os.environ)
    env["PATH"] = str(ROOT / "recomp" / "bin") + os.pathsep + env["PATH"]  # ninja
    if not (NATIVE / "build.ninja").exists():
        run("cmake", "-S", PORT, "-B", NATIVE, "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={(NDK / 'build/cmake/android.toolchain.cmake').as_posix()}",
            "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-31", "-DANDROID_STL=c++_shared",
            "-DCMAKE_BUILD_TYPE=Release", f"-DREXSDK_DIR={SDK_DIR.as_posix()}",
            "-DREXGLUE_USE_VULKAN=ON",
            f"-DREXGLUE_HOST_TOOL={(SDK_DIR / 'out/win-amd64/rexglue.exe').as_posix()}", env=env)
    run("cmake", "--build", NATIVE, "--target", "svr2011", env=env)


def version():
    v = (PORT / "VERSION.txt").read_text().strip()
    major, minor, patch = (int(x) for x in v.split(".")[:3])
    return v, major * 10000 + minor * 100 + patch


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-native", action="store_true", help="package the last native build")
    args = ap.parse_args()

    if not args.skip_native:
        build_native()

    version_name, version_code = version()
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "classes").mkdir(parents=True)

    # Resources and manifest.
    run(BUILD_TOOLS / "aapt2.exe", "compile", "--dir", PORT / "android" / "res", "-o", OUT / "res.zip")
    run(BUILD_TOOLS / "aapt2.exe", "link", "-o", OUT / "base.apk", "-I", PLATFORM_JAR,
        "--manifest", PORT / "android" / "AndroidManifest.xml",
        "--version-name", version_name, "--version-code", str(version_code),
        "--min-sdk-version", "31", "--target-sdk-version", "36", OUT / "res.zip")

    # Java: the app's activities and SDL's (the version SDL's native code expects).
    sources = list((PORT / "android" / "java").rglob("*.java"))
    sources += list((SDK_DIR / "thirdparty" / "sdl3" / "android-project" / "app" / "src" / "main"
                     / "java").rglob("*.java"))
    (OUT / "sources.txt").write_text("\n".join(f'"{s.as_posix()}"' for s in sources))
    run(JAVA / "bin" / "javac.exe", "-nowarn", "-Xlint:-options", "-source", "8", "-target", "8",
        "-encoding", "UTF-8", "-bootclasspath", PLATFORM_JAR,
        "-classpath", BUILD_TOOLS / "core-lambda-stubs.jar", "-d", OUT / "classes",
        f"@{OUT / 'sources.txt'}")
    classes = [str(p) for p in (OUT / "classes").rglob("*.class")]
    env = dict(os.environ, JAVA_HOME=str(JAVA))
    # (the classes in one jar: listed one by one they made the command line too long)
    with zipfile.ZipFile(OUT / "classes.jar", "w") as jar:
        for c in classes:
            jar.write(c, Path(c).relative_to(OUT / "classes").as_posix())
    run(BUILD_TOOLS / "d8.bat", "--release", "--min-api", "31", "--lib", PLATFORM_JAR,
        "--output", OUT, OUT / "classes.jar", env=env)

    # Native libraries, without their debug info (the unstripped ones stay in
    # the build folders for crash symbols).
    # The launcher's native helpers (Movies: the PC launcher's Bink reader and writer).
    tools = OUT / "libsvrtools-full.so"
    run(LLVM / "bin" / "clang.exe", "--target=aarch64-linux-android31", "-shared", "-fPIC", "-O2",
        "-I", PORT / "launcher", PORT / "android" / "jni" / "svrtools_jni.c",
        PORT / "launcher" / "bink_decode.c", PORT / "launcher" / "bink_encode.c", "-o", tools)
    libs = {
        "libsvrtools.so": tools,
        # libadrenotools' hooks (custom GPU drivers; src/gpu_driver.cpp)
        "libhook_impl.so": NATIVE / "adrenotools" / "src" / "hook" / "libhook_impl.so",
        "libmain_hook.so": NATIVE / "adrenotools" / "src" / "hook" / "libmain_hook.so",
        "libfile_redirect_hook.so": NATIVE / "adrenotools" / "src" / "hook" / "libfile_redirect_hook.so",
        "libgsl_alloc_hook.so": NATIVE / "adrenotools" / "src" / "hook" / "libgsl_alloc_hook.so",
        "libmain.so": NATIVE / "libmain.so",
        "librexruntime.so": SDK_DIR / "out" / "linux-arm64" / "librexruntime.so",
        "librexgpu-xenos.so": SDK_DIR / "out" / "linux-arm64" / "librexgpu-xenos.so",
        "libc++_shared.so": LLVM / "sysroot" / "usr" / "lib" / "aarch64-linux-android" / "libc++_shared.so",
    }
    (OUT / "lib").mkdir()
    for name, src in libs.items():
        run(LLVM / "bin" / "llvm-strip.exe", "--strip-debug", "-o", OUT / "lib" / name, src)

    # The Vulkan shaders (one pack, tools/pack_shaders.py) and the known
    # pipelines: the launcher puts them in the game folder's native_shaders, so
    # an install from the disc image on the phone has them, and an APK update
    # brings new ones.
    assets = []
    shaders = PORT / "runs" / "shaders_native"
    stage = OUT / "shaderpack"
    stage.mkdir()
    for f in (shaders / "spirv").glob("*.spv"):
        if not f.name.startswith(("dbg_", "debug_")):
            shutil.copy2(f, stage / f.name)
    for pattern in ("*.inputs", "*.textures"):
        for f in (shaders / "dxil").glob(pattern):
            shutil.copy2(f, stage / f.name)
    if any(stage.glob("*.spv")):
        run(sys.executable, PORT / "tools" / "pack_shaders.py", stage)
        assets.append((stage / "shaders.spv.pak", "assets/native_shaders/shaders.spv.pak"))
    else:
        print("warning: no SPIR-V shaders in", shaders / "spirv", "- the APK carries no shaders")
    if (PORT / "dist" / "pipelines.list").exists():
        assets.append((PORT / "dist" / "pipelines.list", "assets/native_shaders/pipelines.list"))

    with zipfile.ZipFile(OUT / "base.apk", "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(OUT / "classes.dex", "classes.dex")
        for name in libs:
            apk.write(OUT / "lib" / name, f"lib/arm64-v8a/{name}")
        for src, name in assets:
            apk.write(src, name, compress_type=zipfile.ZIP_STORED)

    run(BUILD_TOOLS / "zipalign.exe", "-f", "-P", "16", "4", OUT / "base.apk", OUT / "aligned.apk")

    if not KEYSTORE.exists():
        run(JAVA / "bin" / "keytool.exe", "-genkeypair", "-keystore", KEYSTORE, "-alias", "svr2011",
            "-keyalg", "RSA", "-keysize", "2048", "-validity", "36500",
            "-storepass", KEY_PASS, "-keypass", KEY_PASS,
            "-dname", "CN=WWE SvR2011 PC port")
    apk = OUT / "SvR2011.apk"
    run(BUILD_TOOLS / "apksigner.bat", "sign", "--ks", KEYSTORE, "--ks-pass", f"pass:{KEY_PASS}",
        "--out", apk, OUT / "aligned.apk", env=env)
    for tmp in ("base.apk", "aligned.apk", "res.zip", "sources.txt", "classes.jar", "classes.dex"):
        (OUT / tmp).unlink()
    shutil.rmtree(stage, ignore_errors=True)
    print(f"{apk} ({apk.stat().st_size / 1e6:.1f} MB, version {version_name} / {version_code})")


if __name__ == "__main__":
    sys.exit(main())
