package com.hds.androidtest;

import android.content.Context;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

// Plain Java (not Kotlin) -- see NativeBridge.java's own header comment for why. A thin shell:
// every frame is drawn natively (src/hds_android_gui.c), straight into this view's own Surface --
// no Canvas, no Bitmap, nothing Java-side ever touches a pixel (see that file's own header
// comment for the full architecture and why). SurfaceHolder.Callback drives the native GUI's own
// window lifecycle: its own dedicated redraw thread exists only between surfaceCreated and the
// matching surfaceDestroyed (see hds_android_gui.c's own comment on hds_android_gui_surface_
// created()/_destroyed() for why), which can legitimately happen more than once per process
// (screen off/on, a multi-window resize). OnTouchListener relays touch straight into the native
// touch ring, collapsing MotionEvent's own action-constant space down to the three values
// hds_android_gui.c's own touch dispatch actually needs (see that file's own comment on why it
// needs no more than down/move/up-or-cancel for this single-finger-only GUI).
public class HdsSurfaceView extends SurfaceView implements SurfaceHolder.Callback, View.OnTouchListener {
    public HdsSurfaceView(Context context) {
        super(context);
        getHolder().addCallback(this);
        setOnTouchListener(this);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        NativeBridge.nativeSurfaceCreated(holder.getSurface());
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        // No-op: hds_android_gui_redraw() (hds_android_gui.c) re-reads the locked buffer's own
        // width/height every single frame and calls hds_clap_gui_on_resize() itself whenever they
        // change -- there is nothing for this callback to forward that the native side doesn't
        // already learn on its own, one frame later at most.
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        NativeBridge.nativeSurfaceDestroyed();
    }

    @Override
    public boolean onTouch(View v, MotionEvent event) {
        int action;
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: action = 0; break;
            case MotionEvent.ACTION_MOVE: action = 1; break;
            default: action = 2; break; // ACTION_UP, ACTION_CANCEL, anything else -- see this class's own header comment
        }
        NativeBridge.nativeTouchEvent(action, event.getX(), event.getY());
        return true;
    }
}
