#pragma once

// Mise à jour depuis GitHub : dernière version publiée du dépôt et téléchargement de son firmware (HTTPS, certificats
// racine embarqués, redirection de github.com vers le serveur des fichiers suivie). Appels bloquants : à faire hors
// de la boucle principale (tâche de l'écran Mise à jour).
#include <Arduino.h>

#include <string>

#include "firmware.h"

namespace ghrelease {

constexpr const char* kRepo = "couzinc/wiimscreen";     // dépôt des versions publiées
constexpr const char* kAsset = "wiim-remote-tab5.bin";  // fichier du firmware dans chaque version

// Dernière version publiée qui porte kAsset (avec son empreinte SHA-256 et sa taille). Faux sinon, avec une raison
// courte dans `note` ("HTTP 403", "réseau", "pas de firmware").
bool latest(wr::ReleaseInfo& out, String& note);

// Télécharge `url` vers `sink`. Le transfert s'arrête quand sink.write() écrit moins que demandé. Faux, avec une
// raison courte dans `note`, si la réponse n'est pas 200 ou si le transfert s'arrête avant la fin.
bool download(const std::string& url, Stream& sink, String& note);

}  // namespace ghrelease
