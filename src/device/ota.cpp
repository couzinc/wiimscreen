#include "device/ota.h"

#include <M5Unified.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lwip/sockets.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <atomic>
#include <functional>

#include "firmware.h"
#include "i18n.h"
#include "app/log.h"
#include "app/version.h"
#include "device/key_input.h"
#include "device/web_style.h"
#include "net/github_release.h"

// Corps de la page (après kWebHead, feuille de style commune). Les textes sont des repères %T_xxx%,
// remplacés à l'envoi par la langue courante (kTexts) ; ceux du script sont entre apostrophes JS.
static const char kPage[] PROGMEM = R"HTML(<title>%T_TITLE%</title><style>
#code{font:600 30px ui-monospace,Menlo,monospace;letter-spacing:.35em;text-align:center;padding:14px}
.file{display:flex;align-items:center;gap:12px;background:#111317;border:1px dashed var(--line);border-radius:12px;padding:16px;cursor:pointer;color:var(--muted);font-weight:500}
.file b{color:var(--text)}.file input{display:none}.file.ok{border-style:solid;border-color:var(--accent)}
#m{margin-top:14px}#m:empty{display:none}.btn{margin-top:18px}</style></head><body>
<header><div class=logo><i></i></div><div><h1>%T_H1%</h1><p>%T_SUB% · version <b>%VERSION%</b></p></div></header>
<form id=f class=card><label for=code>%T_CODE%</label>
<input id=code name=code inputmode=numeric autocomplete=one-time-code maxlength=6 pattern='[0-9]{6}' placeholder='······' required>
<label>Firmware</label><label class=file id=z><input type=file name=fw id=fw required>
<span id=n>%T_CHOOSE% <b>wiim-remote-tab5.bin</b></span></label>
<small>%T_HELP%</small>
<button class=btn id=go>%T_INSTALL%</button>
<progress id=p max=100 value=0 hidden></progress><div id=m></div></form>
<script>
var f=document.getElementById('f'),fw=document.getElementById('fw'),p=document.getElementById('p'),
m=document.getElementById('m'),go=document.getElementById('go');
function say(t,c){m.textContent=t;m.className=c||''}
fw.onchange=function(){var x=fw.files[0];if(!x)return;document.getElementById('n').innerHTML='';
var b=document.createElement('b');b.textContent=x.name;document.getElementById('n').append(b,' · '+Math.round(x.size/1024)+' %T_KB%');
document.getElementById('z').classList.add('ok')};
f.onsubmit=function(e){e.preventDefault();var x=new XMLHttpRequest(),d=new FormData();
x.open('POST','/update?code='+encodeURIComponent(f.code.value));
x.upload.onprogress=function(v){p.hidden=false;p.value=v.loaded*100/v.total;
go.textContent=v.loaded<v.total?'%T_SENDING% '+Math.round(p.value)+' %':'%T_INSTALLING%'};
x.onload=function(){go.disabled=false;go.textContent='%T_INSTALL%';
if(x.status==200){say(x.responseText,'okbox');go.hidden=true;p.value=100}else{say(x.responseText,'err');p.hidden=true}};
x.onerror=function(){go.disabled=false;go.textContent='%T_INSTALL%';say('%T_LOST%','err')};
d.append('fw',fw.files[0]);go.disabled=true;go.textContent='%T_SENDING%';say('');x.send(d)};
</script></body></html>)HTML";

// Textes de la page : repère, anglais, français (sans apostrophe dans ceux qui vont dans les chaînes JS du script)
struct PageText {
    const char* key;
    const char* en;
    const char* fr;
};
static const PageText kTexts[] = {
    {"%T_TITLE%", "Tab5 update", "Mise \xC3\xA0 jour du Tab5"},
    {"%T_H1%", "Update", "Mise \xC3\xA0 jour"},
    {"%T_SUB%", "WiiM remote", "T\xC3\xA9l\xC3\xA9" "commande WiiM"},
    {"%T_CODE%", "Code shown on the Tab5", "Code affich\xC3\xA9 sur le Tab5"},
    {"%T_CHOOSE%", "Choose", "Choisir"},
    {"%T_HELP%", "The file of the latest release on GitHub (unzipped if it is in a .zip).",
     "Le fichier de la derni\xC3\xA8re version sur GitHub (d\xC3\xA9zipp\xC3\xA9 s'il est dans un .zip)."},
    {"%T_INSTALL%", "Install", "Installer"},
    {"%T_KB%", "KB", "Ko"},
    {"%T_SENDING%", "Sending\xE2\x80\xA6", "Envoi\xE2\x80\xA6"},
    {"%T_INSTALLING%", "Installing\xE2\x80\xA6", "Installation\xE2\x80\xA6"},
    {"%T_LOST%", "Connection lost: try again.", "Connexion perdue : r\xC3\xA9" "essaie."},
};

