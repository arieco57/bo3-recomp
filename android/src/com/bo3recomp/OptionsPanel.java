package com.bo3recomp;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.concurrent.CountDownLatch;

import org.libsdl.app.SDL;

// The in-game options on Android (OPCIONES button / F1 -> runtime/src/video_options.cpp): a panel in the
// look of the installer instead of the system's message boxes. The calling (native) thread waits; the
// result goes back as { render scale, FPS counter, FPS limit, on-screen controller 0/1 } and the native
// side applies it. Video driver and error reports are saved here directly (GpuSettings / LogUpload files).
public class OptionsPanel {
    // the installer's look (InstallerActivity)
    static final int BG = Color.rgb(11, 13, 16), CARD = Color.rgb(24, 27, 32), LINE = Color.rgb(44, 49, 57),
        TEXT = Color.rgb(236, 238, 241), SUB = Color.rgb(150, 156, 165), ACCENT = Color.rgb(242, 107, 29),
        CHIP = Color.rgb(36, 40, 47), OK = Color.rgb(76, 196, 120);

    static final float[] SCALES = { 0.25f, 0.35f, 0.5f, 0.75f, 1f };   // the same in every port
    static final String[] OVERLAYS = { "Apagado", "FPS", "Detallado" };
    static final int[] LIMITS = { 30, 60, 120, 144, 165, 240 };
    static final String[] DRIVERS = { GpuSettings.AUTO, GpuSettings.TURNIP, GpuSettings.STOCK };
    static final String[] DRIVER_NAMES = { "Automático", "Turnip", "Del teléfono" };

    public static float[] show(final float scale, final int overlay, final int limit, final boolean pad, final int online,
                               final int shadows, final boolean lite) {
        final Context ctx = SDL.getContext();
        if (!(ctx instanceof Activity)) return null;
        final Activity act = (Activity) ctx;
        final float[] result = { scale, overlay, limit, pad ? 1 : 0, shadows, lite ? 1 : 0 };
        final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(() -> {
            try {
                new OptionsPanel(act, result).open(online, done);
            } catch (Throwable t) {
                android.util.Log.w("BO3", "options panel: " + t);
                done.countDown();
            }
        });
        try { done.await(); } catch (InterruptedException e) { return null; }
        return result;
    }

    final Activity a;
    final float[] r;
    final float d;

    OptionsPanel(Activity a, float[] r) {
        this.a = a;
        this.r = r;
        d = a.getResources().getDisplayMetrics().density;
    }

    int dp(float v) { return Math.round(v * d); }

    GradientDrawable round(int color, float radius, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(dp(radius));
        if (stroke != 0) g.setStroke(dp(1), stroke);
        return g;
    }

    TextView text(String s, float size, int color, boolean bold) {
        TextView t = new TextView(a);
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    // a row of chips, one selected; returns the chips so the caller can mark the choice
    interface Pick { void on(int i); }

    View chips(String[] labels, String[] subs, int selected, Pick pick) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        final TextView[] cs = new TextView[labels.length];
        for (int i = 0; i < labels.length; i++) {
            final int k = i;
            TextView c = text(subs == null ? labels[i] : labels[i] + "\n" + subs[i], subs == null ? 14 : 13, TEXT, true);
            c.setGravity(Gravity.CENTER);
            c.setSingleLine(false);
            c.setPadding(dp(4), dp(subs == null ? 9 : 7), dp(4), dp(subs == null ? 9 : 7));
            c.setLineSpacing(0, 1.05f);
            c.setOnClickListener(v -> {
                for (int j = 0; j < cs.length; j++) mark(cs[j], j == k);
                pick.on(k);
            });
            cs[i] = c;
            mark(c, i == selected);
            // the chips share the row's width (no hidden ones off to the side)
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            if (i < labels.length - 1) lp.rightMargin = dp(6);
            row.addView(c, lp);
        }
        return row;
    }

    void mark(TextView c, boolean on) {
        GradientDrawable shape = on ? round(ACCENT, 20, 0) : round(Color.rgb(34, 38, 45), 20, LINE);
        c.setBackground(new RippleDrawable(ColorStateList.valueOf(Color.argb(60, 255, 255, 255)), shape, round(Color.WHITE, 20, 0)));
        c.setTextColor(on ? Color.WHITE : TEXT);
    }

