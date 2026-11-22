// Ui : création des canevas, fond (lueur de l'accent), envoi à l'écran, polices, pochettes.
// Les vues sont dans ui.cpp (commandes, ambiance), ui_search.cpp, ui_screens.cpp ; les petits
// éléments partagés dans ui_widgets.cpp. Constantes et aides communes : ui_internal.h.
#include "ui/ui_internal.h"

#include "ui/fonts/font_ui_24.h"
#include "ui/fonts/font_ui_28.h"
#include "ui/fonts/font_ui_40.h"
#include "ui/fonts/font_ui_56.h"

static auto& D = M5.Display;

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include <driver/ppa.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <esp_cache.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstring>

#include "app/log.h"

// Tab5 : image entière copiée dans la mémoire d'image de l'écran par le PPA (accélérateur graphique du P4 : rotation,
// ordre des octets) en quelques millisecondes, au lieu de M5GFX qui la tourne pixel par pixel pendant que l'écran
// la lit (balayage visible). Le réglage du PPA n'est pas deviné : la première image complète passe par M5GFX, puis le
// PPA refait la même image dans un tampon à part avec chaque réglage possible ; un seul doit donner exactement les
// octets de M5GFX. Sinon (image trop symétrique : on réessaie à l'image suivante ; aucun réglage juste ou PPA
// indisponible : M5GFX reste), rien ne change pour l'écran.
namespace {
struct FastPush {
    int state = 0;  // 0 : à régler, 1 : prêt, -1 : M5GFX seulement
    int tries = 0;  // images examinées pour le réglage
    ppa_client_handle_t client = nullptr;
    void* fb = nullptr;  // mémoire d'image de l'écran (portrait natif)
    uint32_t fbW = 0, fbH = 0, fbBytes = 0;
    int cw = 0, ch = 0;  // taille du canevas au réglage (une autre orientation : nouveau réglage)
    ppa_srm_rotation_angle_t angle = PPA_SRM_ROTATION_ANGLE_0;
    bool mirror = false, swap = false;
};
FastPush g_fast;

esp_err_t ppaCopy(M5Canvas& c, void* out, ppa_srm_rotation_angle_t angle, bool mirror, bool swap) {
    ppa_srm_oper_config_t op = {};
    op.in.buffer = c.getBuffer();
    op.in.pic_w = op.in.block_w = (uint32_t)c.width();
    op.in.pic_h = op.in.block_h = (uint32_t)c.height();
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = out;
    op.out.buffer_size = g_fast.fbBytes;
    op.out.pic_w = g_fast.fbW;
    op.out.pic_h = g_fast.fbH;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.rotation_angle = angle;
    op.scale_x = op.scale_y = 1.0f;
    op.mirror_x = mirror;
    op.byte_swap = swap;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(g_fast.client, &op);
    esp_cache_msync(out, g_fast.fbBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);  // le processeur relira la mémoire, pas son cache
    return err;
}

// Prépare le PPA (une fois) ; faux si le Tab5 n'en a pas l'usage (autre carte, pas de mémoire d'image)
bool fastInit() {
    if (g_fast.client) return true;
    const auto board = M5.getBoard();
    if (board != m5::board_t::board_M5Tab5 && board != m5::board_t::board_M5Tab5X) return false;
    auto* panel = static_cast<lgfx::Panel_DSI*>(D.getPanel());  // ILI9881C ou ST7123 : tous deux des Panel_DSI
    g_fast.fb = panel ? panel->config_detail().buffer : nullptr;
    if (!g_fast.fb) return false;
    g_fast.fbW = panel->config().panel_width;
    g_fast.fbH = panel->config().panel_height;
    g_fast.fbBytes = g_fast.fbW * g_fast.fbH * 2;
    ppa_client_config_t cfg = {};
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;
    return ppa_register_client(&cfg, &g_fast.client) == ESP_OK;
}

// Après une image complète envoyée par M5GFX : cherche le réglage du PPA qui donne les mêmes octets
void fastCalibrate(M5Canvas& c) {
    if (g_fast.state < 0) return;
    if (!fastInit()) {
        g_fast.state = -1;
        logEvent("\xC3\x89" "cran : copie mat\xC3\xA9rielle indisponible, envoi par M5GFX");
        return;
    }
    if ((uint32_t)c.width() * (uint32_t)c.height() != g_fast.fbW * g_fast.fbH) return;
    constexpr uint32_t kAlign = 128;  // lignes des caches L1 et L2
    void* scratch = heap_caps_aligned_alloc(kAlign, (g_fast.fbBytes + kAlign - 1) / kAlign * kAlign, MALLOC_CAP_SPIRAM);
    if (!scratch) return;  // mémoire pleine à cet instant : prochaine image
    static const ppa_srm_rotation_angle_t kAngles[] = {PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
                                                       PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270};
    int found = 0;
    int64_t us = 0;
    FastPush best = g_fast;
    for (int a = 0; a < 4; ++a) {
        const bool quarter = a % 2 == 1;  // 90° / 270° : largeur et hauteur échangées
        if ((uint32_t)(quarter ? c.height() : c.width()) != g_fast.fbW) continue;
        for (int m = 0; m < 2; ++m)
            for (int sw = 0; sw < 2; ++sw) {
                const int64_t t0 = esp_timer_get_time();
                if (ppaCopy(c, scratch, kAngles[a], m, sw) != ESP_OK) continue;
                us = esp_timer_get_time() - t0;
                if (std::memcmp(scratch, g_fast.fb, g_fast.fbBytes) != 0) continue;
                ++found;
                best.angle = kAngles[a];
                best.mirror = m;
                best.swap = sw;
            }
    }
    heap_caps_free(scratch);
    ++g_fast.tries;
    if (found == 1) {
        g_fast.angle = best.angle;
        g_fast.mirror = best.mirror;
        g_fast.swap = best.swap;
        g_fast.cw = c.width();
        g_fast.ch = c.height();
        g_fast.state = 1;
        logEvent("\xC3\x89" "cran : copie mat\xC3\xA9rielle pr\xC3\xAAte (rotation %d\xC2\xB0, miroir %d, octets %d), %d ms par image",
                 (int)best.angle * 90, (int)best.mirror, (int)best.swap, (int)(us / 1000));
    } else if (found == 0 && g_fast.tries >= 3) {
        g_fast.state = -1;
        logEvent("\xC3\x89" "cran : aucun r\xC3\xA9glage de la copie mat\xC3\xA9rielle ne donne la m\xC3\xAAme image, envoi par M5GFX");
    }
    // plusieurs réglages justes (image symétrique) : image suivante
}

// Image complète par le PPA ; faux s'il n'est pas (ou plus) réglé pour ce canevas
bool fastPresent(M5Canvas& c) {
    if (g_fast.state != 1) return false;
    if (c.width() != g_fast.cw || c.height() != g_fast.ch) {  // orientation changée : nouveau réglage
        g_fast.state = 0;
        g_fast.tries = 0;
        return false;
    }
    if (ppaCopy(c, g_fast.fb, g_fast.angle, g_fast.mirror, g_fast.swap) == ESP_OK) return true;
    g_fast.state = -1;
    logEvent("\xC3\x89" "cran : la copie mat\xC3\xA9rielle a \xC3\xA9" "chou\xC3\xA9, envoi par M5GFX");
    return false;
}
}  // namespace
#endif