// Comparaison en temps constant
static bool sameCode(const String& a, const String& b) {
    if (a.length() != b.length() || a.length() == 0) return false;
    uint8_t d = 0;
    for (unsigned i = 0; i < a.length(); ++i) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static String updatePage() {
    String page = FPSTR(kWebHead);
    page.replace("%LANG%", wr::langCode(wr::lang()));
    page += FPSTR(kPage);
    page.replace("%VERSION%", FW_VERSION);
    for (const auto& t : kTexts) page.replace(t.key, wr::L(t.en, t.fr));
    return page;
}

// Écriture du firmware dans la partition inactive, commune à l'envoi depuis un navigateur et au téléchargement depuis
// GitHub. À la première erreur, `error` dit pourquoi et la suite est ignorée.
class Flasher {
public:
    bool feed(const uint8_t* data, size_t n) {
        if (error.length()) return false;
        const uint8_t* out;
        const size_t k = stream_.feed(data, n, &out);
        if (stream_.kind() == wr::FirmwareStream::Invalid) {
            abort();
            error = wr::L("This file is not a Tab5 firmware (did you unzip the GitHub download?)",
                          "Ce fichier n'est pas un firmware du Tab5 (as-tu bien d\xC3\xA9zipp\xC3\xA9 le "
                          "t\xC3\xA9l\xC3\xA9" "chargement de GitHub ?)");
            return false;
        }
        if (k && !Update.isRunning() && !Update.begin(UPDATE_SIZE_UNKNOWN)) {
            error = String(wr::L("Cannot prepare the update: ", "Impossible de pr\xC3\xA9parer la mise \xC3\xA0 jour : ")) +
                    Update.errorString();
            return false;
        }
        if (k && Update.write(const_cast<uint8_t*>(out), k) != k) {
            error = String(wr::L("Write failed: ", "\xC3\x89" "criture impossible : ")) + Update.errorString();
            Update.abort();
            return false;
        }
        return true;
    }
    // Fin du fichier : end(true) vérifie l'image (somme de contrôle, puce) avant de la rendre active
    bool finish() {
        if (error.length()) return false;
        if (stream_.written() > 0 && Update.end(true)) return true;
        error = String(wr::L("Firmware rejected: ", "Firmware refus\xC3\xA9 : ")) +
                (stream_.written() ? Update.errorString() : wr::L("empty or incomplete file", "fichier vide ou incomplet"));
        abort();
        return false;
    }
    void abort() {
        if (Update.isRunning()) Update.abort();
    }
    size_t received() const { return stream_.received(); }
    String error;

private:
    wr::FirmwareStream stream_;
};

// Recherche de la dernière version sur GitHub, dans sa propre tâche (1 à 3 s de TLS) : l'écran et la page locale
// restent actifs. Statique : la tâche peut finir après la fermeture de l'écran ; une nouvelle ouverture attend la
// recherche en cours au lieu d'en lancer une autre.
struct GithubCheck {
    std::atomic<bool> running{false}, done{false};
    bool ok = false;  // lus seulement après done
    wr::ReleaseInfo info;
    String note;
};
static GithubCheck g_check;

// Sockets encore libres (au plus CONFIG_LWIP_MAX_SOCKETS) : ouverts puis refermés aussitôt, sans connexion. Pour le
// Journal : la page locale et le téléchargement ont besoin chacun d'un socket ; une fuite ailleurs les bloquerait.
static int freeSockets() {
    int fds[CONFIG_LWIP_MAX_SOCKETS];
    int n = 0;
    while (n < CONFIG_LWIP_MAX_SOCKETS) {
        const int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) break;
        fds[n++] = s;
    }
    for (int i = 0; i < n; ++i) lwip_close(fds[i]);
    return n;
}

