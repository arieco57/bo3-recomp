package com.bo3recomp;

import android.content.Context;
import android.net.Uri;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

// Video driver choice, as files in the data folder that runtime/src/gpu/vk.cpp reads:
// "use_stock_driver" (the phone's driver), "use_turnip" (Turnip even without KGSL), neither = automatic
// (Turnip on Adreno, the phone's driver on anything else). gpu_state is written by the game:
// "starting <driver>" until 300 frames were presented, then "ok <driver>".
public class GpuSettings {
    public static final String AUTO = "auto", TURNIP = "turnip", STOCK = "stock", CUSTOM = "custom";

    static File dir(Context c) { return Installer.dataDir(c); }

    public static String choice(Context c) {
        if (new File(dir(c), "use_custom_driver").exists() && customName(c) != null) return CUSTOM;
        if (new File(dir(c), "use_stock_driver").exists()) return STOCK;
        if (new File(dir(c), "use_turnip").exists()) return TURNIP;
        return AUTO;
    }

    public static void set(Context c, String choice) {
        File stock = new File(dir(c), "use_stock_driver"), turnip = new File(dir(c), "use_turnip"), custom = new File(dir(c), "use_custom_driver");
        stock.delete();
        turnip.delete();
        custom.delete();
        try {
            if (STOCK.equals(choice)) stock.createNewFile();
            if (TURNIP.equals(choice)) turnip.createNewFile();
            String lib = customLib(c);
            if (CUSTOM.equals(choice) && lib != null) Files.write(custom.toPath(), lib.getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
    }

    // ---- custom drivers: a Turnip / Mesa .zip (adrenotools layout: meta.json + the library) or a bare .so,
    // kept in <internal files>/drivers/custom/ (app-private storage: Android loads libraries from there)
    static File customDir(Context c) { return new File(c.getFilesDir(), "drivers/custom"); }

    // the installed library's file name, or null
    static String customLib(Context c) {
        try {
            File f = new File(customDir(c), "library.txt");
            String n = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
            return new File(customDir(c), n).exists() ? n : null;
        } catch (Exception e) {
            return null;
        }
    }

    // what to show for it ("Turnip v25.x" from meta.json, else the file name), or null
    public static String customName(Context c) {
        if (customLib(c) == null) return null;
        try {
            return new String(Files.readAllBytes(new File(customDir(c), "name.txt").toPath()), StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return customLib(c);
        }
    }

    // installs the picked file; returns its display name, throws with a message the player can read
    public static String installCustom(Context c, Uri uri, String displayName) throws Exception {
        File d = customDir(c);
        File[] old = d.listFiles();
        if (old != null) for (File f : old) f.delete();
        d.mkdirs();
        String lib = null, name = null, metaJson = null;
        boolean zip = displayName != null && displayName.toLowerCase().endsWith(".zip");
        try (InputStream in = c.getContentResolver().openInputStream(uri)) {
            if (in == null) throw new Exception("No se pudo abrir el archivo.");
            if (zip) {
                java.util.zip.ZipInputStream z = new java.util.zip.ZipInputStream(new java.io.BufferedInputStream(in));
                java.util.zip.ZipEntry e;
                byte[] buf = new byte[1 << 16];
                while ((e = z.getNextEntry()) != null) {
                    if (e.isDirectory()) continue;
                    String n = new File(e.getName()).getName();   // (flattened: no paths out of the folder)
                    if (!n.endsWith(".so") && !n.equals("meta.json")) continue;
                    java.io.ByteArrayOutputStream mem = n.equals("meta.json") ? new java.io.ByteArrayOutputStream() : null;
                    try (OutputStream o = mem != null ? mem : new FileOutputStream(new File(d, n))) {
                        int k;
                        while ((k = z.read(buf)) > 0) o.write(buf, 0, k);
                    }
                    if (mem != null) metaJson = mem.toString("UTF-8");
                    else if (lib == null || n.startsWith("vulkan.") || n.contains("freedreno")) lib = n;
                }
            } else {
                String n = displayName != null && displayName.endsWith(".so") ? new File(displayName).getName() : "libvulkan_custom.so";
                try (OutputStream o = new FileOutputStream(new File(d, n))) {
                    byte[] buf = new byte[1 << 16];
                    int k;
                    while ((k = in.read(buf)) > 0) o.write(buf, 0, k);
                }
                lib = n;
            }
        }
        if (metaJson != null) {
            try {
                org.json.JSONObject m = new org.json.JSONObject(metaJson);
                String ln = m.optString("libraryName", null);
                if (ln != null && new File(d, ln).exists()) lib = ln;
                name = (m.optString("name", "") + " " + m.optString("driverVersion", "")).trim();
            } catch (Exception ignored) { }
        }
        if (lib == null) throw new Exception("El archivo no tiene ningún driver (.so). Tiene que ser un .zip de Turnip / Mesa como los de los emuladores, o el .so.");
        if (name == null || name.isEmpty()) name = displayName != null ? displayName : lib;
        Files.write(new File(d, "library.txt").toPath(), lib.getBytes(StandardCharsets.UTF_8));
        Files.write(new File(d, "name.txt").toPath(), name.getBytes(StandardCharsets.UTF_8));
        return name;
    }

    public static String state(Context c) {
        try {
            return new String(Files.readAllBytes(new File(dir(c), "gpu_state").toPath()), StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return "";
        }
    }

    public static void setState(Context c, String s) {
        try (FileOutputStream o = new FileOutputStream(new File(dir(c), "gpu_state"))) {
            o.write((s + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) { }
    }
}
