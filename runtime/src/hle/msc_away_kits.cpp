// Generated away kits for Mario, Luigi, Waluigi and Wario (see msc_away_kits.h).
//
// Colours follow Mario Strikers: Battle League's two-sided team colours (a warm and a cool side) and
// each character's best-known palette swap: Fire Mario white with red trim, Luigi sky blue, Wario
// blue, Waluigi orange. Everything is made at runtime from the player's own game files.
//
// Finding the kit, per texture kind:
//  - Captain kit and extra textures: texels in the kit's hue band. "Strong" texels (clearly the kit
//    colour) seed regions that "weak" texels (the same hue family lighter, darker or less
//    saturated: highlights and shading) may join only when connected to them (hysteresis), so
//    look-alike colours elsewhere stay put.
//  - Sidekick and goalie textures: every team has its own version with the same UV layout, so the
//    team-coloured texels are the ones that differ from the same texture of two other teams (read
//    from the disc) and fall in the kit's hue band. Skin, mouths, shells and the Kritter's own
//    colours are the same for every team and are never touched.
//  - Team-tinted backgrounds (ExtraTextures): every coloured texel.
// White kit (Mario) only: white details enclosed by kit (numbers, outlines, logo circles) become
// red trim, and kit-red shapes sitting on white that is not trim (a red M on a white circle on a
// pennant) are logos and keep their colour.
//
// Recolouring works in OKLab/OKLCh: hue becomes the target's, chroma scales with the target's,
// lightness follows a curve through the kit's reference lightness to the target's (backgrounds keep
// theirs), and colours outside sRGB lose chroma until they fit. Blocks without kit texels keep their
// bytes, blocks entirely of kit get their two endpoints transformed (indices kept), and the rest are
// re-encoded. Mip levels use the level-0 masks.
//
// This file is built without fast-math (PublicProducts.cmake) so every platform makes the same kits.

#include "msc_away_kits.h"

#include "runtime_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

bool DVDReadFileForRuntime(const char* dvdPath, std::vector<uint8_t>& out);  // storage/dvd.cpp

