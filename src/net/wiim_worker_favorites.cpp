// Favoris (copies de files du WiiM, rejouées plus tard), tuile Flow automatique, lecture d'un album à un morceau
// (playAlbumAt). Partie de la classe WiimWorker (wiim_worker.h).
#include "net/wiim_worker.h"

#include "decisions.h"
#include "storage/favorites.h"
#include "i18n.h"
#include "app/log.h"
#include "qobuz.h"
#include "upnp.h"
#include "voice.h"
#include "net/wiim_worker_internal.h"

using namespace wiimq;

// ---------------------------------------------------------------------------
// Favoris : copie de la file en cours, rejouée plus tard

static bool sameText(const std::string& a, const std::string& b) {
    return !a.empty() && wr::normalizeText(a) == wr::normalizeText(b);
}

void WiimWorker::runSaveFavorite(const FavRequest& r) {
    WiimEvent ev;
    ev.kind = WiimEvent::FavSaved;
    std::string control;
    if (!findPlayQueue(control, false)) {
        ev.text = wr::L("the WiiM does not share its play queue", "le WiiM ne donne pas sa file de lecture");
        push(std::move(ev));
        return;
    }
    // La file qui contient le morceau en cours ; à défaut, la première trouvée
    std::string best, fallback;
    for (auto& n : queueNames(control, false)) {
        std::string ctx;
        if (browseQueue(control, n, ctx) != 200 || ctx.empty()) continue;
        wr::QueueInfo q = wr::parseQueueContext(ctx);
        if (!q.ok || (q.tracks == 0 && q.searchUrl.empty())) continue;
        if (fallback.empty()) fallback = ctx;
        for (auto& t : q.titles)
            if (sameText(t, r.title.c_str())) {
                best = ctx;
                break;
            }
        if (!best.empty()) break;
    }
    if (best.empty()) best = fallback;
    if (best.empty()) {
        ev.text = wr::L("no queue on the WiiM (start the album from the WiiM Home app)", "aucune file sur le WiiM (lance l'album depuis l'app WiiM Home)");
        push(std::move(ev));
        return;
    }
    wr::QueueInfo q = wr::parseQueueContext(best);
    if (q.sourceName == "Qobuz") fav::saveQobuzTemplate(best);
    wr::Favorite f;
    f.key = wr::queueKey(q);
    f.title = r.album.length() ? r.album.c_str() : q.listName;
    f.artist = r.artist.c_str();
    f.source = q.sourceName;
    f.artUrl = r.artUrl.c_str();
    f.track = r.title.c_str();  // la lecture du favori reprendra à ce morceau (pas pour un Flow : liveFavorite)
    if (wr::isLiveQueue(q)) {
        liveFavorite(q, best, f);
    } else if (!q.searchUrl.empty()) {
        // Gardée sans ses pistes, comme les tuiles d'album : le WiiM les relit depuis SearchUrl à la lecture
        // (runPlayFavorite). Une file de recherche Qobuz monte à 1000 morceaux, 1,35 Mo : la partition n'en tenait
        // que deux ou trois. Sans SearchUrl (clé USB, DLNA…), les pistes restent dans la copie (taille limitée).
        std::string small = wr::queueFromTemplate(best, q.listName, q.searchUrl);
        if (!small.empty()) best.swap(small);
    }
    std::string err;
    ev.ok = fav::add(f, best, err);
    ev.text = ev.ok ? f.title.c_str() : err.c_str();
    logEvent("Favori \xC2\xAB %s \xC2\xBB (%s, %d piste(s), %u octets) : %s", f.title.c_str(), q.sourceName.c_str(),
             q.declaredTracks >= 0 ? q.declaredTracks : q.tracks, (unsigned)best.size(), ev.ok ? "OK" : err.c_str());
    push(std::move(ev));
}

// Flow Deezer, mix : nom et image de la liste (pas l'album du morceau du moment), gardée sans ses pistes pour que
// le WiiM en donne de nouvelles à chaque lecture
void WiimWorker::liveFavorite(const wr::QueueInfo& q, std::string& ctx, wr::Favorite& f) {
    const bool ours = q.listName.empty() || q.listName.rfind("wiimscreen", 0) == 0;
    f.key = wr::queueKey(q);
    f.title = ours ? wr::L("Deezer mix", "Mix Deezer") : q.listName;
    f.artist.clear();
    f.track.clear();  // de nouvelles pistes à chaque lecture : pas de morceau où reprendre
    f.source = q.sourceName;
    if (!q.picUrl.empty()) f.artUrl = q.picUrl;
    ctx = wr::queueFromTemplate(ctx, q.listName, q.searchUrl);
}

