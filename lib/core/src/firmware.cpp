#include "firmware.h"

#include <cctype>
#include <vector>

#include "json.h"

namespace wr {

size_t FirmwareStream::feed(const uint8_t* d, size_t n, const uint8_t** out) {
    *out = nullptr;
    if (n == 0 || kind_ == Invalid) return 0;
    if (kind_ == Unknown) kind_ = d[0] == 0xE9 ? App : d[0] == 0xFF ? Merged : Invalid;
    size_t keep = 0;
    if (kind_ == App) {
        *out = d;
        keep = n;
    } else if (kind_ == Merged && offset_ + n > kAppOffset) {
        const size_t start = offset_ >= kAppOffset ? 0 : kAppOffset - offset_;
        // Le premier octet de l'application doit être l'octet magique d'une image ESP
        if (offset_ + start == kAppOffset && d[start] != 0xE9) {
            kind_ = Invalid;
        } else {
            *out = d + start;
            keep = n - start;
        }
    }
    offset_ += n;
    if (kind_ == Invalid) {
        *out = nullptr;
        return 0;
    }
    written_ += keep;
    return keep;
}

namespace {

bool safeTag(const std::string& t) {
    if (t.empty() || t.size() > 40) return false;
    for (unsigned char c : t)
        if (!std::isalnum(c) && c != '.' && c != '-' && c != '_' && c != '+') return false;
    return true;
}

bool lowerHex64(const std::string& h) {
    if (h.size() != 64) return false;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// "1.2.3-beta.2+x" → nombres et identifiants de pré-version ; faux si ce n'est pas une version
struct Version {
    long n[3] = {0, 0, 0};
    std::vector<std::string> pre;
};

bool parseVersion(std::string s, Version& v) {
    if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);
    const size_t plus = s.find('+');
    if (plus != std::string::npos) s.resize(plus);
    size_t i = 0;
    for (int k = 0; k < 3; ++k) {
        if (k > 0) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
        const size_t start = i;
        long x = 0;
        while (i < s.size() && std::isdigit((unsigned char)s[i]) && i - start < 9) x = x * 10 + (s[i++] - '0');
        if (i == start || (i < s.size() && std::isdigit((unsigned char)s[i]))) return false;
        v.n[k] = x;
    }
    if (i == s.size()) return true;
    if (s[i] != '-' || i + 1 == s.size()) return false;
    std::string cur;
    for (size_t j = i + 1; j <= s.size(); ++j) {
        if (j == s.size() || s[j] == '.') {
            if (cur.empty()) return false;
            v.pre.push_back(cur);
            cur.clear();
        } else if (std::isalnum((unsigned char)s[j]) || s[j] == '-') {
            cur += s[j];
        } else {
            return false;
        }
    }
    return true;
}

bool allDigits(const std::string& s) {
    for (char c : s)
        if (!std::isdigit((unsigned char)c)) return false;
    return !s.empty();
}

}  // namespace

bool parseLatestRelease(const std::string& json, const std::string& repo, const std::string& asset, ReleaseInfo& out) {
    const std::string prefix = "https://github.com/" + repo + "/releases/download/";
    for (const std::string& rel : jsonArray(json)) {
        if (jsonTop(rel, "draft") == "true") continue;
        const std::string tag = jsonText(jsonTop(rel, "tag_name"));
        if (!safeTag(tag)) continue;
        for (const std::string& a : jsonArray(jsonTop(rel, "assets"))) {
            if (jsonText(jsonTop(a, "name")) != asset) continue;
            const std::string url = jsonText(jsonTop(a, "browser_download_url"));
            std::string digest = jsonText(jsonTop(a, "digest"));
            const std::string sizeText = jsonTop(a, "size");
            if (url != prefix + tag + "/" + asset || digest.compare(0, 7, "sha256:") != 0) break;
            digest.erase(0, 7);
            if (!lowerHex64(digest) || !allDigits(sizeText) || sizeText.size() > 9) break;
            out.tag = tag;
            out.url = url;
            out.sha256 = digest;
            out.size = std::stol(sizeText);
            return out.size > 0;
        }
    }
    return false;
}

bool compareVersions(const std::string& a, const std::string& b, int& cmp) {
    Version x, y;
    if (!parseVersion(a, x) || !parseVersion(b, y)) return false;
    cmp = 0;
    for (int k = 0; k < 3 && cmp == 0; ++k) cmp = x.n[k] < y.n[k] ? -1 : x.n[k] > y.n[k] ? 1 : 0;
    if (cmp != 0) return true;
    // Sans pré-version > avec ; sinon identifiant par identifiant (nombres < mots), la liste la plus courte avant
    if (x.pre.empty() || y.pre.empty()) {
        cmp = x.pre.empty() == y.pre.empty() ? 0 : x.pre.empty() ? 1 : -1;
        return true;
    }
    for (size_t k = 0; k < x.pre.size() && k < y.pre.size() && cmp == 0; ++k) {
        const std::string &p = x.pre[k], &q = y.pre[k];
        const bool np = allDigits(p), nq = allDigits(q);
        if (np && nq) cmp = p.size() != q.size() ? (p.size() < q.size() ? -1 : 1) : p.compare(q) < 0 ? -1 : p.compare(q) > 0 ? 1 : 0;
        else if (np != nq) cmp = np ? -1 : 1;
        else cmp = p < q ? -1 : p > q ? 1 : 0;
    }
    if (cmp == 0 && x.pre.size() != y.pre.size()) cmp = x.pre.size() < y.pre.size() ? -1 : 1;
    return true;
}

ReleaseOffer releaseOffer(const std::string& installed, const std::string& available) {
    int c = 0;
    if (!compareVersions(installed, available, c)) return ReleaseOffer::Unknown;
    return c < 0 ? ReleaseOffer::Newer : c == 0 ? ReleaseOffer::Same : ReleaseOffer::Older;
}

}  // namespace wr