namespace MscAwayKits {
namespace {

// ---------------------------------------------------------------------------------------- captains
struct Rule {
    float hueLo, hueHi, minSat, minVal;  // HSV; a hue range may wrap (lo > hi)
};

struct Captain {
    int32_t id;
    const char* name;
    uint32_t target;             // 0xRRGGBB away colour
    bool white;                  // white kit: red trim and logo protection
    Rule strong, weak, relaxed;  // relaxed: the kit's hue band on sidekicks and goalies
    double refL, refC, refH;     // OKLCh median of the kit texture's strong texels
    std::vector<uint32_t> kit, extras, backgrounds;
    const char* refs[2];         // teams whose sidekick/goalie textures are compared against
};

// Mario's fire emblem (extra texture D17250CE) is left out: a character logo, not kit.
const Captain kCaptains[] = {
    {0, "mario", 0xF3F5F8, true,
     {346, 8, 0.60f, 0.25f}, {336, 12, 0.30f, 0.10f}, {330, 20, 0.25f, 0.08f},
     0.5813180599298153, 0.23686263046799821, 27.926322334420224,
     {0xBBFFCF5Eu}, {0x9AAAEB1Au, 0xBE9289A6u, 0xF0084E8Bu}, {}, {"luigi", "waluigi"}},
    {4, "luigi", 0x3399E6, false,
     {97, 140, 0.55f, 0.15f}, {85, 165, 0.15f, 0.05f}, {70, 175, 0.15f, 0.04f},
     0.485626603219491, 0.13994565932542496, 139.99582817762968,
     {0x4442532Du}, {0x4BFDAE1Cu, 0xA215182Au, 0xCBB371CDu, 0xCC87CDD2u}, {0x8630E179u, 0xED287379u},
     {"mario", "wario"}},
    {6, "waluigi", 0xF28A1E, false,
     {255, 285, 0.55f, 0.20f}, {240, 300, 0.20f, 0.06f}, {225, 315, 0.12f, 0.04f},
     0.37199752524660135, 0.18012455517605216, 297.1545639557531,
     {0x8028327Eu, 0xD04A32DFu}, {0x4D881A94u, 0x5FF7AB5Au, 0xD04FAF02u, 0xFE8B6D45u},
     {0x141D28F1u, 0xAD2596F1u}, {"luigi", "wario"}},
    {7, "wario", 0x2659E0, false,
     {43, 56, 0.60f, 0.30f}, {41, 60, 0.56f, 0.12f}, {25, 70, 0.20f, 0.06f},
     0.8613964221864203, 0.1750852977415117, 89.36134497179374,
     {0x31A701C7u}, {0x6A1BC955u, 0xC0D0E73Au, 0xFB7534A4u}, {0x422EBF01u, 0xA9265101u},
     {"luigi", "waluigi"}},
};

struct Sidekick {
    const char* prefix;  // texture name prefix
    const char* folder;  // Art/characters folder
};
const Sidekick kSidekicks[] = {
    {"birdo", "birdo"}, {"boo", "boo"}, {"drybones", "drybones"}, {"hammer", "hammerbro"},
    {"koopa", "koopa"}, {"montymole", "montymole"}, {"shyguy", "shyguy"}, {"toad", "toad"},
};

const Captain* Find(int32_t captain) {
    for (const Captain& c : kCaptains) {
        if (c.id == captain) return &c;
    }
    return nullptr;
}

uint32_t LowerHash(const std::string& s) {  // nlStringLowerHash
    uint32_t h = 0xFFFFFFFFu;
    for (unsigned char ch : s) h += (h << 5) + static_cast<uint32_t>(std::tolower(ch));
    return h;
}

const std::map<int32_t, std::vector<Texture>>& TextureLists() {
    static const std::map<int32_t, std::vector<Texture>> s_lists = [] {
        std::map<int32_t, std::vector<Texture>> lists;
        for (const Captain& c : kCaptains) {
            std::vector<Texture>& list = lists[c.id];
            const std::string own = std::string("/Art/characters/") + c.name + "/";
            for (uint32_t h : c.kit) list.push_back({h, Kind::Kit, own + c.name + ".rlt", {}, {}});
            for (uint32_t h : c.extras) list.push_back({h, Kind::Extra, own + "extratextures.rlt", {}, {}});
            for (uint32_t h : c.backgrounds) list.push_back({h, Kind::Background, own + "extratextures.rlt", {}, {}});
            // "<team>goalie/<team>goalie" in Art/characters/<team>goalie/<team>goalie.rlt.
            const auto goalie = [](const char* team) { return std::string(team) + "goalie"; };
            Texture keeper{LowerHash(goalie(c.name) + "/" + goalie(c.name)), Kind::Goalie,
                           "/Art/characters/" + goalie(c.name) + "/" + goalie(c.name) + ".rlt", {}, {}};
            for (int i = 0; i < 2; ++i) {
                keeper.refBundle[i] = "/Art/characters/" + goalie(c.refs[i]) + "/" + goalie(c.refs[i]) + ".rlt";
                keeper.refHash[i] = LowerHash(goalie(c.refs[i]) + "/" + goalie(c.refs[i]));
            }
            list.push_back(std::move(keeper));
            // "<prefix>_<team>/<prefix>_<team>" in Art/characters/<folder>/<prefix>_<team>.rlt.
            for (const Sidekick& s : kSidekicks) {
                const auto name = [&](const char* team) { return std::string(s.prefix) + "_" + team; };
                const std::string folder = std::string("/Art/characters/") + s.folder + "/";
                Texture t{LowerHash(name(c.name) + "/" + name(c.name)), Kind::Sidekick, folder + name(c.name) + ".rlt", {}, {}};
                for (int i = 0; i < 2; ++i) {
                    t.refBundle[i] = folder + name(c.refs[i]) + ".rlt";
                    t.refHash[i] = LowerHash(name(c.refs[i]) + "/" + name(c.refs[i]));
                }
                list.push_back(std::move(t));
            }
        }
        return lists;
    }();
    return s_lists;
}

// ------------------------------------------------------------------------------------------ colour
// OKLab (Björn Ottosson). kM1i/kM2i are the exact inverses of kM1/kM2; the published inverse
// coefficients are rounded.
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kM1[3][3] = {{0.4122214708, 0.5363325363, 0.0514459929},
                              {0.2119034982, 0.6806995451, 0.1073969566},
                              {0.0883024619, 0.2817188376, 0.6299787005}};
constexpr double kM2[3][3] = {{0.2104542553, 0.7936177850, -0.0040720468},
                              {1.9779984951, -2.4285922050, 0.4505937099},
                              {0.0259040371, 0.7827717662, -0.8086757660}};
constexpr double kM1i[3][3] = {{4.076741661347994, -3.3077115904081933, 0.23096992872942793},
                               {-1.2684380040921763, 2.6097574006633715, -0.3413193963102196},
                               {-0.004196086541837074, -0.7034186144594495, 1.7076147009309446}};
constexpr double kM2i[3][3] = {{0.9999999984505196, 0.3963377921737678, 0.21580375806075877},
                               {1.0000000088817607, -0.10556134232365633, -0.0638541747717059},
                               {1.0000000546724108, -0.08948418209496574, -1.2914855378640917}};

struct Lab {
    double L, a, b;
};

double SrgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double LinearToSrgb(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

Lab RgbToOklab(const uint8_t* rgb) {
    double lin[3], lms[3];
    for (int i = 0; i < 3; ++i) lin[i] = SrgbToLinear(rgb[i] / 255.0);
    for (int i = 0; i < 3; ++i) lms[i] = std::cbrt(lin[0] * kM1[i][0] + lin[1] * kM1[i][1] + lin[2] * kM1[i][2]);
    return {lms[0] * kM2[0][0] + lms[1] * kM2[0][1] + lms[2] * kM2[0][2],
            lms[0] * kM2[1][0] + lms[1] * kM2[1][1] + lms[2] * kM2[1][2],
            lms[0] * kM2[2][0] + lms[1] * kM2[2][1] + lms[2] * kM2[2][2]};
}

void OklabToLinear(const Lab& c, double out[3]) {
    double lms[3];
    for (int i = 0; i < 3; ++i) lms[i] = std::pow(c.L * kM2i[i][0] + c.a * kM2i[i][1] + c.b * kM2i[i][2], 3.0);
    for (int i = 0; i < 3; ++i) out[i] = lms[0] * kM1i[i][0] + lms[1] * kM1i[i][1] + lms[2] * kM1i[i][2];
}

double Degrees360(double radians) {
    double d = std::fmod(radians * (180.0 / kPi), 360.0);
    if (d < 0) d += 360.0;
    return d;
}

struct Hue {
    double cos, sin;
    explicit Hue(double degrees) : cos(std::cos(degrees * (kPi / 180.0))), sin(std::sin(degrees * (kPi / 180.0))) {}
};

bool InGamut(const double lin[3]) {
    for (int i = 0; i < 3; ++i) {
        if (!(lin[i] >= -1e-4 && lin[i] <= 1 + 1e-4)) return false;
    }
    return true;
}

// OKLCh -> sRGB bytes, lowering chroma (bisection) until the colour fits; lightness and hue stay.
void LchToRgb8(double L, double C, const Hue& h, uint8_t out[3]) {
    L = std::clamp(L, 0.0, 1.0);
    double lin[3];
    OklabToLinear({L, C * h.cos, C * h.sin}, lin);
    if (!InGamut(lin)) {
        double lo = 0.0, hi = C;
        for (int i = 0; i < 18; ++i) {
            const double mid = (lo + hi) / 2;
            double t[3];
            OklabToLinear({L, mid * h.cos, mid * h.sin}, t);
            if (InGamut(t)) lo = mid; else hi = mid;
        }
        OklabToLinear({L, lo * h.cos, lo * h.sin}, lin);
    }
    for (int i = 0; i < 3; ++i) {
        out[i] = static_cast<uint8_t>(std::clamp(std::nearbyint(LinearToSrgb(lin[i]) * 255), 0.0, 255.0));
    }
}

// HSV in single precision, hue in [0, 360].
void Hsv(const uint8_t* p, float& h, float& s, float& v) {
    const float r = p[0] / 255.f, g = p[1] / 255.f, b = p[2] / 255.f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    h = 0.f;
    if (d > 1e-6f) {
        if (mx == r) {
            h = (g - b) / d;
            h = 60.f * h;
            if (h < 0.f) h += 360.f;
        } else if (mx == g) {
            h = (b - r) / d + 2.f;
            h = 60.f * h;
        } else {
            h = (r - g) / d + 4.f;
            h = 60.f * h;
        }
    }
    s = mx > 0.f ? d / mx : 0.f;
    v = mx;
}

bool Matches(const Rule& r, float h, float s, float v) {
    const bool hue = r.hueLo <= r.hueHi ? (h >= r.hueLo && h <= r.hueHi) : (h >= r.hueLo || h <= r.hueHi);
    return hue && s >= r.minSat && v >= r.minVal;
}

// White, for the white kit's trim and logos.
bool IsWhite(const uint8_t* p) {
    float h, s, v;
    Hsv(p, h, s, v);
    return p[3] > 0 && s < 0.28f && v > 0.62f;
}

struct Transform {
    double Lr, Cr, Lt, Ct;
    Hue hr, ht;
    bool keepLightness;

    // Lightness curve through (0,0), (Lr,Lt), (1,1): the kit's reference lightness becomes the
    // target's, darker and lighter shades keep their place in between.
    double Tone(double L) const {
        return L <= Lr ? L * (Lt / std::max(Lr, 1e-6)) : Lt + (L - Lr) * ((1 - Lt) / std::max(1 - Lr, 1e-6));
    }
    void Kit(const uint8_t* in, uint8_t* out) const {
        const Lab c = RgbToOklab(in);
        const double C = std::hypot(c.a, c.b);
        LchToRgb8(keepLightness ? c.L : Tone(c.L), C * (Ct / std::max(Cr, 1e-6)), ht, out);
    }
    // White trim -> the kit's original colour, keeping its shading.
    void Trim(const uint8_t* in, uint8_t* out) const {
        const Lab c = RgbToOklab(in);
        LchToRgb8(std::clamp(c.L / 0.95, 0.0, 1.0) * Lr, Cr, hr, out);
    }
};

Transform MakeTransform(const Captain& c, bool keepLightness) {
    const uint8_t rgb[3] = {uint8_t(c.target >> 16), uint8_t(c.target >> 8), uint8_t(c.target)};
    const Lab t = RgbToOklab(rgb);
    return {c.refL, c.refC, t.L, std::hypot(t.a, t.b), Hue(c.refH), Hue(Degrees360(std::atan2(t.b, t.a))), keepLightness};
}

// ------------------------------------------------------------------------------------------- image
struct Image {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4
    const uint8_t* at(uint32_t x, uint32_t y) const { return &rgba[(size_t(y) * w + x) * 4]; }
    uint8_t* at(uint32_t x, uint32_t y) { return &rgba[(size_t(y) * w + x) * 4]; }
};
using Mask = std::vector<uint8_t>;

uint32_t LevelSize(uint32_t w, uint32_t h) { return ((w + 7) / 8) * ((h + 7) / 8) * 32; }

uint32_t DataSize(uint32_t w, uint32_t h, uint32_t levels) {
    uint32_t size = 0;
    for (uint32_t l = 0; l < levels; ++l) size += LevelSize(std::max(1u, w >> l), std::max(1u, h >> l));
    return size;
}

void Expand565(uint16_t c, int out[3]) {
    out[0] = ((c >> 11) & 31) * 255 / 31;
    out[1] = ((c >> 5) & 63) * 255 / 63;
    out[2] = (c & 31) * 255 / 31;
}

void Palette(uint16_t c0, uint16_t c1, int pal[4][4]) {
    int a[3], b[3];
    Expand565(c0, a);
    Expand565(c1, b);
    for (int i = 0; i < 3; ++i) {
        pal[0][i] = a[i];
        pal[1][i] = b[i];
        pal[2][i] = c0 > c1 ? (2 * a[i] + b[i]) / 3 : (a[i] + b[i]) / 2;
        pal[3][i] = c0 > c1 ? (a[i] + 2 * b[i]) / 3 : 0;
    }
    pal[0][3] = pal[1][3] = pal[2][3] = 255;
    pal[3][3] = c0 > c1 ? 255 : 0;
}

// Visits the 4x4 sub-blocks of a CMPR level in storage order: (byte offset, x, y).
template <typename F>
void ForEachBlock(uint32_t w, uint32_t h, F&& f) {
    uint32_t p = 0;
    for (uint32_t ty = 0; ty < h; ty += 8) {
        for (uint32_t tx = 0; tx < w; tx += 8) {
            for (uint32_t sb = 0; sb < 4; ++sb, p += 8) f(p, tx + (sb & 1) * 4, ty + (sb >> 1) * 4);
        }
    }
}

Image DecodeCmpr(const uint8_t* data, uint32_t w, uint32_t h) {
    Image img;
    img.w = w;
    img.h = h;
    img.rgba.assign(size_t(w) * h * 4, 0);
    ForEachBlock(w, h, [&](uint32_t p, uint32_t bx, uint32_t by) {
        const uint8_t* blk = data + p;
        int pal[4][4];
        Palette(uint16_t((blk[0] << 8) | blk[1]), uint16_t((blk[2] << 8) | blk[3]), pal);
        const uint32_t idx = (uint32_t(blk[4]) << 24) | (uint32_t(blk[5]) << 16) | (uint32_t(blk[6]) << 8) | blk[7];
        for (uint32_t k = 0; k < 16; ++k) {
            const uint32_t x = bx + k % 4, y = by + k / 4;
            if (x >= w || y >= h) continue;
            const int* c = pal[(idx >> (30 - 2 * k)) & 3];
            uint8_t* o = img.at(x, y);
            for (int i = 0; i < 4; ++i) o[i] = uint8_t(c[i]);
        }
    });
    return img;
}

// ----------------------------------------------------------------------------- connected regions
// Labels the set texels' connected regions (8- or 4-neighbourhood) 1..count; 0 = not set.
std::vector<int32_t> Label(const Mask& m, uint32_t w, uint32_t h, bool eight, int32_t& count) {
    std::vector<int32_t> lab(m.size(), 0);
    std::vector<uint32_t> stack;
    count = 0;
    for (uint32_t i = 0; i < m.size(); ++i) {
        if (!m[i] || lab[i]) continue;
        lab[i] = ++count;
        stack.push_back(i);
        while (!stack.empty()) {
            const uint32_t p = stack.back();
            stack.pop_back();
            const int32_t px = int32_t(p % w), py = int32_t(p / w);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((dx == 0 && dy == 0) || (!eight && dx != 0 && dy != 0)) continue;
                    const int32_t nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= int32_t(w) || ny >= int32_t(h)) continue;
                    const uint32_t q = uint32_t(ny) * w + uint32_t(nx);
                    if (m[q] && !lab[q]) {
                        lab[q] = count;
                        stack.push_back(q);
                    }
                }
            }
        }
    }
    return lab;
}