#ifdef PC_SCALED_FONTS
#include <cmath>

// Ordinateur (simulateur, desktop, Windows) : sur un canevas plus grand que 1280x720, les polices grandissent comme
// la mise en page (sc()), faites depuis DejaVuSans.ttf (platform/pc/pc_fonts.cpp) ; jusqu'à 1280x720, ou sans le
// fichier TTF, les polices intégrées. Le Tab5 n'a pas ce code.
const uint8_t* pcScaledFont(int px);
const uint8_t* kTiny = font_ui_24;
const uint8_t* kSmall = font_ui_28;
const uint8_t* kMid = font_ui_40;
const uint8_t* kBig = font_ui_56;

static void scaleFonts(int w, int h) {
    const int s = std::min(w, h);
    const bool larger = s * 100 >= 720 * 105;  // au moins 5 % de plus que 720
    const struct {
        const uint8_t** font;
        const uint8_t* builtIn;
        int px;
    } kFonts[] = {{&kTiny, font_ui_24, 24}, {&kSmall, font_ui_28, 28}, {&kMid, font_ui_40, 40}, {&kBig, font_ui_56, 56}};
    for (const auto& f : kFonts) {
        const uint8_t* scaled = larger ? pcScaledFont((int)std::lround(f.px * s / 720.0)) : nullptr;
        *f.font = scaled ? scaled : f.builtIn;
    }
}
#else
const uint8_t* const kTiny = font_ui_24;  // noms de presets sur deux lignes
const uint8_t* const kSmall = font_ui_28;
const uint8_t* const kMid = font_ui_40;
const uint8_t* const kBig = font_ui_56;
#endif

// Police chargée sur un canevas : M5GFX relit l'en-tête VLW à chaque loadFont ; on évite de recharger la même
// (le clavier de recherche en chargeait 43 par image). Un seul canevas charge des polices sur l'appareil.
static const lgfx::LGFXBase* s_fontCanvas = nullptr;
static const uint8_t* s_font = nullptr;
void useFont(M5Canvas& C, const uint8_t* f) {
    if (s_fontCanvas == &C && s_font == f) return;
    C.loadFont(f);
    s_fontCanvas = &C;
    s_font = f;
}

