// RampageActivity - SDL's activity plus what the viewer's first start needs on Android (src/android.c): the system's
// file picker for a disc image (.iso) of the game, or a folder with RAMPAGE.VOL and the game's ELF, the folder copy, a
// progress dialog and the viewer's messages. Without an "args" extra the viewer starts in the front end (--shell).
// Testing: am start -n io.github.jjmhalew.rampagere/.RampageActivity -e args "Chicago 1 --monster George" -e env "RAMPAGE_TRACE=1"
package io.github.jjmhalew.rampagere;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;

public class RampageActivity extends SDLActivity {
    private static final String TAG = "RampageRE";
    private static final int REQ_ISO = 0x5201, REQ_DIR = 0x5202;
    // the files the viewer reads from a folder (docs/NATIVE.md "Game data"), saved under these names; RAMPAGE.VOL last
    // (src/android.c takes the folder as complete once it is there, so a copy that Android cut short is asked for again)
    private static final String[] FOLDER_FILES = { "SYSTEM.CNF", "SLES_538.00", "RAMPAGE.ELF", "RAMPAGE.VOL" };

    private static final Object sLock = new Object();
    private static boolean sPicked;
    private static Uri sPick;
    private static AlertDialog sProgress;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {                  // testing: the command line of src/main.c; else the front end
        String a = getIntent() != null ? getIntent().getStringExtra("args") : null;
        return a == null || a.trim().isEmpty() ? new String[] { "--shell" } : a.trim().split("\\s+");
    }

    @Override
    protected void onCreate(Bundle state) {
        String env = getIntent() != null ? getIntent().getStringExtra("env") : null;   // testing: RAMPAGE_* settings, "K=V;K=V"
        if (env != null) for (String kv : env.split(";")) {
            int i = kv.indexOf('=');
            if (i > 0) try { Os.setenv(kv.substring(0, i).trim(), kv.substring(i + 1), true); } catch (Exception e) { Log.w(TAG, "setenv " + kv, e); }
        }
        super.onCreate(state);
    }

    // ---- called from the viewer's thread (src/android.c, src/plat_sdl.c) ---------------------------------------------