// Per region: its area, and the size of its one-texel ring (8-neighbourhood, inside the image)
// with how much of the ring is in `of`.
struct Rings {
    std::vector<uint32_t> area, ring, ringIn;
};
Rings RingsOf(const std::vector<int32_t>& lab, int32_t count, uint32_t w, uint32_t h, const Mask& of) {
    Rings r;
    r.area.assign(count + 1, 0);
    r.ring.assign(count + 1, 0);
    r.ringIn.assign(count + 1, 0);
    // Texels grouped by region, so each region's ring is counted with its own stamp.
    for (int32_t l : lab) {
        if (l) ++r.area[l];
    }
    std::vector<uint32_t> first(count + 2, 0);
    for (int32_t c = 1; c <= count; ++c) first[c + 1] = first[c] + r.area[c];
    std::vector<uint32_t> order(first[count + 1]), fill(first);
    for (uint32_t i = 0; i < lab.size(); ++i) {
        if (lab[i]) order[fill[lab[i]]++] = i;
    }
    std::vector<int32_t> stamp(lab.size(), 0);
    for (int32_t c = 1; c <= count; ++c) {
        for (uint32_t k = first[c]; k < first[c + 1]; ++k) {
            const int32_t px = int32_t(order[k] % w), py = int32_t(order[k] / w);
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int32_t nx = px + dx, ny = py + dy;
                    if (nx < 0 || ny < 0 || nx >= int32_t(w) || ny >= int32_t(h)) continue;
                    const uint32_t q = uint32_t(ny) * w + uint32_t(nx);
                    if (lab[q] == c || stamp[q] == c) continue;
                    stamp[q] = c;
                    ++r.ring[c];
                    if (of[q]) ++r.ringIn[c];
                }
            }
        }
    }
    return r;
}

