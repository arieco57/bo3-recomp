package com.bo3recomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.provider.OpenableColumns;
import android.widget.Toast;
import android.os.Bundle;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.TextView;

// Video driver choice and error reports (LogUpload): from the launcher icon's shortcut (long press >
// "Driver de video"), or by itself before the game (GameStart) when the last run never showed a picture
// or the player was never asked about error reports. Then back to the game when it came from there.
public class DriverActivity extends Activity {
    static final String UPLOAD_TEXT =
        "Enviar automáticamente el registro cuando el juego falla, para ayudar a arreglarlo "
        + "(modelo del teléfono y registro del juego, sin contraseñas ni correos)";

    String mode, failed;
    boolean askUpload;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        GameStart.uiOpen = true;   // (GameStart.exitWhenDone)
        mode = getIntent().getStringExtra("mode");
        failed = getIntent().getStringExtra("failed");
        askUpload = getIntent().getBooleanExtra("ask_upload", false);
        if (askUpload && failed == null && mode != null) askUploadOnly();
        else driverDialog();
    }

    String phone() {
        String gpu = Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : Build.HARDWARE;
        return "Teléfono: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + gpu + ")";
    }

    LinearLayout body(String msg, CheckBox upload) {
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(pad, pad / 2, pad, 0);
        TextView tv = new TextView(this);
        tv.setText(msg);
        tv.setTextSize(14);
        l.addView(tv);
        if (upload != null) l.addView(upload);
        return l;
    }

    CheckBox uploadBox() {
        CheckBox cb = new CheckBox(this);
        cb.setText(UPLOAD_TEXT);
        cb.setTextSize(14);
        Boolean c = LogUpload.consent(this);
        cb.setChecked(c == null || c);
        cb.setPadding(0, (int) (8 * getResources().getDisplayMetrics().density), 0, 0);
        return cb;
    }

    // first start: only the error-report question, then the game
    void askUploadOnly() {
        new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
            .setTitle("Registros de errores")
            .setMessage("Cuando el juego falla (no muestra imagen o se cierra), ¿enviamos el registro a los desarrolladores "
                + "para ayudar a arreglarlo?\n\nIncluye el modelo del teléfono, el driver de video y el registro del juego. "
                + "No incluye contraseñas ni correos.\n\nSe puede cambiar manteniendo apretado el ícono > Driver de video.")
            .setPositiveButton("Sí, enviar", (dlg, w) -> { LogUpload.setConsent(this, true); done(true); })
            .setNegativeButton("No", (dlg, w) -> { LogUpload.setConsent(this, false); done(true); })
            .setCancelable(false)
            .show();
    }

    AlertDialog dialog;

    String customLabel() {
        String n = GpuSettings.customName(this);
        return n != null ? "Personalizado: " + n + " (tocá para cambiarlo)" : "Personalizado: elegir un .zip de Turnip / Mesa...";
    }

    void driverDialog() {
        final String[] ids = { GpuSettings.AUTO, GpuSettings.TURNIP, GpuSettings.STOCK, GpuSettings.CUSTOM };
        String[] labels = {
            "Automático (recomendado)",
            "Turnip, incluido en la app (solo Adreno 6xx / 7xx / 8xx)",
            "Driver del teléfono",
            customLabel() };
        String cur = GpuSettings.choice(this);
        int sel = 0;
        for (int i = 0; i < ids.length; i++) if (ids[i].equals(cur)) sel = i;
        final int[] pick = { sel };
        String msg;
        if (failed != null) {
            boolean turnip = failed.endsWith("turnip");
            msg = "La última vez el juego no llegó a mostrar imagen (driver: " + (turnip ? "Turnip" : "del teléfono") + ").\n"
                + "Podés probar con el otro driver.\n\n"
                + "El registro está en Descargas/BO3Recomp (bo3-*-log.txt y logcat-*.txt).\n\n" + phone();
            if (GpuSettings.AUTO.equals(cur) || GpuSettings.TURNIP.equals(cur)) pick[0] = turnip ? 2 : 1;
            else pick[0] = 0;
        } else {
            msg = "Se aplica la próxima vez que abras el juego.\n\n" + phone();
        }
        final CheckBox upload = uploadBox();
        AlertDialog d = new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
            .setTitle("Driver de video")
            .setSingleChoiceItems(labels, pick[0], (dlg, which) -> {
                pick[0] = which;
                // a custom driver: pick the file (a .zip like the ones emulators use, or the .so)
                if (which == 3) {
                    Intent it = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    it.addCategory(Intent.CATEGORY_OPENABLE);
                    it.setType("*/*");
                    startActivityForResult(it, 400);
                }
            })
            .setPositiveButton(failed != null ? "Jugar" : "Guardar", (dlg, w) -> {
                GpuSettings.set(this, ids[pick[0]]);
                LogUpload.setConsent(this, upload.isChecked());
                done(failed != null);
            })
            .setNegativeButton(failed != null ? "Salir" : "Cancelar", (dlg, w) -> {
                if (askUpload) LogUpload.setConsent(this, upload.isChecked());
                finish();
            })
            .setOnCancelListener(dlg -> finish())
            .create();
        // the message under the list (an AlertDialog shows either a message or a list)
        d.setView(body(msg, upload));
        dialog = d;
        d.show();
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != 400) return;
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        Uri u = data.getData();
        String display = null;
        try (Cursor c = getContentResolver().query(u, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst()) display = c.getString(0);
        } catch (Exception ignored) { }
        try {
            String name = GpuSettings.installCustom(this, u, display);
            Toast.makeText(this, "Driver instalado: " + name, Toast.LENGTH_LONG).show();
            // chosen right away; the dialog again, now showing it
            GpuSettings.set(this, GpuSettings.CUSTOM);
            if (dialog != null) { dialog.setOnCancelListener(null); dialog.dismiss(); }
            driverDialog();
        } catch (Exception e) {
            new AlertDialog.Builder(this, android.R.style.Theme_Material_Dialog_Alert)
                .setTitle("Driver personalizado")
                .setMessage("No se pudo instalar: " + e.getMessage())
                .setPositiveButton("OK", null)
                .show();
        }
    }

    void done(boolean play) {
        if (play && mode != null) {
            // the failed run: its report still to send at this start (GameStart -> LogUpload) when agreed
            if (failed != null) {
                String driver = failed.indexOf(' ') < 0 ? "" : failed.substring(failed.indexOf(' '));
                boolean send = Boolean.TRUE.equals(LogUpload.consent(this)) && !getIntent().getBooleanExtra("reported", false);
                GpuSettings.setState(this, (send ? "pending" : "asked") + driver);
            }
            startActivity(new Intent(this, GameStart.activityFor(mode)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        }
        finish();
    }
}