    /** kind 1: a disc image, returns a file descriptor the caller closes; kind 2: a folder, its game files copied into
     *  dest, returns 0. -1 = cancelled, -2 = failed, -3 = not enough room (kind 2). Blocks until the user is done. */
    public static int pickGameData(int kind, String dest) {
        final Activity a = (Activity) SDLActivity.getContext();
        if (a == null) return -2;
        synchronized (sLock) { sPicked = false; sPick = null; }
        a.runOnUiThread(() -> {
            Intent i;
            if (kind == 1) { i = new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*"); }
            else i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            try { a.startActivityForResult(i, kind == 1 ? REQ_ISO : REQ_DIR); }
            catch (Exception e) { Log.e(TAG, "no file picker", e); picked(null); }
        });
        Uri u;
        synchronized (sLock) {
            while (!sPicked) try { sLock.wait(); } catch (InterruptedException e) { return -1; }
            u = sPick;
        }
        if (u == null) return -1;
        try {
            if (kind == 1) {
                ParcelFileDescriptor pfd = a.getContentResolver().openFileDescriptor(u, "r");
                return pfd != null ? pfd.detachFd() : -2;
            }
            int r = copyFolder(a.getContentResolver(), u, new File(dest));
            progress(null);
            return r;
        } catch (Exception e) {
            Log.e(TAG, "pickGameData", e);
            progress(null);
            return -2;
        }
    }

    /** a dialog with this text over the viewer, null closes it */
    public static void progress(final String text) {
        final Activity a = (Activity) SDLActivity.getContext();
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (text == null) { if (sProgress != null) { sProgress.dismiss(); sProgress = null; } return; }
            if (sProgress == null) { sProgress = new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert).setTitle("RampageRE").setMessage(text).setCancelable(false).create(); sProgress.show(); }
            else sProgress.setMessage(text);
        });
    }

    /** the viewer's questions and messages (plat_dialog in src/plat_sdl.c): the text scrolls, the buttons stay on the
     *  screen. Returns 1 for b1, 0 for b2, -1 for b3 (or when there is no activity). Blocks until a button is pressed. */
    public static int dialog(final String text, final String b1, final String b2, final String b3) {
        final Activity a = (Activity) SDLActivity.getContext();
        if (a == null) return -1;
        final Object lock = new Object();
        final int[] r = { -2 };
        a.runOnUiThread(() -> {
            AlertDialog.Builder d = new AlertDialog.Builder(a, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("RampageRE").setMessage(text).setCancelable(false);
            if (b1 != null) d.setPositiveButton(b1, (x, w) -> answer(lock, r, 1));
            if (b2 != null) d.setNeutralButton(b2, (x, w) -> answer(lock, r, 0));
            if (b3 != null) d.setNegativeButton(b3, (x, w) -> answer(lock, r, -1));
            d.show();
        });
        synchronized (lock) {
            while (r[0] == -2) try { lock.wait(); } catch (InterruptedException e) { return -1; }
        }
        return r[0];
    }

    private static void answer(Object lock, int[] r, int v) {
        synchronized (lock) { r[0] = v; lock.notifyAll(); }
    }

    private static void picked(Uri u) {
        synchronized (sLock) { sPick = u; sPicked = true; sLock.notifyAll(); }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request == REQ_ISO || request == REQ_DIR) { picked(result == RESULT_OK && data != null ? data.getData() : null); return; }
        super.onActivityResult(request, result, data);
    }

    /** the game files at the top of the picked folder (names in any case) into dest: 0, -2 = none there / failed, -3 = no room */
    private static int copyFolder(ContentResolver cr, Uri tree, File dest) throws IOException {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
        String[] cols = { Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE };
        String[] ids = new String[FOLDER_FILES.length];
        long[] sizes = new long[FOLDER_FILES.length];
        long total = 0;
        try (Cursor q = cr.query(children, cols, null, null, null)) {
            if (q == null) throw new IOException("cannot list the folder");
            while (q.moveToNext()) {
                String name = q.getString(1);
                if (name == null || Document.MIME_TYPE_DIR.equals(q.getString(2))) continue;
                for (int k = 0; k < FOLDER_FILES.length; k++)
                    if (FOLDER_FILES[k].equals(name.toUpperCase(Locale.ROOT))) { ids[k] = q.getString(0); sizes[k] = q.isNull(3) ? 0 : q.getLong(3); total += sizes[k]; }
            }
        }
        if (ids[FOLDER_FILES.length - 1] == null) return -2;                // no RAMPAGE.VOL: src/android.c says so
        if (dest.getUsableSpace() < total + (16L << 20)) { Log.e(TAG, "no room: " + total + " bytes"); return -3; }
        new File(dest, "RAMPAGE.VOL").delete();                            // not complete until the new one is there
        long[] done = { 0, 0, total };
        for (int k = 0; k < FOLDER_FILES.length; k++)
            if (ids[k] != null) copyFile(cr, DocumentsContract.buildDocumentUriUsingTree(tree, ids[k]), dest, FOLDER_FILES[k], sizes[k], done);
        return 0;
    }

    private static void copyFile(ContentResolver cr, Uri doc, File dest, String name, long size, long[] done) throws IOException {
        File f = new File(dest, name), part = new File(dest, name + ".part");
        try (InputStream in = cr.openInputStream(doc); OutputStream out = new FileOutputStream(part)) {
            if (in == null) throw new IOException("cannot open " + name);
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                done[0] += n;
                if (done[0] - done[1] >= 8 << 20) { done[1] = done[0]; progress("Copying the game files... " + (done[0] >> 20) + " of " + (done[2] >> 20) + " MB"); }
            }
        }
        if (size > 0 && part.length() != size) { part.delete(); throw new IOException(name + ": " + part.length() + " of " + size + " bytes"); }
        if (!part.renameTo(f)) throw new IOException("cannot write " + f);
    }
}