// ------------------------------------------------------------------------------------------- masks
// Captain kit and extra textures: weak texels connected to strong ones.
Mask Classify(const Image& img, const Captain& c) {
    const size_t n = size_t(img.w) * img.h;
    Mask strong(n), weak(n);
    for (size_t i = 0; i < n; ++i) {
        float h, s, v;
        Hsv(&img.rgba[i * 4], h, s, v);
        const bool opaque = img.rgba[i * 4 + 3] > 0;
        strong[i] = opaque && Matches(c.strong, h, s, v);
        weak[i] = strong[i] || (opaque && Matches(c.weak, h, s, v));
    }
    int32_t count = 0;
    const auto lab = Label(weak, img.w, img.h, true, count);
    std::vector<uint8_t> keep(count + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        if (strong[i]) keep[lab[i]] = 1;
    }
    Mask m(n);
    for (size_t i = 0; i < n; ++i) m[i] = lab[i] && keep[lab[i]];
    return m;
}

// Team-tinted backgrounds: every coloured texel.
Mask Background(const Image& img) {
    Mask m(size_t(img.w) * img.h);
    for (size_t i = 0; i < m.size(); ++i) {
        float h, s, v;
        Hsv(&img.rgba[i * 4], h, s, v);
        m[i] = img.rgba[i * 4 + 3] > 0 && s >= 0.08f;
    }
    return m;
}

