// Retour automatique à la version précédente (voir boot_check.h)
#include "device/boot_check.h"

#include <Arduino.h>

#include <cstring>

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <Preferences.h>
#include <esp_ota_ops.h>

#include <atomic>

#include "app/log.h"
#include "appstate.h"

// Le cœur Arduino appelle cette fonction (faible) avant setup() : vrai = la version à l'essai n'est pas confirmée
// tout de suite, c'est nous qui le faisons.
extern "C" bool verifyRollbackLater() { return true; }

namespace bootcheck {

static const char* kNs = "bootcheck";
static const char* kKey = "trial";
static std::atomic<bool> g_trial{false};

static bool onTrial() {
    esp_ota_img_states_t st;
    const esp_partition_t* run = esp_ota_get_running_partition();
    return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

void confirm(const char* why) {
    if (!g_trial.exchange(false)) return;
    const esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    Preferences p;
    if (p.begin(kNs, false)) {
        p.remove(kKey);
        p.end();
    }
    logEvent("Nouvelle version confirm\xC3\xA9" "e (%s)%s", why, e == ESP_OK ? "" : " : \xC3\xA9" "chec");
}

static void confirmLater(void*) {
    vTaskDelay(pdMS_TO_TICKS(wr::kBootTrialMs));
    char why[24];
    snprintf(why, sizeof why, "%u s sans arr\xC3\xAAt", (unsigned)(wr::kBootTrialMs / 1000));
    confirm(why);
    vTaskDelete(nullptr);
}

bool begin(const char* running, char* stopped, unsigned stoppedSize) {
    const bool trial = onTrial();
    g_trial = trial;
    // Confirmation lancée d'abord : sans elle, même une NVS illisible ferait revenir l'ancienne version
    if (trial) {
        logEvent("Nouvelle version \xC3\xA0 l'essai : confirm\xC3\xA9" "e apr\xC3\xA8s %u s sans arr\xC3\xAAt",
                 (unsigned)(wr::kBootTrialMs / 1000));
        if (xTaskCreate(confirmLater, "bootok", 4096, nullptr, 1, nullptr) != pdPASS) confirm("t\xC3\xA2" "che absente");
    }
    if (stoppedSize) stopped[0] = 0;
    Preferences p;
    if (!p.begin(kNs, false)) return false;
    const String stored = p.getString(kKey, "");
    const wr::BootNote note = wr::bootNote(trial, stored.c_str(), running);
    bool rolledBack = false;
    if (note == wr::BootNote::RolledBack) {
        logEvent("Retour automatique : la version %s s'est arr\xC3\xAAt\xC3\xA9" "e au d\xC3\xA9marrage, version %s remise",
                 stored.c_str(), running);
        if (stoppedSize) snprintf(stopped, stoppedSize, "%s", stored.c_str());
        rolledBack = true;
    }
    if (note != wr::BootNote::None) p.remove(kKey);
    if (trial && g_trial) p.putString(kKey, running);  // la version d'avant saura laquelle s'est arrêtée
    p.end();
    return rolledBack;
}

}  // namespace bootcheck

#else

namespace bootcheck {
bool begin(const char*, char* stopped, unsigned stoppedSize) {
    if (stoppedSize) stopped[0] = 0;
    return false;
}
void confirm(const char*) {}
}  // namespace bootcheck

#endif
