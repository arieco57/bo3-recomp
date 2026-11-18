package com.bo3recomp;

import android.content.Intent;
import android.os.Bundle;

import org.libsdl.app.SDLActivity;

// Runs libbo3_mp.so (SDL is linked into it); game data in the app's external files folder.
// Before the game: the installer if not installed yet, the video driver choice if the last run never
// showed a picture (GameStart).
public class MultiplayerActivity extends SDLActivity {
    boolean redirected;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        Intent go = GameStart.check(this, "mp");
        if (go != null) {
            redirected = true;
            startActivity(go);
            finish();
        }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (redirected) GameStart.exitWhenDone();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "bo3_mp" };
    }
}