// Sidekick and goalie textures: kit-hued texels that differ from both other teams' versions.
Mask DiffMask(const Image& img, const Image* const refs[2], const Captain& c) {
    const size_t n = size_t(img.w) * img.h;
    Mask m(n), hued(n);
    for (size_t i = 0; i < n; ++i) {
        float h, s, v;
        Hsv(&img.rgba[i * 4], h, s, v);
        hued[i] = Matches(c.relaxed, h, s, v);
        if (!hued[i] || img.rgba[i * 4 + 3] == 0) continue;
        const Lab a = RgbToOklab(&img.rgba[i * 4]);
        bool differs = true;
        for (int r = 0; r < 2 && differs; ++r) {
            const Lab b = RgbToOklab(&refs[r]->rgba[i * 4]);
            const double dL = a.L - b.L, da = a.a - b.a, db = a.b - b.b;
            differs = std::sqrt(dL * dL + da * da + db * db) > 0.10;
        }
        m[i] = differs;
    }
    // Drop specks.
    int32_t count = 0;
    auto lab = Label(m, img.w, img.h, true, count);
    std::vector<uint32_t> area(count + 1, 0);
    for (size_t i = 0; i < n; ++i) ++area[lab[i]];
    for (size_t i = 0; i < n; ++i) {
        if (lab[i] && area[lab[i]] < 6) m[i] = 0;
    }
    // Fill pinholes (enclosed pockets of up to 4 texels: block noise) that are kit-hued.
    Mask outside(n);
    for (size_t i = 0; i < n; ++i) outside[i] = !m[i];
    lab = Label(outside, img.w, img.h, false, count);
    area.assign(count + 1, 0);
    std::vector<uint8_t> border(count + 1, 0);
    for (uint32_t y = 0; y < img.h; ++y) {
        for (uint32_t x = 0; x < img.w; ++x) {
            const int32_t l = lab[size_t(y) * img.w + x];
            if (!l) continue;
            ++area[l];
            if (x == 0 || y == 0 || x + 1 == img.w || y + 1 == img.h) border[l] = 1;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const int32_t l = lab[i];
        if (l && !border[l] && area[l] <= 4 && hued[i]) m[i] = 1;
    }
    return m;
}

// White kit: white details (numbers, outlines, logo circles) sitting inside kit areas.
Mask TrimMask(const Image& img, const Mask& kit) {
    const size_t n = kit.size();
    Mask cand(n);
    for (size_t i = 0; i < n; ++i) cand[i] = !kit[i] && IsWhite(&img.rgba[i * 4]);
    int32_t count = 0;
    const auto lab = Label(cand, img.w, img.h, true, count);
    const Rings r = RingsOf(lab, count, img.w, img.h, kit);
    Mask trim(n);
    for (size_t i = 0; i < n; ++i) {
        const int32_t l = lab[i];
        if (!l || r.area[l] > n * 0.04 || r.area[l] < 12 || r.ring[l] == 0) continue;
        trim[i] = double(r.ringIn[l]) / r.ring[l] >= 0.80;
    }
    return trim;
}

// White kit: kit shapes on white that is not trim (a red M on a white circle) are logos and keep
// their colour instead of vanishing white on white.
void ProtectLogos(const Image& img, Mask& kit, const Mask& trim) {
    const size_t n = kit.size();
    Mask white(n);
    for (size_t i = 0; i < n; ++i) white[i] = !trim[i] && IsWhite(&img.rgba[i * 4]);
    int32_t count = 0;
    const auto lab = Label(kit, img.w, img.h, true, count);
    const Rings r = RingsOf(lab, count, img.w, img.h, white);
    for (size_t i = 0; i < n; ++i) {
        const int32_t l = lab[i];
        if (!l || r.area[l] > n * 0.04 || r.ring[l] == 0) continue;
        if (double(r.ringIn[l]) / r.ring[l] >= 0.70) kit[i] = 0;
    }
}

// ---------------------------------------------------------------------------------------- bundles
uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
    return o + 4 <= d.size() ? (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3] : 0u;
}
uint32_t Be16(const std::vector<uint8_t>& d, size_t o) {
    return o + 2 <= d.size() ? (uint32_t(d[o]) << 8) | d[o + 1] : 0u;
}

struct BundleTexture {
    uint32_t width = 0, height = 0, levels = 0;
    std::vector<uint8_t> data;  // CMPR, every level
};

// A CMPR texture out of a texture bundle (PTLG: count at +4, 16-byte entries {hash, offset, size}
// from +0x10, then each texture's GXTextureHeader and data).
bool FindInBundle(const std::vector<uint8_t>& file, uint32_t hash, BundleTexture& out) {
    if (Be32(file, 0) != 0x50544C47u) return false;  // "PTLG"
    const uint32_t count = Be32(file, 4);
    if (16 + uint64_t(count) * 16 > file.size()) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = 16 + size_t(i) * 16;
        if (Be32(file, entry) != hash) continue;
        const size_t base = 16 + size_t(count) * 16 + Be32(file, entry + 4);
        if (Be32(file, base + 4) != 2) return false;  // GXTextureHeader::format: CMPR only
        out.levels = std::max(1u, Be32(file, base));
        out.width = Be16(file, base + 0x0E);
        out.height = Be16(file, base + 0x10);
        if (out.levels > 16 || out.width == 0 || out.height == 0) return false;
        const size_t size = DataSize(out.width, out.height, out.levels);
        if (base + 0x20 + size > file.size()) return false;
        out.data.assign(file.begin() + base + 0x20, file.begin() + base + 0x20 + size);
        return true;
    }
    return false;
}

// Level 0 of another team's version of a texture, for the sidekick/goalie difference masks.
const Image* ReferenceImage(const std::string& bundle, uint32_t hash) {
    static std::mutex s_mutex;
    static std::map<std::pair<std::string, uint32_t>, std::unique_ptr<Image>> s_cache;
    std::lock_guard<std::mutex> lock(s_mutex);
    std::unique_ptr<Image>& slot = s_cache[{bundle, hash}];
    if (!slot) {
        slot = std::make_unique<Image>();
        std::vector<uint8_t> file;
        BundleTexture t;
        if (DVDReadFileForRuntime(bundle.c_str(), file) && FindInBundle(file, hash, t)) {
            *slot = DecodeCmpr(t.data.data(), t.width, t.height);
        }
    }
    return slot->w ? slot.get() : nullptr;
}

// Kit (and white kit trim) texels of a texture's level 0. False when it cannot be told apart.
bool FindKit(const Captain& c, const Texture& texture, const Image& base, Mask& kit, Mask& trim) {
    switch (texture.kind) {
    case Kind::Kit:
    case Kind::Extra:
        kit = Classify(base, c);
        break;
    case Kind::Background:
        kit = Background(base);
        break;
    case Kind::Sidekick:
    case Kind::Goalie: {
        const Image* refs[2] = {};
        for (int i = 0; i < 2; ++i) {
            refs[i] = ReferenceImage(texture.refBundle[i], texture.refHash[i]);
            if (refs[i] == nullptr || refs[i]->w != base.w || refs[i]->h != base.h) {
                RT_LOGF(RT_TAG_CONFIG, "Away kits: cannot compare with %s (%08X)\n", texture.refBundle[i].c_str(),
                        texture.refHash[i]);
                return false;
            }
        }
        kit = DiffMask(base, refs, c);
        break;
    }
    }
    trim.assign(kit.size(), 0);
    if (c.white) {
        trim = TrimMask(base, kit);
        if (texture.kind != Kind::Background) ProtectLogos(base, kit, trim);
    }
    return true;
}

