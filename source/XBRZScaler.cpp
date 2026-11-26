// Edge-aware pixel-art upscaler following the xBRZ algorithm.
// Independent implementation for WarpNES; see XBRZScaler.hpp.
//
// Overview
//  1. For every 2x2 block of source pixels, compare the two diagonals using a
//     perceptual colour distance over the surrounding 4x4 area. If one
//     diagonal is clearly the edge, mark the two pixels on the other diagonal
//     to blend toward it in the corner they share.
//  2. For every source pixel and each marked corner, look at the 3x3
//     neighbourhood to decide the shape of the edge (shallow, steep, both,
//     45 degrees, or only a rounded corner) and paint that shape into the
//     scaled output block with partial alpha.
// Corners are handled by rotating the neighbourhood so the shape logic only
// ever deals with the bottom-right corner.

#include "XBRZScaler.hpp"
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

enum Blend : uint8_t { BLEND_NONE = 0, BLEND_NORMAL = 1, BLEND_DOMINANT = 2 };

// Corner indices: 0=top-left 1=top-right 2=bottom-left 3=bottom-right,
// i.e. index = row*2 + col in a 2x2 grid.
struct Corners { uint8_t c[4]; };

const double kEqualColorTolerance = 30.0;
const double kDominantDirectionThreshold = 3.6;
const double kSteepDirectionThreshold = 2.2;

inline int chR(uint32_t p) { return (p >> 16) & 0xFF; }
inline int chG(uint32_t p) { return (p >> 8) & 0xFF; }
inline int chB(uint32_t p) { return p & 0xFF; }

// Perceptual distance in a YCbCr-like space (BT.709 weights).
class ColorDistance {
public:
    double operator()(uint32_t a, uint32_t b) {
        a &= 0xFFFFFF;
        b &= 0xFFFFFF;
        if (a == b) return 0.0;
        // NES frames use few colours, so memoise pairs.
        uint64_t key = a < b ? ((uint64_t)a << 24) | b : ((uint64_t)b << 24) | a;
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;

        double dr = chR(a) - chR(b);
        double dg = chG(a) - chG(b);
        double db = chB(a) - chB(b);
        const double kR = 0.2126, kB = 0.0722, kG = 1 - kR - kB;
        double y = kR * dr + kG * dg + kB * db;
        double cb = 0.5 / (1 - kB) * (db - y);
        double cr = 0.5 / (1 - kR) * (dr - y);
        double d = std::sqrt(y * y + cb * cb + cr * cr);

        if (cache_.size() > 65536) cache_.clear();
        cache_[key] = d;
        return d;
    }
private:
    std::unordered_map<uint64_t, double> cache_;
};

thread_local ColorDistance g_dist;

inline double dist(uint32_t a, uint32_t b) { return g_dist(a, b); }
inline bool eq(uint32_t a, uint32_t b) { return dist(a, b) < kEqualColorTolerance; }

inline uint32_t mix(uint32_t dst, uint32_t src, double alpha) {
    auto lerp = [alpha](int d, int s) { return (int)std::lround(d + (s - d) * alpha); };
    return 0xFF000000u | (lerp(chR(dst), chR(src)) << 16) | (lerp(chG(dst), chG(src)) << 8) | lerp(chB(dst), chB(src));
}

// Rotate a (row, col) coordinate in an n x n grid by 90 degrees, `rot` times.
// Rotation k maps the bottom-right corner to BR, BL, TL, TR for k = 0..3.
inline void rotate(int& i, int& j, int n, int rot) {
    for (int k = 0; k < rot; k++) {
        int ni = j;
        int nj = n - 1 - i;
        i = ni;
        j = nj;
    }
}

// Coverage of the rounded-corner shape for each output pixel, computed once
// per scale factor: the part of the bottom-right quadrant of the block that
// lies outside a circle inscribed in the block (for 2x this is 1 - pi/4).
const std::vector<double>& cornerCoverage(int n) {
    static std::vector<double> tables[5];
    std::vector<double>& t = tables[n];
    if (t.empty()) {
        t.assign(n * n, 0.0);
        const int ss = 64;  // supersamples per axis
        double c = n / 2.0, r2 = c * c;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                int inside = 0;
                for (int sy = 0; sy < ss; sy++) {
                    for (int sx = 0; sx < ss; sx++) {
                        double y = i + (sy + 0.5) / ss;
                        double x = j + (sx + 0.5) / ss;
                        if (x > c && y > c && (x - c) * (x - c) + (y - c) * (y - c) > r2) inside++;
                    }
                }
                t[i * n + j] = (double)inside / (ss * ss);
            }
        }
    }
    return t;
}

