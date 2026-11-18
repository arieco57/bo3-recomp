package com.bo3recomp;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ClipDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.graphics.drawable.RippleDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

// First screen when the game is not installed yet (Multiplayer or Zombies): the player picks their own files
// with Android's file picker - disc image (.iso / .zar), Title Update, Compatibility Pack 1 and 2 - and
// "Convertir" installs them for the phone (libbo3_setup.so). No storage permission: the chosen documents are
// read through their file descriptors. Then the game the player opened starts.
public class InstallerActivity extends Activity {
    static final String[] TITLES = { "Imagen del disco", "Title Update", "Compatibility Pack 1", "Compatibility Pack 2" };
    static final String[] HINTS = {
        "Tu disco de Black Ops III para Xbox 360: .iso, .zar o .zip.",
        "El TU 8 (archivo tu00000001_00000000), su .zip o la carpeta extraída.",
        "Paquete gratuito de Xbox Live: archivo, .zip o carpeta. La carpeta 00000002 instala todos los DLC que tenga.",
        "El otro paquete gratuito (o la misma carpeta 00000002)." };

    // look: dark, BO3 orange
    static final int BG = Color.rgb(11, 13, 16), CARD = Color.rgb(24, 27, 32), CARD_DONE = Color.rgb(22, 32, 26),
        LINE = Color.rgb(44, 49, 57), TEXT = Color.rgb(236, 238, 241), SUB = Color.rgb(150, 156, 165),
        ACCENT = Color.rgb(242, 107, 29), OK = Color.rgb(76, 196, 120), ERR = Color.rgb(255, 120, 100);