// ----------------------------------------------------------------------------------------- encoder
uint16_t Quant565(const double c[3]) {
    const auto q = [](double v, int maxv) { return std::clamp(static_cast<int>(std::nearbyint(v * maxv / 255)), 0, maxv); };
    return uint16_t((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}

// Nearest palette entry per texel (transparent texels take index 3 in three-colour mode).
double FitIndices(const double px[16][3], const bool transparent[16], uint16_t c0, uint16_t c1, uint8_t idx[16]) {
    int pal[4][4];
    Palette(c0, c1, pal);
    const int colours = c0 > c1 ? 4 : 3;
    double err = 0;
    for (int k = 0; k < 16; ++k) {
        if (colours == 3 && transparent[k]) {
            idx[k] = 3;
            continue;
        }
        double best = 0;
        for (int i = 0; i < colours; ++i) {
            double d = 0;
            for (int ch = 0; ch < 3; ++ch) d += (px[k][ch] - pal[i][ch]) * (px[k][ch] - pal[i][ch]);
            if (i == 0 || d < best) {
                best = d;
                idx[k] = uint8_t(i);
            }
        }
        err += best;
    }
    return err;
}

// One 4x4 block: endpoints along the principal axis (two insets), refined by least squares, best
// fit kept. Three-colour mode when any texel is transparent.
void EncodeBlock(const double px[16][3], const bool transparent[16], uint8_t out[8]) {
    bool three = false;
    for (int k = 0; k < 16; ++k) three = three || transparent[k];
    int op[16], n = 0;
    for (int k = 0; k < 16; ++k) {
        if (!three || !transparent[k]) op[n++] = k;
    }
    if (n == 0) {
        static constexpr uint8_t kClear[8] = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        std::memcpy(out, kClear, 8);
        return;
    }
    double mean[3], axis[3];
    for (int c = 0; c < 3; ++c) {
        double sum = 0, mn = px[op[0]][c], mx = px[op[0]][c];
        for (int i = 0; i < n; ++i) {
            sum += px[op[i]][c];
            mn = std::min(mn, px[op[i]][c]);
            mx = std::max(mx, px[op[i]][c]);
        }
        mean[c] = sum / n;
        axis[c] = mx - mn;
    }
    // Covariance (centred again and normalised by n - 1, as numpy.cov).
    double cov[3][3] = {};
    if (n > 1) {
        double x[3][16];
        for (int c = 0; c < 3; ++c) {
            double sum = 0;
            for (int i = 0; i < n; ++i) {
                x[c][i] = px[op[i]][c] - mean[c];
                sum += x[c][i];
            }
            const double avg = sum / n;
            for (int i = 0; i < n; ++i) x[c][i] -= avg;
        }
        const double fact = 1.0 / (n - 1);
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                double sum = 0;
                for (int i = 0; i < n; ++i) sum += x[a][i] * x[b][i];
                cov[a][b] = sum * fact;
            }
        }
    }
    if (std::fabs(axis[0]) <= 1e-8 && std::fabs(axis[1]) <= 1e-8 && std::fabs(axis[2]) <= 1e-8) axis[0] = axis[1] = axis[2] = 1.0;
    for (int it = 0; it < 8; ++it) {  // principal axis by power iteration
        double next[3];
        for (int a = 0; a < 3; ++a) next[a] = cov[a][0] * axis[0] + cov[a][1] * axis[1] + cov[a][2] * axis[2];
        const double norm = std::sqrt(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
        if (norm < 1e-9) break;
        for (int a = 0; a < 3; ++a) axis[a] = next[a] / norm;
    }
    double tmin = 0, tmax = 0;
    for (int i = 0; i < n; ++i) {
        const double* p = px[op[i]];
        const double t = (p[0] - mean[0]) * axis[0] + (p[1] - mean[1]) * axis[1] + (p[2] - mean[2]) * axis[2];
        if (i == 0 || t < tmin) tmin = t;
        if (i == 0 || t > tmax) tmax = t;
    }
    double bestErr = -1;
    uint16_t best0 = 0, best1 = 0;
    uint8_t bestIdx[16] = {};
    for (const double inset : {0.0, 1.0 / 16}) {
        double lo[3], hi[3];
        const double sLo = tmin + (tmax - tmin) * inset, sHi = tmax - (tmax - tmin) * inset;
        for (int c = 0; c < 3; ++c) {
            lo[c] = mean[c] + axis[c] * sLo;
            hi[c] = mean[c] + axis[c] * sHi;
        }
        for (int it = 0; it < 3; ++it) {
            uint16_t c0 = Quant565(hi), c1 = Quant565(lo);
            if (three) {
                if (c0 > c1) std::swap(c0, c1);
            } else {
                if (c0 < c1) std::swap(c0, c1);
                if (c0 == c1) {
                    if ((c0 & 31) < 31) ++c0; else --c1;
                }
            }
            uint8_t idx[16];
            const double err = FitIndices(px, transparent, c0, c1, idx);
            if (bestErr < 0 || err < bestErr) {
                bestErr = err;
                best0 = c0;
                best1 = c1;
                std::memcpy(bestIdx, idx, 16);
            }
            if (three) break;
            // Least-squares endpoints for these indices; the minimum-norm solution when every
            // texel has the same index.
            static constexpr double kWeight[4] = {1.0, 0.0, 2.0 / 3, 1.0 / 3};
            bool oneIndex = true;
            double aa = 0, ab = 0, bb = 0, ax[3] = {}, bx[3] = {}, sum[3] = {};
            for (int k = 0; k < 16; ++k) {
                const double a = kWeight[idx[k]], b = 1.0 - a;
                oneIndex = oneIndex && idx[k] == idx[0];
                aa += a * a;
                ab += a * b;
                bb += b * b;
                for (int c = 0; c < 3; ++c) {
                    ax[c] += a * px[k][c];
                    bx[c] += b * px[k][c];
                    sum[c] += px[k][c];
                }
            }
            if (oneIndex) {
                const double a = kWeight[idx[0]], b = 1.0 - a, rr = a * a + b * b;
                for (int c = 0; c < 3; ++c) {
                    hi[c] = std::clamp(a * (sum[c] / 16) / rr, 0.0, 255.0);
                    lo[c] = std::clamp(b * (sum[c] / 16) / rr, 0.0, 255.0);
                }
            } else {
                const double det = aa * bb - ab * ab;
                for (int c = 0; c < 3; ++c) {
                    hi[c] = std::clamp((ax[c] * bb - bx[c] * ab) / det, 0.0, 255.0);
                    lo[c] = std::clamp((bx[c] * aa - ax[c] * ab) / det, 0.0, 255.0);
                }
            }
        }
    }
    uint32_t word = 0;
    for (int k = 0; k < 16; ++k) word |= uint32_t(bestIdx[k]) << (30 - 2 * k);
    const uint8_t bytes[8] = {uint8_t(best0 >> 8), uint8_t(best0), uint8_t(best1 >> 8), uint8_t(best1),
                              uint8_t(word >> 24), uint8_t(word >> 16), uint8_t(word >> 8), uint8_t(word)};
    std::memcpy(out, bytes, 8);
}

// ------------------------------------------------------------------------------------------ worker
// Textures are made on one worker thread: from the disc after Prepare, or from the game's copy
// when that differs or was not prepared. Results are kept for the session.
struct Entry {
    enum class State { Queued, Done, Unchanged } state = State::Queued;
    bool fromDisc = false;
    std::vector<uint8_t> source;  // home kit data the result is made from
    uint32_t width = 0, height = 0, levels = 0;
    std::vector<uint8_t> result;
};

struct Job {
    int32_t captain = -1;
    const Texture* texture = nullptr;
    std::shared_ptr<Entry> entry;
};

struct Batch {
    uint32_t remaining = 0, recoloured = 0;
    std::chrono::steady_clock::time_point start;
};

std::mutex g_mutex;
std::condition_variable g_wake;
std::deque<Job> g_jobs;
std::map<std::pair<int32_t, uint32_t>, std::shared_ptr<Entry>> g_entries;  // (captain, texture hash)
std::map<int32_t, Batch> g_batches;                                         // Prepare progress, for the log
bool g_workerStarted = false;

void Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait(lock, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        Entry& e = *job.entry;
        std::vector<uint8_t> source;
        uint32_t w = 0, h = 0, levels = 0;
        if (e.fromDisc) {
            std::vector<uint8_t> file;
            BundleTexture t;
            if (DVDReadFileForRuntime(job.texture->bundle.c_str(), file) && FindInBundle(file, job.texture->hash, t)) {
                source = std::move(t.data);
                w = t.width;
                h = t.height;
                levels = t.levels;
            } else {
                RT_LOGF(RT_TAG_CONFIG, "Away kits: texture %08X not found in %s\n", job.texture->hash, job.texture->bundle.c_str());
            }
        } else {
            source = e.source;  // set before the job was queued, never changed
            w = e.width;
            h = e.height;
            levels = e.levels;
        }
        std::vector<uint8_t> result = source;
        const bool changed = !source.empty() && source.size() == DataSize(w, h, levels) &&
                             Recolour(job.captain, *job.texture, result.data(), w, h, levels);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (e.fromDisc) {
            e.source = std::move(source);
            e.width = w;
            e.height = h;
            e.levels = levels;
            if (auto it = g_batches.find(job.captain); it != g_batches.end() && it->second.remaining > 0) {
                it->second.recoloured += changed ? 1 : 0;
                if (--it->second.remaining == 0) {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - it->second.start);
                    RT_LOGF(RT_TAG_CONFIG, "Away kits: %s ready (%u textures recoloured, %lld ms)\n", Find(job.captain)->name,
                            it->second.recoloured, static_cast<long long>(ms.count()));
                }
            }
        }
        if (changed) e.result = std::move(result);
        e.state = changed ? Entry::State::Done : Entry::State::Unchanged;
    }
}