void Ui::begin(const Settings& s) {
    D.setRotation(s.rotation);
    ref_ = std::min(D.width(), D.height());
    accent_ = kAccent;
    for (M5Canvas* c : {&canvas_, &bg_}) {
        c->setPsram(true);
        c->setColorDepth(16);
        c->createSprite(D.width(), D.height());
    }
    art_.setPsram(true);
    art_.setColorDepth(16);
    artBig_.setPsram(true);
    artBig_.setColorDepth(16);
    targets_.reserve(64);  // une fois : l'écran de recherche en inscrit une cinquantaine, sans allocation par image
    setPresets(s.presets);
}

void Ui::relayout() {
#ifdef PC_SCALED_FONTS
    scaleFonts(D.width(), D.height());
#endif
    layout_ = wr::computeLayout(D.width(), D.height(), stripCount_);
    if (art_.width() != layout_.art.w || art_.height() != layout_.art.h) {
        art_.deleteSprite();
        art_.createSprite(layout_.art.w, layout_.art.h);
        setArt(artSrc_);  // redécodée à la nouvelle taille
    }
}

void Ui::beginScreen(View view) {
    view_ = view;
    libTiles_.clear();  // la Bibliothèque les remet en se dessinant
    tileStars_ = wr::Rect{};  // le menu d'une tuile la remet en se dessinant
    targets_.clear();  // l'écran qui commence inscrit ses éléments touchables en se dessinant
}

void Ui::presentRect(const wr::Rect& r) {
    // Anneau du focus redessiné s'il touche la zone (elle vient d'être repeinte) ; découpé à la zone
    canvas_.setClipRect(r.x, r.y, r.w, r.h);
    focusRing();
    canvas_.clearClipRect();
    D.startWrite();
    D.setClipRect(r.x, r.y, r.w, r.h);
    canvas_.pushSprite(&D, 0, 0);
    D.clearClipRect();
    D.endWrite();
    ++presents_;
    lastPresentMs_ = millis();
}

void Ui::present() {
    focusRing();  // en dernier, par-dessus l'écran
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    // Tab5 : par le PPA quand il est réglé (l'écran change d'un coup, fastPresent)
    if (fastPresent(canvas_)) {
        ++presents_;
        lastPresentMs_ = millis();
        return;
    }
    // Sinon M5GFX. L'écran lit sa mémoire d'image en continu, et M5GFX ne la vide du cache qu'à la fin de l'écriture
    // (endWrite). En une fois, l'image arrivait donc à l'écran par morceaux dans le désordre (blocs du cache) ;
    // par bandes, chacune vidée aussitôt, elle passe d'un bord à l'autre avec un bord net. Mêmes octets en tout.
    // Bandes dans le sens des lignes de la dalle (portrait 720×1280) : des colonnes quand l'écran est tourné en paysage
    constexpr int kStrips = 16;
    const int W = canvas_.width(), H = canvas_.height();
    const bool columns = W > H;
    const int len = columns ? W : H, step = (len + kStrips - 1) / kStrips;
    for (int p = 0; p < len; p += step) {
        const int n = std::min(step, len - p);
        D.startWrite();
        if (columns) D.setClipRect(p, 0, n, H);
        else D.setClipRect(0, p, W, n);
        canvas_.pushSprite(&D, 0, 0);
        D.clearClipRect();
        D.endWrite();  // vide cette bande du cache : l'écran la montre tout de suite
    }
    fastCalibrate(canvas_);  // réglage du PPA sur cette image (tant qu'il n'est pas fait)
#else
    D.startWrite();
    canvas_.pushSprite(&D, 0, 0);
    D.endWrite();
#endif
    ++presents_;
    lastPresentMs_ = millis();
}

// Lueur de la couleur d'accent en haut, qui se fond dans le fond sombre
uint32_t Ui::bgAt(int y) const {
    const uint32_t top = mix(kBg, accent_, 0.30f);
    float t = std::min(1.0f, y / (canvas_.height() * 0.7f));
    return mix(top, kBg, t);
}