// Writes into the scaled block of one source pixel, seen through a rotation.
class OutBlock {
public:
    OutBlock(uint32_t* dst, int pitch, int n, int rot) : dst_(dst), pitch_(pitch), n_(n), rot_(rot) {}
    void blend(int i, int j, uint32_t color, double alpha) {
        if (alpha <= 0.0) return;
        rotate(i, j, n_, rot_);
        uint32_t& p = dst_[i * pitch_ + j];
        p = mix(p, color, alpha);
    }
    int n() const { return n_; }
private:
    uint32_t* dst_;
    int pitch_;
    int n_;
    int rot_;
};

// Shape painters. Coordinates are (row, col) in the rotated block, with the
// corner being processed at (n-1, n-1). A "shallow" edge runs mostly
// horizontally along the bottom, a "steep" edge mostly vertically along the
// right side.
void lineShallow(OutBlock& o, uint32_t c) {
    switch (o.n()) {
    case 2:
        o.blend(1, 0, c, 0.25); o.blend(1, 1, c, 0.75);
        break;
    case 3:
        o.blend(2, 0, c, 0.25); o.blend(1, 2, c, 0.25);
        o.blend(2, 1, c, 0.75); o.blend(2, 2, c, 1.0);
        break;
    default:
        o.blend(3, 0, c, 0.25); o.blend(2, 2, c, 0.25);
        o.blend(3, 1, c, 0.75); o.blend(2, 3, c, 0.75);
        o.blend(3, 2, c, 1.0);  o.blend(3, 3, c, 1.0);
        break;
    }
}

void lineSteep(OutBlock& o, uint32_t c) {
    switch (o.n()) {
    case 2:
        o.blend(0, 1, c, 0.25); o.blend(1, 1, c, 0.75);
        break;
    case 3:
        o.blend(0, 2, c, 0.25); o.blend(2, 1, c, 0.25);
        o.blend(1, 2, c, 0.75); o.blend(2, 2, c, 1.0);
        break;
    default:
        o.blend(0, 3, c, 0.25); o.blend(2, 2, c, 0.25);
        o.blend(1, 3, c, 0.75); o.blend(3, 2, c, 0.75);
        o.blend(2, 3, c, 1.0);  o.blend(3, 3, c, 1.0);
        break;
    }
}

void lineSteepAndShallow(OutBlock& o, uint32_t c) {
    switch (o.n()) {
    case 2:
        o.blend(1, 0, c, 0.25); o.blend(0, 1, c, 0.25); o.blend(1, 1, c, 5.0 / 6.0);
        break;
    case 3:
        o.blend(2, 0, c, 0.25); o.blend(0, 2, c, 0.25);
        o.blend(2, 1, c, 0.75); o.blend(1, 2, c, 0.75);
        o.blend(2, 2, c, 1.0);
        break;
    default:
        o.blend(3, 1, c, 0.75); o.blend(1, 3, c, 0.75);
        o.blend(3, 0, c, 0.25); o.blend(0, 3, c, 0.25);
        o.blend(2, 2, c, 1.0 / 3.0);
        o.blend(3, 3, c, 1.0); o.blend(3, 2, c, 1.0); o.blend(2, 3, c, 1.0);
        break;
    }
}

void lineDiagonal(OutBlock& o, uint32_t c) {
    switch (o.n()) {
    case 2:
        o.blend(1, 1, c, 0.5);
        break;
    case 3:
        o.blend(1, 2, c, 0.125); o.blend(2, 1, c, 0.125); o.blend(2, 2, c, 0.875);
        break;
    default:
        o.blend(3, 2, c, 0.5); o.blend(2, 3, c, 0.5); o.blend(3, 3, c, 1.0);
        break;
    }
}

void roundCorner(OutBlock& o, uint32_t c) {
    const std::vector<double>& cov = cornerCoverage(o.n());
    int n = o.n();
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            o.blend(i, j, c, cov[i * n + j]);
}

} // namespace

