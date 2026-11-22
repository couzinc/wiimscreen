#pragma once

// Retour automatique à la version précédente après une mise à jour qui plante au démarrage.
// Le chargeur de démarrage de l'ESP32-P4 sait le faire (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE) : une version
// installée par mise à jour démarre « à l'essai » ; si elle redémarre avant d'être confirmée, il remet la
// précédente. Le cœur Arduino confirme toute version dès son démarrage ; ce module garde l'essai ouvert
// wr::kBootTrialMs (lib/core/src/appstate.h), puis confirme. PC : rien.
namespace bootcheck {

// Au tout début de setup() : note l'essai (nom de la version en NVS), dit dans le Journal si la version d'avant
// s'est arrêtée pendant le sien (retour fait), et lance la confirmation différée. Vrai si un retour vient d'avoir
// lieu : `stopped` reçoit alors le nom de la version arrêtée.
bool begin(const char* running, char* stopped, unsigned stoppedSize);

// Confirme tout de suite (avant un redémarrage voulu, une extinction, une mise à jour) : un redémarrage demandé
// pendant l'essai n'est pas un plantage.
void confirm(const char* why);

}  // namespace bootcheck
