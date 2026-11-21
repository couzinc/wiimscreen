// Tests: OTA stream filter (pio test -e native)
#include <unity.h>

#include <string>
#include <vector>
#include <algorithm>

#include "firmware.h"
#include "test_helpers.h"

using namespace wr;

static void test_firmware_stream() {
    // Application seule : tout est écrit
    std::vector<uint8_t> app(5000, 0x11);
    app[0] = 0xE9;
    FirmwareStream a;
    const uint8_t* out;
    size_t total = 0;
    for (size_t i = 0; i < app.size(); i += 1436) total += a.feed(app.data() + i, std::min<size_t>(1436, app.size() - i), &out);
    TEST_ASSERT_EQUAL(FirmwareStream::App, a.kind());
    TEST_ASSERT_EQUAL(5000, (int)total);

    // Image complète : seule la partie à partir de 0x10000 est écrite, octets identiques
    std::vector<uint8_t> merged(0x10000 + 3000, 0xFF);
    merged[0x2000] = 0xE9;  // chargeur de démarrage
    for (size_t i = 0x10000; i < merged.size(); ++i) merged[i] = (uint8_t)(i * 7);
    merged[0x10000] = 0xE9;
    FirmwareStream m;
    std::vector<uint8_t> written;
    for (size_t i = 0; i < merged.size(); i += 1436) {
        size_t k = m.feed(merged.data() + i, std::min<size_t>(1436, merged.size() - i), &out);
        written.insert(written.end(), out, out + k);
    }
    TEST_ASSERT_EQUAL(FirmwareStream::Merged, m.kind());
    TEST_ASSERT_EQUAL(3000, (int)written.size());
    TEST_ASSERT_TRUE(std::equal(written.begin(), written.end(), merged.begin() + 0x10000));

    // Image complète sans application valide à 0x10000 : refusée
    std::vector<uint8_t> bad(0x10000 + 100, 0xFF);
    FirmwareStream b;
    for (size_t i = 0; i < bad.size(); i += 1436) b.feed(bad.data() + i, std::min<size_t>(1436, bad.size() - i), &out);
    TEST_ASSERT_EQUAL(FirmwareStream::Invalid, b.kind());
    TEST_ASSERT_EQUAL(0, (int)b.written());

    // Autre fichier (zip, image…) : refusé dès le premier octet
    const uint8_t zip[] = {'P', 'K', 3, 4};
    FirmwareStream z;
    TEST_ASSERT_EQUAL(0, (int)z.feed(zip, sizeof zip, &out));
    TEST_ASSERT_EQUAL(FirmwareStream::Invalid, z.kind());
}

// Réponse de GitHub (forme réelle, raccourcie) : objets imbriqués avec leurs propres "name"/"url"
static std::string releaseJson(const std::string& tag, const std::string& assets, bool draft = false) {
    return std::string("{\"url\":\"https://api.github.com/x\",\"author\":{\"login\":\"a\",\"url\":\"u\"},\"name\":\"") +
           tag + "\",\"tag_name\":\"" + tag + "\",\"draft\":" + (draft ? "true" : "false") +
           ",\"prerelease\":true,\"body\":\"Flash \\\"wiim-remote-tab5.bin\\\" at 0x0\",\"assets\":[" + assets + "]}";
}
static std::string assetJson(const std::string& tag, const std::string& name, const std::string& digest,
                             const std::string& size = "2933072") {
    return "{\"name\":\"" + name + "\",\"uploader\":{\"name\":\"x\",\"login\":\"bot\"},\"size\":" + size +
           ",\"digest\":\"" + digest + "\",\"browser_download_url\":\"https://github.com/o/r/releases/download/" + tag +
           "/" + name + "\"}";
}
static const std::string kHex = "aad30f37b5f1501e5ef6e68b55d9a4d80ed8c928311b3e36503c2ac1f526f665";