// Flow vu sur le WiiM (lancé depuis WiiM Home) : tuile Flow créée toute seule, une fois (retirée : pas remise).
// Marquée seulement une fois ajoutée ; un ajout refusé (24 favoris, mémoire pleine) reste silencieux (personne n'a
// rien demandé) et n'est pas retenté avant le prochain démarrage.
void WiimWorker::autoFlowTile(const wr::QueueInfo& q, const std::string& current) {
    const std::string key = wr::queueKey(q), tag = "flow " + key;
    if (key == flowTried_) return;
    for (auto& f : fav::list())
        if (f.key == key) return;  // déjà une tuile (cœur ou ajout automatique)
    if (fav::doneOnce(tag)) return;
    flowTried_ = key;
    std::string ctx = current, err;
    wr::Favorite f;
    liveFavorite(q, ctx, f);
    const bool ok = fav::add(f, ctx, err);
    logEvent("Flow vu sur le WiiM : tuile \xC2\xAB %s \xC2\xBB ajout\xC3\xA9" "e automatiquement : %s", f.title.c_str(),
             ok ? "OK" : err.c_str());
    if (!ok) return;
    fav::markOnce(tag);
    WiimEvent ev;
    ev.kind = WiimEvent::FavSaved;
    ev.automatic = true;  // pas la réponse à un appui sur le cœur : l'enregistrement en cours éventuel continue
    ev.ok = true;
    ev.text = f.title.c_str();
    push(std::move(ev));
}

// File d'un album (le WiiM lit les pistes depuis `searchUrl`), lancée au morceau `title` (trouvé dans la file
// relue : l'ordre peut être mélangé) ; le premier morceau si `title` est vide ou introuvable.
bool WiimWorker::playAlbumAt(const std::string& control, const std::string& templ, const std::string& url,
                             const std::string& title, const char* label, bool randomStart) {
    const std::string name = std::string("wiimscreen-album-") + std::to_string(++searchCount_ % 2);
    deleteQueue(control, name);
    const int create = createQueue(control, wr::queueFromTemplate(templ, name, url));
    int index = 0, lastCount = -1;
    const uint32_t t0 = millis();
    if (create == 200) {
        while (index == 0 && millis() - t0 < 12000) {
            pause(700, true);  // les appuis et le relevé continuent pendant le chargement
            std::string back;
            if (browseQueue(control, name, back) != 200 || back.empty()) continue;
            const wr::QueueInfo q = wr::parseQueueContext(back, wr::kQueueTitlesMax);
            if (title.empty() && !randomStart) index = q.tracks > 0 ? 1 : 0;
            else if (title.empty()) {  // au hasard dans toute la file : le WiiM la remplit par étapes, on attend
                if (q.tracks > 0 && (q.tracks == lastCount || millis() - t0 > 6000))  // deux relevés égaux
                    index = 1 + (int)(esp_random() % (uint32_t)q.tracks);
                lastCount = q.tracks;
            } else index = wr::queueIndexOf(q, title, true);
            if (index == 0 && q.tracks > 0 && millis() - t0 > 6000) index = 1;  // pas trouvé : début de l'album
        }
    }
    int play = index > 0 ? playQueueAt(control, name, index) : -1;
    // Lecture aléatoire : le WiiM remélange la file peu après CreateQueue et PlayQueueWithIndex suit le nouvel ordre
    // (vu sur un WiiM, 2026-11-17 : le résultat 1 a lancé un autre morceau). Titre vérifié, place relue, nouvel essai.
    int tries = 1;
    for (; play == 200 && !title.empty() && tries < 3; ++tries) {
        pause(1500, true);
        wr::TrackMeta m;
        if (client_.metaInfo(m) != WiimError::None || m.title.empty() ||
            wr::normalizeText(m.title) == wr::normalizeText(title))
            break;
        std::string back;
        if (browseQueue(control, name, back) != 200 || back.empty()) break;
        const int k = wr::queueIndexOf(wr::parseQueueContext(back, wr::kQueueTitlesMax), title, true);
        if (k == 0) break;
        index = k;
        play = playQueueAt(control, name, index);
    }
    logTagged("play.result", "create=%d play=%d index=%d tries=%d | "
              "%s \xE2\x96\xB6 \xC2\xAB %s \xC2\xBB (piste %d, essai %d) : CreateQueue %d, Play %d (%lu ms)",
              create, play, index, tries, label, title.c_str(), index, tries, create, play,
              (unsigned long)(millis() - t0));
    nextPoll_ = millis() + 800;
    return play == 200;
}

