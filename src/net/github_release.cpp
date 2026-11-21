#include "net/github_release.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "i18n.h"
#include "net/ca_bundle.h"

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

// GET HTTPS vers `sink` ; code HTTP, ou négatif (erreur de HTTPClient)
int get(const std::string& url, Stream& sink, bool api, String& note) {
    note = "";
    if (WiFi.status() != WL_CONNECTED) {
        note = "Wi-Fi";
        return -1;
    }
    WiFiClientSecure tls;
    tls.setCACertBundle(ca_bundle_start, caBundleSize());
    tls.setHandshakeTimeout(10);
    HTTPClient http;
    http.setConnectTimeout(8000);
    http.setTimeout(15000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);  // github.com -> release-assets.githubusercontent.com
    http.setRedirectLimit(3);
    http.setUserAgent("wiimscreen");  // l'API de GitHub refuse les requêtes sans User-Agent
    if (!http.begin(tls, url.c_str())) {
        note = "URL";
        return -1;
    }
    if (api) {
        http.addHeader("Accept", "application/vnd.github+json");
        http.addHeader("X-GitHub-Api-Version", "2022-11-28");
    }
    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        note = code > 0 ? "HTTP " + String(code) : String(wr::L("network", "r\xC3\xA9seau"));
        http.end();
        return code;
    }
    const int n = http.writeToStream(&sink);
    http.end();
    if (n < 0) {
        note = wr::L("transfer stopped", "transfert interrompu");
        return n;
    }
    return code;
}

}  // namespace

bool latest(wr::ReleaseInfo& out, String& note) {
    TextSink json(256 * 1024);
    const std::string url = std::string("https://api.github.com/repos/") + kRepo + "/releases?per_page=5";
    if (get(url, json, true, note) != HTTP_CODE_OK) return false;
    if (!wr::parseLatestRelease(json.text, kRepo, kAsset, out)) {
        note = wr::L("no firmware", "pas de firmware");
        return false;
    }
    return true;
}

bool download(const std::string& url, Stream& sink, String& note) { return get(url, sink, false, note) == HTTP_CODE_OK; }

}  // namespace ghrelease
