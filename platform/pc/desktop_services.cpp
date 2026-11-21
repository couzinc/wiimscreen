// PC desktop build: the Tab5 services that have no meaning on a computer.
// - Setup portal (Wi-Fi access point for a phone): the computer is already on the network; settings are a text
//   file (shown on the screen). - Update: the latest version on GitHub is shown (net/github_release.cpp, the same
//   check as the Tab5) with where to get it; nothing is downloaded (Windows: a new zip; elsewhere: git pull, build).
// - Sound: platform/pc/pc_sound.cpp (silent). - Dictation: no microphone support yet (the microphone button is not shown without
//   an OpenAI key; with one, « Micro indisponible »).
#include "M5Unified.h"
#include "device/key_input.h"
#include "device/ota.h"
#include "pc.h"
#include "device/portal.h"
#include "i18n.h"
#include "net/voice_service.h"
#include "net/github_release.h"
#include "app/version.h"


// A tap, or the OK or Back key (remote, keyboard), once the screen has shown for afterMs
static bool okOrBack() {
    wr::KeyEvent k;
    bool hit = false;
    while (takeKey(k)) hit = hit || k.key == wr::NavKey::Ok || k.key == wr::NavKey::Back;
    return hit;
}

static void waitForTap(uint32_t afterMs) {
    const uint32_t shown = millis();
    for (;;) {
        M5.update();
        const bool key = okOrBack();  // keys pressed before afterMs are dropped too
        if (millis() - shown > afterMs && (M5.Touch.getDetail().wasPressed() || key)) return;
        delay(10);
    }
}

void runSetupPortal(Settings&, Ui& ui, const String& reason) {
    const String file = String(pc::stateDir().c_str()) + "/settings.txt";
    ui.showInfo(wr::L("Settings", "R\xC3\xA9glages"),
                reason + " " +
                    wr::L("On a computer, the settings are in this file (one line per setting, name and value "
                          "separated by a tab). Edit it, then start wiimscreen again: ",
                          "Sur un ordinateur, les r\xC3\xA9glages sont dans ce fichier (une ligne par r\xC3\xA9glage, nom et "
                          "valeur s\xC3\xA9par\xC3\xA9s par une tabulation). Modifie-le, puis relance wiimscreen : ") +
                    file,
                wr::L("Tap the screen to go back", "Touche l'\xC3\xA9" "cran pour revenir"));
    waitForTap(800);
    ESP.restart();
}

void runUpdateMode(Ui& ui) {
    const char* title = wr::L("Update", "Mise \xC3\xA0 jour");
    ui.showInfo(title, wr::L("Looking for a new version on GitHub\xE2\x80\xA6", "Recherche d'une nouvelle version sur GitHub\xE2\x80\xA6"));
    wr::ReleaseInfo info;
    String note, found;
    if (ghrelease::latest(info, note)) {
        const wr::ReleaseOffer o = wr::releaseOffer(FW_VERSION, info.tag);
        found = String(o == wr::ReleaseOffer::Newer     ? wr::L("New version on GitHub: ", "Nouvelle version sur GitHub : ")
                       : o == wr::ReleaseOffer::Unknown ? wr::L("Latest version on GitHub: ", "Derni\xC3\xA8re version sur GitHub : ")
                                                        : wr::L("Up to date. Latest version on GitHub: ",
                                                                "\xC3\x80 jour. Derni\xC3\xA8re version sur GitHub : ")) +
                info.tag.c_str() + ".";
    } else {
        found = String(wr::L("GitHub not reachable (", "GitHub injoignable (")) + note + ").";
    }
#ifdef _WIN32
    const char* how = wr::L("To update: download wiimscreen-windows-x64.zip from github.com/couzinc/wiimscreen/releases "
                            "and replace the files.",
                            "Pour mettre \xC3\xA0 jour : t\xC3\xA9l\xC3\xA9" "charge wiimscreen-windows-x64.zip sur "
                            "github.com/couzinc/wiimscreen/releases et remplace les fichiers.");
#else
    const char* how = wr::L("To update: in the wiimscreen folder, run git pull, then pio run -e desktop.",
                            "Pour mettre \xC3\xA0 jour : dans le dossier wiimscreen, lance git pull, puis pio run -e desktop.");
#endif
    ui.showInfo(title,
                String(wr::L("Installed version: ", "Version install\xC3\xA9" "e : ")) + FW_VERSION + ". " + found + " " + how,
                wr::L("Tap the screen to go back", "Touche l'\xC3\xA9" "cran pour revenir"));
    waitForTap(500);
}

bool transcribe(const int16_t*, size_t, uint32_t, const String&, const String&, const std::string&, std::string&,
                String& error) {
    error = wr::L("No microphone on this computer", "Pas de micro sur cet ordinateur");
    return false;
}
