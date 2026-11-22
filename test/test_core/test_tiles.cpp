// Historique et ordre des tuiles (lib/core/src/tiles.*)
#include <unity.h>

#include "test_helpers.h"
#include "tiles.h"

using namespace wr;

static std::vector<size_t> order(const std::vector<std::string>& keys, const TileStats& s, bool recent = true) {
    return tileOrder(keys, s, recent);
}

static void test_tile_key() {
    Preset p;
    p.slot = 3;
    TEST_ASSERT_EQUAL_STRING("preset:3", tileKey(p).c_str());
    p.label = "  FIP ";  // preset nommé : la station, pas le numéro
    TEST_ASSERT_EQUAL_STRING("station:FIP", tileKey(p).c_str());
    p.label = "a\tb";  // nom non enregistrable : le numéro
    TEST_ASSERT_EQUAL_STRING("preset:3", tileKey(p).c_str());
    p.favId = "qobuz-album:42";
    TEST_ASSERT_EQUAL_STRING("qobuz-album:42", tileKey(p).c_str());
    p.favId = "a\tb";
    TEST_ASSERT_EQUAL_STRING("", tileKey(p).c_str());
}

static void test_tile_played_and_order() {
    const std::vector<std::string> keys = {"preset:1", "preset:2", "a", "b", "c"};
    TileStats s;
    // Rien de joué : ordre donné
    std::vector<size_t> o = order(keys, s);
    TEST_ASSERT_EQUAL(5, (int)o.size());
    for (size_t i = 0; i < o.size(); ++i) TEST_ASSERT_EQUAL((int)i, (int)o[i]);
    s.played("b");
    s.played("preset:2");
    s.played("b");  // b : la plus récente
    TEST_ASSERT_EQUAL(3u, s.seq);
    TEST_ASSERT_EQUAL(2u, s.find("b")->count);
    o = order(keys, s);
    const size_t want[] = {3, 1, 0, 2, 4};
    for (size_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL((int)want[i], (int)o[i]);
    // Ordre fixe : l'historique ne change rien
    o = order(keys, s, false);
    for (size_t i = 0; i < o.size(); ++i) TEST_ASSERT_EQUAL((int)i, (int)o[i]);
    // Épinglées d'abord, dans l'ordre où on les a épinglées, même jamais jouées et en ordre fixe
    s.setPin("c", true);
    s.setPin("preset:1", true);
    s.setPin("c", true);  // déjà épinglée : place gardée
    o = order(keys, s, false);
    const size_t fixed[] = {4, 0, 1, 2, 3};
    for (size_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL((int)fixed[i], (int)o[i]);
    o = order(keys, s);
    const size_t recent[] = {4, 0, 3, 1, 2};
    for (size_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL((int)recent[i], (int)o[i]);
    s.setPin("c", false);
    TEST_ASSERT_FALSE(s.pinned("c"));
    TEST_ASSERT_TRUE(s.pinned("preset:1"));
    s.setPin("zz", false);  // inconnue : rien
    TEST_ASSERT_NULL(s.find("zz"));
    // Clé illégale : ignorée
    s.played("x\ny");
    TEST_ASSERT_NULL(s.find("x\ny"));
    TEST_ASSERT_EQUAL(3u, s.seq);
}

static void test_tile_stats_text() {
    TileStats s;
    s.played("preset:1");
    s.played("qobuz-album:9");
    s.setPin("fav-2", true);
    const std::string t = tileStatsToText(s);
    TEST_ASSERT_EQUAL_STRING("seq\t2\npreset:1\t1\t1\t0\nqobuz-album:9\t2\t1\t0\nfav-2\t0\t0\t1\n", t.c_str());
    const TileStats r = tileStatsFromText(t);
    TEST_ASSERT_EQUAL(2u, r.seq);
    TEST_ASSERT_EQUAL(3, (int)r.uses.size());
    TEST_ASSERT_EQUAL(1u, r.find("fav-2")->pin);
    TEST_ASSERT_EQUAL(2u, r.find("qobuz-album:9")->last);
    // Lignes abîmées ignorées, doublon ignoré, compteur jamais derrière une entrée, CRLF accepté
    const TileStats b = tileStatsFromText("seq\t1\r\na\t5\t2\t0\r\nb\t-1\t0\t0\nc\t1\t1\nd\t99999999999\t1\t0\na\t7\t7\t7\n\n");
    TEST_ASSERT_EQUAL(5u, b.seq);
    TEST_ASSERT_EQUAL(1, (int)b.uses.size());
    TEST_ASSERT_EQUAL(2u, b.find("a")->count);
    TEST_ASSERT_EQUAL(0, (int)tileStatsFromText("").uses.size());
}

static void test_tile_stats_full() {
    TileStats s;
    s.setPin("pinned", true);
    s.setRating("rated", 2);
    for (size_t i = 0; i < TileStats::kMax + 10; ++i) s.played("k" + std::to_string(i));
    TEST_ASSERT_EQUAL((int)TileStats::kMax, (int)s.uses.size());
    TEST_ASSERT_TRUE(s.pinned("pinned"));  // épinglée : jamais retirée
    TEST_ASSERT_NULL(s.find("k0"));        // les plus anciennes sont parties
    TEST_ASSERT_NOT_NULL(s.find("k209"));
    TEST_ASSERT_EQUAL(2, s.rating("rated"));  // notée : jamais retirée non plus
    // Pleine de tuiles épinglées ou notées : une nouvelle note n'est pas prise, et le dit
    TileStats f;
    for (size_t i = 0; i < TileStats::kMax; ++i) f.setRating("r" + std::to_string(i), 1);
    TEST_ASSERT_FALSE(f.setRating("new", 3));
    TEST_ASSERT_NULL(f.find("new"));
    TEST_ASSERT_TRUE(f.setRating("r5", 4));  // déjà dedans : changée
}

static void test_tile_rating() {
    TileStats s;
    TEST_ASSERT_TRUE(s.setRating("a", 4));
    TEST_ASSERT_FALSE(s.setRating("b", 6));  // hors de 1..5 : rien
    TEST_ASSERT_FALSE(s.setRating("c", -1));
    TEST_ASSERT_EQUAL(4, s.rating("a"));
    TEST_ASSERT_EQUAL(0, s.rating("b"));
    TEST_ASSERT_NULL(s.find("b"));
    TEST_ASSERT_NULL(s.find("c"));
    s.played("a");
    TEST_ASSERT_EQUAL(4, s.rating("a"));  // jouer ne change pas la note
    TEST_ASSERT_FALSE(s.setRating("x\ty", 3));  // clé illégale : ignorée
    TEST_ASSERT_NULL(s.find("x\ty"));
    // Étoile touchée : sa note, ou 0 si c'est la note actuelle
    TEST_ASSERT_EQUAL(0, s.toggledRating("a", 4));
    TEST_ASSERT_EQUAL(2, s.toggledRating("a", 2));
    TEST_ASSERT_EQUAL(3, s.toggledRating("new", 3));
    // Texte : les notes en lignes "rate" après les entrées (les anciennes versions les sautent et gardent les tuiles)
    s.played("d");
    s.setPin("d", true);
    s.setRating("d", 1);
    const std::string t = tileStatsToText(s);
    TEST_ASSERT_EQUAL_STRING("seq\t2\na\t1\t1\t0\nd\t2\t1\t1\nrate\ta\t4\nrate\td\t1\n", t.c_str());
    const TileStats r = tileStatsFromText(t);
    TEST_ASSERT_EQUAL(4, r.rating("a"));
    TEST_ASSERT_EQUAL(1, r.rating("d"));
    TEST_ASSERT_TRUE(r.pinned("d"));
    // Lignes "rate" abîmées, trop grandes, à 0 ou sans entrée : ignorées ; une clé "rate" reste une tuile
    const TileStats b = tileStatsFromText(
        "rate\te\t3\ne\t1\t1\t0\nf\t1\t1\t0\nrate\tf\t9\nrate\tf\t0\nrate\tf\tx\nrate\tnone\t2\n"
        "rate\t1\t1\t0\ng\t1\t1\t0\t5\n");
    TEST_ASSERT_EQUAL(3, b.rating("e"));  // avant son entrée : appliquée quand même
    TEST_ASSERT_EQUAL(0, b.rating("f"));
    TEST_ASSERT_NULL(b.find("none"));
    TEST_ASSERT_NOT_NULL(b.find("rate"));
    TEST_ASSERT_NULL(b.find("g"));  // 5 colonnes : ligne ignorée
    // Retirer la note
    TEST_ASSERT_TRUE(s.setRating("a", 0));
    TEST_ASSERT_EQUAL(0, s.rating("a"));
    s.setRating("zz", 0);  // inconnue : pas d'entrée créée
    TEST_ASSERT_NULL(s.find("zz"));
    // Ordre de la Bibliothèque : notées d'abord (plus d'étoiles d'abord), égalité : ordre donné
    TileStats o;
    o.setRating("k2", 3);
    o.setRating("k4", 5);
    o.setRating("k1", 3);
    const std::vector<std::string> keys = {"k0", "k1", "k2", "k3", "k4"};
    const std::vector<size_t> got = ratingOrder(keys, o);
    const size_t want[] = {4, 1, 2, 0, 3};
    TEST_ASSERT_EQUAL(5, (int)got.size());
    for (size_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL((int)want[i], (int)got[i]);
    TEST_ASSERT_EQUAL(0, (int)ratingOrder({}, o).size());
}

static void test_tile_migrate() {
    // Fichier d'une version 0.6.3 : presets par numéro
    TileStats s = tileStatsFromText("seq\t3\npreset:1\t3\t4\t1\npreset:2\t2\t1\t0\npreset:5\t1\t1\t0\nrate\tpreset:1\t4\n");
    std::vector<Preset> presets(3);
    presets[0].slot = 1;
    presets[0].label = "FIP";
    presets[1].slot = 2;  // sans nom : garde son numéro
    presets[2].slot = 5;
    presets[2].label = "FIP";  // même nom que le 1 : la clé est déjà prise, il garde la sienne
    TEST_ASSERT_TRUE(migratePresetKeys(s, presets));
    TEST_ASSERT_NULL(s.find("preset:1"));
    TEST_ASSERT_EQUAL(4, s.rating("station:FIP"));
    TEST_ASSERT_TRUE(s.pinned("station:FIP"));
    TEST_ASSERT_EQUAL(4u, s.find("station:FIP")->count);
    TEST_ASSERT_NOT_NULL(s.find("preset:2"));
    TEST_ASSERT_NOT_NULL(s.find("preset:5"));
    TEST_ASSERT_FALSE(migratePresetKeys(s, presets));  // une fois
    // Le preset change de place : la note suit la station
    presets[0].slot = 4;
    TEST_ASSERT_EQUAL(4, s.rating(tileKey(presets[0])));
    // Un nouveau preset au numéro 1 n'a pas la note de l'ancien
    Preset other;
    other.slot = 1;
    other.label = "Radio Nova";
    TEST_ASSERT_EQUAL(0, s.rating(tileKey(other)));
    TEST_ASSERT_FALSE(s.rename("absent", "x"));
    TEST_ASSERT_FALSE(s.rename("preset:2", "a\tb"));
}

static void test_tile_kind() {
    Preset p;
    TEST_ASSERT_TRUE(tileKind(p) == TileKind::Preset);
    p.favId = "0a1b2c3d";  // favori enregistré (♥, Flow)
    TEST_ASSERT_TRUE(tileKind(p) == TileKind::Favorite);
    p.favId = "qobuz-album:42";
    TEST_ASSERT_TRUE(tileKind(p) == TileKind::Qobuz);
    p.favId = "qobuz-favoris";
    TEST_ASSERT_TRUE(tileKind(p) == TileKind::Qobuz);
    p.favId = "deezer-album:7";
    TEST_ASSERT_TRUE(tileKind(p) == TileKind::Deezer);
    p.favId = "deezer-favoris";
    TEST_ASSERT_TRUE(tileKind(p) == TileKind::Deezer);
}

void run_tiles_tests() {
    RUN_TEST(test_tile_kind);
    RUN_TEST(test_tile_key);
    RUN_TEST(test_tile_played_and_order);
    RUN_TEST(test_tile_stats_text);
    RUN_TEST(test_tile_stats_full);
    RUN_TEST(test_tile_rating);
    RUN_TEST(test_tile_migrate);
}