    final Uri[] chosen = new Uri[4];
    final boolean[] isFolder = new boolean[4];
    final TextView[] folders = new TextView[4];
    final TextView[] badges = new TextView[4];
    final LinearLayout[] cards = new LinearLayout[4];
    final String[] names = new String[4];
    final TextView[] labels = new TextView[4];
    final TextView[] picks = new TextView[4];
    // optional: more DLC (map packs...), several files at once or a folder
    final java.util.List<Uri> extra = new java.util.ArrayList<>();
    final java.util.List<String> extraNames = new java.util.ArrayList<>();
    final java.util.List<Boolean> extraTree = new java.util.ArrayList<>();
    TextView extraLabel;
    TextView extraFiles, extraFolder, extraBadge;
    TextView status, percentText;
    ProgressBar bar;
    LinearLayout progressCard;
    ScrollView scroll;
    TextView convert;
    final Handler ui = new Handler(Looper.getMainLooper());
    static volatile boolean running, done, failed;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        GameStart.uiOpen = true;   // (GameStart.exitWhenDone)
        if (Installer.isInstalled(this) && !running && !getIntent().getBooleanExtra("debug_ui", false)) { startGame(); return; }
        buildUi();
        tick();
        // tests without the file picker (emulator): am start ... --es debug_paths "disc|tu|cp1|cp2" (paths
        // readable by the app; folders are used in place)
        String dbg = getIntent().getStringExtra("debug_paths");
        if (dbg != null) {
            String[] p = dbg.split("\\|");
            for (int i = 4; i < p.length; i++) {       // more: optional DLC
                File f = new File(p[i]);
                extra.add(Uri.fromFile(f)); extraTree.add(false); extraNames.add(f.getName());
                extraLabel.setText(extra.size() + " elegido(s) (prueba)"); markExtra();
            }
            for (int i = 0; i < 4 && i < p.length; i++) {
                File f = new File(p[i]);
                chosen[i] = Uri.fromFile(f); isFolder[i] = false; debugDir[i] = f.isDirectory(); names[i] = f.getName();
                setChosen(i, names[i] + " (prueba)");
            }
            setEnabled(convert, all());
            // (debug_convert: start right away - tests on a locked phone)
            if (getIntent().getBooleanExtra("debug_convert", false) && all()) startInstall();
        }
    }
    final boolean[] debugDir = new boolean[4];

    int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    GradientDrawable round(int color, float radiusDp, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radiusDp));
        if (stroke != 0) g.setStroke(dp(1), stroke);
        return g;
    }

    TextView text(LinearLayout p, String s, float size, int color) {
        TextView t = new TextView(this);
        t.setText(s); t.setTextSize(size); t.setTextColor(color);
        t.setLineSpacing(0, 1.15f);
        if (p != null) p.addView(t);
        return t;
    }

    // rounded button: filled (orange) or outlined, with the touch ripple
    TextView button(String label, boolean primary) {
        TextView b = new TextView(this);
        b.setText(label);
        b.setTextSize(primary ? 17 : 14);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setGravity(Gravity.CENTER);
        b.setTextColor(primary ? Color.WHITE : TEXT);
        b.setPadding(dp(primary ? 20 : 16), dp(primary ? 15 : 9), dp(primary ? 20 : 16), dp(primary ? 15 : 9));
        Drawable shape = primary ? round(ACCENT, 14, 0) : round(Color.rgb(34, 38, 45), 20, LINE);
        b.setBackground(new RippleDrawable(ColorStateList.valueOf(Color.argb(70, 255, 255, 255)), shape, round(Color.WHITE, primary ? 14 : 20, 0)));
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    void setEnabled(View v, boolean on) {
        v.setEnabled(on);
        v.setAlpha(on ? 1f : 0.38f);
    }

    TextView badge(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(15);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setGravity(Gravity.CENTER);
        t.setTextColor(TEXT);
        GradientDrawable g = new GradientDrawable();
        g.setShape(GradientDrawable.OVAL);
        g.setColor(Color.rgb(40, 45, 53));
        t.setBackground(g);
        return t;
    }

    void markBadge(TextView b, boolean ok, String num) {
        GradientDrawable g = (GradientDrawable) b.getBackground();
        g.setColor(ok ? OK : Color.rgb(40, 45, 53));
        b.setText(ok ? "\u2713" : num);
        b.setTextColor(ok ? Color.rgb(10, 30, 16) : TEXT);
    }

    void setChosen(int i, String name) {
        labels[i].setText(name);
        labels[i].setTextColor(OK);
        markBadge(badges[i], true, "");
        cards[i].setBackground(round(CARD_DONE, 16, Color.rgb(46, 92, 62)));
        picks[i].setText(i == 0 ? "Cambiar" : "Archivo");
    }

    void markExtra() {
        extraLabel.setTextColor(OK);
        markBadge(extraBadge, true, "");
    }

    // one step: number, title, hint, chosen name, buttons
    LinearLayout card(LinearLayout parent, TextView num, String title, String hint, TextView label, TextView... buttons) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(16), dp(14), dp(16), dp(14));
        c.setBackground(round(CARD, 16, LINE));
        LinearLayout top = new LinearLayout(this);
        top.setOrientation(LinearLayout.HORIZONTAL);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.addView(num, new LinearLayout.LayoutParams(dp(32), dp(32)));
        LinearLayout tx = new LinearLayout(this);
        tx.setOrientation(LinearLayout.VERTICAL);
        tx.setPadding(dp(14), 0, 0, 0);
        TextView t = text(tx, title, 17, TEXT);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        text(tx, hint, 13, SUB);
        top.addView(tx, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        c.addView(top);
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(46), dp(10), 0, 0);
        label.setTextSize(13);
        label.setSingleLine(true);
        label.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        row.addView(label, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        for (TextView b : buttons) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.leftMargin = dp(8);
            row.addView(b, lp);
        }
        c.addView(row);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(12);
        parent.addView(c, lp);
        return c;
    }

    void buildUi() {
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        scroll = new ScrollView(this);
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] { Color.rgb(28, 20, 16), BG, BG });
        scroll.setBackground(bg);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(dp(20), dp(28), dp(20), dp(28));
        LinearLayout col = new LinearLayout(this);   // (a centred column on wide screens)
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        col.addView(l, new LinearLayout.LayoutParams(Math.min(getResources().getDisplayMetrics().widthPixels, dp(640)), LinearLayout.LayoutParams.WRAP_CONTENT));
        scroll.addView(col);

        // header: the game's icon, title, what happens
        boolean mp = "mp".equals(getIntent().getStringExtra("mode"));
        LinearLayout head = new LinearLayout(this);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        ImageView icon = new ImageView(this);
        icon.setImageResource(mp ? R.mipmap.ic_mp : R.mipmap.ic_zm);
        head.addView(icon, new LinearLayout.LayoutParams(dp(56), dp(56)));
        LinearLayout ht = new LinearLayout(this);
        ht.setOrientation(LinearLayout.VERTICAL);
        ht.setPadding(dp(14), 0, 0, 0);
        TextView kicker = text(ht, mp ? "BO3 RECOMP \u00b7 MULTIJUGADOR" : "BO3 RECOMP \u00b7 ZOMBIES", 12, ACCENT);
        kicker.setTypeface(Typeface.DEFAULT_BOLD);
        kicker.setLetterSpacing(0.12f);
        TextView title = text(ht, "Instalación", 28, TEXT);
        title.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        head.addView(ht);
        l.addView(head);
        TextView intro = text(l, "Elegí los archivos de tu copia de Call of Duty: Black Ops III para Xbox 360. Con los cuatro elegidos, "
                + "tocá Convertir y se adaptan para el teléfono.", 15, SUB);
        intro.setPadding(0, dp(16), 0, dp(4));
        LinearLayout chips = new LinearLayout(this);
        chips.setOrientation(LinearLayout.HORIZONTAL);
        for (String c : new String[] { "\u23f1  10 a 20 min", "\ud83d\udcbe  8 GB libres" }) {
            TextView t = text(null, c, 12, TEXT);
            t.setPadding(dp(12), dp(6), dp(12), dp(6));
            t.setBackground(round(Color.rgb(32, 36, 42), 14, 0));
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
            lp.rightMargin = dp(8);
            lp.topMargin = dp(8);
            chips.addView(t, lp);
        }
        l.addView(chips);

        for (int i = 0; i < 4; i++) {
            final int k = i;
            badges[i] = badge(String.valueOf(i + 1));
            labels[i] = text(null, "Sin elegir", 13, SUB);
            picks[i] = button(i == 0 ? "Elegir" : "Archivo", false);
            picks[i].setOnClickListener(v -> pick(k));
            if (i > 0) {      // Title Update / Compatibility Packs: also an already extracted folder (Xenia's layout)
                folders[i] = button("Carpeta", false);
                folders[i].setOnClickListener(v -> pickFolder(k));
                cards[i] = card(l, badges[i], TITLES[i], HINTS[i], labels[i], picks[i], folders[i]);
            } else {
                cards[i] = card(l, badges[i], TITLES[i], HINTS[i], labels[i], picks[i]);
            }
        }
        extraBadge = badge("+");
        extraLabel = text(null, "Ninguno", 13, SUB);
        extraFiles = button("Archivos", false);
        extraFiles.setOnClickListener(v -> {
            Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            it.addCategory(Intent.CATEGORY_OPENABLE); it.setType("*/*"); it.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            startActivityForResult(it, 300);
        });
        extraFolder = button("Carpeta", false);
        extraFolder.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), 301));
        card(l, extraBadge, "Otros DLC (opcional)", "Packs de mapas y demás contenido: varios archivos (o .zip) a la vez, o una carpeta.",
             extraLabel, extraFiles, extraFolder);

        convert = button("Convertir", true);
        convert.setOnClickListener(v -> startInstall());
        setEnabled(convert, false);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(22);
        l.addView(convert, lp);

        // progress: percentage, bar, what is being done
        progressCard = new LinearLayout(this);
        progressCard.setOrientation(LinearLayout.VERTICAL);
        progressCard.setPadding(dp(18), dp(16), dp(18), dp(16));
        progressCard.setBackground(round(CARD, 16, LINE));
        progressCard.setVisibility(View.GONE);
        percentText = text(progressCard, "0%", 34, TEXT);
        percentText.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        GradientDrawable track = round(Color.rgb(40, 45, 53), 6, 0);
        GradientDrawable fill = round(ACCENT, 6, 0);
        LayerDrawable ld = new LayerDrawable(new Drawable[] { track, new ClipDrawable(fill, Gravity.START, ClipDrawable.HORIZONTAL) });
        ld.setId(0, android.R.id.background);
        ld.setId(1, android.R.id.progress);
        bar.setProgressDrawable(ld);
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(10));
        bp.topMargin = dp(8);
        bp.bottomMargin = dp(12);
        progressCard.addView(bar, bp);
        status = text(progressCard, "", 14, SUB);
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        pp.topMargin = dp(14);
        l.addView(progressCard, pp);
        setContentView(scroll);
    }

    void pick(int i) {
        Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        it.addCategory(Intent.CATEGORY_OPENABLE);
        it.setType("*/*");
        startActivityForResult(it, 100 + i);
    }

    void pickFolder(int i) {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), 200 + i);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if ((req == 300 || req == 301) && res == RESULT_OK && data != null) {
            java.util.List<Uri> got = new java.util.ArrayList<>();
            if (data.getClipData() != null) for (int k = 0; k < data.getClipData().getItemCount(); k++) got.add(data.getClipData().getItemAt(k).getUri());
            else if (data.getData() != null) got.add(data.getData());
            for (Uri u : got) {
                try { getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception e) { }
                extra.add(u); extraTree.add(req == 301);
                extraNames.add(req == 301 ? treeName(u) + " (carpeta)" : displayName(u));
            }
            extraLabel.setText(extra.size() + " elegido(s)"); markExtra();
            return;
        }
        boolean tree = req >= 200;
        int i = req - (tree ? 200 : 100);
        if (i < 0 || i > 3 || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        try { getContentResolver().takePersistableUriPermission(u, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (Exception e) { }
        chosen[i] = u;
        isFolder[i] = tree;
        names[i] = tree ? treeName(u) + " (carpeta)" : displayName(u);
        setChosen(i, names[i]);
        setEnabled(convert, all() && !running);
    }

    boolean all() { for (Uri u : chosen) if (u == null) return false; return true; }

    String treeName(Uri tree) {
        try {
            Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
            String n = displayName(doc);
            if (n != null && !n.isEmpty()) return n;
        } catch (Exception e) { }
        String id = DocumentsContract.getTreeDocumentId(tree);
        return id.substring(id.lastIndexOf('/') + 1).replace(":", "");
    }

    // copies a picked folder (document tree) into the data folder: the installer moves it into place
    long copied;
    void copyTree(Uri tree, String docId, File dst) throws Exception {
        dst.mkdirs();
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = { DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE };
        try (Cursor c = getContentResolver().query(kids, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
                if (name == null || name.contains("/") || name.equals("..")) continue;
                File out = new File(dst, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) { copyTree(tree, id, out); continue; }
                try (InputStream in = getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
                     OutputStream o = new FileOutputStream(out)) {
                    byte[] buf = new byte[1 << 20]; int n;
                    while ((n = in.read(buf)) > 0) {
                        o.write(buf, 0, n); copied += n;
                        Installer.status = "Copiando la carpeta " + dst.getName() + ": " + (copied >> 20) + " MB";
                    }
                }
            }
        }
    }

    // extracted by libbo3_setup.so from the file's descriptor (setup_jni.cpp: not through Java streams,
    // which kept every buffer in Husk on iOS); the native counter drives the progress text
    void unzip(Uri zip, File dst, String name) throws Exception {
        dst.mkdirs();
        Installer.load();
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(zip, "r")) {
            final long base = Installer.unzipBytes();
            final boolean[] busy = { true };
            Thread mon = new Thread(() -> {
                while (busy[0]) {
                    Installer.status = "Descomprimiendo " + name + ": " + ((Installer.unzipBytes() - base) >> 20) + " MB";
                    try { Thread.sleep(250); } catch (InterruptedException e) { return; }
                }
            });
            mon.start();
            String err;
            try { err = Installer.unzipFd(pfd.getFd(), dst.getAbsolutePath()); }
            finally { busy[0] = false; mon.interrupt(); }
            if (err != null && !err.isEmpty()) throw new Exception(name + ": " + err);
        }
    }

    static void deleteTree(File f) {
        File[] k = f.listFiles();
        if (k != null) for (File c : k) deleteTree(c);
        f.delete();
    }

    String displayName(Uri u) {
        try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) { String n = c.getString(0); if (n != null) return n; }
        } catch (Exception e) { }
        String p = u.getLastPathSegment();
        return p != null ? p : u.toString();
    }

    void startInstall() {
        if (running || !all()) return;
        running = true; done = false; failed = false;
        Installer.message = ""; Installer.status = "Preparando"; Installer.percent = 0;
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        lockInputs(true);
        progressCard.setVisibility(View.VISIBLE);
        percentText.setTextSize(34);
        status.setTextColor(SUB);
        convert.setVisibility(View.GONE);
        ui.postDelayed(() -> scroll.fullScroll(View.FOCUS_DOWN), 250);
        final Uri[] uris = chosen.clone();
        final boolean[] tree = isFolder.clone();
        final String[] nm = names.clone();
        final String dir = Installer.dataDir(this).getAbsolutePath();
        final java.util.List<Uri> extraUris = new java.util.ArrayList<>(extra);
        final java.util.List<String> extraNm = new java.util.ArrayList<>(extraNames);
        final java.util.List<Boolean> extraT = new java.util.ArrayList<>(extraTree);
        new Thread(() -> {
            ParcelFileDescriptor[] fds = new ParcelFileDescriptor[4];
            File staging = new File(dir, "instalar-carpetas");
            try {
                java.util.List<String> paths = new java.util.ArrayList<>(), names2 = new java.util.ArrayList<>();
                java.util.Set<Uri> seen = new java.util.HashSet<>();
                deleteTree(staging);
                copied = 0;
                for (int i = 0; i < 4; i++) {
                    if (!seen.add(uris[i])) continue;          // the same folder for both packs: once
                    if (debugDir[i]) { paths.add(uris[i].getPath()); names2.add(nm[i]); continue; }
                    if (!tree[i] && nm[i] != null && nm[i].toLowerCase().endsWith(".zip")) {
                        // a zip (how these dumps are usually shared): unzipped into the data folder first
                        File dst = new File(staging, String.valueOf(i));
                        unzip(uris[i], dst, nm[i]);
                        paths.add(dst.getAbsolutePath()); names2.add(nm[i]);
                        continue;
                    }
                    if (tree[i]) {
                        File dst = new File(new File(staging, String.valueOf(i)), treeName(uris[i]));
                        copyTree(uris[i], DocumentsContract.getTreeDocumentId(uris[i]), dst);
                        paths.add(dst.getAbsolutePath());
                    } else {
                        fds[i] = getContentResolver().openFileDescriptor(uris[i], "r");
                        paths.add("/proc/self/fd/" + fds[i].getFd());
                    }
                    names2.add(nm[i]);
                }
                // optional DLC
                java.util.List<ParcelFileDescriptor> xfds = new java.util.ArrayList<>();
                for (int j = 0; j < extraUris.size(); j++) {
                    Uri u = extraUris.get(j); String n = extraNm.get(j);
                    if (!seen.add(u)) continue;
                    if (extraT.get(j)) {
                        File dst = new File(new File(staging, "x" + j), treeName(u));
                        copyTree(u, DocumentsContract.getTreeDocumentId(u), dst);
                        paths.add(dst.getAbsolutePath());
                    } else if (n != null && n.toLowerCase().endsWith(".zip")) {
                        File dst = new File(staging, "x" + j);
                        unzip(u, dst, n);
                        paths.add(dst.getAbsolutePath());
                    } else {
                        ParcelFileDescriptor f = getContentResolver().openFileDescriptor(u, "r");
                        xfds.add(f);
                        paths.add("/proc/self/fd/" + f.getFd());
                    }
                    names2.add(n);
                }
                Installer.load();
                boolean ok = Installer.install(dir, paths.toArray(new String[0]), names2.toArray(new String[0]));
                for (ParcelFileDescriptor f : xfds) try { f.close(); } catch (Exception e) { }
                done = ok; failed = !ok;
            } catch (Throwable t) {
                Installer.message = "No se pudo instalar: " + t;
                failed = true;
            } finally {
                for (ParcelFileDescriptor f : fds) if (f != null) try { f.close(); } catch (Exception e) { }
                deleteTree(staging);
                running = false;
            }
        }, "bo3-install").start();
    }

    void lockInputs(boolean lock) {
        for (TextView p : picks) setEnabled(p, !lock);
        for (TextView f : folders) if (f != null) setEnabled(f, !lock);
        setEnabled(extraFiles, !lock);
        setEnabled(extraFolder, !lock);
    }

    void tick() {
        if (isFinishing()) return;
        if (running) {
            // (copying / unzipping the chosen files comes before the converter reports a percentage)
            boolean prep = Installer.percent <= 0;
            percentText.setText(prep ? "Preparando" : Installer.percent + "%");
            percentText.setTextSize(prep ? 26 : 34);
            percentText.setTextColor(TEXT);
            bar.setIndeterminate(false);
            status.setText(Installer.status + "\n\nNo cierres la app mientras se convierte. La pantalla queda encendida.");
            bar.setProgress(Installer.percent);
        } else if (done) {
            percentText.setText("\u2713 Listo");
            percentText.setTextColor(OK);
            status.setText("Arrancando el juego...");
            bar.setProgress(100);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            ui.postDelayed(this::startGame, 900);
            return;
        } else if (failed) {
            percentText.setText("No se pudo instalar");
            percentText.setTextColor(ERR);
            percentText.setTextSize(22);
            status.setText((Installer.message.isEmpty() ? Installer.status : Installer.message)
                    + "\n\nRevisá los archivos elegidos y volvé a tocar Convertir.");
            status.setTextColor(TEXT);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            lockInputs(false);
            convert.setVisibility(View.VISIBLE);
            setEnabled(convert, all());
            failed = false;
        }
        ui.postDelayed(this::tick, 400);
    }

    // back to the game the player opened (Multiplayer or Zombies)
    void startGame() {
        Class<?> game = "mp".equals(getIntent().getStringExtra("mode")) ? MultiplayerActivity.class : ZombiesActivity.class;
        startActivity(new Intent(this, game));
        finish();
    }
}