// Album choisi dans la recherche → favori : file de l'album (modèle Qobuz du WiiM), sans lecture
void WiimWorker::runSaveAlbumFavorite(const AlbumFavRequest& r) {
    WiimEvent ev;
    ev.kind = WiimEvent::FavSaved;
    std::string control, templ;
    if (!findPlayQueue(control, false) || !qobuzModel(control, templ)) {
        ev.text = wr::L("the WiiM does not provide a Qobuz queue template", "le WiiM ne donne pas de mod\xC3\xA8le de file Qobuz");
        push(std::move(ev));
        return;
    }
    const std::string ctx = wr::queueFromTemplate(templ, "wiimscreen-album-" + std::string(r.id.c_str()),
                                                  wr::qobuzAlbumUrl(r.id.c_str()));
    wr::Favorite f;
    f.key = wr::qobuzAlbumKey(r.id.c_str());
    f.title = r.title.c_str();
    f.artist = r.artist.c_str();
    f.source = "Qobuz";
    f.artUrl = r.image.c_str();
    std::string err;
    ev.ok = !ctx.empty() && fav::add(f, ctx, err);
    if (ctx.empty()) err = wr::L("unreadable Qobuz queue template", "mod\xC3\xA8le de file Qobuz illisible");
    ev.text = ev.ok ? f.title.c_str() : err.c_str();
    logEvent("Favori \xC2\xAB %s \xC2\xBB (album Qobuz depuis la recherche) : %s", f.title.c_str(), ev.ok ? "OK" : err.c_str());
    push(std::move(ev));
}

// Chargement d'un favori : CreateQueue lance la file à son début avant que le bon morceau soit choisi. Son coupé
// de là au morceau voulu, noté comme pour une recherche (undoInterruptedSearch le rétablit si le Tab5 redémarre
// entre-temps). Rien n'est coupé si une note d'avant n'est pas encore défaite ou si l'état est illisible.
bool WiimWorker::favMuteBegin() {
    wr::PlayerStatus st;
    if (!undoChecked_ || client_.status(st) != WiimError::None || st.muted) return false;
    if (client_.command("setPlayerCmd:mute:1") != WiimError::None) {
        client_.command("setPlayerCmd:mute:0");  // au cas où la commande serait passée
        return false;
    }
    Playback before;
    before.known = true;
    before.loop = st.loop;
    noteSearchChanges(before, true, false);
    return true;
}

void WiimWorker::favMuteEnd(bool muted) {
    if (!muted) return;
    pause(600, true);  // le morceau choisi a démarré
    const WiimError e = client_.command("setPlayerCmd:mute:0");
    logTagged("fav.unmute", "result=%s | Favori : son r\xC3\xA9tabli (%s)", errorKey(e), wiimErrorName(e));
    if (e == WiimError::None) {
        fav::saveSearchUndo("");
    } else {
        undoKeepsPlayback_ = true;  // au prochain relevé : rendre le son sans arrêter le favori
        undoChecked_ = false;
    }
}

// Favori choisi : la musique d'avant est déjà en pause (main.cpp) ; son coupé pendant le chargement
void WiimWorker::runPlayFavorite(const String& id) {
    if (id.startsWith("qobuz-favoris:") || id.startsWith("deezer-favoris:")) {
        runPlayFavoriteList(id.startsWith("deezer"), id.endsWith(":shuffle"));
        return;
    }
    const bool muted = favMuteBegin();
    runPlayFavoriteNow(id);
    favMuteEnd(muted);
}

