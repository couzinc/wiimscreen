// Simulator (both modes): sound, setup portal and update screens, dictation — no hardware, no web server.
#include <cmath>
#include <map>
#include <mutex>

#include "M5Unified.h"
#include "i18n.h"
#include "device/key_input.h"
#include "device/ota.h"
#include "device/portal.h"
#include "pc.h"
#include "app/log.h"
#include "app/version.h"
#include "net/voice_service.h"


// ------------------------------------------------------------------ setup portal and update page

// The OK or Back key (remote, keyboard): like a tap on the screen / on Cancel
static bool okOrBack() {
    wr::KeyEvent k;
    bool hit = false;
    while (takeKey(k)) hit = hit || k.key == wr::NavKey::Ok || k.key == wr::NavKey::Back;
    return hit;
}

void runSetupPortal(Settings& s, Ui& ui, const String& reason) {
    (void)s;
    ui.showPortal("WiiM-Remote-3F2A", "simulateur", "http://192.168.4.1",
                  reason + wr::L(" Simulator: tap the screen to go back.",
                                 " Simulateur : touche l'\xC3\xA9" "cran pour revenir."));
    const uint32_t shown = millis();
    for (;;) {
        M5.update();
        const bool key = okOrBack();
        if (millis() - shown > 800 && (M5.Touch.getDetail().wasPressed() || key)) ESP.restart();
        delay(10);
    }
}

void runUpdateMode(Ui& ui) {
    // Comme sur le Tab5 : recherche sur GitHub (ici simulée : une version plus récente), Installer simule le
    // téléchargement sans rien installer
    GithubOffer offer;
    offer.state = GithubOffer::Checking;
    const String url = "http://192.168.1.42", code = "481203";
    String status = wr::L("Simulator: no update possible here.", "Simulateur : aucune mise \xC3\xA0 jour possible ici.");
    ui.showUpdate(url, code, FW_VERSION, -1, status, offer);
    const uint32_t shown = millis();
    for (;;) {
        M5.update();
        auto t = M5.Touch.getDetail();
        if (offer.state == GithubOffer::Checking && millis() - shown > 600) {
            offer.state = GithubOffer::Available;
            offer.tag = "v9.9.9-beta";
            status = "";
            ui.showUpdate(url, code, FW_VERSION, -1, status, offer);
        }
        if (t.wasPressed() && ui.hitUpdateInstall(t.x, t.y)) {
            logTagged("update.github", "tag=%s | Mise \xC3\xA0 jour GitHub : t\xC3\xA9l\xC3\xA9" "chargement de %s (simul\xC3\xA9)",
                      offer.tag.c_str(), offer.tag.c_str());
            for (int pct = 0; pct <= 100; pct += 25) {
                ui.showUpdate(url, code, FW_VERSION, pct, wr::L("Downloading v9.9.9-beta from GitHub\xE2\x80\xA6",
                                                                "T\xC3\xA9l\xC3\xA9" "chargement de v9.9.9-beta depuis GitHub\xE2\x80\xA6"));
                delay(150);
            }
            status = wr::L("Simulator: nothing installed.", "Simulateur : rien n'est install\xC3\xA9.");
            ui.showUpdate(url, code, FW_VERSION, -1, status, offer);
            continue;
        }
        if ((t.wasPressed() && ui.hitUpdateCancel(t.x, t.y)) || okOrBack()) return;
        delay(10);
    }
}

// ------------------------------------------------------------------ dictation: a fixed sentence

bool transcribe(const int16_t*, size_t, uint32_t, const String&, const String&, const std::string&, std::string& text,
                String& error) {
    delay(1200);  // the network call takes about this long
    const char* t = getenv("SIM_DICTEE");
    text = t && *t ? t : "Miles Davis";
    error = "";
    return true;
}

