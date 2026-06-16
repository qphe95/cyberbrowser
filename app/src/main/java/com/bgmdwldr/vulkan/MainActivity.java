package com.bgmdwldr.vulkan;

import android.app.NativeActivity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.inputmethod.InputMethodManager;
import android.graphics.Rect;

/**
 * MainActivity that forwards all input events to native C layer.
 * The native layer handles all text input and UI interaction.
 */
public class MainActivity extends NativeActivity {
    private static final String TAG = "MainActivity";

    // Native methods for event forwarding
    private static native void nativeOnTouch(int action, float x, float y);
    private static native void nativeOnKey(int keyCode, int unicodeChar, int action);
    private static native void nativeOnPaste(String text);
    
    // Native methods for keyboard control
    private static native void nativeOnKeyboardShown();
    private static native void nativeOnKeyboardHidden();
    private static native void nativeOnKeyboardHeight(int heightPx);

    static {
        System.loadLibrary("minimalvulkan");
    }

    private View mDecorView;
    private int mLastKeyboardHeight = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "onCreate: Event forwarding initialized");
        mDecorView = getWindow().getDecorView();
        installKeyboardHeightListener();
    }

    /**
     * Detect the visible display frame and report keyboard height to native.
     */
    private void installKeyboardHeightListener() {
        if (mDecorView == null) return;
        mDecorView.getViewTreeObserver().addOnGlobalLayoutListener(
            new ViewTreeObserver.OnGlobalLayoutListener() {
                @Override
                public void onGlobalLayout() {
                    Rect visibleFrame = new Rect();
                    mDecorView.getWindowVisibleDisplayFrame(visibleFrame);
                    int screenHeight = mDecorView.getRootView().getHeight();
                    int keyboardHeight = screenHeight - visibleFrame.bottom;
                    if (keyboardHeight < 0) keyboardHeight = 0;
                    if (keyboardHeight != mLastKeyboardHeight) {
                        mLastKeyboardHeight = keyboardHeight;
                        try {
                            nativeOnKeyboardHeight(keyboardHeight);
                            Log.d(TAG, "Keyboard height: " + keyboardHeight + "px");
                        } catch (Exception e) {
                            Log.e(TAG, "Error reporting keyboard height", e);
                        }
                    }
                }
            }
        );
    }

    /**
     * Capture all touch events and forward to native layer.
     * Native layer decides what to do based on touch coordinates.
     */
    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        float x = event.getX();
        float y = event.getY();
        
        // Forward to native layer
        try {
            nativeOnTouch(action, x, y);
        } catch (Exception e) {
            Log.e(TAG, "Error forwarding touch event", e);
        }
        
        // Always consume touch events - native layer handles everything
        return true;
    }

    /**
     * Capture all key events and forward to native layer.
     * Includes soft keyboard input, hardware keys, etc.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int action = event.getAction();
        int keyCode = event.getKeyCode();
        int unicodeChar = event.getUnicodeChar();
        
        boolean ctrl = event.isCtrlPressed();
        boolean meta = event.isMetaPressed();
        int metaState = event.getMetaState();
        Log.i(TAG, "dispatchKeyEvent: action=" + action + 
              " keyCode=" + keyCode + " unicode=" + unicodeChar +
              " ctrl=" + ctrl + " meta=" + meta + " metaState=" + metaState);
        
        // Detect paste: Ctrl+V, Meta+V, dedicated PASTE key, or ACTION_MULTIPLE text injection
        boolean isPasteKey = (ctrl || meta) && keyCode == KeyEvent.KEYCODE_V;
        boolean isPasteKeyCode = keyCode == KeyEvent.KEYCODE_PASTE;
        if (action == KeyEvent.ACTION_DOWN && (isPasteKey || isPasteKeyCode)) {
            String pasted = getClipboardText();
            if (pasted != null && !pasted.isEmpty()) {
                Log.i(TAG, "Paste detected, text length=" + pasted.length());
                try {
                    nativeOnPaste(pasted);
                } catch (Exception e) {
                    Log.e(TAG, "Error forwarding paste", e);
                }
                return true;
            }
        }
        
        // Handle text injection (e.g., from emulator clipboard sync or IME commit)
        if (action == KeyEvent.ACTION_MULTIPLE && keyCode == KeyEvent.KEYCODE_UNKNOWN) {
            String chars = event.getCharacters();
            if (chars != null && !chars.isEmpty()) {
                Log.i(TAG, "ACTION_MULTIPLE text injection, length=" + chars.length());
                try {
                    nativeOnPaste(chars);
                } catch (Exception e) {
                    Log.e(TAG, "Error forwarding text injection", e);
                }
                return true;
            }
        }
        
        // Forward to native layer
        try {
            nativeOnKey(keyCode, unicodeChar, action);
            Log.i(TAG, "nativeOnKey called successfully");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "nativeOnKey JNI method not found!", e);
        } catch (Exception e) {
            Log.e(TAG, "Error forwarding key event", e);
        }
        
        // Consume event - native layer handles everything
        return true;
    }
    
    private String getClipboardText() {
        ClipboardManager clipboard = (ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
        if (clipboard != null && clipboard.hasPrimaryClip()) {
            ClipData clip = clipboard.getPrimaryClip();
            if (clip != null && clip.getItemCount() > 0) {
                ClipData.Item item = clip.getItemAt(0);
                CharSequence text = item.getText();
                if (text != null) {
                    return text.toString();
                }
            }
        }
        return null;
    }

    /**
     * Called by native layer to show the soft keyboard.
     */
    public void showKeyboard() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                // Request focus so the IME has a target view
                mDecorView.requestFocus();
                imm.showSoftInput(mDecorView, InputMethodManager.SHOW_IMPLICIT);
                nativeOnKeyboardShown();
                Log.d(TAG, "Keyboard shown");
            }
        });
    }

    /**
     * Called by native layer to hide the soft keyboard.
     */
    public void hideKeyboard() {
        runOnUiThread(() -> {
            InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.hideSoftInputFromWindow(mDecorView.getWindowToken(), 0);
                nativeOnKeyboardHidden();
                Log.d(TAG, "Keyboard hidden");
            }
        });
    }

    @Override
    public void onBackPressed() {
        // Let native layer handle back button
        // For now, just finish the activity
        finish();
    }
}
