package com.bo3recomp;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;

import org.libsdl.app.SDL;

// "All files access" (Android 11+): the first-run installation (runtime/src/setup.cpp) reads the
// player's disc image and packages from /sdcard/BO3Recomp.
public class Storage {
    public static boolean hasAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return true;
    }

    public static void request() {
        final Context ctx = SDL.getContext();
        if (!(ctx instanceof Activity) || Build.VERSION.SDK_INT < 30) return;
        Activity act = (Activity) ctx;
        try {
            Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + act.getPackageName()));
            act.startActivity(i);
        } catch (Exception e) {
            act.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }
}
