// RampageActivity - SDL's activity plus what the viewer's first start needs on Android (src/android.c): the system's
// file picker for a disc image (.iso) of the game, or a folder with RAMPAGE.VOL and the game's ELF, the folder copy, a
// progress dialog and the viewer's messages. Without an "args" extra the viewer starts in the front end (--shell).
// The Wii's data (docs/WII_PLAN.md): the launcher's shortcut "Add the Wii's data" (res/xml/shortcuts.xml, the "wii"
// extra) copies the Game, Shell and Audio folders of an extracted Wii disc into the app folder's wii (wiiSetup): before
// the viewer reads its data on a fresh start (--wii-pick, src/android.c), else at once and read at the next start.
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
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

public class RampageActivity extends SDLActivity {
    private static final String TAG = "RampageRE";
    private static final int REQ_ISO = 0x5201, REQ_DIR = 0x5202;
    // the files the viewer reads from a folder (docs/NATIVE.md "Game data"), saved under these names; RAMPAGE.VOL last
    // (src/android.c takes the folder as complete once it is there, so a copy that Android cut short is asked for again)
    private static final String[] FOLDER_FILES = { "SYSTEM.CNF", "SLES_538.00", "SLUS_213.23", "RAMPAGE.ELF", "RAMPAGE.VOL" };
    // the Wii disc's folders the viewer reads (src/vol.c vol_add_wii), in the picked folder, its DATA/files or its files
    private static final String[] WII_DIRS = { "Game", "Shell", "Audio" };
    private static final String[][] WII_UNDER = { {}, { "DATA", "files" }, { "files" } };

    private static final Object sLock = new Object();
    private static boolean sPicked;
    private static Uri sPick;
    private static AlertDialog sProgress;
    private static volatile boolean sWiiBusy;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    @Override
    protected String[] getArguments() {                  // testing: the command line of src/main.c; else the front end
        String a = getIntent() != null ? getIntent().getStringExtra("args") : null;
        String[] r = a == null || a.trim().isEmpty() ? new String[] { "--shell" } : a.trim().split("\\s+");
        if (getIntent() == null || !getIntent().hasExtra("wii")) return r;
        String[] w = Arrays.copyOf(r, r.length + 1);     // the shortcut: wiiSetup before the data is read
        w[r.length] = "--wii-pick";
        return w;
    }

