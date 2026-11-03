package io.github.p5rrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.system.Os;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The home screen in front of the game (a NativeActivity, libp5r.so): checks for the player's data in Internal
 * storage/Persona 5 Royal (asking for all-files access), shows the options and writes them to that folder's
 * config.txt, whose NX_* lines the runtime turns into its switches:
 *   game text   the language of the game's texts and subtitles (language=auto, default: the device's, English when
 *               the game lacks it; or en-US, es, fr, de, it, ja, ko, zh-Hans, zh-Hant). The voices are the game's own
 *               option.
 *   resolution  720p (NX_HANDHELD=1, default) or 1080p (NX_HANDHELD=0)
 *   interface   at 720p (NX_HUD_1080=0, default: faster) or, in 720p, drawn at 1080p (NX_HUD_1080=1: sharper text)
 *   graphics    low (default), medium or high: textures one mip smaller (NX_LOD_BIAS=1), half a mip, or the game's
 *               own. (A lower render resolution needs the engine's target sizes: games/p5r/PROGRESO.md.)
 *   frame rate  fps_mode: 30 (default: upstream wrote NX_FPS_CAP=30), 60, 90, 120 or 0 (no limit). NX_FPS_CAP is kept in
 *               step for the game's own interval patch (games/p5r/game.cpp); 90/120 are experimental and the launcher
 *               warns that the game may run at the wrong speed (docs/PERF-DESIGN.md §1.1)
 *   performance perf_mode (balance/quality/2k/2k90/ultra/custom), render_height (0/720/1080/1440/1600: over 1080 also
 *               writes allow_over_1080=1), refresh_hz (0/60/90/120/144) and present_mode (auto/fifo/mailbox/immediate),
 *               all resolved by runtime/src/perf_profile.h (docs/PERF-DESIGN.md §2-§3). A one-tap 2K90 preset at the top
 *               writes the whole recommended combination for the OnePlus Pad 3 Pro.
 *   FPS counter off (default) or on (NX_SHOW_FPS)
 *   shadows     off (NX_SHADOWS=0, default: the shadow maps are not drawn, ~9% of the GPU on the 3D title) or on
 *   on-screen controls on (default) or off (NX_TOUCH_SHOW), their size (NX_TOUCH_SIZE) and opacity (NX_TOUCH_OPACITY),
 *               and where they are (NX_TOUCH_LAYOUT, written by ControlsEditor)
 * and imports or exports the saves (user/save, SaveFiles.java): a ZIP or a folder in, a ZIP in Download out.
 * The game: the player picks their own NSP or XCI (Persona 5 Royal 1.0.0) and prod.keys here; ImportActivity checks
 * them and translates the game's shaders once, and every start reads the game from that file as it is (the runtime's
 * game_image.cpp, through NX_GAME_FD / NX_GAME_NAME / NX_KEYS). Nothing of the game is copied but its shaders. A data
 * folder made on a PC (exefs/ or analysis/, extract/romfs, spirv/) still works when no file was picked.
 * The icon: a home screen shortcut with a picture of the player's own (Android lets an app pin shortcuts, not change
 * its own icon).
 * Other lines of config.txt are kept as they were. The launcher itself speaks the device's language (Lang.java:
 * Spanish, English, French, German, Italian, Portuguese, Japanese, Korean, Chinese; English otherwise).
 */
public class Launcher extends Activity {
    static final String DATA = "Persona 5 Royal";

    static final class Option {
        final String key, es, en;
        final String[] values, labelsEs, labelsEn;
        int pick;

        Option(String key, String es, String en, String[] values, String[] labelsEs, String[] labelsEn, int pick) {
            this.key = key;
            this.es = es;
            this.en = en;
            this.values = values;
            this.labelsEs = labelsEs;
            this.labelsEn = labelsEn;
            this.pick = pick;
        }
    }

    final List<Option> options = new ArrayList<>();
    final Map<String, String> config = new LinkedHashMap<>();
    TextView status;
    TextView fpsWarning;
    // The home screen is one scroll: without grouping it is 25 flat rows, so the option list gets section headers and
    // the six expert tuning switches sit behind a toggle (only the performance preset and the frame rate are shown by
    // default). perfSummary states what will actually run, which is the feedback that used to require reading logcat.
    TextView perfSummary;
    final List<View> advancedViews = new ArrayList<>();
    boolean showAdvanced = false;
    // The tuning switches hidden until "advanced" is turned on. performance_mode and fps_mode stay visible: they are
    // the two choices a player actually makes.
    static final List<String> ADVANCED_KEYS =
            java.util.Arrays.asList("render_height", "refresh_hz", "present_mode", "thermal_guard", "dyn_res", "dyn_res_min");
    Button play;
    static final int PICK_ZIP = 1, PICK_FOLDER = 2, PICK_GAME = 3, PICK_KEYS = 4, PICK_ICON = 5, PREPARE = 6, PICK_CPK = 7, PICK_DRIVER = 8;

    String t(String spanish, String english) { return Lang.t(spanish, english); }

    /** The section a row belongs to; a header is drawn whenever this changes (contiguous runs of the option list). */
    static String sectionOf(String key) {
        if ("language".equals(key)) return "S1";
        if ("RESOLUTION".equals(key) || "NX_STRETCH".equals(key) || "NX_HUD_1080".equals(key) || "GRAPHICS".equals(key))
            return "S2";
        if (ADVANCED_KEYS.contains(key) || "perf_mode".equals(key) || "fps_mode".equals(key)) return "S3";
        if ("NX_SHADOWS".equals(key) || "NX_SHOW_FPS".equals(key)) return "S4";
        if ("NX_TOUCH_SHOW".equals(key) || "NX_TOUCH_SIZE".equals(key) || "NX_TOUCH_OPACITY".equals(key)) return "S5";
        if ("NX_NO_CROWD".equals(key) || "NX_DISABLE_BLOOM".equals(key)) return "S6";
        return "S7";
    }

    TextView sectionHeader(String section) {
        TextView h = new TextView(this);
        h.setTextColor(Color.rgb(120, 170, 255));
        h.setTextSize(14);
        h.setTypeface(null, Typeface.BOLD);
        h.setPadding(0, dp(14), 0, dp(2));
        if ("S1".equals(section)) h.setText(t("Textos", "Text"));
        else if ("S2".equals(section)) h.setText(t("Imagen", "Picture"));
        else if ("S3".equals(section)) h.setText(t("Rendimiento", "Performance"));
        else if ("S4".equals(section)) h.setText(t("Imagen: detalles", "Picture: extras"));
        else if ("S5".equals(section)) h.setText(t("Controles en pantalla", "On-screen controls"));
        else if ("S6".equals(section)) h.setText(t("Coste del dibujado", "Rendering cost"));
        else h.setText(t("Mando y sensibilidad", "Gamepad and sensitivity"));
        return h;
    }

    String labelOf(String key) {
        for (Option o : options) if (o.key.equals(key)) return label(o);
        return "";
    }

    /** What the runtime will actually do, so "did the preset take effect" needs no logcat. */
    void updatePerfSummary() {
        if (perfSummary == null) return;
        perfSummary.setText(t("En juego: ", "In game: ") + labelOf("perf_mode") + " · " + labelOf("fps_mode") + " · "
                + labelOf("render_height") + " · " + labelOf("refresh_hz") + " · " + labelOf("present_mode") + " · "
                + labelOf("thermal_guard"));
    }

    String label(Option o) { return t(o.labelsEs[o.pick], o.labelsEn[o.pick]); }

