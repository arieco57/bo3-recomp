package com.bo3recomp;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDL;

// On-screen controller (touch), drawn semi-transparent over the game. Its state goes to the native
// input layer (host_input.cpp: nativeUpdate / nativeLook), merged with a physical gamepad. Shown or
// hidden from the in-game options (video_options.cpp -> show). Touches that miss every control fall
// through to the game surface (the OPCIONES button).
//
// Layouts (OptionsPanel picks one; each can be rearranged in the editor and is saved per layout):
//  - Clásico: an Xbox controller - two sticks, A B X Y, shoulders, d-pad.
//  - Competitivo: a touch shooter - the left half moves (the stick appears under the finger, pushed
//    past its ring it sprints), the right half aims by swiping (the mouse's direct look path, not a
//    stick), fire buttons on both sides, an aim-and-fire button, toggle aim down sights; every button's
//    finger aims when it slides.
//  - Personalizado: starts as Competitivo, meant to be rearranged.
public class VirtualPad extends View {
    // XInput button bits (input.h)
    static final int UP = 0x1, DOWN = 0x2, LEFT = 0x4, RIGHT = 0x8, START = 0x10, BACK = 0x20,
                     LTHUMB = 0x40, RTHUMB = 0x80, LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;

    static native void nativeUpdate(int buttons, float lx, float ly, float rx, float ry, float lt, float rt);
    static native void nativeLook(float yawDeg, float pitchDeg);

    // ---- settings (SharedPreferences "pad")
    static final String[] LAYOUTS = { "Clásico", "Competitivo", "Personalizado" };
    static final float[] SENS = { 0.6f, 1f, 1.5f, 2.2f };
    static final String[] SENS_NAMES = { "Baja", "Media", "Alta", "Muy alta" };
    static final float[] OPACITY = { 0.35f, 0.6f, 0.85f, 1f };
    static final String[] OPACITY_NAMES = { "35%", "60%", "85%", "100%" };

    static SharedPreferences prefs(Context c) { return c.getSharedPreferences("pad", Context.MODE_PRIVATE); }
    static int layoutChoice(Context c) { return Math.max(0, Math.min(LAYOUTS.length - 1, prefs(c).getInt("layout", 1))); }
    static int sensChoice(Context c) { return Math.max(0, Math.min(SENS.length - 1, prefs(c).getInt("sens", 1))); }
    static int opacityChoice(Context c) { return Math.max(0, Math.min(OPACITY.length - 1, prefs(c).getInt("opacity", 1))); }
    static void save(Context c, int layout, int sens, int opacity) {
        prefs(c).edit().putInt("layout", layout).putInt("sens", sens).putInt("opacity", opacity).apply();
        new Handler(Looper.getMainLooper()).post(() -> { if (sView != null) sView.reload(); });
    }

    private static VirtualPad sView;
    private static boolean sWanted;   // what the options asked for (the editor shows it regardless)

    // Shows / hides the controller (any thread).
    public static void show(final boolean on) {
        final Context ctx = SDL.getContext();
        if (!(ctx instanceof Activity)) return;
        final Activity act = (Activity) ctx;
        new Handler(Looper.getMainLooper()).post(() -> {
            sWanted = on;
            if (sView != null && sView.editing) return;   // applied when the editor closes
            apply(act, on);
        });
    }