void WiimWorker::runPlayFavoriteNow(const String& id) {
    if (id.startsWith("qobuz-album:")) {
        runPlayQobuzAlbum(id.substring(12).c_str());
        return;
    }
    if (id.startsWith("deezer-album:")) {
        runPlayDeezerAlbum(id.substring(13).c_str());
        return;
    }
    WiimEvent ev;
    ev.kind = WiimEvent::FavPlayed;
    std::string ctx, control;
    if (!fav::context(id.c_str(), ctx)) {
        ev.text = wr::L("favorite not found", "favori introuvable");
    } else if (!findPlayQueue(control, false)) {
        ev.text = wr::L("the WiiM does not accept a play queue", "le WiiM n'accepte pas de file de lecture");
    } else {
        wr::QueueInfo q = wr::parseQueueContext(ctx);
        const uint32_t t0 = millis();
        int c = 0, p = -1, loaded = q.tracks;
        if (wr::isLiveQueue(q) && q.tracks > 0) {  // Flow enregistré avec sa piste du moment : file sans pistes
            ctx = wr::queueFromTemplate(ctx, q.listName, q.searchUrl);
            loaded = 0;
        }
        std::string name = q.listName, track;  // nom affiché : titre de la tuile, sinon nom de la file
        for (auto& f : fav::list())
            if (f.id == id.c_str()) {
                if (!f.title.empty()) name = f.title;
                track = f.track;
            }
        if (wr::isLiveQueue(q)) track.clear();
        // Lancé au morceau qui jouait à l'enregistrement, cherché par son titre dans la file relue (l'ordre peut
        // différer de la copie, lecture aléatoire) ; au premier pour un favori sans morceau noté (plus ancien,
        // album de la recherche). Album sans pistes : le WiiM les lit depuis SearchUrl, on attend qu'il en ait ;
        // les appuis (volume…) restent traités pendant l'attente.
        c = createQueue(control, ctx);
        int index = 0;
        for (bool first = true; c == 200 && index == 0 && millis() - t0 < 10000; first = false) {
            if (track.empty() && (loaded > 0 || q.searchUrl.empty())) {
                index = 1;  // pistes dans la copie : lecture tout de suite
                break;
            }
            if (!first || loaded == 0) pause(500, true);
            std::string back;
            if (browseQueue(control, q.listName, back) != 200 || back.empty()) continue;
            const wr::QueueInfo b = wr::parseQueueContext(back, wr::kQueueTitlesMax);
            loaded = b.tracks;
            if (b.tracks == 0) continue;
            index = track.empty() ? 1 : wr::queueIndexOf(b, track, true);
            if (index == 0 && millis() - t0 > 6000) index = 1;  // morceau pas trouvé : début de la file
        }
        if (c == 200) p = playQueueAt(control, q.listName, index > 0 ? index : 1);
        // Le WiiM peut remélanger la file juste après CreateQueue : titre vérifié, place relue, un nouvel essai
        if (p == 200 && !track.empty()) {
            pause(1500, true);
            wr::TrackMeta m;
            std::string back;
            if (client_.metaInfo(m) == WiimError::None && !m.title.empty() &&
                wr::normalizeText(m.title) != wr::normalizeText(track) &&
                browseQueue(control, q.listName, back) == 200 && !back.empty()) {
                const int k = wr::queueIndexOf(wr::parseQueueContext(back, wr::kQueueTitlesMax), track, true);
                if (k > 0) p = playQueueAt(control, q.listName, index = k);
            }
        }
        logTagged("fav.play", "create=%d tracks=%d play=%d index=%d | "
                  "Favori \xE2\x96\xB6 \xC2\xAB %s \xC2\xBB (\xC2\xAB %s \xC2\xBB, piste %d) : CreateQueue %d, %d piste(s), "
                  "PlayQueueWithIndex %d (%lu ms)",
                  c, loaded, p, index, name.c_str(), track.c_str(), index, c, loaded, p, (unsigned long)(millis() - t0));
        ev.ok = c == 200 && p == 200;
        if (!ev.ok) ev.text = wr::L("the WiiM refused the queue", "le WiiM a refus\xC3\xA9 la file");
    }
    push(std::move(ev));
    nextPoll_ = millis() + 800;  // relire l'état peu après
}