static void test_latest_release() {
    const std::string bin = "wiim-remote-tab5.bin";
    // Brouillon ignoré, puis une version sans le fichier du Tab5, puis la bonne
    const std::string json = "[" + releaseJson("v0.6.0-beta", assetJson("v0.6.0-beta", bin, "sha256:" + kHex), true) +
                             "," + releaseJson("v0.5.0-beta", assetJson("v0.5.0-beta", "SHA256SUMS.txt", "sha256:" + kHex)) +
                             "," + releaseJson("v0.4.2-beta", assetJson("v0.4.2-beta", "SHA256SUMS.txt", "sha256:" + kHex) +
                                                                  "," + assetJson("v0.4.2-beta", bin, "sha256:" + kHex)) +
                             "]";
    ReleaseInfo r;
    TEST_ASSERT_TRUE(parseLatestRelease(json, "o/r", bin, r));
    TEST_ASSERT_EQUAL_STRING("v0.4.2-beta", r.tag.c_str());
    TEST_ASSERT_EQUAL_STRING("https://github.com/o/r/releases/download/v0.4.2-beta/wiim-remote-tab5.bin", r.url.c_str());
    TEST_ASSERT_EQUAL_STRING(kHex.c_str(), r.sha256.c_str());
    TEST_ASSERT_EQUAL(2933072, (int)r.size);
    // Refusés : autre dépôt, empreinte absente ou pas en SHA-256, taille absente, étiquette affichable douteuse
    auto one = [&](const std::string& tag, const std::string& asset) {
        ReleaseInfo x;
        return parseLatestRelease("[" + releaseJson(tag, asset) + "]", "o/r", bin, x);
    };
    TEST_ASSERT_TRUE(one("v1.0.0", assetJson("v1.0.0", bin, "sha256:" + kHex)));
    TEST_ASSERT_FALSE(parseLatestRelease("[" + releaseJson("v1.0.0", assetJson("v1.0.0", bin, "sha256:" + kHex)) + "]",
                                         "evil/r", bin, r));
    TEST_ASSERT_FALSE(one("v1.0.0", assetJson("v1.0.0", bin, "")));
    TEST_ASSERT_FALSE(one("v1.0.0", assetJson("v1.0.0", bin, "md5:" + kHex)));
    TEST_ASSERT_FALSE(one("v1.0.0", assetJson("v1.0.0", bin, "sha256:" + kHex.substr(1))));
    TEST_ASSERT_FALSE(one("v1.0.0", assetJson("v1.0.0", bin, "sha256:" + kHex, "0")));
    TEST_ASSERT_FALSE(one("v1.0.0", assetJson("v1.0.0", bin, "sha256:" + kHex, "-5")));
    TEST_ASSERT_FALSE(one("v1 0", assetJson("v1 0", bin, "sha256:" + kHex)));
    TEST_ASSERT_FALSE(one("v1.0.0", assetJson("v0.9.0", bin, "sha256:" + kHex)));  // fichier d'une autre version
    TEST_ASSERT_FALSE(parseLatestRelease("[]", "o/r", bin, r));
    TEST_ASSERT_FALSE(parseLatestRelease("{\"message\":\"API rate limit exceeded\"}", "o/r", bin, r));
}

static void test_versions() {
    int c = 9;
    auto cmp = [&](const char* a, const char* b) {
        TEST_ASSERT_TRUE_MESSAGE(compareVersions(a, b, c), a);
        return c;
    };
    TEST_ASSERT_EQUAL(-1, cmp("v0.4.1-beta", "v0.4.2-beta"));
    TEST_ASSERT_EQUAL(0, cmp("v0.4.1-beta", "0.4.1-beta"));
    TEST_ASSERT_EQUAL(1, cmp("v0.10.0", "v0.9.9"));
    TEST_ASSERT_EQUAL(-1, cmp("v1.0.0-beta", "v1.0.0"));  // pré-version avant la version
    TEST_ASSERT_EQUAL(-1, cmp("v1.0.0-alpha", "v1.0.0-beta"));
    TEST_ASSERT_EQUAL(-1, cmp("v1.0.0-beta.2", "v1.0.0-beta.11"));
    TEST_ASSERT_EQUAL(-1, cmp("v1.0.0-beta", "v1.0.0-beta.1"));
    TEST_ASSERT_EQUAL(-1, cmp("v1.0.0-1", "v1.0.0-rc"));  // nombre avant mot
    TEST_ASSERT_EQUAL(0, cmp("v1.2.3+abc", "v1.2.3"));
    for (const char* bad : {"126-4daac9a", "local-4daac9a", "dev", "", "v1.2", "v1.2.3.4", "v1.2.3-", "v1..3", "v1.2.3-a..b"})
        TEST_ASSERT_FALSE_MESSAGE(compareVersions(bad, "v1.0.0", c), bad);
    TEST_ASSERT_TRUE(releaseOffer("v0.4.1-beta", "v0.4.2-beta") == ReleaseOffer::Newer);
    TEST_ASSERT_TRUE(releaseOffer("v0.4.2-beta", "v0.4.2-beta") == ReleaseOffer::Same);
    TEST_ASSERT_TRUE(releaseOffer("v0.5.0", "v0.4.2-beta") == ReleaseOffer::Older);
    TEST_ASSERT_TRUE(releaseOffer("126-4daac9a", "v0.4.2-beta") == ReleaseOffer::Unknown);
}

void run_firmware_tests() {
    RUN_TEST(test_firmware_stream);
    RUN_TEST(test_latest_release);
    RUN_TEST(test_versions);
}
