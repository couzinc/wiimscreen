// Bibliothèque : toutes les tuiles (presets, favoris, albums des comptes) en grille, avec des filtres par sorte de
// tuile ; filtre Qobuz / Deezer : les morceaux favoris du compte au-dessus des albums (pas une tuile). Même ordre que la bande (épinglées, dernières lancées). Appuis et appui long : main.cpp
// (activateLibrary, serviceLibraryTouch), qui partagent playPreset() et le menu de la tuile avec la bande.

#include "app/app_internal.h"
#include "app/log.h"
#include "i18n.h"
#include "storage/favorites.h"

namespace app {

void renderLibrary(int pressed) {
    // Filtres proposés : « Tout », puis chaque sorte présente (pas de pastille vide)
    bool present[(int)wr::TileKind::Count] = {};
    for (auto& p : g_tiles.list) present[(int)wr::tileKind(p)] = true;
    // Listes des morceaux favoris des comptes : pas des tuiles, la section du filtre Qobuz / Deezer (le filtre existe
    // même sans album favori)
    const int kQ = (int)wr::TileKind::Qobuz, kD = (int)wr::TileKind::Deezer;
    const bool qList = g_tiles.favOk && fav::hasQobuzModel();
    const bool dList = g_tiles.favOk && g_settings.deezerUser.length() && fav::hasDeezerModel();
    present[kQ] |= qList;
    present[kD] |= dList;
    std::vector<int> filters = {-1};
    for (int k = 0; k < (int)wr::TileKind::Count; ++k)
        if (present[k]) filters.push_back(k);
    if (g_lib.filter >= 0 && !present[g_lib.filter]) g_lib.filter = -1;  // sorte disparue (favori retiré…)
    std::vector<int> shown;
    for (size_t i = 0; i < g_tiles.list.size(); ++i)
        if (g_lib.filter < 0 || (int)wr::tileKind(g_tiles.list[i]) == g_lib.filter) shown.push_back((int)i);
    Ui::LibFavSection sec;
    const bool withList = (g_lib.filter == kQ && qList) || (g_lib.filter == kD && dList);
    if (withList) {
        const bool deezer = g_lib.filter == kD;
        const SearchKind kind = deezer ? SearchKind::DeezerFavorites : SearchKind::QobuzFavorites;
        sec.title = wr::L("Favorite tracks", "Morceaux favoris");
        const bool current = g_search.resultsKind == kind;  // la liste lue en dernier est celle-ci
        // Lue à l'ouverture de la Bibliothèque ou à l'appui sur le filtre, jamais à un simple rafraîchissement (image
        // arrivée…) : sans accès direct, la lecture passe par une file du WiiM (son coupé un moment)
        if (g_lib.favReload) {
            g_lib.favReload = false;
            if ((!current || g_search.results.empty()) && !g_jobs.search) loadFavoriteList(deezer, false);
        }
        if (g_search.resultsKind == kind && g_jobs.search)
            sec.note = deezer ? wr::L("Loading your Deezer favorites\xE2\x80\xA6", "Chargement de tes favoris Deezer\xE2\x80\xA6")
                              : wr::L("Loading your Qobuz favorites\xE2\x80\xA6", "Chargement de tes favoris Qobuz\xE2\x80\xA6");
        else if (g_search.resultsKind != kind)
            sec.note = g_jobs.search ? wr::L("Search in progress, touch the filter again in a moment",
                                             "Recherche en cours, retouche le filtre dans un instant")
                                     : wr::L("Touch the filter to show your favorite tracks",
                                             "Touche le filtre pour voir tes morceaux favoris");
        else if (!g_search.results.empty())
            sec.tracks = &g_search.results;
        else if (g_search.status.length())
            sec.note = g_search.status;
        else
            sec.note = String(wr::L("No favorite track on ", "Aucun morceau favori sur ")) + (deezer ? "Deezer" : "Qobuz");
    }
    g_lib.page = g_ui.renderLibrary(shown, filters, g_lib.filter, g_lib.page, pressed, withList ? &sec : nullptr);
}

void openLibrary() {
    logEvent("Biblioth\xC3\xA8que : %u tuile(s)", (unsigned)g_tiles.list.size());
    g_view.screen = Screen::Library;
    g_lib.touch = -1;
    g_lib.favReload = true;  // filtre Qobuz / Deezer gardé : ses morceaux favoris relus
    g_focus.spot[(int)Screen::Library] = wr::FocusSpot{};
    renderLibrary();
}

}  // namespace app