    private static void apply(Activity act, boolean on) {
        if (on && sView == null) {
            sView = new VirtualPad(act);
            act.addContentView(sView, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        } else if (!on && sView != null) {
            ViewGroup parent = (ViewGroup) sView.getParent();
            if (parent != null) parent.removeView(sView);
            sView = null;
            nativeUpdate(0, 0, 0, 0, 0, 0, 0);
        }
    }

    // Opens the layout editor (main thread).
    static void edit(Activity act) {
        apply(act, true);
        sView.editing = true;
        sView.selected = null;
        sView.release();
        sView.invalidate();
    }

    // ---- controls
    static final long MIN_PRESS_MS = 80;
    // K_FIRE: right trigger; K_ADSFIRE: both triggers (aim down sights and fire with one finger)
    static final int K_BUTTON = 0, K_STICK = 1, K_TRIGGER = 2, K_FIRE = 3, K_FLOAT = 4, K_LOOK = 5, K_ADSFIRE = 6;
    static class Control {
        String id;
        int kind, bit;            // bit: buttons, or 0 = left / 1 = right stick, or 0 = LT / 1 = RT
        float cx, cy, r;          // center, radius (pills: half height)
        float pw, ph;             // pill size (0 = circle)
        float scale = 1f;
        float baseR, basePw, basePh;
        String label = "";
        int icon;                 // drawable id, 0 = none
        int color = Color.WHITE;
        boolean toggle, latched;  // toggle: a tap turns it on, the next one off
        boolean editable = true;
        RectF zone;               // K_FLOAT / K_LOOK: the area they take touches in
        int pointer = -1;
        float dx, dy;             // stick deflection -1..1
        float ox, oy;             // floating stick: where the finger went down
        float lastX, lastY;       // look: last finger position
        boolean dragging;         // a button's finger moved far enough: it aims too
        boolean sprint;           // floating stick pushed past its ring

        boolean pill() { return pw > 0; }
        RectF rect() { return new RectF(cx - pw / 2, cy - ph / 2, cx + pw / 2, cy + ph / 2); }
        long downAt, holdUntil;   // a quick tap stays pressed until holdUntil (uptime ms)
        boolean pressed() { return toggle ? latched : pointer >= 0 || android.os.SystemClock.uptimeMillis() < holdUntil; }
        boolean hit(float x, float y) {
            if (zone != null) return zone.contains(x, y);
            if (pill()) {
                float pad = r * 0.35f;
                return x >= cx - pw / 2 - pad && x <= cx + pw / 2 + pad && y >= cy - ph / 2 - pad && y <= cy + ph / 2 + pad;
            }
            float m = kind == K_STICK ? 1.6f : 1.2f;
            float ddx = x - cx, ddy = y - cy;
            return ddx * ddx + ddy * ddy <= (r * m) * (r * m);
        }
        void applyScale() { r = baseR * scale; pw = basePw * scale; ph = basePh * scale; if (pill()) r = ph / 2; }
    }

    private final List<Control> controls = new ArrayList<>();
    private int laidW = -1, laidH = -1, layout;
    private float sens, alpha;
    private final float density;
    private boolean editing;
    private Control selected;
    private float grabX, grabY;
    private final RectF[] tools = new RectF[4];   // editor: smaller, bigger, reset, done
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
                        text = new Paint(Paint.ANTI_ALIAS_FLAG), dash = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final android.util.SparseArray<Drawable> icons = new android.util.SparseArray<>();

    VirtualPad(Context c) {
        super(c);
        density = c.getResources().getDisplayMetrics().density;
        line.setStyle(Paint.Style.STROKE);
        dash.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        reload();
    }

    void reload() {
        Context c = getContext();
        layout = layoutChoice(c);
        sens = SENS[sensChoice(c)];
        alpha = OPACITY[opacityChoice(c)];
        laidW = laidH = -1;
        release();
        invalidate();
    }

    // ---- layouts (u = a tenth of the screen height)
    private Control add(String id, int kind, int bit, float cx, float cy, float r) {
        Control c = new Control();
        c.id = id; c.kind = kind; c.bit = bit; c.cx = cx; c.cy = cy; c.baseR = c.r = r;
        controls.add(c);
        return c;
    }
    private Control button(String id, int bit, int icon, String label, int color, float cx, float cy, float r) {
        Control c = add(id, K_BUTTON, bit, cx, cy, r);
        c.icon = icon; c.label = label; c.color = color;
        return c;
    }
    private Control pill(String id, int kind, int bit, String label, int icon, float cx, float cy, float w, float h) {
        Control c = add(id, kind, bit, cx, cy, h / 2);
        c.label = label; c.icon = icon;
        c.basePw = c.pw = w; c.basePh = c.ph = h;
        return c;
    }
    private void dpad(float x, float y, float s) {
        pill("up", K_BUTTON, UP, "", 0, x, y - s, s, s);
        pill("down", K_BUTTON, DOWN, "", 0, x, y + s, s, s);
        pill("left", K_BUTTON, LEFT, "", 0, x - s, y, s, s);
        pill("right", K_BUTTON, RIGHT, "", 0, x + s, y, s, s);
    }

    static final int GREEN = Color.rgb(90, 200, 70), RED = Color.rgb(230, 60, 60), BLUE = Color.rgb(60, 130, 240),
                     YELLOW = Color.rgb(240, 200, 40), ORANGE = Color.rgb(242, 107, 29), DARK = Color.rgb(40, 44, 52);

    private void layoutClassic(int w, int h) {
        float u = h / 10f;
        float ax = w - u * 2.3f, ay = u * 5.6f, off = u * 1.15f, br = u * 0.62f;
        add("ls", K_STICK, 0, u * 2.6f, h - u * 2.5f, u * 1.55f);
        add("rs", K_STICK, 1, w - u * 5.6f, h - u * 2.0f, u * 1.35f);
        button("a", A, 0, "A", GREEN, ax, ay + off, br);
        button("b", B, 0, "B", RED, ax + off, ay, br);
        button("x", X, 0, "X", BLUE, ax - off, ay, br);
        button("y", Y, 0, "Y", YELLOW, ax, ay - off, br);
        dpad(u * 5.7f, u * 6.0f, u * 0.95f);
        pill("lt", K_TRIGGER, 0, "LT", 0, u * 1.7f, u * 1.9f, u * 2.4f, u * 0.9f);
        pill("lb", K_BUTTON, LB, "LB", 0, u * 1.7f, u * 3.1f, u * 2.4f, u * 0.9f);
        pill("rt", K_TRIGGER, 1, "RT", 0, w - u * 1.7f, u * 1.9f, u * 2.4f, u * 0.9f);
        pill("rb", K_BUTTON, RB, "RB", 0, w - u * 1.7f, u * 3.1f, u * 2.4f, u * 0.9f);
        pill("back", K_BUTTON, BACK, "", R.drawable.pad_score, w / 2f - u * 1.4f, u * 0.9f, u * 1.8f, u * 0.8f);
        pill("start", K_BUTTON, START, "", R.drawable.pad_menu, w / 2f + u * 1.4f, u * 0.9f, u * 1.8f, u * 0.8f);
        button("l3", LTHUMB, 0, "L3", Color.WHITE, u * 5.0f, h - u * 0.95f, u * 0.48f);
        button("r3", RTHUMB, 0, "R3", Color.WHITE, w - u * 8.0f, h - u * 0.95f, u * 0.48f);
    }

    private void layoutCompetitive(int w, int h) {
        float u = h / 10f, W = w;
        float split = w * 0.42f, top = h * 0.15f;   // (the OPCIONES button sits above `top`, top right)
        // the zones go first: buttons on top of them win (find() prefers non-zone controls)
        Control mv = add("move", K_FLOAT, 0, u * 3.0f, h - u * 2.8f, u * 1.5f);
        mv.zone = new RectF(0, top, split, h);
        Control look = add("look", K_LOOK, 0, 0, 0, 0);
        look.zone = new RectF(split, top, w, h);
        look.editable = false;
        // right hand
        Control fire = button("fire", 1, R.drawable.pad_fire, "", RED, W - u * 5.0f, h - u * 4.0f, u * 1.05f);
        fire.kind = K_FIRE;
        Control adsFire = button("adsfire", 0, R.drawable.pad_adsfire, "", RED, W - u * 7.4f, h - u * 3.8f, u * 0.8f);
        adsFire.kind = K_ADSFIRE;
        Control ads = button("ads", 0, R.drawable.pad_ads, "", DARK, W - u * 2.0f, h - u * 5.6f, u * 0.8f);
        ads.kind = K_TRIGGER; ads.toggle = true;
        button("jump", A, R.drawable.pad_jump, "", DARK, W - u * 1.7f, h - u * 3.3f, u * 0.85f);
        button("crouch", B, R.drawable.pad_crouch, "", DARK, W - u * 1.7f, h - u * 1.3f, u * 0.75f);
        button("reload", X, R.drawable.pad_reload, "", DARK, W - u * 3.5f, h - u * 1.5f, u * 0.7f);
        button("swap", Y, R.drawable.pad_swap, "", DARK, W - u * 6.6f, h - u * 1.3f, u * 0.65f);
        button("melee", RTHUMB, R.drawable.pad_melee, "", DARK, W - u * 3.5f, h - u * 6.6f, u * 0.6f);
        button("lethal", RB, R.drawable.pad_lethal, "", DARK, W - u * 1.6f, h - u * 7.4f, u * 0.6f);
        button("tactical", LB, R.drawable.pad_tactical, "", DARK, W - u * 5.2f, h - u * 7.0f, u * 0.6f);
        button("special", LB | RB, R.drawable.pad_special, "", ORANGE, W - u * 6.8f, h - u * 6.4f, u * 0.6f);
        // left hand
        Control lfire = button("lfire", 1, R.drawable.pad_fire, "", RED, u * 2.0f, u * 4.0f, u * 0.75f);
        lfire.kind = K_TRIGGER;
        button("sprint", LTHUMB, R.drawable.pad_sprint, "", DARK, u * 4.3f, u * 3.2f, u * 0.6f);
        dpad(u * 8.0f, u * 3.4f, u * 0.75f);
        pill("back", K_BUTTON, BACK, "", R.drawable.pad_score, w / 2f - u * 1.2f, u * 0.8f, u * 1.6f, u * 0.75f);
        pill("start", K_BUTTON, START, "", R.drawable.pad_menu, w / 2f + u * 1.2f, u * 0.8f, u * 1.6f, u * 0.75f);
    }

    private void layout(int w, int h) {
        laidW = w; laidH = h;
        controls.clear();
        if (layout == 0) layoutClassic(w, h); else layoutCompetitive(w, h);
        // the user's arrangement: "id:x,y,scale;..." with x, y as fractions of the screen
        String saved = prefs(getContext()).getString("layout" + layout, "");
        for (String item : saved.split(";")) {
            int colon = item.indexOf(':');
            if (colon <= 0) continue;
            String[] v = item.substring(colon + 1).split(",");
            Control c = byId(item.substring(0, colon));
            if (c == null || v.length != 3) continue;
            try {
                c.cx = Float.parseFloat(v[0]) * w;
                c.cy = Float.parseFloat(v[1]) * h;
                c.scale = Math.max(0.5f, Math.min(2.2f, Float.parseFloat(v[2])));
                c.applyScale();
            } catch (NumberFormatException ignored) { }
        }
    }

    private Control byId(String id) {
        for (Control c : controls) if (c.id.equals(id)) return c;
        return null;
    }

    private void saveArrangement() {
        StringBuilder sb = new StringBuilder();
        for (Control c : controls) {
            if (!c.editable) continue;
            sb.append(c.id).append(':').append(c.cx / laidW).append(',').append(c.cy / laidH).append(',').append(c.scale).append(';');
        }
        prefs(getContext()).edit().putString("layout" + layout, sb.toString()).apply();
    }

    // ---- drawing
    private int a(int v) { return Math.round(v * alpha); }

    private Drawable icon(int id) {
        Drawable d = icons.get(id);
        if (d == null) {
            d = getContext().getDrawable(id).mutate();
            d.setTint(Color.WHITE);
            icons.put(id, d);
        }
        return d;
    }

    @Override
    protected void onDraw(Canvas cv) {
        if (getWidth() != laidW || getHeight() != laidH) layout(getWidth(), getHeight());
        float u = getHeight() / 10f;
        line.setStrokeWidth(Math.max(2f, u * 0.05f));
        if (editing) cv.drawColor(Color.argb(110, 0, 0, 0));
        for (Control c : controls) {
            if (c.kind == K_LOOK) {
                if (editing) drawZone(cv, c.zone, "Deslizá para apuntar", u);
                continue;
            }
            if (c.kind == K_FLOAT) { drawFloating(cv, c, u); continue; }
            boolean on = c.pressed() && !editing;
            if (c.kind == K_STICK) {
                fill.setColor(Color.argb(a(on ? 70 : 45), 255, 255, 255));
                cv.drawCircle(c.cx, c.cy, c.r, fill);
                line.setColor(Color.argb(a(140), 255, 255, 255));
                cv.drawCircle(c.cx, c.cy, c.r, line);
                fill.setColor(Color.argb(a(on ? 170 : 110), 255, 255, 255));
                cv.drawCircle(c.cx + c.dx * c.r, c.cy + c.dy * c.r, c.r * 0.45f, fill);
            } else {
                int col = c.color;
                fill.setColor(Color.argb(a(on ? 200 : (col == DARK ? 120 : 95)), Color.red(col), Color.green(col), Color.blue(col)));
                line.setColor(Color.argb(a(on ? 240 : 150), 255, 255, 255));
                if (c.pill()) {
                    RectF rr = c.rect();
                    cv.drawRoundRect(rr, c.r * 0.5f, c.r * 0.5f, fill);
                    cv.drawRoundRect(rr, c.r * 0.5f, c.r * 0.5f, line);
                } else {
                    cv.drawCircle(c.cx, c.cy, c.r, fill);
                    cv.drawCircle(c.cx, c.cy, c.r, line);
                }
                if (c.icon != 0) {
                    Drawable d = icon(c.icon);
                    float s = (c.pill() ? Math.min(c.pw, c.ph) : c.r * 2) * 0.3f;
                    d.setBounds(Math.round(c.cx - s), Math.round(c.cy - s), Math.round(c.cx + s), Math.round(c.cy + s));
                    d.setAlpha(a(on ? 255 : 215));
                    d.draw(cv);
                } else if (c.label.length() > 0) {
                    text.setTextSize(c.r * (c.label.length() > 2 ? 0.75f : 1.0f));
                    text.setColor(Color.argb(a(on ? 255 : 200), 255, 255, 255));
                    cv.drawText(c.label, c.cx, c.cy + text.getTextSize() * 0.36f, text);
                } else if ((c.bit & 0xF) != 0) {
                    drawArrow(cv, c, on);
                }
            }
            if (editing && c == selected) {
                line.setColor(ORANGE);
                float g = u * 0.15f;
                if (c.pill()) { RectF rr = c.rect(); rr.inset(-g, -g); cv.drawRoundRect(rr, c.r, c.r, line); }
                else cv.drawCircle(c.cx, c.cy, c.r + g, line);
            }
        }
        if (editing) drawTools(cv, u);
    }

    private void drawFloating(Canvas cv, Control c, float u) {
        boolean on = c.pointer >= 0 && !editing;
        float bx = on ? c.ox : c.cx, by = on ? c.oy : c.cy;
        if (editing) drawZone(cv, c.zone, "Mover (el stick aparece donde tocás)", u);
        fill.setColor(Color.argb(a(on ? 60 : 30), 255, 255, 255));
        cv.drawCircle(bx, by, c.r, fill);
        line.setColor(Color.argb(a(on ? 160 : 90), 255, 255, 255));
        cv.drawCircle(bx, by, c.r, line);
        fill.setColor(Color.argb(a(on ? 170 : 80), 255, 255, 255));
        cv.drawCircle(bx + c.dx * c.r, by + c.dy * c.r, c.r * 0.42f, fill);
        // sprint mark above the ring: lit when pushed past it
        Drawable d = icon(R.drawable.pad_sprint);
        float s = c.r * 0.22f, sy = by - c.r * 1.45f;
        d.setBounds(Math.round(bx - s), Math.round(sy - s), Math.round(bx + s), Math.round(sy + s));
        d.setAlpha(a(c.sprint ? 255 : 70));
        cv.save();
        cv.rotate(-90, bx, sy);
        d.draw(cv);
        cv.restore();
        if (editing && c == selected) {
            line.setColor(ORANGE);
            cv.drawCircle(c.cx, c.cy, c.r + u * 0.15f, line);
        }
    }

    private void drawZone(Canvas cv, RectF z, String label, float u) {
        dash.setStrokeWidth(Math.max(2f, u * 0.03f));
        dash.setColor(Color.argb(120, 255, 255, 255));
        dash.setPathEffect(new DashPathEffect(new float[] { u * 0.2f, u * 0.15f }, 0));
        RectF r = new RectF(z);
        r.inset(u * 0.1f, u * 0.1f);
        cv.drawRoundRect(r, u * 0.3f, u * 0.3f, dash);
        text.setTextSize(u * 0.32f);
        text.setColor(Color.argb(150, 255, 255, 255));
        cv.drawText(label, r.centerX(), r.top + u * 0.55f, text);
    }

    private void drawArrow(Canvas cv, Control c, boolean on) {
        float s = c.r * 0.45f;
        fill.setColor(Color.argb(a(on ? 255 : 190), 255, 255, 255));
        Path p = new Path();
        if (c.bit == UP) { p.moveTo(c.cx, c.cy - s); p.lineTo(c.cx - s, c.cy + s * 0.6f); p.lineTo(c.cx + s, c.cy + s * 0.6f); }
        else if (c.bit == DOWN) { p.moveTo(c.cx, c.cy + s); p.lineTo(c.cx - s, c.cy - s * 0.6f); p.lineTo(c.cx + s, c.cy - s * 0.6f); }
        else if (c.bit == LEFT) { p.moveTo(c.cx - s, c.cy); p.lineTo(c.cx + s * 0.6f, c.cy - s); p.lineTo(c.cx + s * 0.6f, c.cy + s); }
        else { p.moveTo(c.cx + s, c.cy); p.lineTo(c.cx - s * 0.6f, c.cy - s); p.lineTo(c.cx - s * 0.6f, c.cy + s); }
        p.close();
        cv.drawPath(p, fill);
    }

    private void drawTools(Canvas cv, float u) {
        String[] labels = { "Tamaño −", "Tamaño +", "Restablecer", "Listo" };
        float bw = u * 2.1f, bh = u * 0.85f, gap = u * 0.25f;
        float x = getWidth() / 2f - (bw * 4 + gap * 3) / 2, y = getHeight() * 0.5f - bh / 2;
        text.setTextSize(u * 0.34f);
        for (int i = 0; i < 4; i++) {
            tools[i] = new RectF(x, y, x + bw, y + bh);
            fill.setColor(i == 3 ? ORANGE : Color.argb(230, 36, 40, 47));
            cv.drawRoundRect(tools[i], bh / 2, bh / 2, fill);
            boolean dim = (i < 2) && selected == null;
            text.setColor(dim ? Color.argb(110, 255, 255, 255) : Color.WHITE);
            cv.drawText(labels[i], tools[i].centerX(), tools[i].centerY() + text.getTextSize() * 0.36f, text);
            x += bw + gap;
        }
        text.setTextSize(u * 0.3f);
        text.setColor(Color.argb(200, 255, 255, 255));
        String hint = selected == null ? "Tocá un control y arrastralo. Diseño: " + LAYOUTS[layout] : "Arrastrá para mover · Tamaño −/+ para cambiarlo";
        cv.drawText(hint, getWidth() / 2f, y + bh + u * 0.6f, text);
    }

    // ---- touch
    private Control find(float x, float y) {
        Control best = null;
        float bestD = Float.MAX_VALUE;
        for (Control c : controls) {
            if (c.zone != null || c.pointer >= 0 || !c.hit(x, y)) continue;
            float d = (x - c.cx) * (x - c.cx) + (y - c.cy) * (y - c.cy);
            if (d < bestD) { bestD = d; best = c; }
        }
        if (best != null) return best;
        for (Control c : controls) if (c.zone != null && c.pointer < 0 && c.hit(x, y)) return c;
        return null;
    }

    private void moveStick(Control c, float x, float y) {
        float bx = c.kind == K_FLOAT ? c.ox : c.cx, by = c.kind == K_FLOAT ? c.oy : c.cy;
        float dx = (x - bx) / c.r, dy = (y - by) / c.r;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (c.kind == K_FLOAT) {
            // pushed well past the ring, forward: sprint (L3 held)
            c.sprint = len > 1.45f && dy < -0.8f * len;
            // the base follows a finger that goes far, so turning back is quick
            if (len > 2.2f) {
                float k = (len - 2.2f) / len;
                c.ox += (x - bx) * k; c.oy += (y - by) * k;
            }
        }
        if (len > 1f) { dx /= len; dy /= len; }
        c.dx = dx; c.dy = dy;
    }

    // Any button's finger aims when it slides (like the fire button in touch shooters): firing,
    // jumping or reloading without lifting the aiming finger. Buttons start after a few dp so a tap
    // does not nudge the view; the look area and the fire buttons right away.
    private void look(Control c, float x, float y) {
        if (!c.dragging) {
            boolean instant = c.kind == K_LOOK || c.kind == K_FIRE || c.kind == K_ADSFIRE;
            float mx = x - c.lastX, my = y - c.lastY;
            if (!instant && mx * mx + my * my < (8 * density) * (8 * density)) return;
            c.dragging = true;
            if (!instant) { c.lastX = x; c.lastY = y; return; }
        }
        float ddx = x - c.lastX, ddy = y - c.lastY;
        c.lastX = x; c.lastY = y;
        if (ddx == 0 && ddy == 0) return;
        // degrees per dp; aiming down sights slows it
        float k = 0.2f * sens / density;
        if (adsOn()) k *= 0.6f;
        nativeLook(ddx * k, ddy * k);
    }

    private boolean adsOn() {
        for (Control c : controls) if (((c.kind == K_TRIGGER && c.bit == 0) || c.kind == K_ADSFIRE) && c.pressed()) return true;
        return false;
    }

    private void release() {
        for (Control c : controls) { c.pointer = -1; c.dx = c.dy = 0; c.sprint = false; c.latched = false; c.holdUntil = 0; }
        nativeUpdate(0, 0, 0, 0, 0, 0, 0);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (getWidth() != laidW || getHeight() != laidH) layout(getWidth(), getHeight());
        if (editing) return editTouch(e);
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(idx), y = e.getY(idx);
                Control c = find(x, y);
                if (c == null) {
                    if (action == MotionEvent.ACTION_DOWN) return false;   // the game surface gets the touch
                    break;
                }
                c.pointer = e.getPointerId(idx);
                c.downAt = android.os.SystemClock.uptimeMillis();
                c.holdUntil = 0;
                c.lastX = x; c.lastY = y;
                c.dragging = false;
                if (c.toggle) c.latched = !c.latched;
                if (c.kind == K_FLOAT) { c.ox = x; c.oy = y; moveStick(c, x, y); }
                else if (c.kind == K_STICK) moveStick(c, x, y);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    for (Control c : controls) {
                        if (c.pointer != id) continue;
                        if (c.kind == K_STICK || c.kind == K_FLOAT) moveStick(c, e.getX(i), e.getY(i));
                        else look(c, e.getX(i), e.getY(i));   // look area, and any button whose finger slides
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int id = action == MotionEvent.ACTION_CANCEL ? -2 : e.getPointerId(idx);
                for (Control c : controls)
                    if (c.pointer >= 0 && (id == -2 || c.pointer == id)) {
                        c.pointer = -1; c.dx = c.dy = 0; c.sprint = false;
                        // the game reads the pad once a frame (33 ms at 30 fps): a quicker tap was
                        // missed - keep it pressed for two frames
                        long until = c.downAt + MIN_PRESS_MS;
                        if (id != -2 && until > android.os.SystemClock.uptimeMillis()) {
                            c.holdUntil = until;
                            ui.postDelayed(() -> { send(); invalidate(); }, until - android.os.SystemClock.uptimeMillis() + 1);
                        }
                    }
                break;
            }
        }
        send();
        invalidate();
        return true;
    }