    LinearLayout section(LinearLayout parent, String title, String hint) {
        LinearLayout s = new LinearLayout(a);
        s.setOrientation(LinearLayout.VERTICAL);
        s.setPadding(dp(16), dp(12), dp(16), dp(14));
        s.setBackground(round(CARD, 16, LINE));
        s.addView(text(title, 17, TEXT, true));
        if (hint != null) {
            TextView h = text(hint, 13, SUB, false);
            h.setPadding(0, dp(2), 0, 0);
            s.addView(h);
        }
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        parent.addView(s, lp);
        return s;
    }

    void addSpaced(LinearLayout s, View v) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(10);
        s.addView(v, lp);
    }

    // title + hint on the left, a switch on the right
    // a pill switch drawn here (the system Switch looks different on every phone)
    class Toggle extends android.widget.FrameLayout {
        boolean checked;
        final View knob = new View(a);
        Toggle(boolean on) {
            super(a);
            GradientDrawable k = new GradientDrawable();
            k.setShape(GradientDrawable.OVAL);
            k.setColor(Color.WHITE);
            knob.setBackground(k);
            LayoutParams lp = new LayoutParams(dp(24), dp(24));
            lp.gravity = Gravity.CENTER_VERTICAL;
            lp.leftMargin = lp.rightMargin = dp(3);
            addView(knob, lp);
            setOnClickListener(v -> toggle());
            set(on);
        }
        void set(boolean on) {
            checked = on;
            setBackground(round(on ? ACCENT : Color.rgb(60, 66, 76), 15, 0));
            LayoutParams lp = (LayoutParams) knob.getLayoutParams();
            lp.gravity = Gravity.CENTER_VERTICAL | (on ? Gravity.END : Gravity.START);
            knob.setLayoutParams(lp);
        }
        void toggle() { set(!checked); }
        boolean isChecked() { return checked; }
    }

    Toggle toggle(LinearLayout parent, String title, String hint, boolean on) {
        LinearLayout s = section(parent, title, hint);
        // (the section's own title/hint moved into a row with the switch)
        s.removeAllViews();
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout tx = new LinearLayout(a);
        tx.setOrientation(LinearLayout.VERTICAL);
        tx.addView(text(title, 17, TEXT, true));
        if (hint != null) tx.addView(text(hint, 13, SUB, false));
        row.addView(tx, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Toggle sw = new Toggle(on);
        row.addView(sw, new LinearLayout.LayoutParams(dp(52), dp(30)));
        row.setOnClickListener(v -> sw.toggle());
        s.addView(row);
        return sw;
    }

    static String res(float sc) { return Math.round(1280 * sc) + "×" + Math.round(720 * sc); }

    void open(int online, CountDownLatch done) {
        final Dialog dlg = new Dialog(a, android.R.style.Theme_DeviceDefault_NoActionBar_Fullscreen);
        boolean mp = a instanceof MultiplayerActivity;
        android.util.DisplayMetrics dm = a.getResources().getDisplayMetrics();
        boolean wide = dm.widthPixels / d >= 700;
        LinearLayout page = new LinearLayout(a);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackground(new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] { Color.rgb(28, 20, 16), BG, BG }));
        page.setGravity(Gravity.CENTER_HORIZONTAL);
        LinearLayout panel = new LinearLayout(a);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(wide ? 14 : 28), dp(20), dp(14));
        page.addView(panel, new LinearLayout.LayoutParams(Math.min(dm.widthPixels, dp(wide ? 980 : 640)), ViewGroup.LayoutParams.MATCH_PARENT));

        // header like the installer's: the game's icon, BO3 RECOMP · mode, title; online players at the right
        LinearLayout head = new LinearLayout(a);
        head.setOrientation(LinearLayout.HORIZONTAL);
        head.setGravity(Gravity.CENTER_VERTICAL);
        android.widget.ImageView icon = new android.widget.ImageView(a);
        icon.setImageResource(mp ? R.mipmap.ic_mp : R.mipmap.ic_zm);
        head.addView(icon, new LinearLayout.LayoutParams(dp(wide ? 46 : 56), dp(wide ? 46 : 56)));
        LinearLayout ht = new LinearLayout(a);
        ht.setOrientation(LinearLayout.VERTICAL);
        ht.setPadding(dp(14), 0, 0, 0);
        TextView kicker = text(mp ? "BO3 RECOMP \u00b7 MULTIJUGADOR" : "BO3 RECOMP \u00b7 ZOMBIES", 12, ACCENT, true);
        kicker.setLetterSpacing(0.12f);
        ht.addView(kicker);
        TextView title = text("Opciones", wide ? 24 : 28, TEXT, false);
        title.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        ht.addView(title);
        head.addView(ht, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        TextView on = text(online >= 0 ? "\u25cf  " + online + " online" : "\u25cf  offline", 13, online >= 0 ? OK : SUB, true);
        on.setPadding(dp(12), dp(6), dp(12), dp(6));
        on.setBackground(round(online >= 0 ? Color.rgb(28, 40, 33) : Color.rgb(32, 36, 42), 14, 0));
        head.addView(on);
        panel.addView(head);

        // two columns on a wide (landscape) screen, one otherwise
        LinearLayout cols = new LinearLayout(a);
        cols.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        LinearLayout body = new LinearLayout(a), right = new LinearLayout(a);
        body.setOrientation(LinearLayout.VERTICAL);
        right.setOrientation(LinearLayout.VERTICAL);
        if (wide) {
            LinearLayout.LayoutParams cl = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.25f);
            cl.rightMargin = dp(10);
            cols.addView(body, cl);
            cols.addView(right, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        } else {
            cols.addView(body);
            cols.addView(right);
        }
        ScrollView sv = new ScrollView(a);
        sv.setVerticalScrollBarEnabled(false);
        sv.addView(cols);

        // internal resolution
        int scaleSel = -1;
        for (int i = 0; i < SCALES.length; i++) if (Math.abs(SCALES[i] - r[0]) < 0.01f) scaleSel = i;
        String[] sl = new String[SCALES.length], ss = new String[SCALES.length];
        for (int i = 0; i < SCALES.length; i++) { sl[i] = Math.round(SCALES[i] * 100) + "%"; ss[i] = res(SCALES[i]); }
        LinearLayout s1 = section(body, "Resolución interna", "Más baja = más FPS. 100% es la de la consola.");
        addSpaced(s1, chips(sl, ss, scaleSel, i -> r[0] = SCALES[i]));

        // FPS counter / limit
        LinearLayout s2 = section(body, "Contador de FPS", null);
        addSpaced(s2, chips(OVERLAYS, null, Math.max(0, Math.min(2, (int) r[1])), i -> r[1] = i));
        int limSel = -1;
        String[] ll = new String[LIMITS.length];
        for (int i = 0; i < LIMITS.length; i++) { ll[i] = String.valueOf(LIMITS[i]); if (LIMITS[i] == (int) r[2]) limSel = i; }
        LinearLayout s3 = section(body, "Límite de FPS", "La velocidad del juego no cambia.");
        addSpaced(s3, chips(ll, null, limSel, i -> r[2] = LIMITS[i]));

        // graphics: shadows (live), light shaders (next start)
        LinearLayout s5 = section(body, "Sombras", "Altas: a resolución completa. Bajas: más FPS.");
        addSpaced(s5, chips(new String[] { "Bajas", "Altas" }, null, r[4] != 0 ? 1 : 0, i -> r[4] = i));
        final Toggle liteSw = toggle(right, "Shaders livianos", "Más FPS en GPUs débiles; algún efecto puede verse con puntos. Al reiniciar el juego", r[5] != 0);

        // on-screen controller, video driver, error reports
        final Toggle padSw = toggle(right, "Mando virtual", "Controles en pantalla para jugar sin mando", r[3] != 0);
        // its layout, aim speed (Competitivo: swipe to aim) and opacity; the editor rearranges it
        final int[] pad = { VirtualPad.layoutChoice(a), VirtualPad.sensChoice(a), VirtualPad.opacityChoice(a) };
        final boolean[] editPad = { false };
        LinearLayout s6 = section(right, "Diseño de los controles", "Competitivo: la mitad izquierda mueve, la derecha apunta deslizando.");
        addSpaced(s6, chips(VirtualPad.LAYOUTS, null, pad[0], i -> pad[0] = i));
        TextView sensTitle = text("Sensibilidad al apuntar", 14, SUB, true);
        addSpaced(s6, sensTitle);
        addSpaced(s6, chips(VirtualPad.SENS_NAMES, null, pad[1], i -> pad[1] = i));
        addSpaced(s6, text("Opacidad", 14, SUB, true));
        addSpaced(s6, chips(VirtualPad.OPACITY_NAMES, null, pad[2], i -> pad[2] = i));
        TextView editBtn = text("Mover y cambiar el tamaño de los botones", 14, TEXT, true);
        editBtn.setGravity(Gravity.CENTER);
        editBtn.setPadding(0, dp(10), 0, dp(10));
        editBtn.setBackground(new RippleDrawable(ColorStateList.valueOf(Color.argb(60, 255, 255, 255)), round(CHIP, 14, LINE), round(Color.WHITE, 14, 0)));
        addSpaced(s6, editBtn);
        // (a custom driver, if one is installed, is one more choice - not lost when saving)
        final String customName = GpuSettings.customName(a);
        final String[] drivers = customName != null ? new String[] { GpuSettings.AUTO, GpuSettings.TURNIP, GpuSettings.STOCK, GpuSettings.CUSTOM } : DRIVERS;
        final String[] driverNames = customName != null ? new String[] { DRIVER_NAMES[0], DRIVER_NAMES[1], DRIVER_NAMES[2], "Personalizado" } : DRIVER_NAMES;
        int drvSel = 0;
        String cur = GpuSettings.choice(a);
        for (int i = 0; i < drivers.length; i++) if (drivers[i].equals(cur)) drvSel = i;
        LinearLayout s4 = section(right, "Driver de video", "Se aplica la próxima vez que abras el juego." +
            (customName != null ? " Personalizado: " + customName + "." : " Uno propio (.zip de Turnip / Mesa): mantené apretado el ícono > Driver de video."));
        final int[] drv = { drvSel };
        addSpaced(s4, chips(driverNames, null, drvSel, i -> drv[0] = i));
        Boolean consent = LogUpload.consent(a);
        final Toggle logSw = toggle(right, "Enviar registros de errores", "Cuando el juego falla, para ayudar a arreglarlo", consent == null || consent);

        LinearLayout.LayoutParams sp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        sp.topMargin = dp(4);
        panel.addView(sv, sp);

        TextView ok = text("Listo", 17, Color.WHITE, true);
        ok.setGravity(Gravity.CENTER);
        ok.setPadding(0, dp(wide ? 12 : 15), 0, dp(wide ? 12 : 15));
        ok.setBackground(new RippleDrawable(ColorStateList.valueOf(Color.argb(70, 255, 255, 255)), round(ACCENT, 14, 0), round(Color.WHITE, 14, 0)));
        LinearLayout.LayoutParams op = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        op.topMargin = dp(12);
        panel.addView(ok, op);
        ok.setOnClickListener(v -> dlg.dismiss());
        editBtn.setOnClickListener(v -> { editPad[0] = true; dlg.dismiss(); });

        dlg.setContentView(page);
        dlg.setOnDismissListener(x -> {
            r[3] = padSw.isChecked() ? 1 : 0;
            r[5] = liteSw.isChecked() ? 1 : 0;
            GpuSettings.set(a, drivers[drv[0]]);
            LogUpload.setConsent(a, logSw.isChecked());
            VirtualPad.save(a, pad[0], pad[1], pad[2]);
            // after the native side has applied the result (it may show / hide the controller)
            if (editPad[0]) new Handler(Looper.getMainLooper()).postDelayed(() -> VirtualPad.edit(a), 300);
            done.countDown();
        });
        Window w = dlg.getWindow();
        if (w != null) {
            w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
            w.setStatusBarColor(BG);
            w.setNavigationBarColor(BG);
            // keep the game's full screen (no system bars popping up behind the page)
            w.setFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE, WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        }
        dlg.show();
        if (w != null) {
            w.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
            w.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
        }
    }
}
