#include "net/github_release.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <cstdio>

#include "i18n.h"
#include "net/ca_bundle.h"
#include "app/log.h"

namespace ghrelease {

namespace {

// Réponse texte bornée (la liste des versions fait environ 8 Ko par version)
class TextSink : public Stream {
public:
    explicit TextSink(size_t cap) : cap_(cap) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* d, size_t n) override {
        if (text.size() + n > cap_) return 0;
        text.append(reinterpret_cast<const char*>(d), n);
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    std::string text;

private:
    size_t cap_;
};

// Hôte d'une URL, sans le chemin ni la requête (l'adresse des fichiers porte une signature : jamais au Journal)
std::string hostOf(const std::string& url) {
    const size_t b = url.find("://");
    const size_t s = b == std::string::npos ? 0 : b + 3;
    return url.substr(s, url.find_first_of("/?#", s) - s);
}

// Réponse d'un GET : code HTTP (ou négatif : erreur de HTTPClient, `tlsErr` = erreur TLS d'ESP-IDF), redirection,
// transfert coupé (`streamErr` < 0 : réseau, ou `sink` qui a refusé), plage refusée
struct Answer {
    int code = -1, tlsErr = 0, streamErr = 0;
    bool badRange = false;
    std::string location;
};

// Un GET HTTPS, sans suivre de redirection, avec son propre client TLS (une connexion neuve par adresse). from > 0 :
// la suite du fichier à partir de cet octet (réponse 206 attendue, qui commence bien là).
Answer getOnce(const std::string& url, Stream& sink, bool api, size_t from) {
    Answer a;
    WiFiClientSecure tls;
    tls.setCACertBundle(ca_bundle_start, caBundleSize());
    tls.setHandshakeTimeout(10);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);  // suivies dans get(), avec un client neuf
    http.setUserAgent("wiimscreen");                         // l'API de GitHub refuse les requêtes sans User-Agent
    const char* keep[] = {"Location", "Content-Range"};
    http.collectHeaders(keep, 2);
    if (!http.begin(tls, url.c_str())) return a;
    if (api) {
        http.addHeader("Accept", "application/vnd.github+json");
        http.addHeader("X-GitHub-Api-Version", "2022-11-28");
    }
    if (from) http.addHeader("Range", ("bytes=" + std::to_string(from) + "-").c_str());
    a.code = http.GET();
    if (a.code == 206 && from) {
        const std::string range = http.header("Content-Range").c_str();  // "bytes <from>-<fin>/<taille>"
        a.badRange = range.compare(0, 6 + std::to_string(from).size() + 1, "bytes " + std::to_string(from) + "-") != 0;
    }
    if (a.code != (from ? 206 : HTTP_CODE_OK) || a.badRange) {
        if (a.code >= 300 && a.code < 400) a.location = http.header("Location").c_str();
        if (a.code < 0) {
            char buf[8];
            a.tlsErr = tls.lastError(buf, sizeof buf);
        }
        http.end();
        return a;
    }
    const int n = http.writeToStream(&sink);
    http.end();
    if (n < 0) a.streamErr = n;
    return a;
}

// GET HTTPS vers `sink` (depuis l'octet `from`), redirections suivies (github.com -> release-assets.githubusercontent.com),
// une seconde tentative après une erreur réseau. Vrai si le fichier est arrivé jusqu'au bout ; sinon, raison courte
// dans `note`, détail au Journal.
bool get(const std::string& url, Stream& sink, bool api, size_t from, String& note) {
    note = "";
    if (WiFi.status() != WL_CONNECTED) {
        note = "Wi-Fi";
        return false;
    }
    std::string at = url;
    Answer a;
    for (int hop = 0, retry = 0; hop < 4;) {
        a = getOnce(at, sink, api, from);
        if (a.code == (from ? 206 : HTTP_CODE_OK) && !a.badRange) {
            if (!a.streamErr) return true;
            note = String(wr::L("transfer stopped ", "transfert interrompu ")) + String(a.streamErr);
            return false;
        }
        if (a.badRange) {
            note = wr::L("resume refused", "reprise refus\xC3\xA9" "e");
            return false;
        }
        if (a.code >= 300 && a.code < 400) {
            if (a.location.compare(0, 8, "https://") != 0) break;  // vide ou pas en HTTPS : pas suivie
            at = a.location;
            ++hop;
            retry = 0;
            continue;
        }
        if (a.code < 0) {
            logEvent("Mise \xC3\xA0 jour GitHub : %s, erreur %d (TLS -0x%04X), m\xC3\xA9moire interne %u (bloc %u)%s",
                     hostOf(at).c_str(), a.code, (unsigned)-a.tlsErr, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), retry ? "" : ", nouvel essai");
            if (!retry++) {
                delay(1500);
                continue;
            }
            char tls[24] = "";
            if (a.tlsErr) snprintf(tls, sizeof tls, ", TLS -0x%04X", (unsigned)-a.tlsErr);
            note = String(wr::L("network ", "r\xC3\xA9seau ")) + String(a.code) + tls;
            return false;
        }
        break;
    }
    note = a.code > 0 ? "HTTP " + String(a.code) : String(wr::L("network", "r\xC3\xA9seau"));
    return false;
}

}  // namespace

bool latest(wr::ReleaseInfo& out, String& note) {
    TextSink json(256 * 1024);
    const std::string url = std::string("https://api.github.com/repos/") + kRepo + "/releases?per_page=5";
    if (!get(url, json, true, 0, note)) return false;
    if (!wr::parseLatestRelease(json.text, kRepo, kAsset, out)) {
        note = wr::L("no firmware", "pas de firmware");
        return false;
    }
    return true;
}

bool download(const std::string& url, DownloadSink& sink, size_t size, String& note) {
    for (int attempt = 1, stalls = 0;; ++attempt) {
        const size_t from = sink.received();
        if (get(url, sink, false, from, note)) return true;
        if (sink.refused() || sink.received() >= size) return false;
        const size_t now = sink.received();
        stalls = now > from ? 0 : stalls + 1;  // essai sans un octet de plus
        const bool again = stalls < 2 && attempt < 10;
        logEvent("Mise \xC3\xA0 jour GitHub : coup\xC3\xA9 \xC3\xA0 %u octets sur %u (%s)%s", (unsigned)now,
                 (unsigned)size, note.c_str(), again ? ", reprise" : "");
        if (!again) return false;
        delay(1000);
    }
}

}  // namespace ghrelease
