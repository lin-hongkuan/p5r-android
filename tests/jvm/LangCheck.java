// A guard for the launcher's language table. Lang.put() throws IllegalArgumentException when a row does not carry
// exactly one translation per Lang.COLUMNS entry, and that throw happens in a static initialiser: the app then dies
// with ExceptionInInitializerError the moment the launcher opens, before any screen is drawn. javac cannot see it --
// the argument counts are perfectly legal Java -- so it has to be RUN. This is that run.
//
// The build caught neither the compile nor the d8 step: an APK with a broken table builds, signs and installs fine.
// Wired into .github/workflows/build-apk.yml.
package io.github.p5rrecomp;

public final class LangCheck {
    public static void main(String[] args) {
        // Every English key the launcher asks for through t(es, en): if a row exists it must be well formed, and a
        // missing row is not an error (Lang.t falls back to the English string).
        String[] keys = {
            "Text", "Picture", "Performance", "Picture: extras", "On-screen controls", "Rendering cost",
            "Gamepad and sensitivity", "Advanced performance settings", "Show", "Hide", "In game: ",
            "Performance mode", "Frame rate", "Resolution", "Interface", "Graphics", "Shadows", "FPS counter",
            "On-screen controls", "Button size", "Button opacity", "Crowd NPCs", "Post-processing (bloom)",
            "Physical gamepad buttons", "Touch stick sensitivity", "Touch camera sensitivity",
            "Render height", "Refresh rate", "Present mode", "Thermal guard", "Dynamic resolution",
            "Min render height", "Text and subtitle language", "Automatic (device language)",
        };
        int checked = 0;
        for (String k : keys) {
            String v = Lang.t(k, k);
            if (v == null) throw new IllegalStateException("Lang.t returned null for: " + k);
            checked++;
        }
        // The table is package-private; the static initialiser above has already run by now, which is the real test.
        System.out.println("LangCheck: " + checked + " keys, static initialiser ran with no exception");
    }
}
