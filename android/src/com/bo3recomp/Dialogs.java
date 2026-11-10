package com.bo3recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.concurrent.CountDownLatch;

import org.libsdl.app.SDL;

// Text-input form for the native account flow (runtime/src/ui.cpp): shown on the UI thread, the
// calling (native) thread waits for it. Returns the values, or null when cancelled.
public class Dialogs {
    public static String[] form(final String title, final String text, final String[] labels, final boolean[] secret) {
        final Context ctx = SDL.getContext();
        if (!(ctx instanceof Activity)) return null;
        final Activity act = (Activity) ctx;
        final String[][] result = { null };
        final CountDownLatch done = new CountDownLatch(1);
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                LinearLayout box = new LinearLayout(act);
                box.setOrientation(LinearLayout.VERTICAL);
                int pad = (int) (16 * act.getResources().getDisplayMetrics().density);
                box.setPadding(pad, pad / 2, pad, 0);
                TextView msg = new TextView(act);
                msg.setText(text);
                box.addView(msg);
                final EditText[] edits = new EditText[labels.length];
                for (int i = 0; i < labels.length; i++) {
                    EditText e = new EditText(act);
                    e.setHint(labels[i]);
                    e.setSingleLine(true);
                    e.setInputType(secret[i] ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD
                                             : InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
                    edits[i] = e;
                    box.addView(e);
                }
                ScrollView scroll = new ScrollView(act);
                scroll.addView(box);
                AlertDialog d = new AlertDialog.Builder(act)
                    .setTitle(title)
                    .setView(scroll)
                    .setPositiveButton("Aceptar", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface di, int w) {
                            String[] v = new String[edits.length];
                            for (int i = 0; i < edits.length; i++) v[i] = edits[i].getText().toString();
                            result[0] = v;
                        }
                    })
                    .setNegativeButton("Cancelar", null)
                    .create();
                d.setOnDismissListener(new DialogInterface.OnDismissListener() {
                    @Override
                    public void onDismiss(DialogInterface di) { done.countDown(); }
                });
                d.show();
            }
        });
        try { done.await(); } catch (InterruptedException e) { return null; }
        return result[0];
    }
}