// Dégradé tramé (matrice de Bayer 4×4) : en RGB565, un dégradé sombre aussi doux
// ferait des bandes visibles. Calculé une fois par couleur d'accent.
void Ui::buildBackground() {
    if (bgAccent_ == accent_) return;
    bgAccent_ = accent_;
    static const uint8_t kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    auto* buf = static_cast<uint16_t*>(bg_.getBuffer());
    const int W = bg_.width(), H = bg_.height();
    if (!buf) return;
    for (int y = 0; y < H; ++y) {
        const uint32_t c = bgAt(y);
        const float r = ((c >> 16) & 0xFF) * 31 / 255.0f, g = ((c >> 8) & 0xFF) * 63 / 255.0f,
                    b = (c & 0xFF) * 31 / 255.0f;
        uint16_t row[4];
        for (int k = 0; k < 4; ++k) {
            float d = (kBayer[(y & 3) * 4 + k] + 0.5f) / 16.0f;
            uint16_t v = (uint16_t)(std::min(31, int(r + d)) << 11 | std::min(63, int(g + d)) << 5 |
                                    std::min(31, int(b + d)));
            row[k] = (uint16_t)(v >> 8 | v << 8);  // les canevas 16 bits sont en RGB565 octets inversés
        }
        uint16_t* p = buf + (size_t)y * W;
        for (int x = 0; x < W; ++x) p[x] = row[x & 3];
    }
}

void Ui::background() {
    buildBackground();
    bg_.pushSprite(&canvas_, 0, 0);
}

// ----------------------------------------------------------------------- images

// Décode un JPEG/PNG ajusté au canevas (proportions gardées, centré) sur fond `bg`
bool Ui::decode(M5Canvas& dst, const ArtImage* img, uint32_t bg) {
    dst.fillScreen(bg);
    if (!img || !img->ok()) return false;
    const int w = dst.width(), h = dst.height();
    switch (wr::sniffImage(img->data, img->len)) {
        case wr::ImageType::Jpeg:
            return dst.drawJpg(img->data, img->len, 0, 0, w, h, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        case wr::ImageType::Png:
            return dst.drawPng(img->data, img->len, 0, 0, w, h, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
        default: return false;
    }
}

// Copie un canevas avec coins arrondis (masqués avec la couleur du dessous)
void Ui::roundedPush(M5Canvas& src, int x, int y, int R, bool bgIsGradient, uint32_t bg) {
    src.pushSprite(&canvas_, x, y);
    const int w = src.width(), h = src.height();
    for (int i = 0; i < R; ++i) {
        float dy = R - i - 0.5f;
        int dx = R - (int)std::lround(std::sqrt(float(R * R) - dy * dy));
        if (dx <= 0) continue;
        for (int yy : {y + i, y + h - 1 - i}) {
            uint32_t c = bgIsGradient ? bgAt(yy) : bg;
            canvas_.drawFastHLine(x, yy, dx, c);
            canvas_.drawFastHLine(x + w - dx, yy, dx, c);
        }
    }
}

// Illustration par défaut : deux croches sur carte sombre
void Ui::placeholderArt(M5Canvas& A) {
    const int s = std::min(A.width(), A.height());
    A.fillScreen(kCard);
    const int cx = A.width() / 2, cy = A.height() / 2;
    const uint32_t c = 0x5A5D66u;
    const int rx = s * 11 / 100, ry = s * 8 / 100, t = std::max(3, s * 4 / 100), bt = s * 9 / 100;
    const int h1x = cx - s * 15 / 100, h1y = cy + s * 20 / 100;
    const int h2x = cx + s * 17 / 100, h2y = cy + s * 14 / 100;
    const int s1x = h1x + rx - t, s2x = h2x + rx - t;
    const int top1 = cy - s * 22 / 100, top2 = cy - s * 28 / 100;
    A.fillEllipse(h1x, h1y, rx, ry, c);
    A.fillEllipse(h2x, h2y, rx, ry, c);
    A.fillRect(s1x, top1, t, h1y - top1, c);
    A.fillRect(s2x, top2, t, h2y - top2, c);
    A.fillTriangle(s1x, top1, s2x + t, top2, s2x + t, top2 + bt, c);
    A.fillTriangle(s1x, top1, s2x + t, top2 + bt, s1x, top1 + bt, c);
}

void Ui::setArt(const ArtImage* img) {
    artSrc_ = img;
    artBigValid_ = false;
    if (!decode(art_, img, kBg)) {
        placeholderArt(art_);
        accent_ = kAccent;
        return;
    }
    // Accent : échantillon d'un pixel sur 3 dans chaque direction
    std::vector<uint8_t> px;
    px.reserve((art_.width() / 3 + 1) * (art_.height() / 3 + 1) * 3);
    for (int y = 0; y < art_.height(); y += 3)
        for (int x = 0; x < art_.width(); x += 3) {
            auto c = art_.readPixelRGB(x, y);
            px.push_back(c.R8());
            px.push_back(c.G8());
            px.push_back(c.B8());
        }
    accent_ = wr::accentColor(px.data(), px.size() / 3, kAccent);
}
