package com.bo3recomp;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

import java.io.File;

// Sends the error reports LogUpload saved (cache/reports/*.json) from the app's main process, so a game
// process that crashes right after starting does not take the upload down with it.
public class LogUploadService extends Service {
    boolean running;

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!running) {
            running = true;
            new Thread(() -> {
                File[] fs = new File(getCacheDir(), "reports").listFiles();
                if (fs != null) {
                    // only the newest 5 wait for a retry
                    java.util.Arrays.sort(fs, (x, y) -> y.getName().compareTo(x.getName()));
                    for (int i = 5; i < fs.length; i++) fs[i].delete();
                    if (fs.length > 5) fs = java.util.Arrays.copyOf(fs, 5);
                }
                if (fs != null)
                    for (File f : fs)
                        if (f.getName().endsWith(".json") && LogUpload.post(f)) f.delete();
                running = false;
                stopSelf();
            }, "log-upload").start();
        }
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }
}
