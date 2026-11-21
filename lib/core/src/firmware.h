// Mise à jour par le réseau : tri des octets reçus (sans Arduino, testable sur PC).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace wr {

// Accepte les deux fichiers possibles :
//  - l'application seule (firmware.bin), qui commence par l'octet magique 0xE9
//  - l'image complète produite par la CI pour le flash USB (wiim-remote-tab5.bin) :
//    remplissage 0xFF, chargeur de démarrage à 0x2000, … application à 0x10000.
//    Seule la partie application (à partir de 0x10000) est gardée.
class FirmwareStream {
public:
    enum Kind { Unknown, App, Merged, Invalid };
    static constexpr size_t kAppOffset = 0x10000;

    // Morceau reçu → partie à écrire dans la partition (*out, renvoie sa longueur ; 0 : rien)
    size_t feed(const uint8_t* data, size_t len, const uint8_t** out);
    Kind kind() const { return kind_; }
    size_t received() const { return offset_; }
    size_t written() const { return written_; }

private:
    Kind kind_ = Unknown;
    size_t offset_ = 0, written_ = 0;
};

// --- Mise à jour depuis GitHub (Réglages › Mise à jour)

// Dernière version publiée qui porte le firmware du Tab5
struct ReleaseInfo {
    std::string tag;     // "v0.4.2-beta" (lettres, chiffres, . - _ + : affichable tel quel)
    std::string url;     // https://github.com/<dépôt>/releases/download/<tag>/<fichier>
    std::string sha256;  // 64 chiffres hexadécimaux en minuscules
    long size = 0;       // octets
};

// Réponse de GET https://api.github.com/repos/<dépôt>/releases (tableau, la plus récente d'abord) : la première
// version publiée (pas un brouillon) qui a le fichier `asset` avec son empreinte SHA-256 ("digest") et sa taille.
// Faux si aucune, ou si l'adresse du fichier n'est pas sur https://github.com/<repo>/releases/download/.
bool parseLatestRelease(const std::string& json, const std::string& repo, const std::string& asset, ReleaseInfo& out);

// Ordre de deux versions "v1.2.3" / "v1.2.3-beta.2" (semver ; le "v" est facultatif, "+build" ignoré) :
// *cmp < 0, 0 ou > 0. Faux si l'une n'est pas une version (build de la CI "126-4daac9a", "local-…", "dev").
bool compareVersions(const std::string& a, const std::string& b, int& cmp);

// Que proposer : Newer (installer), Same / Older (à jour), Unknown (version installée sans numéro : on propose
// quand même la dernière, sans dire si elle est plus récente)
enum class ReleaseOffer { Newer, Same, Older, Unknown };
ReleaseOffer releaseOffer(const std::string& installed, const std::string& available);

}  // namespace wr