    private boolean editTouch(MotionEvent e) {
        int action = e.getActionMasked();
        float x = e.getX(e.getActionIndex()), y = e.getY(e.getActionIndex());
        if (action == MotionEvent.ACTION_DOWN) {
            for (int i = 0; i < tools.length; i++) {
                if (tools[i] == null || !tools[i].contains(x, y)) continue;
                tool(i);
                return true;
            }
            selected = null;
            for (Control c : controls) {
                if (!c.editable) continue;
                boolean on = c.kind == K_FLOAT ? Math.hypot(x - c.cx, y - c.cy) <= c.r * 1.2f : c.hit(x, y);
                if (on && (selected == null || Math.hypot(x - c.cx, y - c.cy) < Math.hypot(x - selected.cx, y - selected.cy))) selected = c;
            }
            if (selected != null) { grabX = x - selected.cx; grabY = y - selected.cy; }
        } else if (action == MotionEvent.ACTION_MOVE && selected != null && e.getPointerCount() == 1) {
            selected.cx = Math.max(0, Math.min(getWidth(), x - grabX));
            selected.cy = Math.max(0, Math.min(getHeight(), y - grabY));
        }
        invalidate();
        return true;
    }

    private void tool(int i) {
        if (i < 2) {
            if (selected == null) return;
            selected.scale = Math.max(0.5f, Math.min(2.2f, selected.scale * (i == 0 ? 0.9f : 1.1f)));
            selected.applyScale();
        } else if (i == 2) {
            prefs(getContext()).edit().remove("layout" + layout).apply();
            selected = null;
            layout(getWidth(), getHeight());
        } else {
            saveArrangement();
            editing = false;
            selected = null;
            if (!sWanted) ui.post(() -> apply((Activity) getContext(), false));
        }
        invalidate();
    }

    private void send() {
        int buttons = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        for (Control c : controls) {
            if (c.kind == K_STICK || c.kind == K_FLOAT) {
                if (c.bit == 0) { lx = c.dx; ly = -c.dy; } else { rx = c.dx; ry = -c.dy; }
                if (c.sprint) buttons |= LTHUMB;
            } else if (c.kind == K_LOOK) {
                // (aim goes through nativeLook)
            } else if (c.pressed()) {
                if (c.kind == K_ADSFIRE) { lt = 1; rt = 1; }
                else if (c.kind == K_TRIGGER || c.kind == K_FIRE) { if (c.bit == 0) lt = 1; else rt = 1; }
                else buttons |= c.bit;
            }
        }
        nativeUpdate(buttons, lx, ly, rx, ry, lt, rt);
    }
}