static void githubCheckTask(void*) {
    wr::ReleaseInfo info;
    String note;
    const bool ok = ghrelease::latest(info, note);
    g_check.info = info;
    g_check.note = note;
    g_check.ok = ok;
    g_check.done = true;
    g_check.running = false;
    vTaskDelete(nullptr);
}

static void startGithubCheck() {
    if (g_check.running) return;
    g_check.done = false;
    g_check.running = true;
    if (xTaskCreatePinnedToCore(githubCheckTask, "ghcheck", 16 * 1024, nullptr, 1, nullptr, 0) != pdPASS) {
        g_check.ok = false;
        g_check.note = "task";
        g_check.running = false;
        g_check.done = true;
    }
}

// Téléchargement depuis GitHub vers le Flasher : empreinte SHA-256 calculée au passage, progression à l'écran,
// Annuler écouté entre deux morceaux (écrire moins que demandé arrête le transfert).
class GithubSink : public ghrelease::DownloadSink {
public:
    GithubSink(Flasher& flasher, size_t size, std::function<void(int)> progress, std::function<bool()> cancelled)
        : flasher_(flasher), size_(size), progress_(std::move(progress)), cancelled_(std::move(cancelled)) {
        mbedtls_sha256_init(&sha_);
        mbedtls_sha256_starts(&sha_, 0);
    }
    ~GithubSink() override { mbedtls_sha256_free(&sha_); }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* d, size_t n) override {
        if (stop || got_ + n > size_) {
            stop = true;
            return 0;
        }
        if (cancelled_()) {
            stop = cancel = true;
            return 0;
        }
        mbedtls_sha256_update(&sha_, d, n);
        got_ += n;
        if (!flasher_.feed(d, n)) {
            stop = true;
            return 0;
        }
        progress_((int)(got_ * 100 / size_));
        return n;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
    // Fichier complet et identique à celui publié (taille et empreinte annoncées par GitHub)
    bool complete(const std::string& sha256) {
        uint8_t h[32];
        mbedtls_sha256_finish(&sha_, h);
        char hex[65];
        for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", h[i]);
        return got_ == size_ && sha256 == hex;
    }
    size_t received() const override { return got_; }
    bool refused() override {  // entre deux essais : Annuler touché pendant l'attente compte aussi
        if (!stop && cancelled_()) stop = cancel = true;
        return stop;
    }
    bool stop = false, cancel = false;

private:
    Flasher& flasher_;
    size_t size_, got_ = 0;
    std::function<void(int)> progress_;
    std::function<bool()> cancelled_;
    mbedtls_sha256_context sha_;
};