void Enqueue(Job job, bool urgent) {  // with g_mutex held
    if (urgent) g_jobs.push_front(std::move(job)); else g_jobs.push_back(std::move(job));
    if (!g_workerStarted) {
        g_workerStarted = true;
        std::thread(Worker).detach();
    }
    g_wake.notify_one();
}

} // namespace

bool Has(int32_t captain) { return Find(captain) != nullptr; }

uint32_t TeamColour(int32_t captain) {
    const Captain* c = Find(captain);
    return c ? c->target : 0;
}

const std::vector<Texture>& Textures(int32_t captain) {
    static const std::vector<Texture> s_none;
    const auto& lists = TextureLists();
    const auto it = lists.find(captain);
    return it != lists.end() ? it->second : s_none;
}

void Prepare(int32_t captain) {
    const std::vector<Texture>& list = Textures(captain);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const Texture& t : list) {
        std::shared_ptr<Entry>& slot = g_entries[{captain, t.hash}];
        if (slot) continue;
        slot = std::make_shared<Entry>();
        slot->fromDisc = true;
        Batch& batch = g_batches[captain];
        if (batch.remaining++ == 0) {
            batch.start = std::chrono::steady_clock::now();
            batch.recoloured = 0;
        }
        Enqueue({captain, &t, slot}, false);
    }
}

Result Get(int32_t captain, const Texture& texture, const uint8_t* data, uint32_t size, uint32_t width,
           uint32_t height, uint32_t levels, std::vector<uint8_t>& out) {
    if (!Has(captain) || data == nullptr) return Result::Unchanged;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::shared_ptr<Entry>& slot = g_entries[{captain, texture.hash}];
    if (slot) {
        if (slot->state == Entry::State::Queued) return Result::Pending;
        if (slot->source.size() == size && slot->width == width && slot->height == height && slot->levels == levels &&
            std::memcmp(slot->source.data(), data, size) == 0) {
            if (slot->state == Entry::State::Unchanged) return Result::Unchanged;
            out = slot->result;
            return Result::Ready;
        }
    }
    // Not prepared, or the game's copy is not the disc's: make it from the game's copy.
    slot = std::make_shared<Entry>();
    slot->source.assign(data, data + size);
    slot->width = width;
    slot->height = height;
    slot->levels = levels;
    Enqueue({captain, &texture, slot}, true);
    return Result::Pending;
}

