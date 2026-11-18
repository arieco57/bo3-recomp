package com.bo3recomp;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.system.Os;
import android.util.Log;

import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

// Logs a tester can send without a PC (Android/data is out of reach on Android 11+):
// Descargas/BO3Recomp/bo3-<mode>-log.txt (the game's own log, written by runtime/src/log.cpp through the fd
// in BO3_LOG_FD) and logcat-<mode>.txt (this app's logcat: the driver, Java, crashes, live).
public class LogExport {
    static final String DIR = "Download/BO3Recomp/";

    static ParcelFileDescriptor create(Activity a, String name) {
        ContentResolver cr = a.getContentResolver();
        Uri coll = MediaStore.Downloads.EXTERNAL_CONTENT_URI;
        try {
            // the previous run's file (ours: deletable without permission)
            cr.delete(coll, MediaStore.MediaColumns.RELATIVE_PATH + "=? AND " + MediaStore.MediaColumns.DISPLAY_NAME + "=?",
                      new String[] { DIR, name });
        } catch (Exception ignored) { }
        try {
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
            v.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
            v.put(MediaStore.MediaColumns.RELATIVE_PATH, DIR);
            Uri u = cr.insert(coll, v);
            return u != null ? cr.openFileDescriptor(u, "wt") : null;
        } catch (Exception e) {
            Log.w("BO3", "log export: " + e);
            return null;
        }
    }

    static void start(Activity a, String mode) {
        ParcelFileDescriptor game = create(a, "bo3-" + mode + "-log.txt");
        if (game != null) {
            // header (also there when the game dies before its first line), then a dup of the fd for the game
            try {
                String fd = Integer.toString(game.dup().detachFd());
                Os.setenv("BO3_LOG_FD", fd, true);
                try (FileOutputStream o = new FileOutputStream(new java.io.File(Installer.dataDir(a), "log_fd"))) {
                    o.write(fd.getBytes(StandardCharsets.UTF_8));
                }
            } catch (Throwable e) {
                Log.w("BO3", "log export: " + e);
            }
            try (FileOutputStream h = new FileOutputStream(game.getFileDescriptor())) {
                String soc = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : Build.HARDWARE;
                h.write(("BO3 Recomp " + mode + " - " + Build.MANUFACTURER + " " + Build.MODEL + " (" + soc + "), Android "
                         + Build.VERSION.RELEASE + ", driver: " + GpuSettings.choice(a) + ", last run: " + GpuSettings.state(a) + "\n")
                        .getBytes(StandardCharsets.UTF_8));
            } catch (Throwable e) {
                Log.w("BO3", "log export: " + e);
            }
            try { game.close(); } catch (Exception ignored) { }
        }
        final ParcelFileDescriptor lc = create(a, "logcat-" + mode + ".txt");
        if (lc == null) return;
        Thread t = new Thread(() -> {
            try (OutputStream out = new FileOutputStream(lc.getFileDescriptor())) {
                // (an app reads only its own logs: this run and the previous ones still in the buffer)
                Process p = new ProcessBuilder("logcat", "-v", "threadtime").redirectErrorStream(true).start();
                InputStream in = p.getInputStream();
                byte[] buf = new byte[16384];
                for (int n; (n = in.read(buf)) > 0; ) { out.write(buf, 0, n); out.flush(); }
            } catch (Exception e) {
                Log.w("BO3", "logcat export: " + e);
            }
        }, "logcat-export");
        t.setDaemon(true);
        t.start();
    }
}