    static File dataDir() { return new File(Environment.getExternalStorageDirectory(), DATA); }

    static final class Api30Helper {
        static boolean isExternalStorageManager() {
            return Environment.isExternalStorageManager();
        }
    }

    boolean hasAccess() {
        if (Build.VERSION.SDK_INT < 30) return true;
        return Api30Helper.isExternalStorageManager();
    }

    // ---- the game file, read in place at every start ----

    SharedPreferences prefs() { return getSharedPreferences("game", MODE_PRIVATE); }

    String gameUri() { return prefs().getString("uri", null); }

    String gameName() { return prefs().getString("name", ""); }

    static File keysFile(Context c, String name) { return new File(new File(c.getFilesDir(), "keys"), name); }

    boolean hasKeys() { return keysFile(this, "prod.keys").length() > 0; }

    boolean gameReadable() {
        try (ParcelFileDescriptor f = getContentResolver().openFileDescriptor(Uri.parse(gameUri()), "r")) {
            return f != null;
        } catch (Exception e) {
            return false;
        }
    }

    // The shaders were translated from this game file (ImportActivity writes spirv/import.txt last).
    boolean prepared() {
        File note = new File(dataDir(), "spirv/import.txt");
        if (!note.canRead()) return false;
        try (BufferedReader r = new BufferedReader(new FileReader(note))) {
            return gameName().equals(r.readLine());
        } catch (Exception e) {
            return false;
        }
    }