void xbrzStyleScale(int factor, const uint32_t* src, uint32_t* dst, int width, int height) {
    if (factor < 2) factor = 2;
    if (factor > 4) factor = 4;
    const int n = factor;
    const int dstPitch = width * n;

    auto px = [&](int x, int y) -> uint32_t {
        if (x < 0) x = 0; else if (x >= width) x = width - 1;
        if (y < 0) y = 0; else if (y >= height) y = height - 1;
        return src[y * width + x];
    };

    // ---- Step 1: decide corner blending for every 2x2 block -------------
    //   A B C D
    //   E F G H      the block is F G / J K; its centre is the corner shared
    //   I J K L      by those four pixels
    //   M N O P
    std::vector<Corners> blend((size_t)width * height);
    std::memset(blend.data(), 0, blend.size() * sizeof(Corners));

    for (int y = -1; y < height; y++) {
        for (int x = -1; x < width; x++) {
            uint32_t b = px(x, y - 1),     c = px(x + 1, y - 1);
            uint32_t e = px(x - 1, y),     f = px(x, y),     g = px(x + 1, y),     h = px(x + 2, y);
            uint32_t i = px(x - 1, y + 1), j = px(x, y + 1), k = px(x + 1, y + 1), l = px(x + 2, y + 1);
            uint32_t n2 = px(x, y + 2),    o = px(x + 1, y + 2);

            if ((f == g && j == k) || (f == j && g == k)) continue;  // flat or plain stripe

            double jg = dist(i, f) + dist(f, c) + dist(n2, k) + dist(k, h) + 4.0 * dist(j, g);
            double fk = dist(e, j) + dist(j, o) + dist(b, g) + dist(g, l) + 4.0 * dist(f, k);

            auto mark = [&](int px_, int py_, int cornerIdx, uint8_t type) {
                if (px_ >= 0 && py_ >= 0 && px_ < width && py_ < height) {
                    uint8_t& slot = blend[(size_t)py_ * width + px_].c[cornerIdx];
                    if (type > slot) slot = type;
                }
            };

            if (jg < fk) {
                // J-G is the edge: blend F and K toward it
                uint8_t type = (kDominantDirectionThreshold * jg < fk) ? BLEND_DOMINANT : BLEND_NORMAL;
                if (f != g && f != j) mark(x, y, 3, type);          // F's bottom-right
                if (k != j && k != g) mark(x + 1, y + 1, 0, type);  // K's top-left
            } else if (fk < jg) {
                // F-K is the edge: blend J and G toward it
                uint8_t type = (kDominantDirectionThreshold * fk < jg) ? BLEND_DOMINANT : BLEND_NORMAL;
                if (j != f && j != k) mark(x, y + 1, 1, type);      // J's top-right
                if (g != f && g != k) mark(x + 1, y, 2, type);      // G's bottom-left
            }
        }
    }

    // ---- Step 2: paint each source pixel's scaled block ------------------
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t e = src[y * width + x] | 0xFF000000u;
            uint32_t* out = dst + (size_t)(y * n) * dstPitch + x * n;
            for (int r = 0; r < n; r++)
                for (int col = 0; col < n; col++)
                    out[r * dstPitch + col] = e;

            const Corners& bl = blend[(size_t)y * width + x];
            if (!(bl.c[0] | bl.c[1] | bl.c[2] | bl.c[3])) continue;

            // 3x3 neighbourhood, row-major:  a b c / d e f / g h i
            uint32_t ker[3][3];
            for (int ky = 0; ky < 3; ky++)
                for (int kx = 0; kx < 3; kx++)
                    ker[ky][kx] = px(x + kx - 1, y + ky - 1);

            for (int rot = 0; rot < 4; rot++) {
                // View the kernel and corners rotated so this corner is bottom-right
                auto K = [&](int ky, int kx) { rotate(ky, kx, 3, rot); return ker[ky][kx]; };
                auto C = [&](int cy, int cx) { rotate(cy, cx, 2, rot); return bl.c[cy * 2 + cx]; };

                uint8_t bottomRight = C(1, 1);
                if (bottomRight == BLEND_NONE) continue;
                uint8_t topRight = C(0, 1);
                uint8_t bottomLeft = C(1, 0);

                uint32_t b = K(0, 1), c = K(0, 2);
                uint32_t d = K(1, 0), f = K(1, 2);
                uint32_t g = K(2, 0), h = K(2, 1), i = K(2, 2);

                bool doLine = true;
                if (bottomRight < BLEND_DOMINANT) {
                    // Don't draw a line if a neighbouring corner already blends
                    // toward a different colour (isolated pixels, e.g. eyes)
                    if (topRight != BLEND_NONE && !eq(e, g)) doLine = false;
                    else if (bottomLeft != BLEND_NONE && !eq(e, c)) doLine = false;
                    // L-shaped joints only get a rounded corner
                    else if (!eq(e, i) && eq(g, h) && eq(h, i) && eq(i, f) && eq(f, c)) doLine = false;
                }

                uint32_t blendColor = dist(e, f) <= dist(e, h) ? f : h;
                OutBlock ob(out, dstPitch, n, rot);

                if (doLine) {
                    double fg = dist(f, g);
                    double hc = dist(h, c);
                    bool shallow = kSteepDirectionThreshold * fg <= hc && e != g && d != g;
                    bool steep = kSteepDirectionThreshold * hc <= fg && e != c && b != c;
                    if (shallow && steep) lineSteepAndShallow(ob, blendColor);
                    else if (shallow) lineShallow(ob, blendColor);
                    else if (steep) lineSteep(ob, blendColor);
                    else lineDiagonal(ob, blendColor);
                } else {
                    roundCorner(ob, blendColor);
                }
            }
        }
    }
}
