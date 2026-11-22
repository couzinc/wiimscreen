// Bibliothèque : toutes les tuiles (presets, favoris, albums et listes des comptes) en grille, avec des filtres par
// sorte de tuile. Les tuiles notées d'abord (plus d'étoiles d'abord), puis l'ordre de la bande (épinglées, dernières
// lancées). Appuis et appui long : main.cpp
// (activateLibrary, serviceLibraryTouch), qui partagent playPreset() et le menu de la tuile avec la bande.

#include "app/app_internal.h"
#include "app/log.h"

namespace app {

void renderLibrary(int pressed) {
    // Filtres proposés : « Tout », puis chaque sorte présente (pas de pastille vide)
    bool present[(int)wr::TileKind::Count] = {};
    for (auto& p : g_tiles.list) present[(int)wr::tileKind(p)] = true;
    std::vector<int> filters = {-1};
    for (int k = 0; k < (int)wr::TileKind::Count; ++k)
        if (present[k]) filters.push_back(k);
    if (g_lib.filter >= 0 && !present[g_lib.filter]) g_lib.filter = -1;  // sorte disparue (favori retiré…)
    std::vector<int> picked;
    for (size_t i = 0; i < g_tiles.list.size(); ++i)
        if (g_lib.filter < 0 || (int)wr::tileKind(g_tiles.list[i]) == g_lib.filter) picked.push_back((int)i);
    // Notées d'abord (plus d'étoiles d'abord), puis les autres dans l'ordre de la bande
    std::vector<std::string> keys;
    for (int i : picked) keys.push_back(wr::tileKey(g_tiles.list[i]));
    std::vector<int> shown;
    for (size_t k : wr::ratingOrder(keys, g_tiles.stats)) shown.push_back(picked[k]);
    std::vector<int> stars(g_tiles.list.size());
    for (size_t i = 0; i < g_tiles.list.size(); ++i) stars[i] = g_tiles.stats.rating(wr::tileKey(g_tiles.list[i]));
    g_ui.setLibraryStars(std::move(stars));
    g_lib.page = g_ui.renderLibrary(shown, filters, g_lib.filter, g_lib.page, pressed, g_lib.follow);
    g_lib.follow = -1;
}

void openLibrary() {
    logEvent("Biblioth\xC3\xA8que : %u tuile(s)", (unsigned)g_tiles.list.size());
    g_view.screen = Screen::Library;
    g_lib.touch = -1;
    g_focus.spot[(int)Screen::Library] = wr::FocusSpot{};
    renderLibrary();
}

}  // namespace app
