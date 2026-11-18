package com.bo3recomp;

import android.content.Context;

import java.io.File;

// First-run installation (libbo3_setup.so = runtime/src/setup.cpp + setup_jni.cpp): from the player's disc
// image, Title Update and Compatibility Packs 1 and 2, chosen in InstallerActivity.
public class Installer {
    static boolean loaded;

    static synchronized void load() {
        if (!loaded) { System.loadLibrary("bo3_setup"); loaded = true; }
    }

    // where the game libraries keep the game (SDL's external files folder)
    public static File dataDir(Context c) {
        File d = c.getExternalFilesDir(null);
        return d != null ? d : c.getFilesDir();
    }

    // same check as SetupInstalled() in setup.cpp, without loading any library (the game activities ask
    // before starting)
    public static boolean isInstalled(Context c) {
        File d = dataDir(c);
        String[] need = {
            "extracted/game/default.xex", "extracted/game/default_zm.xex",
            "content/0000000000000000/4156091D/000B0000/tu00000001_00000000/default.xexp",
            "content/0000000000000000/4156091D/000B0000/tu00000001_00000000/default_zm.xexp",
            "private/mp/default_patched.xex", "private/zm/default_patched.xex",
            "content/0000000000000000/4156091D/00000002/A92CACF8DF6D9DB1C32970035A6B8B63B09C2F1641",
            "content/0000000000000000/4156091D/00000002/4FD64D1A99FF002F54CEE0FE922808E1D2D21A5341" };
        for (String n : need) if (!new File(d, n).exists()) return false;
        return true;
    }

    // progress from the native side (the installing thread): text + 0-100, or -1 for a message/error
    public static volatile String status = "";
    public static volatile String message = "";
    public static volatile int percent;

    public static void progress(String text, int pct) {
        if (pct < 0) message = text;
        else { status = text; percent = pct; }
    }

    static native boolean installed(String dataDir);
    static native boolean install(String dataDir, String[] paths, String[] names);
    static native String unzipFd(int fd, String dst);   // "" = done, otherwise the error
    static native long unzipBytes();                     // bytes extracted so far (all zips)
}
