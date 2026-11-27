// Tile history and order without hardware: when each tile was last played, how often, which ones are pinned.
// Stored as text in LittleFS (/fav/tiles.txt, src/storage/favorites.cpp); the app sorts the strip with tileOrder().
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wiim_api.h"

namespace wr {

// One tile seen in the history. last: value of TileStats::seq at its last play (0 = never played).
// pin: 0 = not pinned, else its place among the pinned tiles (smaller first). rating: 0 = none, else 1 to 5 stars.
struct TileUse {
    std::string key;
    uint32_t last = 0, count = 0, pin = 0, rating = 0;
};

struct TileStats {
    static constexpr size_t kMax = 200;  // entries kept; the oldest one neither pinned nor rated goes first
    uint32_t seq = 0;                    // play counter: no clock needed, only the order of the plays
    std::vector<TileUse> uses;

    const TileUse* find(const std::string& key) const;
    void played(const std::string& key);            // last = ++seq, count + 1
    void setPin(const std::string& key, bool on);   // on: after the other pinned tiles
    bool pinned(const std::string& key) const;
    static constexpr int kMaxRating = 5;
    // 0 removes the rating. False when nothing changed: out of 0..5, key not storable, or the list is full of pinned
    // and rated tiles
    bool setRating(const std::string& key, int stars);
    int rating(const std::string& key) const;                         // 0 when none
    int toggledRating(const std::string& key, int stars) const;       // a star touched: its rating, or 0 if it is the current one
    // The entry `from` takes the key `to` (history, pin and rating kept). False when `from` is absent or `to` is
    // already there or not storable.
    bool rename(const std::string& from, const std::string& to);
};

// Key of a tile in the history: the favorite id ("qobuz-album:…", a saved queue, a list); for a WiiM preset
// "station:<name>", so that its history, pin and rating stay with the station when the presets change (two presets
// of the same name share them), or "preset:<slot>" when it has no name. Empty when a favorite id cannot be stored.
std::string tileKey(const Preset& p);
// Versions before 0.6.4 kept presets as "preset:<slot>": each such entry goes to the key of the preset now in that
// slot (once: the old key is gone after it). True when an entry changed (the file is then written again).
bool migratePresetKeys(TileStats& s, const std::vector<Preset>& presets);

// Text file: "seq\t<n>", one "key\tlast\tcount\tpin" line per tile, then one "rate\tkey\t<1..5>" line per rated tile
// (versions before 0.6 skip the "rate" lines and keep the tile lines, pins included). Bad lines are skipped.
std::string tileStatsToText(const TileStats& s);
TileStats tileStatsFromText(const std::string& text);

// Kind of a tile, for the Library filters: a WiiM preset, a favorite saved on the remote (♥, Flow), or a tile of a
// streaming account (album or favorites list: ids "qobuz-…", "deezer-…").
enum class TileKind { Preset = 0, Favorite, Qobuz, Deezer, Count };
TileKind tileKind(const Preset& p);
// Shown on the home strip: WiiM presets and favorites saved on the remote. The tiles of the streaming accounts
// (Qobuz, Deezer: albums and favorites lists) are in the Library only (its Qobuz / Deezer filters).
bool onStrip(const Preset& p);

// Display order of the tiles (indexes into keys): pinned tiles first (by pin), then, with recentFirst, the played
// ones (most recent first), then the others in their given order. Every index appears once.
std::vector<size_t> tileOrder(const std::vector<std::string>& keys, const TileStats& s, bool recentFirst);

// Library order (indexes into keys): rated tiles first, most stars first, then the others; equal ratings keep their
// given order (the order of the strip). Every index appears once.
std::vector<size_t> ratingOrder(const std::vector<std::string>& keys, const TileStats& s);

}  // namespace wr
