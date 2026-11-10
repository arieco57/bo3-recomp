package com.bo3recomp;

import org.libsdl.app.SDLActivity;

// Runs libbo3_zm.so (SDL is linked into it); game data in the app's external files folder.
public class ZombiesActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "bo3_zm" };
    }
}
