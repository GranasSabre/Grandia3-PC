// Language (setting "language", English by default): the game's own text stays English unless lang\<code>.tsv
// and its companions translate it (text_hook.cpp, sys_text.cpp, gs_texrep.cpp); the port's interface is
// German with "de" and English otherwise. The interface texts are written in German in the code;
// tr() returns their English version.
#include "platform.h"
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace platform {

static const wchar_t* const TEXTS[][2] = {
    // in-game menu (overlay.cpp)
    {L"Einstellungen", L"Settings"},
    {L"An", L"On"},
    {L"Aus", L"Off"},
    {L"Grafik", L"Graphics"},
    {L"Menüs/Texte im 4:3-Format", L"Menus/text in 4:3 format"},
    {L"Große Dialogfenster", L"Large dialogue windows"},
    {L"Untertitel in Zwischensequenzen", L"Subtitles in cutscenes"},
    {L"braucht Widescreen", L"needs widescreen"},
    {L"Auto (an: Steam Deck)", L"Auto (on: Steam Deck)"},
    {L"Auto (aus)", L"Auto (off)"},
    {L"Kinobalken entfernen (experimentell)", L"Remove cinema bars (experimental)"},
    {L"HD-Texturen", L"HD textures"},
    {L"Scharfe Schrift", L"Sharp text"},
    {L"60 fps (experimentell)", L"60 fps (experimental)"},
    {L"60 fps auch in Kämpfen (experimentell)", L"60 fps in battles too (experimental)"},
    {L"Zwischensequenzen mit START überspringen", L"Skip cutscenes with START"},
    {L"Filme mit START überspringen", L"Skip movies with START"},
    {L"Nachschärfen", L"Sharpening"},
    {L"Bildfilter", L"Image filter"},
    {L"Weich", L"Smooth"},
    {L"Scharf-weich", L"Sharp-smooth"},
    {L"Seitenverhältnis", L"Aspect ratio"},
    {L"Fenster füllen", L"Fill window"},
    {L"Vollbild", L"Fullscreen"},
    {L"Bildrate anzeigen", L"Show frame rate"},
    {L"Interne Auflösung (ab Neustart)", L"Internal resolution (after restart)"},
    {L"Automatisch", L"Automatic"},
    {L"Lautstärke gesamt", L"Master volume"},
    {L"Musik", L"Music"},
    {L"Sprache", L"Voice"},
    {L"Effekte", L"Effects"},
    {L"Videos", L"Movies"},
    {L"Sprache (Language)", L"Language"},
    {L"Die Spieltexte wechseln beim nächsten Start.", L"The game text changes at the next start."},
    {L"Disc wechseln (Neustart)", L"Change disc (restart)"},
    {L"Nochmal bestätigen!", L"Confirm again!"},
    {L"Nicht gespeicherter Fortschritt geht verloren.", L"Unsaved progress will be lost."},
    {L"Savestates (Spielstand an jeder Stelle)", L"Savestates (save anywhere)"},
    {L"  (leer)", L"  (empty)"},
    {L"Savestate speichern (F5, Back+R1)", L"Save savestate (F5, Back+R1)"},
    {L"speichern", L"save"},
    {L"Savestate laden (F9, Back+L1)", L"Load savestate (F9, Back+L1)"},
    {L"laden", L"load"},
    {L"Tasten-/Controllerbelegung …", L"Keyboard/controller mapping …"},
    {L"öffnen", L"open"},
    {L"Zurück zum Spiel", L"Back to the game"},
    {L"↑↓ Auswahl    ←→ ändern    Kreuz/Enter: auswählen    Kreis/Esc/F1: schließen",
     L"↑↓ select    ←→ change    Cross/Enter: choose    Circle/Esc/F1: close"},
    {L"Zwischensequenz wird übersprungen …", L"Skipping the cutscene …"},
    {L"START: abbrechen", L"START: cancel"},
    // savestates (savestate.cpp)
    {L"Speichern läuft noch …", L"Still saving …"},
    {L" ist leer", L" is empty"},
    {L"Speichern gerade nicht möglich", L"Cannot save right now"},
    {L"Gespeichert in Slot ", L"Saved to slot "},
    {L"Speichern fehlgeschlagen", L"Saving failed"},
    {L"Laden fehlgeschlagen", L"Loading failed"},
    {L"Laden nicht möglich", L"Cannot load"},
    {L"Der Spielstand gehört zu Disc ", L"The savestate belongs to disc "},
    {L"Savestate einer anderen Programmversion", L"Savestate of another program version"},
    {L"Savestate unvollständig", L"Savestate incomplete"},
    {L" geladen", L" loaded"},
    {L"Zwischensequenz wird übersprungen", L"a cutscene is being skipped"},
    {L"während eines Films nicht möglich", L"not possible during a movie"},
    {L"Speicherkarte wird gerade benutzt", L"the memory card is in use"},
    {L"das Spiel ist noch nicht fertig gestartet", L"the game has not finished starting"},
    {L"Datei nicht gefunden", L"file not found"},
    {L"anderes Savestate-Format", L"different savestate format"},
    {L"Datei unvollständig", L"file incomplete"},
    {L"Entpacken nicht möglich", L"cannot decompress"},
    {L"Datei beschädigt", L"file damaged"},
    // first start: the player's disc image (main.cpp)
    {L"Grandia III (USA) Disc %d: Abbild deiner Disc wählen (ISO)", L"Grandia III (USA) disc %d: choose the image of your disc (ISO)"},
    {L"Grandia III braucht ein Abbild (ISO) deiner eigenen Disc 1 von Grandia III (USA). Bitte im nächsten Fenster auswählen; es wird nicht verändert oder kopiert.",
     L"Grandia III needs an image (ISO) of your own Grandia III (USA) disc 1. Please choose it in the next window; it is neither changed nor copied."},
    {L"Für Disc 2 braucht Grandia III das Abbild (ISO) deiner Disc 2 von Grandia III (USA). Bitte im nächsten Fenster auswählen.",
     L"For disc 2 Grandia III needs the image (ISO) of your Grandia III (USA) disc 2. Please choose it in the next window."},
    {L"Das ist kein Abbild von Grandia III (USA) Disc %d. Bitte das passende auswählen.",
     L"This is not an image of Grandia III (USA) disc %d. Please choose the right one."},
    // settings window (settings.cpp)
    {L"Grandia III  -  Einstellungen", L"Grandia III  -  Settings"},
    {L"Kreuz (\x2715)", L"Cross (\x2715)"},
    {L"Kreis (\x25CB)", L"Circle (\x25CB)"},
    {L"Quadrat (\x25A1)", L"Square (\x25A1)"},
    {L"Dreieck (\x25B3)", L"Triangle (\x25B3)"},
    {L"Steuerkreuz hoch", L"D-pad up"},
    {L"Steuerkreuz runter", L"D-pad down"},
    {L"Steuerkreuz links", L"D-pad left"},
    {L"Steuerkreuz rechts", L"D-pad right"},
    {L"L3 (linker Stick drücken)", L"L3 (press left stick)"},
    {L"R3 (rechter Stick drücken)", L"R3 (press right stick)"},
    {L"Rennen (gedrückt halten)", L"Run (hold)"},
    {L"Linker Stick drücken", L"Press left stick"},
    {L"Rechter Stick drücken", L"Press right stick"},
    {L"Stick ganz durchdrücken", L"Push the stick all the way"},
    {L"Taste 0x%02X", L"Key 0x%02X"},
    {L"Bitte zuerst eine Zeile in der Liste auswählen.", L"Please select a row in the list first."},
    {L"Rennen geht mit dem Controller über den Analogstick.", L"With a controller you run by pushing the analog stick all the way."},
    {L"Drücke jetzt die Taste für \"", L"Now press the key for \""},
    {L"Drücke jetzt die Controller-Taste für \"", L"Now press the controller button for \""},
    {L"\"  (Esc = abbrechen)", L"\"  (Esc = cancel)"},
    {L"Abgebrochen.", L"Cancelled."},
    {L"Zugewiesen: ", L"Assigned: "},
    {L"Kein Controller gefunden (XInput, Anschluss 1).  Esc = abbrechen", L"No controller found (XInput, port 1).  Esc = cancel"},
    {L"aus", L"off"},
    {L"Das Spiel wird jetzt mit Disc 2 neu gestartet.\nNicht gespeicherter Fortschritt geht verloren.",
     L"The game now restarts with disc 2.\nUnsaved progress will be lost."},
    {L"Das Spiel wird jetzt mit Disc 1 neu gestartet.\nNicht gespeicherter Fortschritt geht verloren.",
     L"The game now restarts with disc 1.\nUnsaved progress will be lost."},
    {L"Disc wechseln", L"Change disc"},
    {L"Renderer und interne Auflösung werden beim nächsten Start des Spiels übernommen.",
     L"Renderer and internal resolution apply at the next start of the game."},
    {L"Interne Auflösung:", L"Internal resolution:"},
    {L"GPU (hohe Auflösung)", L"GPU (high resolution)"},
    {L"Seitenverhältnis:", L"Aspect ratio:"},
    {L"Bildfilter:", L"Image filter:"},
    {L"Pixel (scharf)", L"Pixel (sharp)"},
    {L"Bilinear (weich)", L"Bilinear (smooth)"},
    {L"Scharf-bilinear", L"Sharp bilinear"},
    {L"Widescreen (Taste F6)", L"Widescreen (key F6)"},
    {L"Menüs/Texte im 4:3-Format (Widescreen)", L"Menus/text in 4:3 format (widescreen)"},
    {L"Vollbild (Taste F11 / Alt+Enter)", L"Fullscreen (F11 / Alt+Enter)"},
    {L"VSync (kein Tearing)", L"VSync (no tearing)"},
    {L"Bildrate anzeigen (F7)", L"Show frame rate (F7)"},
    {L"HD-Texturen (Ordner \"textures\", Taste F8)", L"HD textures (folder \"textures\", key F8)"},
    {L"60 fps (experimentell, Taste F3)", L"60 fps (experimental, key F3)"},
    {L"Nachschärfen:", L"Sharpening:"},
    {L"Renderer/Auflösung gelten ab nächstem Start.", L"Renderer/resolution apply at the next start."},
    {L"Disc 2 (Neustart)", L"Disc 2 (restart)"},
    {L"Musik:", L"Music:"},
    {L"Sprache:", L"Voice:"},
    {L"Effekte:", L"Effects:"},
    {L"Videos:", L"Movies:"},
    {L"Gesamt:", L"Master:"},
    {L"Steuerung", L"Controls"},
    {L"PS2-Taste", L"PS2 button"},
    {L"Tastatur", L"Keyboard"},
    {L"Tastaturtaste ändern", L"Change key"},
    {L"Controllertaste ändern", L"Change button"},
    {L"Standardbelegung", L"Defaults"},
    {L"Zeile auswählen (Doppelklick = Tastaturtaste ändern).", L"Select a row (double-click = change key)."},
    {L"Speichern", L"Save"},
    {L"Abbrechen", L"Cancel"},
    {L"Standardbelegung wiederhergestellt (wird mit \"Speichern\" übernommen).", L"Default mapping restored (applied with \"Save\")."},
};

bool ui_german() { return config().language == "de"; }

const wchar_t* tr(const wchar_t* de) {
    if (!de || ui_german()) return de;
    static const std::unordered_map<std::wstring, const wchar_t*> map = [] {
        std::unordered_map<std::wstring, const wchar_t*> m;
        for (auto& t : TEXTS) m[t[0]] = t[1];
        return m;
    }();
    auto it = map.find(de);
    return it == map.end() ? de : it->second;
}

std::wstring tr(const std::wstring& de) { return tr(de.c_str()); }

// "en" and every lang\<code>.tsv (a two-letter code) next to the executable
std::vector<std::string> languages() {
    std::vector<std::string> v{"en"};
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((exe_dir() + "\\lang\\*.tsv").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::string n = fd.cFileName;
            if (n.size() == 6 && n.compare(2, 4, ".tsv") == 0) {
                std::string code = n.substr(0, 2);
                for (char& ch : code) ch = (char)tolower((unsigned char)ch);
                if (code != "en") v.push_back(code);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return v;
}

std::wstring language_name(const std::string& code) {
    if (code == "en") return L"English";
    if (code == "de") return L"Deutsch";
    if (code == "fr") return L"Français";
    if (code == "es") return L"Español";
    if (code == "it") return L"Italiano";
    return std::wstring(code.begin(), code.end());
}

}  // namespace platform
