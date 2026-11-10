package com.bo3recomp;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;

import org.libsdl.app.SDL;

// On-screen Xbox-style controller (touch), drawn semi-transparent over the game. Its state goes to
// the native input layer (host_input.cpp: nativeUpdate), merged with a physical gamepad. Shown or
// hidden from the in-game options (video_options.cpp -> show). Touches that miss every
// control fall through to the game surface (the OPCIONES button).
public class VirtualPad extends View {
    // XInput button bits (input.h)
    static final int UP = 0x1, DOWN = 0x2, LEFT = 0x4, RIGHT = 0x8, START = 0x10, BACK = 0x20,
                     LTHUMB = 0x40, RTHUMB = 0x80, LB = 0x100, RB = 0x200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;

    static native void nativeUpdate(int buttons, float lx, float ly, float rx, float ry, float lt, float rt);

    private static VirtualPad sView;

    // Shows / hides the controller (any thread).
    public static void show(final boolean on) {
        final Context ctx = SDL.getContext();
        if (!(ctx instanceof Activity)) return;
        final Activity act = (Activity) ctx;
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
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
        });
    }

    // ---- controls
    static final int K_BUTTON = 0, K_STICK = 1, K_TRIGGER = 2;
    static class Control {
        int kind, bit;                // bit: button bit, or 0 = left stick / 1 = right stick, or 0 = LT / 1 = RT
        float cx, cy, r;              // center, radius (buttons, sticks); rect for pills
        RectF rect;                   // pill-shaped buttons (shoulders, view / menu, d-pad)
        String label;
        int color;
        int pointer = -1;             // pointer holding it
        float dx, dy;                 // stick deflection -1..1
        boolean hit(float x, float y) {
            if (rect != null) {
                float pad = r * 0.35f;
                return x >= rect.left - pad && x <= rect.right + pad && y >= rect.top - pad && y <= rect.bottom + pad;
            }
            float m = kind == K_STICK ? 1.6f : 1.25f;
            float ddx = x - cx, ddy = y - cy;
            return ddx * ddx + ddy * ddy <= (r * m) * (r * m);
        }
    }
    private Control[] controls = new Control[0];
    private int laidW = -1, laidH = -1;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);

    VirtualPad(Context c) {
        super(c);
        line.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    private Control button(int bit, String label, int color, float cx, float cy, float r) {
        Control c = new Control();
        c.kind = K_BUTTON; c.bit = bit; c.label = label; c.color = color; c.cx = cx; c.cy = cy; c.r = r;
        return c;
    }
    private Control pill(int kind, int bit, String label, float cx, float cy, float w, float h) {
        Control c = new Control();
        c.kind = kind; c.bit = bit; c.label = label; c.color = Color.WHITE; c.cx = cx; c.cy = cy; c.r = h / 2;
        c.rect = new RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        return c;
    }
    private Control stick(int which, float cx, float cy, float r) {
        Control c = new Control();
        c.kind = K_STICK; c.bit = which; c.cx = cx; c.cy = cy; c.r = r; c.color = Color.WHITE;
        return c;
    }

    private void layout(int w, int h) {
        laidW = w; laidH = h;
        float u = h / 10f;
        int green = Color.rgb(90, 200, 70), red = Color.rgb(230, 60, 60), blue = Color.rgb(60, 130, 240), yellow = Color.rgb(240, 200, 40);
        float ax = w - u * 2.3f, ay = u * 5.6f, off = u * 1.15f, br = u * 0.62f;
        float dpx = u * 5.7f, dpy = u * 6.0f, ds = u * 0.95f;
        controls = new Control[] {
            stick(0, u * 2.6f, h - u * 2.5f, u * 1.55f),
            stick(1, w - u * 5.6f, h - u * 2.0f, u * 1.35f),
            button(A, "A", green, ax, ay + off, br),
            button(B, "B", red, ax + off, ay, br),
            button(X, "X", blue, ax - off, ay, br),
            button(Y, "Y", yellow, ax, ay - off, br),
            pill(K_BUTTON, UP, "", dpx, dpy - ds, ds, ds),
            pill(K_BUTTON, DOWN, "", dpx, dpy + ds, ds, ds),
            pill(K_BUTTON, LEFT, "", dpx - ds, dpy, ds, ds),
            pill(K_BUTTON, RIGHT, "", dpx + ds, dpy, ds, ds),
            pill(K_TRIGGER, 0, "LT", u * 1.7f, u * 1.9f, u * 2.4f, u * 0.9f),
            pill(K_BUTTON, LB, "LB", u * 1.7f, u * 3.1f, u * 2.4f, u * 0.9f),
            pill(K_TRIGGER, 1, "RT", w - u * 1.7f, u * 1.9f, u * 2.4f, u * 0.9f),
            pill(K_BUTTON, RB, "RB", w - u * 1.7f, u * 3.1f, u * 2.4f, u * 0.9f),
            pill(K_BUTTON, BACK, "VIEW", w / 2f - u * 1.4f, u * 0.9f, u * 1.8f, u * 0.8f),
            pill(K_BUTTON, START, "MENU", w / 2f + u * 1.4f, u * 0.9f, u * 1.8f, u * 0.8f),
            button(LTHUMB, "L3", Color.WHITE, u * 5.0f, h - u * 0.95f, u * 0.48f),
            button(RTHUMB, "R3", Color.WHITE, w - u * 8.0f, h - u * 0.95f, u * 0.48f),
        };
    }

    @Override
    protected void onDraw(Canvas cv) {
        if (getWidth() != laidW || getHeight() != laidH) layout(getWidth(), getHeight());
        float u = getHeight() / 10f;
        line.setStrokeWidth(Math.max(2f, u * 0.05f));
        for (Control c : controls) {
            boolean on = c.pointer >= 0;
            if (c.kind == K_STICK) {
                fill.setColor(Color.argb(on ? 70 : 45, 255, 255, 255));
                cv.drawCircle(c.cx, c.cy, c.r, fill);
                line.setColor(Color.argb(140, 255, 255, 255));
                cv.drawCircle(c.cx, c.cy, c.r, line);
                fill.setColor(Color.argb(on ? 170 : 110, 255, 255, 255));
                cv.drawCircle(c.cx + c.dx * c.r, c.cy + c.dy * c.r, c.r * 0.45f, fill);
                continue;
            }
            int col = c.color;
            fill.setColor(Color.argb(on ? 190 : 90, Color.red(col), Color.green(col), Color.blue(col)));
            line.setColor(Color.argb(on ? 230 : 150, 255, 255, 255));
            if (c.rect != null) {
                cv.drawRoundRect(c.rect, c.r * 0.5f, c.r * 0.5f, fill);
                cv.drawRoundRect(c.rect, c.r * 0.5f, c.r * 0.5f, line);
            } else {
                cv.drawCircle(c.cx, c.cy, c.r, fill);
                cv.drawCircle(c.cx, c.cy, c.r, line);
            }
            if (c.label.length() > 0) {
                text.setTextSize(c.r * (c.label.length() > 2 ? 0.75f : 1.0f));
                text.setColor(Color.argb(on ? 255 : 200, 255, 255, 255));
                cv.drawText(c.label, c.cx, c.cy + text.getTextSize() * 0.36f, text);
            } else if (c.kind == K_BUTTON && (c.bit & 0xF) != 0) {
                // d-pad arrow
                float s = c.r * 0.45f;
                fill.setColor(Color.argb(on ? 255 : 190, 255, 255, 255));
                android.graphics.Path p = new android.graphics.Path();
                if (c.bit == UP) { p.moveTo(c.cx, c.cy - s); p.lineTo(c.cx - s, c.cy + s * 0.6f); p.lineTo(c.cx + s, c.cy + s * 0.6f); }
                else if (c.bit == DOWN) { p.moveTo(c.cx, c.cy + s); p.lineTo(c.cx - s, c.cy - s * 0.6f); p.lineTo(c.cx + s, c.cy - s * 0.6f); }
                else if (c.bit == LEFT) { p.moveTo(c.cx - s, c.cy); p.lineTo(c.cx + s * 0.6f, c.cy - s); p.lineTo(c.cx + s * 0.6f, c.cy + s); }
                else { p.moveTo(c.cx + s, c.cy); p.lineTo(c.cx - s * 0.6f, c.cy - s); p.lineTo(c.cx - s * 0.6f, c.cy + s); }
                p.close();
                cv.drawPath(p, fill);
            }
        }
    }

    private Control find(float x, float y) {
        Control best = null;
        float bestD = Float.MAX_VALUE;
        for (Control c : controls) {
            if (c.pointer >= 0 || !c.hit(x, y)) continue;
            float d = (x - c.cx) * (x - c.cx) + (y - c.cy) * (y - c.cy);
            if (d < bestD) { bestD = d; best = c; }
        }
        return best;
    }

    private void moveStick(Control c, float x, float y) {
        float dx = (x - c.cx) / c.r, dy = (y - c.cy) / c.r;
        float len = (float) Math.sqrt(dx * dx + dy * dy);
        if (len > 1f) { dx /= len; dy /= len; }
        c.dx = dx; c.dy = dy;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (getWidth() != laidW || getHeight() != laidH) layout(getWidth(), getHeight());
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        boolean any = false;
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                Control c = find(e.getX(idx), e.getY(idx));
                if (c != null) {
                    c.pointer = e.getPointerId(idx);
                    if (c.kind == K_STICK) moveStick(c, e.getX(idx), e.getY(idx));
                    any = true;
                } else if (action == MotionEvent.ACTION_DOWN) {
                    return false;   // not on a control: the game surface gets the touch
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    for (Control c : controls)
                        if (c.pointer == id && c.kind == K_STICK) moveStick(c, e.getX(i), e.getY(i));
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int id = action == MotionEvent.ACTION_CANCEL ? -2 : e.getPointerId(idx);
                for (Control c : controls)
                    if (c.pointer >= 0 && (id == -2 || c.pointer == id)) { c.pointer = -1; c.dx = c.dy = 0; }
                break;
            }
        }
        send();
        invalidate();
        return true;
    }

    private void send() {
        int buttons = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
        for (Control c : controls) {
            if (c.kind == K_STICK) {
                if (c.bit == 0) { lx = c.dx; ly = -c.dy; } else { rx = c.dx; ry = -c.dy; }
            } else if (c.pointer >= 0) {
                if (c.kind == K_TRIGGER) { if (c.bit == 0) lt = 1; else rt = 1; }
                else buttons |= c.bit;
            }
        }
        nativeUpdate(buttons, lx, ly, rx, ry, lt, rt);
    }
}