    @Override
    protected void onNewIntent(Intent intent) {          // the shortcut while the viewer runs: the copy now, read next start
        super.onNewIntent(intent);
        if (intent == null || !intent.hasExtra("wii") || sWiiBusy) return;
        File ext = getExternalFilesDir(null);
        final String home = (ext != null ? ext : getFilesDir()).getPath();
        new Thread(() -> wiiSetup(home, 1), "wiiSetup").start();
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
     *  dest, returns 0; kind 3: an extracted Wii disc, its Game, Shell and Audio copied into dest/wii, returns 0.
     *  -1 = cancelled, -2 = failed, -3 = not enough room (kinds 2, 3), -4 = no Game folder (kind 3). Blocks until the
     *  user is done. */
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
            int r = kind == 3 ? copyWii(a.getContentResolver(), u, new File(dest)) : copyFolder(a.getContentResolver(), u, new File(dest));
            progress(null);
            return r;
        } catch (Exception e) {
            Log.e(TAG, "pickGameData", e);
            progress(null);
            return -2;
        }
    }

    /** the Wii's data: asks for the folder of an extracted Wii disc and copies its Game, Shell and Audio into dest/wii,
     *  until that is done or "Not now". running: the viewer has read its data already, so the copy counts from the next
     *  start. 1 = copied, 0 = not. Blocks (not on the UI thread). */
    public static int wiiSetup(String dest, int running) {
        if (sWiiBusy) return 0;
        sWiiBusy = true;
        try {
            for (;;) {
                if (dialog("The Wii's monsters and Dallas (optional): choose the folder of your own extracted Wii disc of "
                        + "Rampage - Total Destruction (Dolphin's Extract Entire Disc), or its DATA/files. Its Game, Shell "
                        + "and Audio folders (about 320 MB) are copied once into the app's folder.\n\nOr copy those three "
                        + "folders with a USB cable into\n" + dest + "/wii", "Folder", null, "Not now") != 1)
                    return 0;
                int r = pickGameData(3, dest);
                if (r == -1) continue;
                if (r == 0) {
                    if (running != 0) dialog("The Wii's files are in. RampageRE reads them at its next start: close it in "
                            + "the recent apps and start it again.", "OK", null, null);
                    return 1;
                }
                dialog(r == -3 ? "Not enough free room on the device for the Wii's files."
                     : r == -4 ? "That folder holds no Game folder (nor DATA/files/Game): choose the extracted Wii disc, "
                                 + "the folder with DATA in it, or DATA/files."
                     : "Could not copy the Wii's files.", "OK", null, null);
            }
        } finally {
            sWiiBusy = false;
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
            if (ids[k] != null) copyFile(cr, DocumentsContract.buildDocumentUriUsingTree(tree, ids[k]), dest, FOLDER_FILES[k], sizes[k], done, "the game files");
        return 0;
    }

    /** a folder's entries: { id, name, mime type, size } */
    private static List<String[]> children(ContentResolver cr, Uri tree, String id) throws IOException {
        String[] cols = { Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE };
        List<String[]> r = new ArrayList<>();
        try (Cursor q = cr.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, id), cols, null, null, null)) {
            if (q == null) throw new IOException("cannot list a folder");
            while (q.moveToNext()) r.add(new String[] { q.getString(0), q.getString(1), q.getString(2), q.isNull(3) ? "0" : q.getString(3) });
        }
        return r;
    }

    private static String subfolder(ContentResolver cr, Uri tree, String id, String name) throws IOException {
        for (String[] c : children(cr, tree, id))
            if (Document.MIME_TYPE_DIR.equals(c[2]) && name.equalsIgnoreCase(c[1])) return c[0];
        return null;
    }

    private static void deleteTree(File f) {
        File[] in = f.listFiles();
        if (in != null) for (File g : in) deleteTree(g);
        f.delete();
    }

    /** the files of the Wii's Game, Shell and Audio (names in any case) into dest/wii, through dest/wii.part (renamed
     *  when complete, so a copy that Android cut short leaves no wii): 0, -2 = failed, -3 = no room, -4 = no Game */
    private static int copyWii(ContentResolver cr, Uri tree, File dest) throws IOException {
        String[] dirs = null;
        for (String[] under : WII_UNDER) {
            String at = DocumentsContract.getTreeDocumentId(tree);
            for (String p : under) if (at != null) at = subfolder(cr, tree, at, p);
            String game = at != null ? subfolder(cr, tree, at, WII_DIRS[0]) : null;
            if (game == null) continue;
            dirs = new String[WII_DIRS.length];
            dirs[0] = game;
            for (int k = 1; k < WII_DIRS.length; k++) dirs[k] = subfolder(cr, tree, at, WII_DIRS[k]);
            break;
        }
        if (dirs == null) return -4;
        List<List<String[]>> files = new ArrayList<>();
        long total = 0;
        for (String d : dirs) {
            List<String[]> f = new ArrayList<>();
            if (d != null) for (String[] c : children(cr, tree, d))
                if (!Document.MIME_TYPE_DIR.equals(c[2]) && c[1] != null) { f.add(c); total += Long.parseLong(c[3]); }
            files.add(f);
        }
        File part = new File(dest, "wii.part"), wii = new File(dest, "wii");
        deleteTree(part);
        if (dest.getUsableSpace() < total + (16L << 20)) { Log.e(TAG, "no room: " + total + " bytes"); return -3; }
        long[] done = { 0, 0, total };
        for (int k = 0; k < WII_DIRS.length; k++) {
            File d = new File(part, WII_DIRS[k]);
            if (!d.mkdirs()) throw new IOException("cannot make " + d);
            for (String[] c : files.get(k))
                copyFile(cr, DocumentsContract.buildDocumentUriUsingTree(tree, c[0]), d, c[1], Long.parseLong(c[3]), done, "the Wii's files");
        }
        deleteTree(wii);
        if (!part.renameTo(wii)) throw new IOException("cannot write " + wii);
        Log.i(TAG, "wii: " + (total >> 20) + " MB copied");
        return 0;
    }

    private static void copyFile(ContentResolver cr, Uri doc, File dest, String name, long size, long[] done, String what) throws IOException {
        File f = new File(dest, name), part = new File(dest, name + ".part");
        try (InputStream in = cr.openInputStream(doc); OutputStream out = new FileOutputStream(part)) {
            if (in == null) throw new IOException("cannot open " + name);
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) {
                out.write(buf, 0, n);
                done[0] += n;
                if (done[0] - done[1] >= 8 << 20) { done[1] = done[0]; progress("Copying " + what + "... " + (done[0] >> 20) + " of " + (done[2] >> 20) + " MB"); }
            }
        }
        if (size > 0 && part.length() != size) { part.delete(); throw new IOException(name + ": " + part.length() + " of " + size + " bytes"); }
        if (!part.renameTo(f)) throw new IOException("cannot write " + f);
    }
}