bool Recolour(int32_t captain, const Texture& texture, uint8_t* data, uint32_t width, uint32_t height,
              uint32_t levels) {
    const Captain* c = Find(captain);
    if (c == nullptr || data == nullptr || width == 0 || height == 0) return false;
    levels = std::max(1u, levels);
    const Image base = DecodeCmpr(data, width, height);
    Mask kit, trim;
    if (!FindKit(*c, texture, base, kit, trim)) return false;
    if (std::find(kit.begin(), kit.end(), 1) == kit.end() && std::find(trim.begin(), trim.end(), 1) == trim.end()) return false;

    // Texels of a block share few colours: transform each colour once.
    const Transform tr = MakeTransform(*c, texture.kind == Kind::Background);
    std::unordered_map<uint32_t, uint32_t> kitColours, trimColours;
    const auto recolour = [&](bool isTrim, const uint8_t* in, uint8_t* out) {
        auto [it, fresh] = (isTrim ? trimColours : kitColours).try_emplace((uint32_t(in[0]) << 16) | (uint32_t(in[1]) << 8) | in[2], 0u);
        if (fresh) {
            uint8_t o[3];
            if (isTrim) tr.Trim(in, o); else tr.Kit(in, o);
            it->second = (uint32_t(o[0]) << 16) | (uint32_t(o[1]) << 8) | o[2];
        }
        out[0] = uint8_t(it->second >> 16);
        out[1] = uint8_t(it->second >> 8);
        out[2] = uint8_t(it->second);
    };
    const auto endpoint = [&](uint16_t colour) {
        int e[3];
        Expand565(colour, e);
        const uint8_t in[3] = {uint8_t(e[0]), uint8_t(e[1]), uint8_t(e[2])};
        uint8_t o[3];
        recolour(false, in, o);
        const double f[3] = {double(o[0]), double(o[1]), double(o[2])};
        return Quant565(f);
    };

    uint32_t offset = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        const uint32_t lw = std::max(1u, width >> level), lh = std::max(1u, height >> level);
        uint8_t* lvl = data + offset;
        offset += LevelSize(lw, lh);
        const Image img = level == 0 ? base : DecodeCmpr(lvl, lw, lh);
        // This level's masks: kit/trim where at least half of the level-0 texels underneath are.
        const uint32_t fx = width / lw, fy = height / lh;
        Mask km(size_t(lw) * lh), tm(size_t(lw) * lh);
        for (uint32_t y = 0; y < lh; ++y) {
            for (uint32_t x = 0; x < lw; ++x) {
                uint32_t k = 0, t = 0;
                for (uint32_t yy = 0; yy < fy; ++yy) {
                    for (uint32_t xx = 0; xx < fx; ++xx) {
                        const size_t i = size_t(y * fy + yy) * width + (x * fx + xx);
                        k += kit[i];
                        t += trim[i];
                    }
                }
                km[size_t(y) * lw + x] = 2 * k >= fx * fy;
                tm[size_t(y) * lw + x] = 2 * t >= fx * fy;
            }
        }
        Image out = img;
        for (size_t i = 0; i < km.size(); ++i) {
            if (tm[i]) recolour(true, &img.rgba[i * 4], &out.rgba[i * 4]);
            else if (km[i]) recolour(false, &img.rgba[i * 4], &out.rgba[i * 4]);
        }
        ForEachBlock(lw, lh, [&](uint32_t p, uint32_t bx, uint32_t by) {
            if (bx >= lw || by >= lh) return;
            bool anyKit = false, anyTrim = false, allKit = true;
            uint32_t inside = 0;
            for (uint32_t k = 0; k < 16; ++k) {
                const uint32_t x = bx + k % 4, y = by + k / 4;
                if (x >= lw || y >= lh) continue;
                ++inside;
                const size_t i = size_t(y) * lw + x;
                anyKit = anyKit || km[i];
                anyTrim = anyTrim || tm[i];
                allKit = allKit && km[i];
            }
            if (!anyKit && !anyTrim) return;
            uint8_t* blk = lvl + p;
            const uint16_t c0 = uint16_t((blk[0] << 8) | blk[1]), c1 = uint16_t((blk[2] << 8) | blk[3]);
            if (allKit && !anyTrim && inside == 16 && c0 > c1) {
                // All kit, four colours: transform the two endpoints and keep the indices.
                uint16_t n0 = endpoint(c0), n1 = endpoint(c1);
                uint32_t idx = (uint32_t(blk[4]) << 24) | (uint32_t(blk[5]) << 16) | (uint32_t(blk[6]) << 8) | blk[7];
                if (n0 < n1) {
                    std::swap(n0, n1);
                    idx ^= 0x55555555u;  // 0<->1, 2<->3
                }
                if (n0 == n1) {  // four-colour mode needs c0 > c1
                    if ((n0 & 31) < 31) ++n0; else --n1;
                }
                const uint8_t bytes[8] = {uint8_t(n0 >> 8), uint8_t(n0), uint8_t(n1 >> 8), uint8_t(n1),
                                          uint8_t(idx >> 24), uint8_t(idx >> 16), uint8_t(idx >> 8), uint8_t(idx)};
                std::memcpy(blk, bytes, 8);
                return;
            }
            double px[16][3];
            bool transparent[16] = {};
            for (uint32_t k = 0; k < 16; ++k) {
                const uint32_t x = bx + k % 4, y = by + k / 4;
                if (x < lw && y < lh) {
                    const uint8_t* q = out.at(x, y);
                    for (int ch = 0; ch < 3; ++ch) px[k][ch] = q[ch];
                    transparent[k] = q[3] == 0;
                } else {
                    for (int ch = 0; ch < 3; ++ch) px[k][ch] = k ? px[0][ch] : 0.0;  // padding outside the level
                }
            }
            EncodeBlock(px, transparent, blk);
        });
    }
    return true;
}

} // namespace MscAwayKits
