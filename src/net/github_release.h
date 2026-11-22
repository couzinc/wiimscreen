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

// Fichier reçu par morceaux. write() écrit moins que demandé pour arrêter le transfert (annulé, erreur d'écriture).
class DownloadSink : public Stream {
public:
    virtual size_t received() const = 0;  // octets acceptés jusqu'ici : une reprise demande la suite (Range)
    virtual bool refused() = 0;           // a arrêté le transfert lui-même (ou Annuler touché) : pas de reprise
};

// Télécharge `url` (`size` octets) vers `sink`. Une connexion coupée en route reprend où elle en était (nouvelle
// redirection, Range), tant que chaque essai avance. Faux, avec une raison courte dans `note`, sinon ; détail au
// Journal.
bool download(const std::string& url, DownloadSink& sink, size_t size, String& note);

}  // namespace ghrelease
