#include "terraincomposite.h"
#include <functional>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "source.h"
#include "splat.h"
#include "terrainlayers.h"
#include "terrainstatic.h"
#include "texture.h"

namespace bf6 {

// ===========================================================================
// PART 1 - BCn decode.
//
// Written here rather than reached for elsewhere because there IS nowhere
// else: texture.h returns compressed blocks by design and splat.cpp's BC4 page
// codec is private and only ever sees 66x66 weight pages. Straight from the
// DXGI block-compression spec; the tables are the spec's tables.
// ===========================================================================
namespace {

// ---- BC1/BC3/BC4/BC5 -------------------------------------------------------

inline void rgb565(uint16_t v, uint8_t* o)
{
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    // BLUE IS FIVE BITS, LIKE RED. It was expanded with the SIX-bit constants,
    // so a fully saturated blue endpoint decoded to 126 instead of 255 and
    // every BC1 image came back starved of blue. Layer sheets are all BC7 in
    // the shipped fleet, so materials never showed it; the victim was the
    // COLOUR MAP, and the levels whose colour map is BC1 were exactly the
    // worst-scoring levels against the shipped Portal overlays.
    o[0] = (uint8_t)((r * 527 + 23) >> 6);
    o[1] = (uint8_t)((g * 259 + 33) >> 6);
    o[2] = (uint8_t)((b * 527 + 23) >> 6);
}

// `opaque_only` is BC1-in-BC2/BC3: the c0 <= c1 punch-through case does not
// exist there, so the four-colour ramp is always used.
void bc1_block(const uint8_t* b, uint8_t out[16][4], bool opaque_only)
{
    uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
    uint8_t p[4][4];
    rgb565(c0, p[0]); p[0][3] = 255;
    rgb565(c1, p[1]); p[1][3] = 255;
    if (c0 > c1 || opaque_only)
    {
        for (int i = 0; i < 3; i++)
        {
            p[2][i] = (uint8_t)((2 * p[0][i] + p[1][i] + 1) / 3);
            p[3][i] = (uint8_t)((p[0][i] + 2 * p[1][i] + 1) / 3);
        }
        p[2][3] = p[3][3] = 255;
    }
    else
    {
        for (int i = 0; i < 3; i++)
        {
            p[2][i] = (uint8_t)((p[0][i] + p[1][i] + 1) / 2);
            p[3][i] = 0;
        }
        p[2][3] = 255; p[3][3] = 0;
    }
    const uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                          ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    for (int i = 0; i < 16; i++)
        std::memcpy(out[i], p[(bits >> (2 * i)) & 3], 4);
}

void bc4_block(const uint8_t* b, uint8_t out[16])
{
    int a[8];
    a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1])
        for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1] + 3) / 7;
    else
    {
        for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1] + 2) / 5;
        a[6] = 0; a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)a[(bits >> (3 * i)) & 7];
}

// ---- BC7 -------------------------------------------------------------------

struct BitReader {
    const uint8_t* d; int pos;
    uint32_t get(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++)
        { v |= (uint32_t)((d[pos >> 3] >> (pos & 7)) & 1u) << i; pos++; }
        return v;
    }
};

struct Bc7Mode { int ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; };
const Bc7Mode kBc7[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0},
    {2, 6, 0, 0, 6, 0, 0, 1, 3, 0},
    {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3},
    {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0},
    {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};

