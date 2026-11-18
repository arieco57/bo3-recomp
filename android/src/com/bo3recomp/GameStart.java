package com.bo3recomp;

import android.app.Activity;
import android.content.Intent;

// What a game activity does before the game starts (after SDLActivity.onCreate, before the game's
// main runs): not installed -> the installer; error reports never asked / the last run never showed a
// picture -> DriverActivity; the last run failed -> its log to our backend (LogUpload); the logs for
// testers (LogExport).
public class GameStart {
    // the activity to go to instead of starting the game, or null
    static Intent check(Activity a, String mode) {
        if (!Installer.isInstalled(a))
            return new Intent(a, InstallerActivity.class).putExtra("mode", mode)
                    .putExtra("debug_paths", a.getIntent().getStringExtra("debug_paths"))
                    .putExtra("debug_convert", a.getIntent().getBooleanExtra("debug_convert", false));
        String st = GpuSettings.state(a);
        // error reports: asked once (with the driver choice when the last run failed)
        if (LogUpload.consent(a) == null) {
            Intent i = new Intent(a, DriverActivity.class).putExtra("mode", mode).putExtra("ask_upload", true);
            if (st.startsWith("starting")) i.putExtra("failed", st);
            return i;
        }
        boolean sent = LogUpload.sendPending(a, mode);
        if (sent && (st.startsWith("starting") || st.startsWith("pending")))
            GpuSettings.setState(a, "reported" + st.substring(st.indexOf(' ') < 0 ? st.length() : st.indexOf(' ')));
        if (st.startsWith("starting"))
            return new Intent(a, DriverActivity.class).putExtra("mode", mode).putExtra("failed", st)
                    .putExtra("reported", sent);
        LogExport.start(a, mode);
        return null;
    }

    // the game never started in this process: leave it, so the game starts clean afterwards - only when
    // it is the game's own process (":zm" / ":mp" in the manifest): environments that run every activity
    // in one process (Husk on iOS) would close the installer just opened too
    static volatile boolean uiOpen;   // the installer / driver screen runs in this same process

    static void exitWhenDone() {
        String proc = android.app.Application.getProcessName();
        if (uiOpen || (proc != null && proc.indexOf(':') < 0)) {
            android.util.Log.i("BO3", "redirected: the next screen runs in this process (" + proc + "), not exiting");
            // the game activity comes back in this same process afterwards: SDL refuses a second
            // creation (it exits) unless allowed - the first one never started the game
            try { org.libsdl.app.SDLActivity.nativeSetenv("SDL_ANDROID_ALLOW_RECREATE_ACTIVITY", "1"); } catch (Throwable t) { }
            return;
        }
        System.exit(0);
    }

    static Class<?> activityFor(String mode) {
        return "mp".equals(mode) ? MultiplayerActivity.class : ZombiesActivity.class;
    }
}
