package com.bo3recomp;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

// Error reports to our backend (the Supabase project the online mode uses; table error_reports,
// tools/supabase_logs.sql): when the last run never showed a picture or crashed, and the player agreed
// once (file "log_upload" in the data folder: 1 / 0, asked by DriverActivity). Sent at the next start
// from the game's process: the end of the last run's bo3.log and of this app's logcat (an app reads only
// its own logs). E-mail addresses and tokens are blanked out.
public class LogUpload {
    static final int MAX = 300000;   // characters of each log (the table refuses more than 400000)

    static native String defaultConfig(String key);   // the backend compiled into the game library (xam.cpp)

    static File dir(Context c) { return Installer.dataDir(c); }

    // null = never asked
    public static Boolean consent(Context c) {
        try {
            String v = new String(Files.readAllBytes(new File(dir(c), "log_upload").toPath()), StandardCharsets.UTF_8).trim();
            return "1".equals(v);
        } catch (Exception e) {
            return null;
        }
    }

    public static void setConsent(Context c, boolean yes) {
        try (FileOutputStream o = new FileOutputStream(new File(dir(c), "log_upload"))) {
            o.write((yes ? "1\n" : "0\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
    }

    // reason to report the last run, or null: it never showed a picture, or it crashed
    public static String pendingReason(Context c) {
        String st = GpuSettings.state(c);
        // ("pending": the driver choice was made, the report still to send)
        if (st.startsWith("starting") || st.startsWith("pending")) return "no picture (" + st + ")";
        File crash = new File(dir(c), "last_crash");
        if (crash.exists()) {
            try {
                return "crash: " + new String(Files.readAllBytes(crash.toPath()), StandardCharsets.UTF_8).trim();
            } catch (Exception e) {
                return "crash";
            }
        }
        return null;
    }

    // Reads the logs now (before this run overwrites them) and hands them to LogUploadService; true if queued.
    public static boolean sendPending(Context c, String mode) {
        String reason = pendingReason(c);
        new File(dir(c), "last_crash").delete();
        if (reason == null || !Boolean.TRUE.equals(consent(c))) return false;
        String url = config(c, "supabase_url"), key = config(c, "supabase_key");
        if (url.isEmpty() || key.isEmpty()) { Log.i("BO3", "log upload: no backend in this build"); return false; }
        final JSONObject row = new JSONObject();
        try {
            String soc = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : Build.HARDWARE;
            row.put("app_version", version(c));
            row.put("mode", mode);
            row.put("reason", cut(reason, 1000));
            row.put("device", cut(Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")", 200));
            row.put("soc", soc + " / " + Build.BOARD);
            row.put("android", Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")");
            row.put("driver", GpuSettings.choice(c) + ", last run: " + GpuSettings.state(c));
            row.put("game_log", scrub(tail(new File(dir(c), "bo3.log"))));
            row.put("logcat", scrub(logcat()));
        } catch (Exception e) {
            Log.w("BO3", "log upload: " + e);
            return false;
        }
        // sent by LogUploadService in the app's main process: it outlives a game that crashes right away,
        // and keeps the report for the next start when offline
        try {
            row.put("_endpoint", url.replaceAll("/+$", "") + "/rest/v1/error_reports");
            row.put("_key", key);
            File out = new File(c.getCacheDir(), "reports");
            out.mkdirs();
            File f = new File(out, "report-" + System.currentTimeMillis() + ".json");
            try (FileOutputStream o = new FileOutputStream(f)) { o.write(row.toString().getBytes(StandardCharsets.UTF_8)); }
            c.startService(new android.content.Intent(c, LogUploadService.class));
            return true;
        } catch (Exception e) {
            Log.w("BO3", "log upload: " + e);
            return false;
        }
    }

    // POST of one saved report (LogUploadService); true = done with it (sent or refused for good)
    static boolean post(File f) {
        try {
            JSONObject row = new JSONObject(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
            String endpoint = (String) row.remove("_endpoint"), key = (String) row.remove("_key");
            byte[] r = Http.request("POST", endpoint, new String[] {
                "apikey", key, "Authorization", "Bearer " + key,
                "Content-Type", "application/json", "Prefer", "return=minimal" },
                row.toString().getBytes(StandardCharsets.UTF_8), 30000);
            int status = ((r[0] & 255) << 24) | ((r[1] & 255) << 16) | ((r[2] & 255) << 8) | (r[3] & 255);
            Log.i("BO3", "log upload: HTTP " + status + (status >= 300 || status == 0
                    ? " " + new String(r, 4, Math.min(r.length - 4, 300), StandardCharsets.UTF_8) : ""));
            // offline / server trouble / table not created yet (404): kept for the next start; refused: dropped
            return status != 0 && status < 500 && status != 404;
        } catch (Exception e) {
            Log.w("BO3", "log upload: " + e);
            return true;
        }
    }

    // bo3recomp.ini [online] (the player's own backend) or the one compiled in
    static String config(Context c, String k) {
        try {
            for (String l : Files.readAllLines(new File(dir(c), "bo3recomp.ini").toPath(), StandardCharsets.UTF_8))
                if (l.startsWith(k + "=")) return l.substring(k.length() + 1).trim();
        } catch (Exception ignored) { }
        try {
            String v = defaultConfig(k);
            return v != null ? v : "";
        } catch (Throwable e) {
            return "";
        }
    }

    static String version(Context c) {
        try {
            PackageInfo p = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return p.versionName + " (" + p.versionCode + ", " + new File(p.applicationInfo.sourceDir).lastModified() / 1000 + ")";
        } catch (Exception e) {
            return "?";
        }
    }

    static String tail(File f) {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            long n = r.length(), from = Math.max(0, n - MAX);
            byte[] b = new byte[(int) (n - from)];
            r.seek(from);
            r.readFully(b);
            // the start of the log (device, driver) matters too when it is cut
            String head = "";
            if (from > 0) {
                byte[] h = new byte[8192];
                r.seek(0);
                int k = r.read(h);
                head = new String(h, 0, Math.max(k, 0), StandardCharsets.UTF_8) + "\n[...]\n";
            }
            return head + new String(b, StandardCharsets.UTF_8);
        } catch (Exception e) {
            return "(no bo3.log: " + e + ")";
        }
    }

    static String logcat() {
        try {
            Process p = new ProcessBuilder("logcat", "-d", "-v", "threadtime", "-t", "4000").redirectErrorStream(true).start();
            ByteArrayOutputStream o = new ByteArrayOutputStream();
            InputStream in = p.getInputStream();
            byte[] buf = new byte[16384];
            for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
            p.waitFor();
            String s = o.toString("UTF-8");
            return s.length() > MAX ? s.substring(s.length() - MAX) : s;
        } catch (Exception e) {
            return "(no logcat: " + e + ")";
        }
    }

    static String scrub(String s) {
        return s.replaceAll("[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}", "<email>")
                .replaceAll("eyJ[A-Za-z0-9_-]{10,}\\.[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+", "<token>")
                .replaceAll("(?i)(password|refresh_token|access_token)([\"=: ]+)[^\\s\",&]+", "$1$2<hidden>");
    }

    static String cut(String s, int n) { return s.length() > n ? s.substring(0, n) : s; }
}