const uint8_t kP2[64][16] = {
{0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1},{0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},
{0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},{0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
{0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
{0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
{0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
{0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
{0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1},{0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
{0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0},{0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
{0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},{0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},
{0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},{0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
{0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0},{0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0},
{0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},{0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
{0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0},{0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
{0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0},{0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
{0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},{0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},
{0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0},{0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
{0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0},{0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},
{0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},{0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
{0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0},{0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
{0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0},{0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
{0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},{0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},
{0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1},{0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
{0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0},{0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},
{0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},{0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
{0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
{0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0},{0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
{0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1},{0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},
{0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1},{0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
{0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1},{0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},
{0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0},{0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1},
};

const uint8_t kP3[64][16] = {
{0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},
{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
{0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},
{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
{0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},
{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
{0,0,0,1,0,0,1,2,0,1,2,2,1,2,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
{0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
{0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},
{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
{0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},
{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
{0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
{0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
{0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},
{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
{0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},
{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
{0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
{0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
{0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},
{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
{0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},
{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
{0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
{0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
{0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},
{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
{0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},
{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0},
};

const uint8_t kA2of2[64] = {
15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6,
 6, 2, 6, 8,15,15, 2, 2,15,15,15,15,15, 2, 2,15};
const uint8_t kA2of3[64] = {
 3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3,
 3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
 8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15,
 3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3};
const uint8_t kA3of3[64] = {
15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8,
15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8,
15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8};

const uint16_t kW2[4]  = {0, 21, 43, 64};
const uint16_t kW3[8]  = {0, 9, 18, 27, 37, 46, 55, 64};
const uint16_t kW4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

inline const uint16_t* weights(int bits)
{ return bits == 2 ? kW2 : bits == 3 ? kW3 : kW4; }

void bc7_block(const uint8_t* b, uint8_t out[16][4])
{
    BitReader br{b, 0};
    int mode = -1;
    for (int i = 0; i < 8; i++) if (br.get(1)) { mode = i; break; }
    if (mode < 0)                       // all-zero mode field: reserved, black
    { for (int i = 0; i < 16; i++) { out[i][0] = out[i][1] = out[i][2] = 0; out[i][3] = 255; } return; }

    const Bc7Mode& m = kBc7[mode];
    const int part = m.pb ? (int)br.get(m.pb) : 0;
    const int rot  = m.rb ? (int)br.get(m.rb) : 0;
    const int isel = m.isb ? (int)br.get(m.isb) : 0;

    int ep[6][4];
    for (int c = 0; c < 3; c++)
        for (int e = 0; e < m.ns * 2; e++) ep[e][c] = (int)br.get(m.cb);
    for (int e = 0; e < m.ns * 2; e++) ep[e][3] = m.ab ? (int)br.get(m.ab) : 255;

    int pbit[6] = {0, 0, 0, 0, 0, 0};
    if (m.epb) for (int e = 0; e < m.ns * 2; e++) pbit[e] = (int)br.get(1);
    if (m.spb) for (int s = 0; s < m.ns; s++)
    { const int p = (int)br.get(1); pbit[2 * s] = pbit[2 * s + 1] = p; }

    const int extra = (m.epb || m.spb) ? 1 : 0;
    const int cbits = m.cb + extra;
    const int abits = m.ab ? m.ab + extra : 0;
    for (int e = 0; e < m.ns * 2; e++)
    {
        for (int c = 0; c < 3; c++)
        {
            int v = ep[e][c];
            if (extra) v = (v << 1) | pbit[e];
            v = (v << (8 - cbits)) | (v >> (2 * cbits - 8));
            ep[e][c] = v & 0xFF;
        }
        if (m.ab)
        {
            int v = ep[e][3];
            if (extra) v = (v << 1) | pbit[e];
            v = (v << (8 - abits)) | (v >> (2 * abits - 8));
            ep[e][3] = v & 0xFF;
        }
    }

    const uint8_t* pt = m.ns == 3 ? kP3[part] : m.ns == 2 ? kP2[part] : nullptr;
    int anchor[3] = {0, 0, 0};
    if (m.ns == 2) anchor[1] = kA2of2[part];
    else if (m.ns == 3) { anchor[1] = kA2of3[part]; anchor[2] = kA3of3[part]; }

    int idx1[16], idx2[16];
    for (int i = 0; i < 16; i++)
    {
        const int s = pt ? pt[i] : 0;
        const bool anch = (i == anchor[s]);
        idx1[i] = (int)br.get(m.ib - (anch ? 1 : 0));
    }
    if (m.ib2)
        for (int i = 0; i < 16; i++)
        {
            const bool anch = (i == 0);   // ib2 only exists for ns == 1
            idx2[i] = (int)br.get(m.ib2 - (anch ? 1 : 0));
        }

    const uint16_t* w1 = weights(m.ib);
    const uint16_t* w2 = m.ib2 ? weights(m.ib2) : nullptr;
    for (int i = 0; i < 16; i++)
    {
        const int s = pt ? pt[i] : 0;
        int cw, aw;
        if (!m.ib2) { cw = w1[idx1[i]]; aw = cw; }
        else if (!isel) { cw = w1[idx1[i]]; aw = w2[idx2[i]]; }
        else            { cw = w2[idx2[i]]; aw = w1[idx1[i]]; }

        const int* e0 = ep[2 * s];
        const int* e1 = ep[2 * s + 1];
        for (int c = 0; c < 3; c++)
            out[i][c] = (uint8_t)((e0[c] * (64 - cw) + e1[c] * cw + 32) >> 6);
        out[i][3] = m.ab ? (uint8_t)((e0[3] * (64 - aw) + e1[3] * aw + 32) >> 6)
                         : (uint8_t)255;
        if (rot) { const int c = rot - 1; std::swap(out[i][3], out[i][c]); }
    }
}

}  // namespace

bool bcn_to_rgba8(const uint8_t* blocks, size_t nbytes, int w, int h, int dxgi,
                  std::vector<uint8_t>& rgba, std::string& err)
{
    if (w <= 0 || h <= 0) { err = "bad dimensions"; return false; }
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const int bb = Texture::block_bytes(dxgi);
    const size_t need = (size_t)bw * (size_t)bh * (size_t)bb;
    if (nbytes < need)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "short block payload: %zu of %zu bytes",
                      nbytes, need);
        err = msg;
        return false;
    }
    const bool bc1 = (dxgi == 71 || dxgi == 72);
    const bool bc3 = (dxgi == 77 || dxgi == 78);
    const bool bc4 = (dxgi == 80);
    const bool bc5 = (dxgi == 83);
    const bool bc7 = (dxgi == 98 || dxgi == 99);
    if (!bc1 && !bc3 && !bc4 && !bc5 && !bc7)
    {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "unsupported dxgi %d", dxgi);
        err = msg;
        return false;
    }

    rgba.assign((size_t)w * (size_t)h * 4, 0);
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
        {
            const uint8_t* src = blocks + ((size_t)by * bw + bx) * bb;
            uint8_t px[16][4];
            if (bc1)      bc1_block(src, px, false);
            else if (bc7) bc7_block(src, px);
            else if (bc3)
            {
                uint8_t a[16];
                bc4_block(src, a);
                bc1_block(src + 8, px, true);
                for (int i = 0; i < 16; i++) px[i][3] = a[i];
            }
            else if (bc4)
            {
                uint8_t r[16];
                bc4_block(src, r);
                for (int i = 0; i < 16; i++)
                { px[i][0] = px[i][1] = px[i][2] = r[i]; px[i][3] = 255; }
            }
            else   // bc5: two BC4 halves -> R, G
            {
                uint8_t r[16], g[16];
                bc4_block(src, r);
                bc4_block(src + 8, g);
                for (int i = 0; i < 16; i++)
                { px[i][0] = r[i]; px[i][1] = g[i]; px[i][2] = 0; px[i][3] = 255; }
            }
            for (int ty = 0; ty < 4; ty++)
            {
                const int y = by * 4 + ty;
                if (y >= h) break;
                for (int tx = 0; tx < 4; tx++)
                {
                    const int x = bx * 4 + tx;
                    if (x >= w) break;
                    std::memcpy(&rgba[((size_t)y * w + x) * 4], px[ty * 4 + tx], 4);
                }
            }
        }
    return true;
}

// ===========================================================================
// PART 2 - the composite.
// ===========================================================================
namespace {

inline float saturatef(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// binary16 round trip. The kernel stores every accumulator as a half and the
// next layer reads it back, so a port that keeps float drifts by a fraction of
// a level per layer and by something visible over forty of them.
float quant_half(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    int exp = (int)((u >> 23) & 0xFF) - 127 + 15;
    uint32_t man = u & 0x7FFFFFu;
    uint16_t hv;
    if (exp <= 0)
    {
        if (exp < -10) hv = (uint16_t)sign;
        else
        {
            man |= 0x800000u;
            const int shift = 14 - exp;
            uint32_t sub = man >> shift;
            if ((man >> (shift - 1)) & 1u) sub++;
            hv = (uint16_t)(sign | sub);
        }
    }
    else if (exp >= 31) hv = (uint16_t)(sign | 0x7C00u);
    else
    {
        uint32_t m = man >> 13;
        if ((man >> 12) & 1u) m++;
        uint32_t v = (uint32_t)(exp << 10) + m;
        hv = (uint16_t)(sign | (v & 0x7FFFu));
    }
    // back to float
    const uint32_t s = (uint32_t)(hv & 0x8000u) << 16;
    int e = (hv >> 10) & 0x1F;
    uint32_t mm = hv & 0x3FF;
    uint32_t o;
    if (e == 0)
    {
        if (!mm) o = s;
        else
        {
            e = -1;
            do { e++; mm <<= 1; } while (!(mm & 0x400));
            mm &= 0x3FF;
            o = s | (uint32_t)((127 - 15 - e) << 23) | (mm << 13);
        }
    }
    else if (e == 31) o = s | 0x7F800000u | (mm << 13);
    else o = s | (uint32_t)((e - 15 + 127) << 23) | (mm << 13);
    float r;
    std::memcpy(&r, &o, 4);
    return r;
}

// IEC 61966-2-1, both directions. The shipped kernel writes the encode form
// literally; the decode form is what an sRGB texture fetch does for free and
// therefore what a software sampler owes its caller.
inline float srgb_to_linear(float c)
{ return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float linear_to_srgb(float c)
{
    c = saturatef(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

// A decoded layer sheet. Base colour is kept LINEAR because the game samples an
// sRGB texture and gets linear back; filtering the encoded bytes and
// linearising afterwards is a different (and wrong) answer.
struct Sheet {
    int w = 0, h = 0;
    std::vector<float>   lin;    // RGBA linear, base colour only
    std::vector<uint8_t> raw;    // RGBA8 as it lies, data maps only
    bool ok() const { return w > 0 && h > 0; }
};

// Bilinear, wrapping. Wrap is right for a tiling ground sheet and is what the
// shader's sampler does.
inline void sample_lin(const Sheet& s, float u, float v, float out[4])
{
    const float fx = u * (float)s.w - 0.5f, fy = v * (float)s.h - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = ((x0 % s.w) + s.w) % s.w; x1 = ((x1 % s.w) + s.w) % s.w;
    y0 = ((y0 % s.h) + s.h) % s.h; y1 = ((y1 % s.h) + s.h) % s.h;
    const float* a = &s.lin[((size_t)y0 * s.w + x0) * 4];
    const float* b = &s.lin[((size_t)y0 * s.w + x1) * 4];
    const float* c = &s.lin[((size_t)y1 * s.w + x0) * 4];
    const float* d = &s.lin[((size_t)y1 * s.w + x1) * 4];
    for (int i = 0; i < 4; i++)
        out[i] = lerpf(lerpf(a[i], b[i], tx), lerpf(c[i], d[i], tx), ty);
}

inline void sample_raw(const Sheet& s, float u, float v, float out[4])
{
    const float fx = u * (float)s.w - 0.5f, fy = v * (float)s.h - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    const float tx = fx - (float)x0, ty = fy - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = ((x0 % s.w) + s.w) % s.w; x1 = ((x1 % s.w) + s.w) % s.w;
    y0 = ((y0 % s.h) + s.h) % s.h; y1 = ((y1 % s.h) + s.h) % s.h;
    const uint8_t* a = &s.raw[((size_t)y0 * s.w + x0) * 4];
    const uint8_t* b = &s.raw[((size_t)y0 * s.w + x1) * 4];
    const uint8_t* c = &s.raw[((size_t)y1 * s.w + x0) * 4];
    const uint8_t* d = &s.raw[((size_t)y1 * s.w + x1) * 4];
    for (int i = 0; i < 4; i++)
        out[i] = lerpf(lerpf(a[i] / 255.f, b[i] / 255.f, tx),
                       lerpf(c[i] / 255.f, d[i] / 255.f, tx), ty);
}

// One layer, ready to evaluate.
struct LayerRT {
    bool  usable = false;
    Sheet base, nrmh;
    float mpr = 4.f;                // metres per texture repeat
    float rot_cos = 1.f, rot_sin = 0.f;
    float off[2] = {0, 0};
    float scale[2] = {1, 1};
    float displace = 0.f;
    float base_height = 0.f;
    float ramp_exp = 1.f;
    float height_blend = 0.f;
    float overlay = 0.f;
    float tint[3] = {1, 1, 1};
};

// §4 of the findings: the stochastic tiling that breaks tile repetition.
//
// The TAP TRANSFORM is the disassembly's, constant for constant: the 45-degree
// rotation, the 1.83852 post-scale, the sign-preserving wrap on
// (113.377, 117.827), the three sin/43758.5 hashes, the 6.2831855 angle and the
// (1.05 - 0.097618 h) tap scale.
//
// The BLEND is NOT. The kernel's radial ramp - saturate((d2 - 0.2) / -0.05)
// driving a chained lerp(prevTap, thisTap, w) - only works because the shader's
// taps come from adjacent work items in a wave, so every texel sees a different
// neighbour set and the seams average out across the quad. Reproduced literally
// on one texel at a time it is not a partition of unity: w snaps from 0 to 1
// over a d2 band of 0.05, which drew hard 45-degree diamond seams straight
// across the gravel in the first mp_aftermath bake. Replaced here with a
// normalised smooth falloff over the FOUR cells around the point, which is
// smooth by construction and keeps the same per-cell tap placement. That makes
// the breakup an approximation of the shader's, and it is the largest single
// deviation in this file.
struct Tap { float u, v, w; };

void stochastic_taps(float px, float py, Tap taps[4], int& n)
{
    const float r0 =  0.70710678f * px + 0.70710678f * py;
    const float r1 = -0.70710678f * px + 0.70710678f * py;
    const float p1x = r0 * 1.83852f, p1y = r1 * 1.83852f;

    const float Kx = 113.377f, Ky = 117.827f;
    auto hash = [](float x, float y, float a, float b) {
        const float s = std::sin(x * a + y * b) * 43758.5f;
        return s - std::floor(s);
    };

    const float bx = std::floor(p1x), by = std::floor(p1y);
    n = 0;
    for (int j = 0; j <= 1; j++)
        for (int i = 0; i <= 1; i++)
        {
            const float qx = bx + (float)i, qy = by + (float)j;
            // The wrap: sign-preserving frac of q/K scaled back by K, so the
            // cell identity stays exact a long way from the origin instead of
            // losing mantissa bits.
            const float rx = qx / Kx, ry = qy / Ky;
            const float cx = (rx >= 0.f ?  (std::fabs(rx) - std::floor(std::fabs(rx)))
                                        : -(std::fabs(rx) - std::floor(std::fabs(rx)))) * Kx;
            const float cy = (ry >= 0.f ?  (std::fabs(ry) - std::floor(std::fabs(ry)))
                                        : -(std::fabs(ry) - std::floor(std::fabs(ry)))) * Ky;
            const float hx = hash(cx, cy, 127.11f, 311.80f);
            const float hy = hash(cx, cy, 269.52f, 183.38f);
            const float hz = hash(cx, cy, 419.27f, 371.93f);
            const float ang = (hx - 0.5f) * 6.2831855f;
            const float ca = std::cos(ang), sa = std::sin(ang);
            const float sc = 1.05f - 0.097618f * hy;
            const float ax = px + hy, ay = py + hz;
            Tap& t = taps[n];
            t.u = (ax * ca - ay * sa) * sc;
            t.v = (ax * sa + ay * ca) * sc;
            // Smooth falloff from this cell's centre. Cubic so the derivative
            // vanishes at the support edge and no seam is visible.
            const float dx = p1x - (qx + 0.5f), dy = p1y - (qy + 0.5f);
            const float d = std::sqrt(dx * dx + dy * dy);
            const float f = std::max(0.f, 1.f - d / 1.35f);
            t.w = f * f * f;
            n++;
        }
}

std::string strip_ebx(std::string n)
{
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ebx") == 0) n.resize(n.size() - 4);
    return n;
}

bool load_sheet(Source& src, const std::map<std::string, std::string>& pidx,
                const std::string& guid, int max_dim, bool want_linear,
                Sheet& out, int& dxgi_out, std::string& why)
{
    if (guid.empty()) { why = "no texture bound"; return false; }
    auto it = pidx.find(guid);
    if (it == pidx.end()) { why = "guid " + guid + " not in the partition index"; return false; }
    const std::string name = strip_ebx(it->second);
    std::string e;
    std::vector<uint8_t> res = src.get_res(name, e);
    if (res.empty()) { why = "resource " + name + ": " + e; return false; }
    TextureImage img;
    auto fetch = [&](const std::string& g) { std::string e2; return src.get_chunk(g, e2); };
    if (!Texture::decode(res, fetch, img, max_dim, e)) { why = name + ": " + e; return false; }
    dxgi_out = img.dxgi;
    std::vector<uint8_t> rgba;
    if (!bcn_to_rgba8(img.blocks.data(), img.blocks.size(), img.width, img.height,
                      img.dxgi, rgba, e))
    { why = name + ": " + e; return false; }
    int w = img.width, h = img.height;

    // Texture::decode's max_dim can only choose a level WITHIN the chunk that
    // holds mip0, and on a streamed texture that chunk holds mip0 alone - so
    // asking for a small level gets the full sheet back anyway. Box-halve to
    // the requested cap here rather than sample an 800x-minified top mip. The
    // filter is a plain 2x2 box, which is what a mip chain is.
    while (w > max_dim && h > max_dim && w > 1 && h > 1)
    {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<uint8_t> half((size_t)nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++)
                {
                    const int s = rgba[((size_t)(2 * y) * w + 2 * x) * 4 + c]
                                + rgba[((size_t)(2 * y) * w + std::min(2 * x + 1, w - 1)) * 4 + c]
                                + rgba[((size_t)std::min(2 * y + 1, h - 1) * w + 2 * x) * 4 + c]
                                + rgba[((size_t)std::min(2 * y + 1, h - 1) * w
                                        + std::min(2 * x + 1, w - 1)) * 4 + c];
                    half[((size_t)y * nw + x) * 4 + c] = (uint8_t)((s + 2) / 4);
                }
        rgba.swap(half); w = nw; h = nh;
    }
    out.w = w;
    out.h = h;
    if (want_linear)
    {
        // The sRGB flag on the header decides whether the bytes are encoded.
        // A colour sheet that is NOT flagged is already linear and must not be
        // put through the curve twice.
        float lut[256];
        for (int i = 0; i < 256; i++)
            lut[i] = img.srgb ? srgb_to_linear(i / 255.f) : i / 255.f;
        out.lin.resize((size_t)w * h * 4);
        for (size_t i = 0; i < out.lin.size(); i += 4)
        {
            out.lin[i + 0] = lut[rgba[i + 0]];
            out.lin[i + 1] = lut[rgba[i + 1]];
            out.lin[i + 2] = lut[rgba[i + 2]];
            out.lin[i + 3] = rgba[i + 3] / 255.f;
        }
    }
    else out.raw = std::move(rgba);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------

bool paint_colour_map(const Splat& sp, const SplatChunkDir& dir,
                      const std::function<std::vector<uint8_t>(const std::string&)>& fetch,
                      const float lo[2], const float hi[2], int size,
                      std::vector<uint8_t>& rgb,
                      std::vector<std::string>* failures)
{
    rgb.clear();
    if (size <= 0 || sp.no_colour()) return false;

    const float* rlo = sp.root_min();
    const float* rhi = sp.root_max();
    if (rhi[0] <= rlo[0] || rhi[1] <= rlo[1]) return false;

    std::vector<ColorSlice> slices = sp.color_slices(dir, fetch);
    std::stable_sort(slices.begin(), slices.end(),
                     [](const ColorSlice& a, const ColorSlice& b)
                     { return Splat::depth_of(a.key) < Splat::depth_of(b.key); });

    const int side = sp.tile_side();
    if (side <= 4) return false;
    const int apron = (side - (side - 4)) / 2;   // 132 -> 2
    const int use = side - 2 * apron;

    std::map<std::string, std::vector<uint8_t>> chunk_cache;
    int painted = 0;
    for (const ColorSlice& cs : slices)
    {
        float tlo[2], thi[2];
        Splat::bounds_of(cs.key, rlo, rhi, tlo, thi);
        if (thi[0] <= lo[0] || tlo[0] >= hi[0] || thi[1] <= lo[1] || tlo[1] >= hi[1])
            continue;
        auto ci = chunk_cache.find(cs.chunk);
        if (ci == chunk_cache.end())
            ci = chunk_cache.emplace(cs.chunk, fetch(cs.chunk)).first;
        const std::vector<uint8_t>& raw = ci->second;
        if (cs.offset + cs.bytes > raw.size()) continue;
        std::vector<uint8_t> rgba;
        std::string e;
        if (!bcn_to_rgba8(raw.data() + cs.offset, cs.bytes, side, side,
                          sp.tile_is_bc1() ? 72 : 99, rgba, e))
        {
            if (failures) failures->push_back("colour tile: " + e);
            continue;
        }
        // 128, NOT 0. A texel no tile reaches has no authored aerial colour, and
        // the consumer of this buffer is a Photoshop Overlay: overlay(c, 0.5)
        // == c, so mid grey is the identity and leaves the ground alone, while a
        // zero-initialised black multiplies it to nothing. The map's own
        // authored mean is 127.4 / 127.4 / 127.5 over mp_dumbo, so mid grey is
        // also what the data itself is centred on.
        //
        // This was invisible while the tile discriminator was accepting filler
        // rasters, because those covered the gaps with something. mp_abbasid's
        // colour map reaches about a third of its playable window; with the
        // filler correctly rejected and the rest left black, its bake mean fell
        // from 129.7 / 123.8 / 117.4 to 74.2 / 68.8 / 61.7 and its chroma score
        // went from 2.19 to 3.06.
        if (rgb.empty()) rgb.assign((size_t)size * size * 3, 128);
        painted++;

        // Bilinear, not nearest. A colour tile is 128 usable texels over
        // whatever world rect its quadtree key owns, so on a tight window one
        // tile texel can be twelve metres across - nearest turns the aerial
        // photograph into a chequerboard of flat squares and the bake reads as
        // broken when it is not. The apron is what makes the filter safe at the
        // tile edge: those texels ARE the neighbour's.
        const float tw = thi[0] - tlo[0], th = thi[1] - tlo[1];
        if (tw <= 0.f || th <= 0.f) continue;
        for (int y = 0; y < size; y++)
        {
            const float wz = lo[1] + (hi[1] - lo[1]) * ((float)y + 0.5f) / (float)size;
            if (wz < tlo[1] || wz >= thi[1]) continue;
            const float fy = (wz - tlo[1]) / th * (float)use + (float)apron - 0.5f;
            const int y0 = std::max(0, std::min(side - 1, (int)std::floor(fy)));
            const int y1 = std::max(0, std::min(side - 1, y0 + 1));
            const float ty = fy - std::floor(fy);
            for (int x = 0; x < size; x++)
            {
                const float wx = lo[0] + (hi[0] - lo[0]) * ((float)x + 0.5f) / (float)size;
                if (wx < tlo[0] || wx >= thi[0]) continue;
                const float fx = (wx - tlo[0]) / tw * (float)use + (float)apron - 0.5f;
                const int x0 = std::max(0, std::min(side - 1, (int)std::floor(fx)));
                const int x1 = std::max(0, std::min(side - 1, x0 + 1));
                const float txf = fx - std::floor(fx);
                const uint8_t* a = &rgba[((size_t)y0 * side + x0) * 4];
                const uint8_t* b2 = &rgba[((size_t)y0 * side + x1) * 4];
                const uint8_t* c2 = &rgba[((size_t)y1 * side + x0) * 4];
                const uint8_t* d2 = &rgba[((size_t)y1 * side + x1) * 4];
                uint8_t* d = &rgb[((size_t)y * size + x) * 3];
                for (int k = 0; k < 3; k++)
                    d[k] = (uint8_t)(lerpf(lerpf(a[k], b2[k], txf),
                                           lerpf(c2[k], d2[k], txf), ty) + 0.5f);
            }
        }
    }
    return painted > 0 && !rgb.empty();
}

bool TerrainComposite::bake(Source& src, const std::string& level,
                            const TerrainBakeOpts& opt, TerrainBake& out,
                            std::string& err)
{
    out = TerrainBake();

    // ---- 1. the streaming tree ---------------------------------------------
    std::string lvl = level;
    for (char& c : lvl) c = (char)std::tolower((unsigned char)c);
    std::string tree;
    for (const auto& kv : src.res())
    {
        std::string n = kv.first;
        for (char& c : n) c = (char)std::tolower((unsigned char)c);
        if (n.find("streamingtree") != std::string::npos && n.find(lvl) != std::string::npos)
        { tree = kv.first; break; }
    }
    if (tree.empty()) { err = "no streaming tree for " + level; return false; }
    std::vector<uint8_t> res = src.get_res(tree, err);
    if (res.empty()) { err = "streaming tree: " + err; return false; }

    std::vector<uint8_t> b1;
    if (!Splat::find_block(res, 1, b1, err)) { err = "block 1: " + err; return false; }
    Splat sp;
    if (!sp.parse(b1, err)) { err = "block 1 parse: " + err; return false; }
    SplatChunkDir dir;
    if (!Splat::read_chunk_dir(res, dir, err)) { err = "chunk dir: " + err; return false; }
    if (!sp.detect_layout(dir, err)) { err = "detect_layout: " + err; return false; }
    auto fetch = [&](const std::string& g) { std::string e; return src.get_chunk(g, e); };

    // ---- 2. the window ------------------------------------------------------
    const float* rlo = sp.root_min();
    const float* rhi = sp.root_max();
    float lo[2], hi[2];
    if (opt.rect_size > 0.f)
    {
        lo[0] = opt.rect_min[0]; lo[1] = opt.rect_min[1];
        hi[0] = lo[0] + opt.rect_size; hi[1] = lo[1] + opt.rect_size;
    }
    else {
        // No explicit window means the footprint authored in the mounted
        // game's splat root. The former Portal overlay-box shortcut was an SDK
        // intermediate and can disagree with the actual gameplay volume.
        lo[0] = rlo[0]; lo[1] = rlo[1]; hi[0] = rhi[0]; hi[1] = rhi[1];
    }
    const int size = opt.size > 0 ? opt.size : 1024;
    const int ssize = opt.splat_size > 0 ? opt.splat_size : size;
    out.size = size;
    out.lo[0] = lo[0]; out.lo[1] = lo[1];
    out.hi[0] = hi[0]; out.hi[1] = hi[1];
    out.metres_per_texel = (hi[0] - lo[0]) / (float)size;

    // ---- 3. coverage --------------------------------------------------------
    SplatCoverage cov;
    SplatCompositeOpts sopt;
    sopt.threads = opt.threads;
    sopt.smooth  = true;
    sopt.max_slots = 8;
    if (opt.rect_size > 0.f)
    { sopt.rect_min[0] = lo[0]; sopt.rect_min[1] = lo[1]; sopt.rect_size = opt.rect_size; }
    if (!sp.composite(dir, fetch, ssize, cov, err, sopt))
    { err = "splat composite: " + err; return false; }

    // ---- 4. the palette -----------------------------------------------------
    TerrainLayers tl;
    std::string perr;
    const bool have_palette = tl.load(src, level, perr);
    if (!have_palette) out.failures.push_back("palette: " + perr);
    out.layers_in_palette = (int)tl.layer_count();

    // ---- 5. the base material field (block 7) -------------------------------
    //
    // MaterialTree::rasterize has no window, so the whole footprint is
    // rasterised once and sampled by world position. That costs nothing worth
    // saving: the field is 46 data nodes on Aftermath and is flat over most of
    // a window anyway.
    MaterialRaster mr;
    bool have_base = false;
    {
        std::vector<uint8_t> b7;
        std::string e7;
        if (Splat::find_block(res, 7, b7, e7))
        {
            MaterialTree mt;
            if (mt.parse(b7, e7))
            {
                const int bsz = std::min(2048, std::max(512, ssize));
                if (mt.rasterize(bsz, [&](float cx, float cz, float w)
                                 { return sp.base_list_at(cx, cz, w); },
                                 sp.full_list(), have_palette ? tl.linked_list()
                                                              : std::vector<int>(),
                                 sp.global_base_list(), mr, e7))
                    have_base = true;
                else out.failures.push_back("block 7 rasterize: " + e7);
            }
            else out.failures.push_back("block 7 parse: " + e7);
        }
    }

    // ---- 6. which layers can reach this window ------------------------------
    std::set<int> present;
    for (int l = 0; l < 256; l++) if (cov.layer_texels[l]) present.insert(l);
    if (have_base)
        for (uint8_t l : mr.layer) if (l != 255) present.insert((int)l);

    std::map<int, int> painted, basecnt;
    sp.layer_usage(painted, basecnt);

    const std::map<std::string, std::string>& pidx = src.partition_index();
    const int tex_dim = opt.texture_dim > 0 ? opt.texture_dim : 512;

    // ---- 6a. the statically bound half of the palette ------------------------
    //
    // A layer whose ShaderLayerInfos row carries no bindless colour is not
    // shader-computed: its sheets are bound directly by the compute permutation
    // and sampled by register. terrainstatic.h resolves that table and joins it
    // to layers; the join's accuracy, and why it is switchable, are documented
    // there and on TerrainBakeOpts::static_fallback.
    //
    // The STATIC-LAYER LIST is built over the whole palette, not over the layers
    // this window happens to reach, because the join is ORDINAL: dropping a
    // layer that is off-window would slide every later layer onto its
    // neighbour's sheet.
    TerrainStaticTable stat;
    bool have_static = false;
    if (opt.static_fallback && have_palette)
    {
        std::string serr;
        if (stat.load(src, level, serr))
            have_static = stat.join_exact();
        else out.failures.push_back("static texture table: " + serr);
    }

    std::map<int, LayerRT> rt;
    for (int li : present)
    {
        TerrainBakeLayer rep;
        rep.layer = li;
        rep.painted = painted.count(li) != 0;
        rep.base    = basecnt.count(li) != 0;
        rep.texels  = cov.layer_texels[li];

        if (!have_palette || li >= (int)tl.layer_count())
        {
            rep.failure = "layer index outside the palette";
            out.layers.push_back(rep);
            continue;
        }
        const TerrainLayer& L = tl.layers()[(size_t)li];
        const TerrainLayerMaterial& M = L.material;
        rep.metres_per_repeat = M.metres_per_repeat(opt.default_metres_per_repeat);
        rep.tiling_authored = M.uv_tiling_set;
        // Route one: the bindless colour the layer-graph depot bound. Route two:
        // the compositor's own static table. The constants (tiling, height
        // blend, overlay, tint) come from the depot record either way - only the
        // SHEETS move.
        std::string base_guid = M.base_color();
        std::string nrmh_guid = M.normal_height();
        // The mounted evaluator bytecode is the join. A descriptor of -1 is a
        // real answer meaning the case binds no colour there; it must not pick
        // up a neighbouring sheet.
        if (base_guid.empty() && opt.static_fallback)
        {
            int tcv = -1, tnh = -1, tthird = -1;
            if (have_static && stat.layer_descriptors(li, tcv, tnh, tthird))
            {
                auto by_descriptor = [&stat](int d) -> const TerrainStaticTexture*
                {
                    if (d < 0) return nullptr;
                    for (const TerrainStaticGroup& g : stat.groups())
                        for (const TerrainStaticTexture& tx : g.tex)
                            if ((int)tx.descriptor == d) return &tx;
                    return nullptr;
                };
                if (const TerrainStaticTexture* t = by_descriptor(tcv))
                    base_guid = t->file_guid;
                if (nrmh_guid.empty())
                    if (const TerrainStaticTexture* t = by_descriptor(tnh))
                        nrmh_guid = t->file_guid;
                // The table covers this layer. Whatever it says stands, an
                // empty answer included: falling through to the walk here is
                // what put a road sheet on a modifier.
                rep.has_sheet = !base_guid.empty();
                if (!rep.has_sheet)
                {
                    rep.failure = "the evaluator bytecode binds no base colour "
                                  "for this layer (modifier or untextured)";
                    out.layers.push_back(rep);
                    continue;
                }
            }
        }
        rep.has_sheet = !base_guid.empty();
        if (!rep.has_sheet)
        {
            // Neither route reaches this layer. Skipped, never faked.
            rep.failure = "no base colour on either route "
                          "(layer-graph depot empty, no static group assigned)";
            out.layers.push_back(rep);
            continue;
        }
        out.layers_with_sheet++;

        // MIP SELECTION, which is not a nicety at these scales. A 2048 px sheet
        // over a 1.25 m repeat is 1,638 texture pixels per metre; a 0.5 m/texel
        // raster asks for 2. Sampling the top mip at that ratio is 800x
        // undersampling and comes out as coloured static, which is exactly the
        // "noise rather than ground" failure this bake is judged on. The game
        // uses SampleGrad and gets the right level for free; a software bake
        // has to pick one. Rounded UP to the next power of two, so the choice
        // errs toward detail rather than blur, and floored at 4 so a sheet
        // never collapses to a single texel.
        int want = (int)std::ceil(rep.metres_per_repeat / out.metres_per_texel);
        int cap = 4;
        while (cap < want && cap < tex_dim) cap *= 2;
        cap = std::min(cap, tex_dim);

        LayerRT r;
        std::string why;
        int dxgi = 0;
        if (!load_sheet(src, pidx, base_guid, cap, true, r.base, dxgi, why))
        {
            rep.failure = why;
            out.failures.push_back("L" + std::to_string(li) + " base colour: " + why);
            out.layers.push_back(rep);
            continue;
        }
        rep.decoded = true;
        rep.width = r.base.w; rep.height = r.base.h; rep.dxgi = dxgi;
        auto it = pidx.find(base_guid);
        if (it != pidx.end()) rep.asset = strip_ebx(it->second);

        int ndxgi = 0;
        std::string nwhy;
        if (!load_sheet(src, pidx, nrmh_guid, cap, false, r.nrmh, ndxgi, nwhy))
            r.nrmh = Sheet();     // height falls back to neutral 0.5

        r.mpr = rep.metres_per_repeat;
        const float a = M.uv_rotation_deg * 0.01745329f;
        r.rot_cos = std::cos(a); r.rot_sin = std::sin(a);
        r.off[0] = M.uv_offset[0]; r.off[1] = M.uv_offset[1];
        r.scale[0] = M.coord_scale_set ? M.coord_scale[0] : 1.f;
        r.scale[1] = M.coord_scale_set ? M.coord_scale[1] : 1.f;
        if (r.scale[0] == 0.f) r.scale[0] = 1.f;
        if (r.scale[1] == 0.f) r.scale[1] = 1.f;
        r.displace     = M.displace_range;
        r.base_height  = M.base_height;
        r.ramp_exp     = M.mask_ramp_exp_set && M.mask_ramp_exp > 0.f ? M.mask_ramp_exp : 1.f;
        r.height_blend = M.height_blend;
        // AN UNAUTHORED OVERLAY STRENGTH IS FULL, NOT ZERO.
        //
        // Between a quarter and a half of layers omit 0xE68B2B10, and on
        // dumbo, battery, plaza, abbasid and capstone NONE of the residual
        // base layers carries it. Defaulting those to zero switches the
        // colour map off exactly where it does the most work: the neutral
        // t_ter_defaulttexture_cv cover layers are a flat 206/206/206 and
        // take ALL their hue from this path, so with the map off they stay
        // grey. Measured, the wrong default put one colour on 92.5% of dumbo
        // against 1.1%, and 100% of plaza against 17%.
        r.overlay      = M.overlay_strength_set ? M.overlay_strength : 1.f;
        r.tint[0] = M.tint[0]; r.tint[1] = M.tint[1]; r.tint[2] = M.tint[2];
        r.usable = true;
        rt[li] = std::move(r);
        out.layers_decoded++;
        out.layers.push_back(rep);
    }
    out.layers_present = (int)present.size();

    out.splat_evictions = cov.slot_evictions;
    out.splat_evicted_mask = cov.size > 0
        ? cov.evicted_weight / ((double)cov.size * (double)cov.size) : 0.0;

    // ---- 7. the colour map ---------------------------------------------------
    //
    // The block-1 colour raster is an aerial photograph in 132^2 BC7 tiles with
    // a 2-texel apron, one per quadtree key. Painted coarse-first into a
    // window-sized buffer so a finer tile overwrites a coarser one, exactly as
    // the weight pages composite.
    std::vector<uint8_t> cmap;    // size*size*3, sRGB bytes as they lie
    if (opt.colour_map)
    {
        paint_colour_map(sp, dir, fetch, lo, hi, size, cmap, &out.failures);
        out.colour_map_used = !cmap.empty();
    }

    // ---- 8. evaluate ---------------------------------------------------------
    out.albedo.assign((size_t)size * size * 4, 0);
    if (opt.want_normal) out.normal.assign((size_t)size * size * 4, 0);

    const float sx_step = (hi[0] - lo[0]) / (float)size;
    const float sz_step = (hi[1] - lo[1]) / (float)size;
    const int bsz = have_base ? mr.size : 0;

    struct Band {
        double sum[3] = {0, 0, 0};
        uint64_t untouched = 0;
        // See TerrainBake's mix block: how many layers the evaluator drew here
        // and how much of the result the winner actually owns.
        uint64_t hist[9] = {};
        double mixdom = 0, maskdom = 0, part = 0;
        uint64_t mixn = 0;
    };
    int nthreads = opt.threads > 0 ? opt.threads
                                   : (int)std::max(1u, std::thread::hardware_concurrency());
    nthreads = std::max(1, std::min(nthreads, size));
    std::vector<Band> bands((size_t)nthreads);

    auto run = [&](int band)
    {
        const int y0 = (int)((int64_t)size * band / nthreads);
        const int y1 = (int)((int64_t)size * (band + 1) / nthreads);
        Band& acc = bands[(size_t)band];
        int order[16];
        float drawn_cov[8];       // the coverage each drawn layer resolved to
        float drawn_mask[8];      // and the raw mask it came from

        for (int y = y0; y < y1; y++)
        {
            const float wz = lo[1] + ((float)y + 0.5f) * sz_step;
            for (int x = 0; x < size; x++)
            {
                const float wx = lo[0] + ((float)x + 0.5f) * sx_step;

                // The stack: the block-7 base material at full coverage, then
                // the splat's top four in ASCENDING layer order, which is the
                // order the kernel's work list is walked in.
                int n = 0;
                float mask[16];
                int base_layer = -1;
                if (have_base)
                {
                    const int bx = std::min(bsz - 1, std::max(0,
                        (int)((wx - rlo[0]) / (rhi[0] - rlo[0]) * (float)bsz)));
                    const int by = std::min(bsz - 1, std::max(0,
                        (int)((wz - rlo[1]) / (rhi[1] - rlo[1]) * (float)bsz)));
                    const uint8_t bl = mr.layer[(size_t)by * bsz + bx];
                    if (bl != 255) base_layer = (int)bl;
                }
                if (base_layer >= 0) { order[n] = base_layer; mask[n] = 1.f; n++; }

                const int sxi = std::min(cov.size - 1,
                    (int)((wx - cov.lo[0]) / (cov.hi[0] - cov.lo[0]) * (float)cov.size));
                const int szi = std::min(cov.size - 1,
                    (int)((wz - cov.lo[1]) / (cov.hi[1] - cov.lo[1]) * (float)cov.size));
                if (sxi >= 0 && szi >= 0)
                {
                    const size_t o = ((size_t)szi * cov.size + sxi) * (size_t)cov.slots;
                    int tmp[16]; float tw[16]; int tn = 0;
                    for (int s = 0; s < cov.slots; s++)
                    {
                        if (cov.w[o + s] == 0) break;
                        tmp[tn] = cov.idx[o + s];
                        tw[tn]  = cov.w[o + s] / 255.f;
                        tn++;
                    }
                    // ascending by layer index
                    for (int i = 1; i < tn; i++)
                        for (int j = i; j > 0 && tmp[j] < tmp[j - 1]; j--)
                        { std::swap(tmp[j], tmp[j - 1]); std::swap(tw[j], tw[j - 1]); }
                    for (int i = 0; i < tn && n < 16; i++)
                    { order[n] = tmp[i]; mask[n] = tw[i]; n++; }
                }

                // §6 of the findings: the accumulator initial values.
                float col[3] = {opt.fallback[0], opt.fallback[1], opt.fallback[2]};
                float alpha = 1.f;
                float nx = 0.f, ny = 0.f;
                float accH = 0.f, accLoMax = 0.f, accHiMax = 0.f;
                bool touched = false;
                int drawn = 0;

                for (int k = 0; k < n; k++)
                {
                    auto it = rt.find(order[k]);
                    if (it == rt.end()) continue;         // no sheet: skipped
                    const LayerRT& L = it->second;
                    const float m = saturatef(mask[k]);

                    // UV: world metres over the layer's own repeat length, its
                    // coordinate scale, its offset, then its rotation. THIS is
                    // the thing that makes ground read as ground.
                    const float px = (wx / L.mpr) * L.scale[0] + L.off[0];
                    const float py = (wz / L.mpr) * L.scale[1] + L.off[1];

                    float bc[4] = {0, 0, 0, 1}, nh[4] = {0.5f, 0.5f, 0.5f, 0.5f};
                    if (opt.stochastic)
                    {
                        Tap t[4]; int tn2 = 0;
                        stochastic_taps(px, py, t, tn2);
                        float wsum = 0.f;
                        float sb[4] = {0, 0, 0, 0}, sn[4] = {0, 0, 0, 0};
                        for (int i = 0; i < tn2; i++)
                        {
                            const float u = t[i].u * L.rot_cos - t[i].v * L.rot_sin;
                            const float v = t[i].u * L.rot_sin + t[i].v * L.rot_cos;
                            float a4[4];
                            sample_lin(L.base, u, v, a4);
                            for (int c = 0; c < 4; c++) sb[c] += a4[c] * t[i].w;
                            if (L.nrmh.ok())
                            {
                                sample_raw(L.nrmh, u, v, a4);
                                for (int c = 0; c < 4; c++) sn[c] += a4[c] * t[i].w;
                            }
                            wsum += t[i].w;
                        }
                        if (wsum <= 1e-6f) wsum = 1.f;
                        for (int c = 0; c < 4; c++) bc[c] = sb[c] / wsum;
                        if (L.nrmh.ok()) for (int c = 0; c < 4; c++) nh[c] = sn[c] / wsum;
                    }
                    else
                    {
                        const float u = px * L.rot_cos - py * L.rot_sin;
                        const float v = px * L.rot_sin + py * L.rot_cos;
                        sample_lin(L.base, u, v, bc);
                        if (L.nrmh.ok()) sample_raw(L.nrmh, u, v, nh);
                    }

                    // §5.2 the layer's own height
                    const float ht = saturatef(nh[2]);
                    const float h_layer = L.base_height + (ht - 0.5f) * L.displace;
                    const float h_max = L.base_height + 0.5f * L.displace;
                    const float h_min = L.base_height - 0.5f * L.displace;

                    // §5.3 coverage
                    const float w_hi = std::pow(saturatef((m - 0.9f) / -0.85f), L.ramp_exp);
                    const float w_lo = std::pow(saturatef((m - 0.05f) / 0.95f), L.ramp_exp);
                    const float hiRef = h_layer + (accHiMax - h_max) * w_hi;
                    const float loRef = accH + (h_min - accLoMax) * w_lo;
                    const float raw = m + (hiRef - loRef) * L.height_blend;
                    float c;
                    if (m >= 1.f) c = 1.f;
                    else if (m <= 0.f) c = 0.f;
                    else c = saturatef(raw);
                    if (c <= 0.f) continue;

                    // §5.5 the colour map Overlay, then §5.4 the lerp
                    float lc[3] = {bc[0] * L.tint[0], bc[1] * L.tint[1], bc[2] * L.tint[2]};
                    if (L.overlay > 0.f && !cmap.empty())
                    {
                        const uint8_t* o8 = &cmap[((size_t)y * size + x) * 3];
                        for (int ci = 0; ci < 3; ci++)
                        {
                            // NOT srgb_to_linear. The colour map ships as
                            // BC1_UNORM / BC7_UNORM, not the _SRGB variants,
                            // so the sampler hands the shader the raw byte and
                            // the Overlay blend is authored against that. De-
                            // gamma'ing it turns Overlay's 0.500 identity into
                            // 0.216, which darkens and over-saturates every
                            // level that has a colour map.
                            const float ov = o8[ci] / 255.f;
                            const float cc = lc[ci];
                            const float bl = cc < 0.5f ? 2.f * cc * ov
                                                       : 1.f - 2.f * (1.f - cc) * (1.f - ov);
                            lc[ci] = cc + L.overlay * (bl - cc);
                        }
                    }

                    // See TerrainBakeOpts::prime_first_layer. The colour lerp
                    // alone is promoted; the height chain below keeps `raw`.
                    const float cc2 = (opt.prime_first_layer && !touched) ? 1.f : c;
                    if (drawn < 8)
                    { drawn_cov[drawn] = cc2; drawn_mask[drawn] = m; drawn++; }
                    for (int ci = 0; ci < 3; ci++)
                    {
                        col[ci] = lerpf(col[ci], lc[ci], cc2);
                        if (opt.quantise_f16) col[ci] = quant_half(col[ci]);
                    }
                    alpha = lerpf(alpha, bc[3], cc2);
                    nx = lerpf(nx, nh[0] * 2.f - 1.f, cc2);
                    ny = lerpf(ny, nh[1] * 2.f - 1.f, cc2);
                    if (opt.quantise_f16)
                    { alpha = quant_half(alpha); nx = quant_half(nx); ny = quant_half(ny); }

                    // The height accumulator uses the UNSATURATED coverage.
                    accH = std::min(10.f, std::max(-1.f, lerpf(loRef, hiRef, raw)));
                    accLoMax = lerpf(accLoMax, h_min, raw);
                    accHiMax = lerpf(accHiMax, h_max, raw);
                    touched = true;
                }

                // ---- the mix statistics, see TerrainBake's mix block --------
                acc.hist[drawn > 8 ? 8 : drawn]++;
                if (drawn > 0)
                {
                    float eff[8];
                    float tail = 1.f;
                    for (int k = drawn - 1; k >= 0; k--)
                    { eff[k] = drawn_cov[k] * tail; tail *= 1.f - drawn_cov[k]; }
                    float s = 0.f, mx = 0.f, sq = 0.f;
                    for (int k = 0; k < drawn; k++) s += eff[k];
                    if (s > 1e-6f)
                        for (int k = 0; k < drawn; k++)
                        {
                            const float e = eff[k] / s;
                            if (e > mx) mx = e;
                            sq += e * e;
                        }
                    float ms = 0.f, mmx = 0.f;
                    for (int k = 0; k < drawn; k++) ms += drawn_mask[k];
                    if (ms > 1e-6f)
                        for (int k = 0; k < drawn; k++)
                        { const float e = drawn_mask[k] / ms; if (e > mmx) mmx = e; }
                    acc.mixdom  += mx;
                    acc.maskdom += mmx;
                    acc.part    += sq > 1e-6f ? 1.0 / sq : 1.0;
                    acc.mixn++;
                }

                // A texel no textured layer reached. See
                // TerrainBakeOpts::fallback_colour_map.
                if (!touched && opt.fallback_colour_map && !cmap.empty())
                {
                    const uint8_t* o8 = &cmap[((size_t)y * size + x) * 3];
                    for (int ci = 0; ci < 3; ci++)
                        col[ci] = o8[ci] / 255.f;   // linear, see the Overlay above
                }

                uint8_t* d = &out.albedo[((size_t)y * size + x) * 4];
                for (int ci = 0; ci < 3; ci++)
                {
                    const int v = (int)(linear_to_srgb(col[ci]) * 255.f + 0.5f);
                    d[ci] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                    acc.sum[ci] += d[ci];
                }
                d[3] = (uint8_t)(saturatef(alpha) * 255.f + 0.5f);
                if (!touched) acc.untouched++;

                if (opt.want_normal)
                {
                    const float nz = std::sqrt(std::max(0.f, 1.f - nx * nx - ny * ny));
                    uint8_t* q = &out.normal[((size_t)y * size + x) * 4];
                    q[0] = (uint8_t)(saturatef(nx * 0.5f + 0.5f) * 255.f + 0.5f);
                    q[1] = (uint8_t)(saturatef(ny * 0.5f + 0.5f) * 255.f + 0.5f);
                    q[2] = (uint8_t)(saturatef(nz * 0.5f + 0.5f) * 255.f + 0.5f);
                    q[3] = (uint8_t)(saturatef(accH * 0.5f + 0.5f) * 255.f + 0.5f);
                }
            }
        }
    };

    if (nthreads == 1) run(0);
    else
    {
        std::vector<std::thread> th;
        for (int i = 0; i < nthreads; i++) th.emplace_back(run, i);
        for (std::thread& t : th) t.join();
    }

    double s[3] = {0, 0, 0};
    double mixd = 0, maskd = 0, part = 0;
    uint64_t mixn = 0;
    for (const Band& b : bands)
    {
        for (int i = 0; i < 3; i++) s[i] += b.sum[i];
        out.texels_untouched += b.untouched;
        for (int i = 0; i < 9; i++) out.stack_hist[i] += b.hist[i];
        mixd += b.mixdom; maskd += b.maskdom; part += b.part; mixn += b.mixn;
    }
    if (mixn)
    {
        out.mix_dominant      = mixd / (double)mixn;
        out.mask_dominant     = maskd / (double)mixn;
        out.mix_participation = part / (double)mixn;
    }
    const double n = (double)size * (double)size;
    for (int i = 0; i < 3; i++) out.mean_rgb[i] = s[i] / n;
    return true;
}

}  // namespace bf6