void runUpdateMode(Ui& ui) {
    if (WiFi.status() != WL_CONNECTED) {
        ui.showInfo(wr::L("Update", "Mise \xC3\xA0 jour"),
                    wr::L("The Wi-Fi is not connected: cannot receive a firmware.",
                          "Le Wi-Fi n'est pas connect\xC3\xA9 : impossible de recevoir un firmware."),
                    wr::L("Tap the screen to go back", "Touche l'\xC3\xA9" "cran pour revenir"));
        while (!M5.Touch.getDetail().wasPressed()) M5.update(), delay(20);
        return;
    }
    char codeBuf[8];
    snprintf(codeBuf, sizeof codeBuf, "%06lu", (unsigned long)(esp_random() % 1000000UL));
    const String code = codeBuf;
    const String url = "http://" + WiFi.localIP().toString();

    WebServer server(80);
    int wrongCodes = 0;
    uint32_t retryAt = millis();  // pas d'essai examiné avant cet instant
    bool waited = false;          // dernier essai refusé sans examen (attente en cours)
    bool accepted = false;        // code juste pour le transfert en cours
    bool done = false;
    String result;
    int lastPercent = -2;
    Flasher flasher;
    size_t expected = 0;
    uint32_t lastDraw = 0;
    GithubOffer offer;           // recherche sur GitHub, lancée à l'ouverture
    bool downloading = false;    // téléchargement depuis GitHub en cours : ni ligne GitHub ni bouton Installer
    offer.state = GithubOffer::Checking;
    startGithubCheck();

    auto draw = [&](int percent, const String& status) {
        ui.showUpdate(url, code, FW_VERSION, percent, status, downloading ? GithubOffer() : offer);
        lastPercent = percent;
        lastDraw = millis();
    };

    server.on("/", HTTP_GET, [&] {
        logEvent("Mise \xC3\xA0 jour : page ouverte par un navigateur");
        server.send(200, "text/html; charset=utf-8", updatePage());
    });
    server.on(
        "/update", HTTP_POST,
        // Fin de la requête : réponse au navigateur
        [&] {
            if (!accepted) {
                server.send(403, "text/plain; charset=utf-8",
                            waited ? wr::L("Too many wrong codes: wait a few seconds, then try again.",
                                           "Trop de codes incorrects : attends quelques secondes, puis r\xC3\xA9" "essaie.")
                                   : wr::L("Wrong code.", "Code incorrect."));
                return;
            }
            accepted = false;  // la requête suivante doit redonner le code
            server.send(done ? 200 : 400, "text/plain; charset=utf-8", result);
        },
        // Morceaux du fichier
        [&] {
            HTTPUpload& up = server.upload();
            if (up.status == UPLOAD_FILE_START) {
                // Après un code faux, le suivant n'est examiné qu'après une attente qui double (1 s, 2 s… 32 s au
                // plus) : environ 25 essais en 10 minutes pour un million de codes. Un autre appareil du réseau
                // ne peut plus fermer le mode mise à jour avec cinq envois faux.
                waited = (int32_t)(millis() - retryAt) < 0;
                accepted = !waited && sameCode(server.arg("code"), code);
                done = false;
                result = "";
                flasher = Flasher();
                expected = server.clientContentLength() > 0 ? server.clientContentLength() : 0;
                if (waited) return;  // ni compté ni affiché : l'attente ne s'allonge pas
                if (!accepted) {
                    ++wrongCodes;
                    retryAt = millis() + (1000UL << std::min(wrongCodes - 1, 5));
                    draw(-1, String(wr::L("Wrong code received (", "Code incorrect re\xC3\xA7u (")) + String(wrongCodes) +
                                 ")");
                    return;
                }
                draw(0, wr::L("Receiving the firmware\xE2\x80\xA6", "R\xC3\xA9" "ception du firmware\xE2\x80\xA6"));
            } else if (up.status == UPLOAD_FILE_WRITE && accepted && result.length() == 0) {
                if (!flasher.feed(up.buf, up.currentSize)) {
                    result = flasher.error;
                    return;
                }
                int pct = expected ? (int)(flasher.received() * 100 / expected) : -1;
                if (pct != lastPercent && millis() - lastDraw > 250)
                    draw(pct, wr::L("Receiving the firmware\xE2\x80\xA6", "R\xC3\xA9" "ception du firmware\xE2\x80\xA6"));
            } else if (up.status == UPLOAD_FILE_END && accepted && result.length() == 0) {
                if (flasher.finish()) {
                    done = true;
                    result = wr::L("Firmware installed: the Tab5 restarts.", "Firmware install\xC3\xA9 : le Tab5 red\xC3\xA9marre.");
                } else {
                    result = flasher.error;
                }
            } else if (up.status == UPLOAD_FILE_ABORTED) {
                flasher.abort();
                result = wr::L("Transfer interrupted", "Transfert interrompu");
            }
        });

    // Installer : la version trouvée sur GitHub, téléchargée et écrite au passage, vérifiée (taille, SHA-256) avant
    // d'être rendue active. Annuler pendant le téléchargement garde le firmware actuel.
    auto installFromGithub = [&] {
        const wr::ReleaseInfo info = g_check.info;
        const String what = String(wr::L("Downloading ", "T\xC3\xA9l\xC3\xA9" "chargement de ")) + info.tag.c_str() +
                            wr::L(" from GitHub\xE2\x80\xA6", " depuis GitHub\xE2\x80\xA6");
        logEvent("Mise \xC3\xA0 jour GitHub : t\xC3\xA9l\xC3\xA9" "chargement de %s (%ld octets)", info.tag.c_str(), info.size);
        downloading = true;
        result = "";
        draw(0, what);
        Flasher gh;
        GithubSink sink(
            gh, (size_t)info.size,
            [&](int pct) {
                if (pct != lastPercent && millis() - lastDraw > 250) draw(pct, what);
            },
            [&] {
                M5.update();
                auto t = M5.Touch.getDetail();
                wr::KeyEvent k;
                bool key = false;
                while (takeKey(k)) key = key || k.key == wr::NavKey::Back;
                return key || (t.wasPressed() && ui.hitUpdateCancel(t.x, t.y));
            });
        String note;
        const bool ok = ghrelease::download(info.url, sink, (size_t)info.size, note);
        downloading = false;
        if (sink.cancel) {
            gh.abort();
            result = wr::L("Download cancelled: nothing changed.", "T\xC3\xA9l\xC3\xA9" "chargement annul\xC3\xA9 : rien n'a chang\xC3\xA9.");
        } else if (gh.error.length()) {
            result = gh.error;
        } else if (!ok) {
            gh.abort();
            result = String(wr::L("Download failed (", "T\xC3\xA9l\xC3\xA9" "chargement impossible (")) + note +
                     wr::L("): try again.", ") : r\xC3\xA9" "essaie.");
        } else if (!sink.complete(info.sha256)) {
            gh.abort();
            result = wr::L("The downloaded file is damaged (checksum): try again.",
                           "Le fichier t\xC3\xA9l\xC3\xA9" "charg\xC3\xA9 est ab\xC3\xAEm\xC3\xA9 (empreinte) : r\xC3\xA9" "essaie.");
        } else if (gh.finish()) {
            done = true;
            result = String(wr::L("Installed ", "Install\xC3\xA9 : ")) + info.tag.c_str() +
                     wr::L(". The Tab5 restarts.", ". Le Tab5 red\xC3\xA9marre.");
        } else {
            result = gh.error;
        }
        logEvent("Mise \xC3\xA0 jour GitHub : %s", done ? "install\xC3\xA9" "e, red\xC3\xA9marrage" : result.c_str());
        draw(-1, result);
    };

    server.begin();
    logEvent("Mise \xC3\xA0 jour : \xC3\xA9" "cran ouvert, sockets libres %d/%d, m\xC3\xA9moire interne %u (bloc %u), en marche depuis %lu min",
             freeSockets(), CONFIG_LWIP_MAX_SOCKETS, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned long)(millis() / 60000));
    draw(-1, "");

    const uint32_t start = millis();
    for (;;) {
        server.handleClient();
        M5.update();
        auto t = M5.Touch.getDetail();
        if (done) {
            draw(100, result);
            delay(1500);  // laisse partir la réponse au navigateur
            ESP.restart();
        }
        if (offer.state == GithubOffer::Checking && g_check.done) {
            if (g_check.ok) {
                const wr::ReleaseOffer o = wr::releaseOffer(FW_VERSION, g_check.info.tag);
                offer.tag = g_check.info.tag.c_str();
                offer.newer = o == wr::ReleaseOffer::Newer;
                offer.state = o == wr::ReleaseOffer::Newer || o == wr::ReleaseOffer::Unknown ? GithubOffer::Available
                                                                                            : GithubOffer::UpToDate;
                logEvent("Mise \xC3\xA0 jour GitHub : derni\xC3\xA8re version %s (install\xC3\xA9" "e : %s)",
                         g_check.info.tag.c_str(), FW_VERSION);
            } else {
                offer.state = GithubOffer::Failed;
                offer.note = g_check.note;
                logEvent("Mise \xC3\xA0 jour GitHub : injoignable (%s)", g_check.note.c_str());
            }
            draw(lastPercent >= 0 ? lastPercent : -1, result);
        }
        if (result.length() && lastPercent != -1 && millis() - lastDraw > 400) draw(-1, result);
        if (t.wasPressed() && offer.state == GithubOffer::Available && ui.hitUpdateInstall(t.x, t.y)) {
            installFromGithub();
            continue;
        }
        wr::KeyEvent k;
        bool keyCancel = false;  // OK ou Retour (télécommande, clavier) : comme Annuler ; jamais sur le Tab5
        while (takeKey(k)) keyCancel = keyCancel || k.key == wr::NavKey::Ok || k.key == wr::NavKey::Back;
        if ((t.wasPressed() && ui.hitUpdateCancel(t.x, t.y)) || keyCancel || millis() - start > 10UL * 60 * 1000)
            break;
        delay(2);
    }
    flasher.abort();
    server.stop();
}