    String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last == null ? "" : last.substring(last.lastIndexOf('/') + 1);
    }

    void pick(int request, String type, boolean multiple) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(type);
        if (multiple) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, request);
    }

    void onGamePicked(Uri uri) {
        String name = displayName(uri);
        String lower = name.toLowerCase(Locale.ROOT);
        if (!lower.endsWith(".nsp") && !lower.endsWith(".xci")) {
            tell(t("Archivo no válido", "Not a game file"),
                    t("Elige el archivo .nsp o .xci del juego base (no el de la actualización).",
                            "Choose the base game's .nsp or .xci file (not the update's)."));
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {  // a provider without persistable grants: works while the app runs
        }
        prefs().edit().putString("uri", uri.toString()).putString("name", name).apply();
    }

    // prod.keys (and title.keys, when the dump needs it): copied into the app's own storage, never shared or logged.
    void onKeysPicked(Intent data) {
        List<Uri> uris = new ArrayList<>();
        ClipData clip = data.getClipData();
        if (clip != null)
            for (int k = 0; k < clip.getItemCount(); k++) uris.add(clip.getItemAt(k).getUri());
        else if (data.getData() != null) uris.add(data.getData());
        boolean prod = false;
        for (Uri uri : uris) {
            String name = displayName(uri).toLowerCase(Locale.ROOT);
            String target = name.contains("title") ? "title.keys" : "prod.keys";
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                byte[] buf = new byte[1 << 20];
                int n = 0, r;
                while (in != null && n < buf.length && (r = in.read(buf, n, buf.length - n)) > 0) n += r;
                String text = new String(buf, 0, n, "UTF-8");
                if (target.equals("prod.keys") && !text.contains("header_key") && !text.contains("master_key_00")) continue;
                File f = keysFile(this, target);
                f.getParentFile().mkdirs();
                try (OutputStream out = new FileOutputStream(f)) {
                    out.write(buf, 0, n);
                }
                prod |= target.equals("prod.keys");
            } catch (Exception ignored) {
            }
        }
        if (!prod && !hasKeys())
            tell(t("Claves no válidas", "Not a keys file"),
                    t("Ese archivo no tiene las claves (header_key). Elige el prod.keys de tu consola.",
                            "That file has no keys (header_key). Choose your console's prod.keys."));
    }

    void prepare() {
        Intent i = new Intent(this, ImportActivity.class);
        i.putExtra(ImportActivity.EXTRA_URI, gameUri());
        i.putExtra(ImportActivity.EXTRA_NAME, gameName());
        startActivityForResult(i, PREPARE);
    }

    // The runtime reads the game from this descriptor (same process: the NativeActivity runs in the launcher's).
    boolean openGameForRuntime() {
        try {
            ParcelFileDescriptor f = getContentResolver().openFileDescriptor(Uri.parse(gameUri()), "r");
            if (f == null) return false;
            Os.setenv("NX_GAME_FD", String.valueOf(f.detachFd()), true);
            Os.setenv("NX_GAME_NAME", gameName(), true);
            Os.setenv("NX_KEYS", keysFile(this, "prod.keys").getPath(), true);
            File tk = keysFile(this, "title.keys");
            if (tk.exists()) Os.setenv("NX_TITLE_KEYS", tk.getPath(), true);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // A home screen shortcut with the player's picture (cropped to a square; an adaptive icon, so the home screen gives
    // it its own shape).
    void onIconPicked(Uri uri) {
        try {
            Bitmap src;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                src = BitmapFactory.decodeStream(in);
            }
            if (src == null) throw new IllegalArgumentException("not a picture");
            int side = Math.min(src.getWidth(), src.getHeight());
            Bitmap square = Bitmap.createBitmap(src, (src.getWidth() - side) / 2, (src.getHeight() - side) / 2, side, side);
            Bitmap icon = Bitmap.createScaledBitmap(square, 432, 432, true);
            ShortcutManager sm = getSystemService(ShortcutManager.class);
            if (sm == null || !sm.isRequestPinShortcutSupported()) {
                tell(t("Icono", "Icon"), t("Tu pantalla de inicio no admite accesos directos.",
                        "Your home screen does not support shortcuts."));
                return;
            }
            Intent launch = new Intent(this, Launcher.class).setAction(Intent.ACTION_MAIN);
            ShortcutInfo info = new ShortcutInfo.Builder(this, "picture_" + System.currentTimeMillis())
                    .setShortLabel("P5R").setLongLabel("Persona 5 Royal").setIcon(Icon.createWithAdaptiveBitmap(icon))
                    .setIntent(launch).build();
            sm.requestPinShortcut(info, null);
            tell(t("Icono", "Icon"), t("Acepta el aviso para poner el acceso directo con tu imagen en la pantalla de inicio. "
                            + "El icono del cajón de apps no cambia: Android no deja que una app cambie el suyo.",
                    "Accept the prompt to put the shortcut with your picture on the home screen. The icon in the app "
                            + "drawer stays: Android does not let an app change its own."));
        } catch (Exception e) {
            tell(t("Icono", "Icon"), t("No se pudo usar esa imagen.", "That picture could not be used."));
        }
    }

    // The recompiled build reads analysis/main.bin, the native one (the game's own code) exefs/main.
    boolean hasData() {
        return new File(dataDir(), "exefs/main").canRead() || new File(dataDir(), "analysis/main.bin").canRead();
    }

    // The build ID the port is made for (Persona 5 Royal 1.0.0, the base game): the start of exefs/main's module ID
    // (NSO header +0x40). The native build refuses another (runtime main.cpp); say so here instead of closing.
    static final String[] KNOWN_BUILDS = {
        "708BCACA873EA32B",  // US
        "B78A03E489E5BD92",  // EU
        "6E1F4CAE92A28C91",  // JP
        "2F2253ACD6BE5D87"   // Asia
    };

    // Null when exefs/main is missing or a recognized base game version, else its build ID.
    String wrongBuild() {
        File main = new File(dataDir(), "exefs/main");
        if (!main.canRead()) return null;
        byte[] id = new byte[8];
        try (java.io.RandomAccessFile f = new java.io.RandomAccessFile(main, "r")) {
            f.seek(0x40);
            f.readFully(id);
        } catch (java.io.IOException e) {
            return "?";
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : id) hex.append(String.format("%02X", b & 0xFF));
        String have = hex.toString();
        for (String k : KNOWN_BUILDS) {
            if (have.equals(k)) return null;
        }
        return have;
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        // The game's texts and subtitles (runtime language.h): its own language names, as a console lists them.
        options.add(new Option("language", "Idioma de textos y subtítulos", "Text and subtitle language",
                new String[] {"auto", "en-US", "es", "fr", "de", "it", "ja", "ko", "zh-Hans", "zh-Hant"},
                new String[] {"Automático (idioma del dispositivo)", "English", "Español", "Français", "Deutsch",
                        "Italiano", "日本語", "한국어", "简体中文", "繁體中文"},
                new String[] {"Automatic (device language)", "English", "Español", "Français", "Deutsch", "Italiano",
                        "日本語", "한국어", "简体中文", "繁體中文"}, 0));
        options.add(new Option("RESOLUTION", "Resolución", "Resolution",
                new String[] {"360p", "480p", "720p", "1080p"},
                new String[] {"360p", "480p", "720p", "1080p"},
                new String[] {"360p", "480p", "720p", "1080p"}, 2));
        options.add(new Option("NX_STRETCH", "Pantalla completa (estirar)", "Fullscreen (stretch)",
                new String[] {"0", "1"},
                new String[] {"Desactivada", "Activada"},
                new String[] {"Off", "On"}, 0));
        options.add(new Option("NX_HUD_1080", "Interfaz", "Interface", new String[] {"0", "1"},
                new String[] {"720p (más rápida)", "1080p (textos nítidos)"},
                new String[] {"720p (faster)", "1080p (sharp text)"}, 0));
        options.add(new Option("GRAPHICS", "Gráficos", "Graphics", new String[] {"low", "medium", "high"},
                new String[] {"Bajos", "Medios", "Altos"}, new String[] {"Low", "Medium", "High"}, 0));
        // The performance profile (docs/PERF-DESIGN.md §2-§3): the preset first, then the single switches it fills in.
        options.add(new Option("perf_mode", "Modo de rendimiento", "Performance mode",
                new String[] {"balance", "quality", "2k", "2k90", "ultra", "custom"},
                new String[] {"Equilibrado (predeterminado)", "Calidad 1080p60", "2K 60", "2K 90 (experimental)",
                        "Ultra 2.5K 120 (experimental)", "Personalizado"},
                new String[] {"Balanced (default)", "Quality 1080p60", "2K 60", "2K 90 (experimental)",
                        "Ultra 2.5K 120 (experimental)", "Custom"}, 0));
        // save() mirrors only 30/60 into NX_FPS_CAP and deletes it for 90/120/0: that switch can only express 30 or
        // 60 and wins over fps_mode, so writing 90 there would silently run at 60 (docs/PERF-DESIGN.md §3.2).
        options.add(new Option("fps_mode", "Fotogramas por segundo", "Frame rate",
                new String[] {"30", "60", "90", "120", "0"},
                new String[] {"30 fps", "60 fps (experimental)", "90 fps (experimental)", "120 fps (experimental)",
                        "Sin límite"},
                new String[] {"30 fps", "60 fps (experimental)", "90 fps (experimental)", "120 fps (experimental)",
                        "No limit"}, 0));
        // Over 1080 also writes allow_over_1080=1 (window_size() clamps the render scale to 1080 without it).
        options.add(new Option("render_height", "Altura de renderizado", "Render height",
                new String[] {"0", "720", "1080", "1440", "1600"},
                new String[] {"Automática (según el modo)", "720p", "1080p", "1440p (2K)", "1600p (2.5K)"},
                new String[] {"Automatic (follows the mode)", "720p", "1080p", "1440p (2K)", "1600p (2.5K)"}, 0));
        options.add(new Option("refresh_hz", "Frecuencia de actualización", "Refresh rate",
                new String[] {"0", "60", "90", "120", "144"},
                new String[] {"Seguir al sistema", "60 Hz", "90 Hz", "120 Hz", "144 Hz"},
                new String[] {"Follow the system", "60 Hz", "90 Hz", "120 Hz", "144 Hz"}, 0));
        options.add(new Option("present_mode", "Modo de presentación", "Present mode",
                new String[] {"auto", "fifo", "mailbox", "immediate"},
                new String[] {"Automático", "FIFO (vsync)", "Mailbox (baja latencia)", "Immediate (sin vsync)"},
                new String[] {"Automatic", "FIFO (vsync)", "Mailbox (low latency)", "Immediate (no vsync)"}, 0));
        // §4.5: heat is the user's main worry, so the guard is on by default and cannot be fully turned off.
        options.add(new Option("thermal_guard", "Protección térmica", "Thermal guard",
                new String[] {"1", "2", "0"},
                new String[] {"Estándar (recomendada, no se puede desactivar del todo)",
                        "Agresiva (más fría, baja antes el rendimiento)",
                        "Solo registrar (pruebas: NO protege del calor)"},
                new String[] {"Standard (recommended, cannot be fully disabled)",
                        "Aggressive (cooler, backs off sooner)",
                        "Log only (testing: does NOT protect against heat)"}, 0));
        options.add(new Option("dyn_res", "Resolución dinámica", "Dynamic resolution",
                new String[] {"1", "0"},
                new String[] {"Activada (baja la resolución si hace falta)", "Desactivada"},
                new String[] {"On (lowers the resolution when needed)", "Off"}, 0));
        options.add(new Option("dyn_res_min", "Altura mínima de renderizado", "Min render height",
                new String[] {"1080", "900", "720"},
                new String[] {"1080p", "900p", "720p"},
                new String[] {"1080p", "900p", "720p"}, 0));
        options.add(new Option("NX_SHADOWS", "Sombras", "Shadows", new String[] {"0", "1"},
                new String[] {"Desactivadas", "Activadas"}, new String[] {"Off", "On"}, 0));
        options.add(new Option("NX_SHOW_FPS", "Contador de FPS", "FPS counter", new String[] {"0", "1"},
                new String[] {"Desactivado", "Activado"}, new String[] {"Off", "On"}, 0));
        options.add(new Option("NX_TOUCH_SHOW", "Controles táctiles", "On-screen controls", new String[] {"1", "0"},
                new String[] {"Activados", "Desactivados"}, new String[] {"On", "Off"}, 0));
        options.add(new Option("NX_TOUCH_SIZE", "Tamaño de los botones", "Button size",
                new String[] {"80", "100", "125"}, new String[] {"Pequeño", "Normal", "Grande"},
                new String[] {"Small", "Normal", "Large"}, 1));
        options.add(new Option("NX_TOUCH_OPACITY", "Opacidad de los botones", "Button opacity",
                new String[] {"50", "85", "100"}, new String[] {"Baja", "Normal", "Alta"},
                new String[] {"Low", "Normal", "High"}, 1));
        options.add(new Option("NX_NO_CROWD", "Gente / Multitudes de NPCs", "Crowd NPCs",
                new String[] {"0", "1"},
                new String[] {"Activada (normal)", "Desactivada (más FPS)"},
                new String[] {"Enabled (normal)", "Disabled (more FPS)"}, 0));
        options.add(new Option("NX_DISABLE_BLOOM", "Post-procesado (bloom/brillo)", "Post-processing (bloom)",
                new String[] {"0", "1"},
                new String[] {"Activado (normal)", "Desactivado (más FPS)"},
                new String[] {"Enabled (normal)", "Disabled (more FPS)"}, 0));
        options.add(new Option("NX_GAMEPAD_LAYOUT", "Botones del mando físico", "Physical gamepad buttons",
                new String[] {"0", "1"},
                new String[] {"Nintendo (A dcha, B abajo)", "Xbox / PC (A abajo, B dcha)"},
                new String[] {"Nintendo (A right, B down)", "Xbox / PC (A down, B right)"}, 0));
        options.add(new Option("NX_STICK_SENSITIVITY", "Sensibilidad palanca táctil", "Touch stick sensitivity",
                new String[] {"50", "75", "100", "125", "150", "200"},
                new String[] {"50%", "75%", "100%", "125%", "150%", "200%"},
                new String[] {"50%", "75%", "100%", "125%", "150%", "200%"}, 2));
        options.add(new Option("NX_CAMERA_SENSITIVITY", "Sensibilidad cámara táctil", "Touch camera sensitivity",
                new String[] {"50", "75", "100", "125", "150", "200", "250"},
                new String[] {"50%", "75%", "100%", "125%", "150%", "200%", "250%"},
                new String[] {"50%", "75%", "100%", "125%", "150%", "200%", "250%"}, 2));
        build();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    void build() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(20, 4, 6));
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("PERSONA 5 ROYAL");
        title.setTextColor(Color.rgb(230, 0, 18));
        title.setTextSize(30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(220, 220, 220));
        status.setTextSize(14);
        status.setPadding(0, dp(6), 0, dp(14));
        root.addView(status);

        // One tap for the OnePlus Pad 3 Pro's recommended combination (docs/PERF-DESIGN.md §3): the whole preset at once.
        Button preset = new Button(this);
        preset.setText(t("Un toque: 2K90 (recomendado para OnePlus Pad 3 Pro)",
                "One-tap 2K90 (recommended for the OnePlus Pad 3 Pro)"));
        preset.setAllCaps(false);
        preset.setTextSize(17);
        preset.setBackgroundColor(Color.rgb(230, 0, 18));
        preset.setTextColor(Color.WHITE);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        plp.topMargin = dp(4);
        plp.bottomMargin = dp(12);
        root.addView(preset, plp);
        preset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                askPreset();
            }
        });

        addRow(root, t("Archivo del juego (.nsp / .xci)", "Game file (.nsp / .xci)"), t("Elegir", "Choose"),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        pick(PICK_GAME, "*/*", false);
                    }
                });
        addRow(root, t("Claves (prod.keys)", "Keys (prod.keys)"), t("Elegir", "Choose"), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_KEYS, "*/*", true);
            }
        });
        addRow(root, t("Icono", "Icon"), t("Elegir imagen", "Choose a picture"), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_ICON, "image/*", false);
            }
        });

        String lastSection = "";
        for (final Option o : options) {
            // A header whenever the section changes, so the list reads as 语言 / 画面 / 性能 / 操作 / 其他 instead of
            // one undifferentiated column of 25 rows.
            final String section = sectionOf(o.key);
            if (!section.equals(lastSection)) {
                lastSection = section;
                root.addView(sectionHeader(section));
            }
            final boolean advanced = ADVANCED_KEYS.contains(o.key);
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(4), 0, dp(4));
            TextView name = new TextView(this);
            name.setText(t(o.es, o.en));
            name.setTextColor(Color.WHITE);
            name.setTextSize(17);
            row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            final Button value = new Button(this);
            value.setAllCaps(false);
            value.setMinWidth(dp(180));
            row.addView(value);
            value.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    o.pick = (o.pick + 1) % o.values.length;
                    value.setText(label(o));
                    save();
                    updateFpsWarning();
                }
            });
            value.setTag(o);
            root.addView(row);
            if (advanced) {
                advancedViews.add(row);
                row.setVisibility(View.GONE);  // revealed by the toggle below
            }
            // Above 60 the game's logic steps faster than the engine was tuned for: say so under the frame rate row
            // (docs/PERF-DESIGN.md §1.1, §5). Shown or hidden by refresh() and by the button itself.
            if ("fps_mode".equals(o.key)) {
                fpsWarning = new TextView(this);
                fpsWarning.setTextColor(Color.rgb(255, 176, 0));
                fpsWarning.setTextSize(13);
                fpsWarning.setPadding(0, 0, 0, dp(8));
                root.addView(fpsWarning);
            }
        }

        // What the runtime will actually do with the current settings, plus the expert switches behind a toggle.
        perfSummary = new TextView(this);
        perfSummary.setTextColor(Color.rgb(140, 220, 140));
        perfSummary.setTextSize(14);
        perfSummary.setPadding(0, dp(6), 0, dp(2));
        root.addView(perfSummary);
        LinearLayout advRow = new LinearLayout(this);
        advRow.setOrientation(LinearLayout.HORIZONTAL);
        advRow.setGravity(Gravity.CENTER_VERTICAL);
        advRow.setPadding(0, dp(2), 0, dp(8));
        TextView advName = new TextView(this);
        advName.setText(t("Ajustes avanzados de rendimiento", "Advanced performance settings"));
        advName.setTextColor(Color.WHITE);
        advName.setTextSize(17);
        advRow.addView(advName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        final Button advButton = new Button(this);
        advButton.setAllCaps(false);
        advButton.setMinWidth(dp(180));
        advButton.setText(t("Mostrar", "Show"));
        advRow.addView(advButton);
        advButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showAdvanced = !showAdvanced;
                for (View view : advancedViews) view.setVisibility(showAdvanced ? View.VISIBLE : View.GONE);
                advButton.setText(showAdvanced ? t("Ocultar", "Hide") : t("Mostrar", "Show"));
            }
        });
        root.addView(advRow);

        LinearLayout edit = new LinearLayout(this);
        edit.setOrientation(LinearLayout.HORIZONTAL);
        edit.setGravity(Gravity.CENTER_VERTICAL);
        edit.setPadding(0, dp(4), 0, dp(4));
        TextView editName = new TextView(this);
        editName.setText(t("Posición de los controles", "Controls position"));
        editName.setTextColor(Color.WHITE);
        editName.setTextSize(17);
        edit.addView(editName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button editButton = new Button(this);
        editButton.setAllCaps(false);
        editButton.setMinWidth(dp(180));
        editButton.setText(t("Mover", "Move"));
        edit.addView(editButton);
        editButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasAccess()) {
                    askAccess();
                    return;
                }
                save();
                startActivity(new Intent(Launcher.this, ControlsEditor.class));
            }
        });
        root.addView(edit);

        // Saves: in from a ZIP or a folder (a Switch backup or another player's), out to a ZIP in Download.
        LinearLayout saves = new LinearLayout(this);
        saves.setOrientation(LinearLayout.HORIZONTAL);
        saves.setGravity(Gravity.CENTER_VERTICAL);
        saves.setPadding(0, dp(4), 0, dp(4));
        TextView savesName = new TextView(this);
        savesName.setText(t("Partidas guardadas", "Saves"));
        savesName.setTextColor(Color.WHITE);
        savesName.setTextSize(17);
        saves.addView(savesName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button importButton = new Button(this);
        importButton.setAllCaps(false);
        importButton.setMinWidth(dp(88));
        importButton.setText(t("Importar", "Import"));
        saves.addView(importButton);
        Button exportButton = new Button(this);
        exportButton.setAllCaps(false);
        exportButton.setMinWidth(dp(88));
        exportButton.setText(t("Exportar", "Export"));
        saves.addView(exportButton);
        importButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasAccess()) {
                    askAccess();
                    return;
                }
                askImport();
            }
        });
        exportButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasAccess()) {
                    askAccess();
                    return;
                }
                exportSaves();
            }
        });
        root.addView(saves);

        // Translation CPK import
        LinearLayout transRow = new LinearLayout(this);
        transRow.setOrientation(LinearLayout.HORIZONTAL);
        transRow.setGravity(Gravity.CENTER_VERTICAL);
        transRow.setPadding(0, dp(4), 0, dp(4));
        TextView transName = new TextView(this);
        transName.setText(t("Traducción / Paquete (.cpk)", "Translation / CPK mod"));
        transName.setTextColor(Color.WHITE);
        transName.setTextSize(17);
        transRow.addView(transName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button transButton = new Button(this);
        transButton.setAllCaps(false);
        transButton.setMinWidth(dp(180));
        transButton.setText(t("Importar CPK", "Import CPK"));
        transRow.addView(transButton);
        transButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasAccess()) {
                    askAccess();
                    return;
                }
                pick(PICK_CPK, "*/*", false);
            }
        });
        root.addView(transRow);

        // Custom GPU driver (e.g. Mesa Turnip for Adreno): imported into the app's private files, opened by the runtime
        LinearLayout drvRow = new LinearLayout(this);
        drvRow.setOrientation(LinearLayout.HORIZONTAL);
        drvRow.setGravity(Gravity.CENTER_VERTICAL);
        drvRow.setPadding(0, dp(4), 0, dp(4));
        driverName = new TextView(this);
        driverName.setTextColor(Color.WHITE);
        driverName.setTextSize(17);
        drvRow.addView(driverName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button drvImport = new Button(this);
        drvImport.setAllCaps(false);
        drvImport.setMinWidth(dp(88));
        drvImport.setText(t("Importar", "Import"));
        drvRow.addView(drvImport);
        Button drvRemove = new Button(this);
        drvRemove.setAllCaps(false);
        drvRemove.setMinWidth(dp(88));
        drvRemove.setText(t("Quitar", "Remove"));
        drvRow.addView(drvRemove);
        drvImport.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_DRIVER, "*/*", false);
            }
        });
        drvRemove.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                removeDriver();
            }
        });
        root.addView(drvRow);
        showDriver();

        // Debug logs export
        LinearLayout logsRow = new LinearLayout(this);
        logsRow.setOrientation(LinearLayout.HORIZONTAL);
        logsRow.setGravity(Gravity.CENTER_VERTICAL);
        logsRow.setPadding(0, dp(4), 0, dp(4));
        TextView logsName = new TextView(this);
        logsName.setText(t("Registros de depuración", "Debug logs"));
        logsName.setTextColor(Color.WHITE);
        logsName.setTextSize(17);
        logsRow.addView(logsName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button logsButton = new Button(this);
        logsButton.setAllCaps(false);
        logsButton.setMinWidth(dp(180));
        logsButton.setText(t("Exportar registros", "Export logs"));
        logsRow.addView(logsButton);
        logsButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                exportLogs();
            }
        });
        root.addView(logsRow);

        play = new Button(this);
        play.setTextSize(20);
        play.setAllCaps(false);
        play.setBackgroundColor(Color.rgb(230, 0, 18));
        play.setTextColor(Color.WHITE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(64));
        lp.topMargin = dp(18);
        root.addView(play, lp);
        play.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasAccess()) {
                    askAccess();
                    return;
                }
                if (gameUri() != null) {  // the game file the player picked
                    if (!gameReadable()) pick(PICK_GAME, "*/*", false);
                    else if (!hasKeys()) pick(PICK_KEYS, "*/*", true);
                    else if (!prepared()) prepare();
                    else if (openGameForRuntime()) {
                        save();
                        startActivity(new Intent(Launcher.this, android.app.NativeActivity.class));
                    } else {
                        tell(t("No se pudo abrir el juego", "The game could not be opened"), gameName());
                    }
                    return;
                }
                if (!hasData()) {
                    pick(PICK_GAME, "*/*", false);
                    return;
                }
                if (wrongBuild() != null) {
                    refresh();
                    return;
                }
                save();
                startActivity(new Intent(Launcher.this, android.app.NativeActivity.class));
            }
        });

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    void refresh() {
        load();
        updatePerfSummary();
        for (Option o : options) {
            String v = config.get(o.key);
            if ("RESOLUTION".equals(o.key) && v == null) {
                String hh = config.get("NX_HANDHELD");
                if ("0".equals(hh)) v = "1080p";
                else {
                    String r = config.get("resolution");
                    if (r != null && r.contains("360")) v = "360p";
                    else if (r != null && r.contains("480")) v = "480p";
                    else v = "720p";
                }
            }
            // A config.txt from the upstream launcher has NX_FPS_CAP but no fps_mode: migrate it (§3.2).
            // NX_FPS_CAP only ever meant 30 or 60, so the mapping is exact; anything else falls back to the 30 default.
            if ("fps_mode".equals(o.key) && v == null) {
                String cap = config.get("NX_FPS_CAP");
                if ("60".equals(cap)) v = "60";
                else if ("30".equals(cap)) v = "30";
            }
            for (int k = 0; v != null && k < o.values.length; k++)
                if (o.values[k].equals(v)) o.pick = k;
        }
        updateButtons(getWindow().getDecorView());
        updateFpsWarning();
        if (!hasAccess()) {
            status.setText(t("Hace falta el permiso de acceso a todos los archivos para leer los datos del juego.",
                    "All-files access is needed to read the game data."));
            play.setText(t("Dar permiso", "Grant access"));
        } else if (gameUri() != null && !gameReadable()) {
            status.setText(Lang.f("Ya no se puede leer el archivo del juego (%s): vuelve a elegirlo.",
                    "The game file can no longer be read (%s): choose it again.", gameName()));
            play.setText(t("Elegir el juego", "Choose the game"));
        } else if (gameUri() != null && !hasKeys()) {
            status.setText(t("Falta tu archivo prod.keys.", "Your prod.keys file is missing."));
            play.setText(t("Elegir prod.keys", "Choose prod.keys"));
        } else if (gameUri() != null && !prepared()) {
            status.setText(Lang.f("Juego: %s. Falta prepararlo: solo una vez, unos minutos.",
                    "Game: %s. It needs preparing: once, a few minutes.", gameName()));
            play.setText(t("Preparar el juego", "Prepare the game"));
        } else if (gameUri() != null) {
            status.setText(Lang.f("Juego listo: %s", "Game ready: %s", gameName()));
            play.setText(t("Jugar", "Play"));
        } else if (!hasData()) {
            status.setText(t("Elige el archivo de tu juego (.nsp o .xci de Persona 5 Royal 1.0.0) y tu prod.keys.",
                    "Choose your game file (a Persona 5 Royal 1.0.0 .nsp or .xci) and your prod.keys."));
            play.setText(t("Elegir el juego", "Choose the game"));
        } else if (wrongBuild() != null) {
            status.setText(Lang.f("Almacenamiento interno/%s/exefs/main no es Persona 5 Royal 1.0.0 (build %s). Copia el "
                            + "exefs del juego base, sin la actualización.",
                    "Internal storage/%s/exefs/main is not Persona 5 Royal 1.0.0 (build %s). Copy the exefs of the base "
                            + "game, without the update.", DATA, wrongBuild()));
            play.setText(t("Volver a buscar", "Look again"));
        } else {
            status.setText(Lang.f("Datos listos: %s", "Data ready: %s", dataDir().getPath()));
            play.setText(t("Jugar", "Play"));
        }
    }

    void updateButtons(View v) {
        if (v instanceof Button && v.getTag() instanceof Option) {
            Option o = (Option) v.getTag();
            ((Button) v).setText(label((Option) v.getTag()));
        } else if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int k = 0; k < g.getChildCount(); k++) updateButtons(g.getChildAt(k));
        }
    }

    void tell(String title, String message) {
        new AlertDialog.Builder(this).setTitle(title).setMessage(message).setPositiveButton("OK", null).show();
    }

    // ---- performance profile (docs/PERF-DESIGN.md §2-§3) ---------------------------------------------------------

    // The one-tap preset: the whole recommended combination for the OnePlus Pad 3 Pro, after a confirmation that says
    // what it does and that it is experimental (90 fps changes the game's speed if it cannot be held).
    void askPreset() {
        if (!hasAccess()) {
            askAccess();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle(t("Un toque: 2K90", "One-tap 2K90"))
                .setMessage(t("Se escribirán estos ajustes para el OnePlus Pad 3 Pro:\n\n"
                                + "• Modo de rendimiento: 2K 90\n"
                                + "• Altura de renderizado: 1440p (2K), con allow_over_1080=1\n"
                                + "• Fotogramas por segundo: 90\n"
                                + "• Frecuencia de actualización: 90 Hz\n"
                                + "• Modo de presentación: mailbox\n"
                                + "• Protección térmica: estándar, con resolución dinámica (mínimo 1080p)\n\n"
                                + "Es EXPERIMENTAL: a 90 fps el juego avanza más rápido que su ritmo original, y si no se "
                                + "sostienen los 90 fps se verá a cámara lenta. Vuelve a Modo de rendimiento → "
                                + "Equilibrado si algo va mal.",
                        "These settings will be written for the OnePlus Pad 3 Pro:\n\n"
                                + "• Performance mode: 2K 90\n"
                                + "• Render height: 1440p (2K), with allow_over_1080=1\n"
                                + "• Frame rate: 90\n"
                                + "• Refresh rate: 90 Hz\n"
                                + "• Present mode: mailbox\n"
                                + "• Thermal guard: standard, with dynamic resolution (1080p minimum)\n\n"
                                + "It is EXPERIMENTAL: at 90 fps the game runs faster than its original pace, and if "
                                + "90 fps cannot be held it will look slow. Go back to Performance mode → Balanced if "
                                + "anything goes wrong."))
                .setPositiveButton(t("Aplicar", "Apply"), new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        applyPreset();
                    }
                })
                .setNegativeButton(t("Cancelar", "Cancel"), null)
                .show();
    }

    // The 2K90 combination of §3, written as single switches so the runtime's precedence (single > preset > upstream)
    // is the same whether the player got here by the button or by hand.
    void applyPreset() {
        if (!hasAccess()) return;
        load();
        config.put("perf_mode", "2k90");
        config.put("render_height", "1440");
        config.put("allow_over_1080", "1");
        config.put("fps_mode", "90");
        // NOT NX_FPS_CAP: it only expresses 30 or 60 and wins over fps_mode, so NX_FPS_CAP=90 would run at 60 while
        // the UI said 90 (§3.2). Removed here too, not just left unwritten: load() above keeps a stale one from an
        // earlier config, and this method writes the file directly instead of going through save().
        config.remove("NX_FPS_CAP");
        config.put("refresh_hz", "90");
        config.put("present_mode", "mailbox");
        // A hot tablet degrades resolution instead of stuttering (docs/PERF-DESIGN.md §4.5).
        config.put("thermal_guard", "1");
        config.put("dyn_res", "1");
        config.put("dyn_res_min", "1080");
        // The game's own render size (svc_am.cpp GetDefaultDisplayResolution) and the window: 2560x1440.
        config.put("NX_RENDER_RES", "2560x1440");
        config.put("resolution", "2560x1440");
        config.put("NX_RES", "2560x1440");
        config.put("NX_HANDHELD", "0");
        writeConfig(new File(dataDir(), "config.txt"), config);
        refresh();
        Toast.makeText(this, t("Ajustes 2K90 aplicados", "2K90 settings applied"), Toast.LENGTH_LONG).show();
    }

    // Above 60 fps the game's logic steps faster than its engine was tuned for (§1.1): warn, don't forbid.
    // Also says what the runtime will do to the panel (§3.1): 90/120 with "follow the system" derives refresh_hz from
    // fps, because the present interval is a whole number of vsyncs and a 60 Hz panel could not present 90 at all.
    void updateFpsWarning() {
        if (fpsWarning == null) return;
        Option fps = option("fps_mode");
        String v = fps == null ? null : fps.values[fps.pick];
        boolean experimental = "90".equals(v) || "120".equals(v);
        fpsWarning.setVisibility(experimental ? View.VISIBLE : View.GONE);
        if (!experimental) return;
        Option refresh = option("refresh_hz");
        String hz = refresh == null ? null : refresh.values[refresh.pick];
        String text = Lang.f(
                "A %s fps el juego puede ir a velocidad equivocada (experimental). Si no se sostienen, irá a cámara lenta.",
                "At %s fps the game may run at the wrong speed (experimental). If it cannot hold them, it slows down.", v);
        if (hz == null || "0".equals(hz)) {
            text += " " + Lang.f(
                    "Con «seguir al sistema» la pantalla se pedirá a %s Hz automáticamente.",
                    "With \"follow the system\" the panel is requested at %s Hz automatically.", v);
        }
        fpsWarning.setText(text);
    }

    Option option(String key) {
        for (Option o : options) if (o.key.equals(key)) return o;
        return null;
    }

    void askImport() {
        new AlertDialog.Builder(this)
                .setTitle(t("Importar partidas", "Import saves"))
                .setMessage(t("Elige un archivo ZIP o una carpeta con partidas de Persona 5 Royal (las carpetas DATA01, "
                                + "DATA02... y SYSTEM, como las exporta JKSV o Checkpoint, o el ZIP que exporta este botón). "
                                + "Antes se guarda una copia de tus partidas actuales.",
                        "Pick a ZIP file or a folder with Persona 5 Royal saves (the DATA01, DATA02... and SYSTEM folders, "
                                + "as JKSV or Checkpoint export them, or the ZIP the Export button makes). Your current saves "
                                + "are copied aside first."))
                .setPositiveButton(t("Archivo ZIP", "ZIP file"), new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                        i.addCategory(Intent.CATEGORY_OPENABLE);
                        i.setType("*/*");
                        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip",
                                "application/x-zip-compressed", "application/octet-stream"});
                        startActivityForResult(i, PICK_ZIP);
                    }
                })
                .setNeutralButton(t("Carpeta", "Folder"), new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
                    }
                })
                .setNegativeButton(t("Cancelar", "Cancel"), null)
                .show();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PREPARE) {
            refresh();
            return;
        }
        if (result != RESULT_OK || data == null) return;
        if (request == PICK_KEYS) {
            onKeysPicked(data);
            refresh();
            return;
        }
        if (data.getData() == null) return;
        if (request == PICK_GAME) {
            onGamePicked(data.getData());
            refresh();
            return;
        }
        if (request == PICK_ICON) {
            onIconPicked(data.getData());
            return;
        }
        if (request == PICK_DRIVER) {
            importDriver(data.getData());
            return;
        }
        if (request == PICK_CPK) {
            importCpk(data.getData());
            return;
        }
        try {
            SaveFiles.Result r = request == PICK_ZIP ? SaveFiles.importZip(getContentResolver(), data.getData())
                    : request == PICK_FOLDER ? SaveFiles.importTree(getContentResolver(), data.getData()) : null;
            if (r == null) {
                tell(t("No hay partidas", "No saves found"),
                        t("Ahí no encontré partidas de Persona 5 Royal: busca las carpetas DATA01, DATA02... o SYSTEM.",
                                "No Persona 5 Royal saves there: look for the DATA01, DATA02... or SYSTEM folders."));
                return;
            }
            String slots = r.slots.isEmpty() ? t("solo ajustes del sistema", "system settings only")
                    : Lang.f("ranuras %s", "slots %s", android.text.TextUtils.join(", ", r.slots));
            String kept = r.backup == null ? "" : Lang.f("\n\nTus partidas anteriores quedaron en user/%s.",
                    "\n\nYour previous saves are in user/%s.", r.backup);
            tell(t("Partidas importadas", "Saves imported"),
                    Lang.f("%d archivos (%s). Ábrelas desde Cargar partida.", "%d files (%s). Open them from Load Game.",
                            r.files, slots) + kept);
        } catch (Exception e) {
            tell(t("No se pudo importar", "Import failed"), String.valueOf(e.getMessage()));
        }
    }

    void exportSaves() {
        try {
            java.io.File out = SaveFiles.export();
            // Listed at once by the file pickers and the Files app (written straight to disk, it would wait for a scan).
            if (out != null) android.media.MediaScannerConnection.scanFile(this, new String[] {out.getPath()}, null, null);
            if (out == null) tell(t("No hay partidas", "No saves"), t("Todavía no hay partidas guardadas.", "There are no saves yet."));
            else tell(t("Partidas exportadas", "Saves exported"),
                    Lang.f("Guardadas en Descargas/%s. Se importan tal cual con el botón Importar.",
                            "Saved to Download/%s. The Import button takes it back as is.", out.getName()));
        } catch (Exception e) {
            tell(t("No se pudo exportar", "Export failed"), String.valueOf(e.getMessage()));
        }
    }

    void askAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    // config.txt: KEY=value lines, in order.
    static Map<String, String> readConfig(File f) {
        Map<String, String> out = new LinkedHashMap<>();
        if (!f.canRead()) return out;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String line; (line = r.readLine()) != null; ) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (eq > 0) out.put(line.substring(0, eq), line.substring(eq + 1));
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    static boolean writeConfig(File f, Map<String, String> config) {
        try (FileWriter w = new FileWriter(f)) {
            for (Map.Entry<String, String> e : config.entrySet()) w.write(e.getKey() + "=" + e.getValue() + "\n");
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    void load() {
        config.clear();
        config.putAll(readConfig(new File(dataDir(), "config.txt")));
    }

    void save() {
        if (!hasAccess()) return;
        dataDir().mkdirs();
        if (!dataDir().isDirectory()) return;
        load();
        for (Option o : options) config.put(o.key, o.values[o.pick]);
        final String res = config.get("RESOLUTION");
        if ("360p".equals(res)) {
            config.put("NX_HANDHELD", "1");
            config.put("resolution", "360p");
            config.put("NX_RENDER_RES", "360p");
            config.put("NX_RES", "360p");
        } else if ("480p".equals(res)) {
            config.put("NX_HANDHELD", "1");
            config.put("resolution", "480p");
            config.put("NX_RENDER_RES", "480p");
            config.put("NX_RES", "480p");
        } else if ("1080p".equals(res)) {
            config.put("NX_HANDHELD", "0");
            config.put("resolution", "1080p");
            config.put("NX_RENDER_RES", "1080p");
            config.put("NX_RES", "1080p");
        } else {
            config.put("NX_HANDHELD", "1");
            config.put("resolution", "720p");
            config.put("NX_RENDER_RES", "720p");
            config.put("NX_RES", "720p");
        }
        for (String k : new String[] {"RENDER_SCALE", "NX_ANISO_MAX", "NX_LOD_BIAS"}) config.remove(k);
        final String preset = config.get("GRAPHICS");
        if ("low".equals(preset)) config.put("NX_LOD_BIAS", "1");
        else if ("medium".equals(preset)) config.put("NX_LOD_BIAS", "0.5");

        // ---- performance profile (docs/PERF-DESIGN.md §2-§3) ----
        // The option rows above already put perf_mode / fps_mode / render_height / refresh_hz / present_mode /
        // thermal_guard / dyn_res / dyn_res_min in as the player picked them. What follows keeps the older switches and
        // the derived ones in step, so the runtime and games/p5r/game.cpp read one consistent set.

        // games/p5r/game.cpp::pace() still reads NX_FPS_CAP, and it WINS over fps_mode with upstream's meaning of
        // ">= 60 ? 60 : 30": it can only express 30 or 60 (docs/PERF-DESIGN.md §3.2). Writing NX_FPS_CAP=90 there
        // would collapse to 60, so the UI would say 90 while the game ran at 60 -- the worst kind of bug, because it
        // looks like it worked. So it is written only for the two values it can express and deleted otherwise; 90,
        // 120 and "no limit" travel through fps_mode alone.
        final String fps = config.get("fps_mode");
        if ("30".equals(fps) || "60".equals(fps)) config.put("NX_FPS_CAP", fps);
        else config.remove("NX_FPS_CAP");

        // Over 1080 the render scale is clamped to 1080 unless allow_over_1080 is set (platform_android.cpp
        // window_size()); the 2k / 2k90 / ultra presets imply it. An explicit render_height wins over the preset
        // (§3: single switch > preset), so choosing 720/1080 under a 2K preset does not leave the clamp lifted.
        // Absent otherwise, so balance stays upstream.
        final String height = config.get("render_height");
        final String mode = config.get("perf_mode");
        boolean explicitHeight = height != null && !"0".equals(height);
        boolean over1080 = explicitHeight
                ? "1440".equals(height) || "1600".equals(height)
                : "2k".equals(mode) || "2k90".equals(mode) || "ultra".equals(mode);
        if (over1080) config.put("allow_over_1080", "1");
        else config.remove("allow_over_1080");

        // The exact render size. render_height "0" (automatic) takes the height its perf_mode asks for (§3): otherwise
        // the resolution line left by the RESOLUTION option above would win over the preset, because window_size()
        // reads resolution before the profile's render_height (platform_android.cpp). balance/custom ask for none, so
        // the resolution option stands and the upstream render scale decides.
        // NX_RENDER_RES is what the game sizes its own render targets from (svc_am.cpp GetDefaultDisplayResolution);
        // NX_RES and resolution size the window (platform_android.cpp window_size()).
        String effective = height != null && !"0".equals(height) ? height : presetHeight(mode);
        if (effective != null) {
            String wh = renderSize(effective);
            config.put("resolution", wh);
            config.put("NX_RES", wh);
            config.put("NX_RENDER_RES", wh);
            config.put("NX_HANDHELD", "0");
        }

        writeConfig(new File(dataDir(), "config.txt"), config);
        updatePerfSummary();
    }

    // The render height a perf_mode implies when render_height is left on automatic (docs/PERF-DESIGN.md §3).
    // Null for balance (upstream: the GPU's render scale) and custom (the single switches decide).
    static String presetHeight(String mode) {
        if ("quality".equals(mode)) return "1080";
        if ("2k".equals(mode) || "2k90".equals(mode)) return "1440";
        if ("ultra".equals(mode)) return "1600";
        return null;
    }

    // The window/render size for a render_height, in the runtime's WxH form. The screen's own aspect is not known
    // here; window_size() takes only the height and scales the width to the display, so the width matters only for
    // NX_RENDER_RES (the game's own render targets, svc_am.cpp). 1600 is 2.5K's usual 2560x1600.
    static String renderSize(String height) {
        if ("720".equals(height)) return "1280x720";
        if ("1080".equals(height)) return "1920x1080";
        if ("1440".equals(height)) return "2560x1440";
        if ("1600".equals(height)) return "2560x1600";
        return "";
    }

    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    // ---- custom GPU driver ------------------------------------------------------------------------------------
    // <files>/gpu_driver/<name>.so and active.txt (its name): the runtime opens it with libadrenotools instead of the
    // system Vulkan driver (platform_android.cpp). A .so, or a .zip holding one (the AdrenoTools driver packages:
    // the .so and a meta.json whose "libraryName" is the file to load).
    TextView driverName;

    File driverDir() {
        File d = new File(getFilesDir(), "gpu_driver");
        if (!d.exists()) d.mkdirs();
        return d;
    }

    String activeDriver() {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new FileReader(new File(driverDir(), "active.txt")))) {
            String line = r.readLine();
            return line == null ? "" : line.trim();
        } catch (Exception e) {
            return "";
        }
    }

    void showDriver() {
        if (driverName == null) return;
        String a = activeDriver();
        driverName.setText(a.isEmpty() ? t("Driver de GPU: del sistema", "GPU driver: system")
                : Lang.f("Driver de GPU: %s", "GPU driver: %s", a));
    }

    void removeDriver() {
        File[] files = driverDir().listFiles();
        if (files != null) for (File f : files) f.delete();
        showDriver();
        Toast.makeText(this, t("Driver del sistema activado", "System driver restored"), Toast.LENGTH_LONG).show();
    }

    static void copy(InputStream in, File dest) throws java.io.IOException {
        try (OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    void importDriver(Uri uri) {
        try {
            String picked = displayName(uri);
            File dir = driverDir();
            File[] old = dir.listFiles();
            if (old != null) for (File f : old) f.delete();
            String libName = null;
            if (picked.toLowerCase(Locale.ROOT).endsWith(".zip")) {
                String soEntry = null, wanted = null;
                try (java.util.zip.ZipInputStream zin = new java.util.zip.ZipInputStream(getContentResolver().openInputStream(uri))) {
                    java.util.zip.ZipEntry e;
                    while ((e = zin.getNextEntry()) != null) {
                        String base = new File(e.getName()).getName();
                        if (e.isDirectory()) continue;
                        if (base.equals("meta.json")) {
                            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                            byte[] buf = new byte[8192];
                            int n;
                            while ((n = zin.read(buf)) > 0) bo.write(buf, 0, n);
                            java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"]+)\"")
                                    .matcher(bo.toString("UTF-8"));
                            if (m.find()) wanted = m.group(1);
                        } else if (base.endsWith(".so")) {
                            copy(zin, new File(dir, base));
                            if (soEntry == null) soEntry = base;
                        }
                    }
                }
                libName = wanted != null && new File(dir, wanted).exists() ? wanted : soEntry;
            } else {
                libName = picked.endsWith(".so") ? picked : picked + ".so";
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    if (in == null) throw new java.io.IOException("Cannot open selected file");
                    copy(in, new File(dir, libName));
                }
            }
            if (libName == null) throw new java.io.IOException(t("No hay ningun .so dentro", "No .so file inside"));
            try (FileWriter w = new FileWriter(new File(dir, "active.txt"))) {
                w.write(libName + "\n");
            }
            showDriver();
            tell(t("Driver importado", "Driver imported"),
                    Lang.f("%s\n\nCierra el juego por completo y abrelo otra vez. Para volver al driver del sistema pulsa Quitar.",
                            "%s\n\nClose the game completely and start it again. To go back to the system driver press Remove.",
                            libName));
        } catch (Exception e) {
            tell(t("No se pudo importar el driver", "Driver import failed"), String.valueOf(e.getMessage()));
        }
    }

    void importCpk(Uri uri) {
        try {
            File cpkDir = new File(dataDir(), "mods" + File.separator + "translation" + File.separator + "romfs" + File.separator + "CPK");
            if (!cpkDir.exists()) cpkDir.mkdirs();
            File dest = new File(cpkDir, "PATCH1.CPK");
            try (InputStream in = getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(dest)) {
                if (in == null) throw new java.io.IOException("Cannot open selected file");
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            Toast.makeText(this, t("Traducción instalada en mods/translation/ (PATCH1.CPK)",
                    "Translation installed to mods/translation/ (as PATCH1.CPK)"), Toast.LENGTH_LONG).show();
        } catch (Exception e) {
            Toast.makeText(this, t("Error al importar CPK: " + e.getMessage(),
                    "Error importing CPK: " + e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    void exportLogs() {
        try {
            File dl = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
            if (!dl.exists()) dl.mkdirs();
            File out = new File(dl, "p5r-log.txt");
            StringBuilder sb = new StringBuilder();
            sb.append("=== Persona 5 Royal Recomp Log Export ===\n");
            sb.append("Device: ").append(Build.MANUFACTURER).append(" ").append(Build.MODEL).append("\n");
            sb.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
            sb.append("Date: ").append(new java.util.Date().toString()).append("\n\n");

            try {
                Process p = Runtime.getRuntime().exec(new String[] {"logcat", "-d", "-t", "500"});
                try (BufferedReader br = new BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line).append("\n");
                }
            } catch (Exception e) {
                sb.append("logcat capture error: ").append(e.getMessage()).append("\n");
            }

            File nxLog = new File(dataDir(), "nx.log");
            if (nxLog.exists()) {
                sb.append("\n=== nx.log ===\n");
                try (BufferedReader br = new BufferedReader(new FileReader(nxLog))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line).append("\n");
                }
            }

            File nxLogPrev = new File(dataDir(), "nx.log.prev");
            if (nxLogPrev.exists()) {
                sb.append("\n=== nx.log.prev ===\n");
                try (BufferedReader br = new BufferedReader(new FileReader(nxLogPrev))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line).append("\n");
                }
            }

            try (FileWriter fw = new FileWriter(out)) {
                fw.write(sb.toString());
            }

            Toast.makeText(this, t("Registros exportados a Download/p5r-log.txt",
                    "Logs exported to Download/p5r-log.txt"), Toast.LENGTH_SHORT).show();

            Uri logUri = Uri.parse("content://" + getPackageName() + ".logprovider" + out.getAbsolutePath());
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("text/plain");
            share.putExtra(Intent.EXTRA_STREAM, logUri);
            share.putExtra(Intent.EXTRA_SUBJECT, "Persona 5 Royal Debug Log");
            share.setClipData(android.content.ClipData.newRawUri("Log", logUri));
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, t("Compartir archivo de registros", "Share log file")));
        } catch (Exception e) {
            Toast.makeText(this, t("Error al exportar registros: " + e.getMessage(),
                    "Error exporting logs: " + e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    void addRow(LinearLayout root, String label, String button, View.OnClickListener click) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(4), 0, dp(4));
        TextView name = new TextView(this);
        name.setText(label);
        name.setTextColor(Color.WHITE);
        name.setTextSize(17);
        row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setMinWidth(dp(180));
        b.setText(button);
        b.setOnClickListener(click);
        row.addView(b);
        root.addView(row);
    }
}
