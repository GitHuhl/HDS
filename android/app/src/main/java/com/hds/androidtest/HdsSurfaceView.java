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
// (screen off/on, a multi-window resize).
//
// OnTouchListener relays touch into the native touch ring -- action is collapsed down to the
// four values hds_android_gui.c's own dispatch actually needs (0=sequence start, 1=move,
// 2=sequence end, 3=pointer count changed mid-sequence), and x/y is either the one real finger's
// own coordinate (pointerCount==1) or the plain centroid (average) of every currently active
// pointer (pointerCount>=2) -- computed here, where MotionEvent's own per-pointer accessors are
// available, rather than reconstructed from a flattened ring event on the native side. One
// finger vs. two-or-more is itself the whole signal hds_android_gui.c's own gesture_mode needs
// (one finger taps/drags a parameter, two or more only ever scroll -- explicit product
// direction, see that file's own header comment) -- getPointerCount()'s own exact value at the
// single instant of an ACTION_POINTER_DOWN/_UP transition (it still counts the pointer about to
// be added/removed either way) only ever matters for that one event, which just re-baselines the
// scroll accumulator on the native side rather than applying a delta -- so no special-casing of
// that transition instant is needed here.
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
        // width/height every single frame and recomputes its own scale_x/scale_y from them
        // directly -- there is nothing for this callback to forward that the native side doesn't
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
            case MotionEvent.ACTION_POINTER_DOWN:
            case MotionEvent.ACTION_POINTER_UP: action = 3; break;
            default: action = 2; break; // ACTION_UP, ACTION_CANCEL, anything else
        }
        int pointerCount = event.getPointerCount();
        float x, y;
        if (pointerCount > 1) {
            float sumX = 0f, sumY = 0f;
            for (int i = 0; i < pointerCount; i++) {
                sumX += event.getX(i);
                sumY += event.getY(i);
            }
            x = sumX / pointerCount;
            y = sumY / pointerCount;
        } else {
            x = event.getX();
            y = event.getY();
        }
        NativeBridge.nativeTouchEvent(action, pointerCount, x, y);
        return true;
    }
}
