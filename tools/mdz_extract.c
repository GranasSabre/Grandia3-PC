// mdz_extract: unpack Grandia III DATA/*.MDZ archives.
// An MDZ is a level-2 LHA archive whose method id "-lh6-" is renamed to "MDZ\0\0"
// (static Huffman LZ, 32 KB dictionary; the game's decoder is field_00243CB0).
// usage: mdz_extract <in.MDZ> <out.MDT>     or     mdz_extract -d <indir> <outdir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#endif

enum { NC = 510, NT = 19, TBIT = 5, CBIT = 9, NPT = 0x80, DICBIT = 15, NP = DICBIT + 1, PBIT = 5 };

static const uint8_t *src; static size_t srclen, srcpos;
static uint16_t bitbuf; static uint8_t subbitbuf; static int bitcount;
static uint16_t left_[2 * NC - 1], right_[2 * NC - 1];
static uint8_t c_len[NC], pt_len[NPT];
static uint16_t c_table[4096], pt_table[256];
static int blocksize, bad;

static void fillbuf(int n) {
    while (n > bitcount) {
        n -= bitcount;
        bitbuf = (uint16_t)((bitbuf << bitcount) + (subbitbuf >> (8 - bitcount)));
        subbitbuf = srcpos < srclen ? src[srcpos++] : 0;
        bitcount = 8;
    }
    bitcount -= n;
    bitbuf = (uint16_t)((bitbuf << n) + (subbitbuf >> (8 - n)));
    subbitbuf = (uint8_t)(subbitbuf << n);
}
static int getbits(int n) { int x = bitbuf >> (16 - n); fillbuf(n); return x; }

static void make_table(int nchar, const uint8_t *bitlen, int tablebits, uint16_t *table) {
    unsigned count[17] = {0}, weight[17], start[18];
    for (int i = 0; i < nchar; i++) if (bitlen[i] <= 16) count[bitlen[i]]++; else bad = 1;
    start[1] = 0;
    for (int i = 1; i <= 16; i++) start[i + 1] = start[i] + (count[i] << (16 - i));
    if ((start[17] & 0xffff) != 0) bad = 1;
    int jutbits = 16 - tablebits;
    for (int i = 1; i <= tablebits; i++) { start[i] >>= jutbits; weight[i] = 1u << (tablebits - i); }
    for (int i = tablebits + 1; i <= 16; i++) weight[i] = 1u << (16 - i);
    unsigned i = start[tablebits + 1] >> jutbits;
    if (i != (1u << 16)) { unsigned k = 1u << tablebits; while (i < k) table[i++] = 0; }
    unsigned avail = nchar, mask = 1u << (15 - tablebits);
    for (int ch = 0; ch < nchar; ch++) {
        int len = bitlen[ch];
        if (len == 0) continue;
        unsigned k = start[len], nextcode = k + weight[len];
        if (len <= tablebits) {
            if (nextcode > (1u << tablebits)) { bad = 1; return; }
            for (unsigned j = k; j < nextcode; j++) table[j] = (uint16_t)ch;
        } else {
            uint16_t *p = &table[k >> jutbits];
            for (int n = len - tablebits; n; n--) {
                if (*p == 0) {
                    if (avail >= 2 * NC - 1) { bad = 1; return; }
                    right_[avail] = left_[avail] = 0; *p = (uint16_t)avail++;
                }
                p = (k & mask) ? &right_[*p] : &left_[*p];
                k <<= 1;
            }
            *p = (uint16_t)ch;
        }
        start[len] = nextcode;
    }
}

static void read_pt_len(int nn, int nbit, int special) {
    int n = getbits(nbit);
    if (n == 0) {
        int c = getbits(nbit);
        memset(pt_len, 0, nn);
        for (int i = 0; i < 256; i++) pt_table[i] = (uint16_t)c;
        return;
    }
    int i = 0;
    while (i < n && i < NPT) {
        int c = bitbuf >> 13;
        if (c == 7) { unsigned mask = 1u << 12; while (mask & bitbuf) { mask >>= 1; c++; } }
        fillbuf(c < 7 ? 3 : c - 3);
        pt_len[i++] = (uint8_t)c;
        if (i == special) { c = getbits(2); while (--c >= 0 && i < NPT) pt_len[i++] = 0; }
    }
    while (i < nn) pt_len[i++] = 0;
    make_table(nn, pt_len, 8, pt_table);
}

static void read_c_len(void) {
    int n = getbits(CBIT);
    if (n == 0) {
        int c = getbits(CBIT);
        memset(c_len, 0, NC);
        for (int i = 0; i < 4096; i++) c_table[i] = (uint16_t)c;
        return;
    }
    int i = 0;
    while (i < n && i < NC) {
        int c = pt_table[bitbuf >> 8];
        if (c >= NT) {
            unsigned mask = 1u << 7;
            do { c = (bitbuf & mask) ? right_[c] : left_[c]; mask >>= 1; } while (c >= NT && mask);
        }
        fillbuf(pt_len[c]);
        if (c <= 2) {
            if (c == 0) c = 1;
            else if (c == 1) c = getbits(4) + 3;
            else c = getbits(CBIT) + 20;
            while (--c >= 0 && i < NC) c_len[i++] = 0;
        } else c_len[i++] = (uint8_t)(c - 2);
    }
    while (i < NC) c_len[i++] = 0;
    make_table(NC, c_len, 12, c_table);
}

static int decode_c(void) {
    if (blocksize == 0) {
        blocksize = getbits(16);
        read_pt_len(NT, TBIT, 3);
        read_c_len();
        read_pt_len(NP, PBIT, -1);
    }
    blocksize--;
    int j = c_table[bitbuf >> 4];
    if (j < NC) fillbuf(c_len[j]);
    else {
        fillbuf(12);
        unsigned mask = 1u << 15;
        do { j = (bitbuf & mask) ? right_[j] : left_[j]; mask >>= 1; } while (j >= NC && mask);
        fillbuf(c_len[j] - 12);
    }
    return j;
}

static int decode_p(void) {
    int j = pt_table[bitbuf >> 8];
    if (j < NP) fillbuf(pt_len[j]);
    else {
        fillbuf(8);
        unsigned mask = 1u << 15;
        do { j = (bitbuf & mask) ? right_[j] : left_[j]; mask >>= 1; } while (j >= NP && mask);
        fillbuf(pt_len[j] - 8);
    }
    if (j != 0) j = (1 << (j - 1)) + getbits(j - 1);
    return j;
}

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1; }
    return crc;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// returns malloc'd output, sets *outlen and name (embedded file name); NULL on error
static uint8_t *unpack(const uint8_t *in, size_t inlen, size_t *outlen, char *name, size_t namecap) {
    if (inlen < 24 || memcmp(in + 2, "MDZ\0\0", 5) != 0 || in[20] != 2) return NULL;
    size_t hdr = in[0] | in[1] << 8;
    uint32_t csize = rd32(in + 7), osize = rd32(in + 11);
    uint16_t want_crc = (uint16_t)(in[21] | in[22] << 8);
    name[0] = 0;
    // level-2 extended headers start at 24: u16 size, u8 type, data
    for (size_t p = 24; p + 3 <= hdr;) {
        size_t sz = in[p] | in[p + 1] << 8;
        if (sz == 0) break;
        if (in[p + 2] == 1 && sz - 3 < namecap) { memcpy(name, in + p + 3, sz - 3); name[sz - 3] = 0; }
        p += sz;
    }
    if (hdr + csize > inlen) return NULL;
    uint8_t *out = malloc(osize ? osize : 1);
    src = in + hdr; srclen = csize; srcpos = 0;
    bitbuf = 0; subbitbuf = 0; bitcount = 0; blocksize = 0; bad = 0;
    fillbuf(16);
    size_t pos = 0;
    while (pos < osize && !bad) {
        int c = decode_c();
        if (c <= 255) out[pos++] = (uint8_t)c;
        else {
            int len = c - 253;
            size_t from = pos - decode_p() - 1;
            if (from > pos) { bad = 1; break; } // reference before start
            while (len-- && pos < osize) { out[pos] = out[from++]; pos++; }
        }
    }
    if (bad || crc16(out, osize) != want_crc) { free(out); return NULL; }
    *outlen = osize;
    return out;
}

static uint8_t *load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = n; return b;
}

static int one(const char *in, const char *out) {
    size_t n, on; char name[256];
    uint8_t *b = load(in, &n);
    if (!b) { fprintf(stderr, "%s: cannot read\n", in); return 1; }
    uint8_t *o = unpack(b, n, &on, name, sizeof name);
    free(b);
    if (!o) { fprintf(stderr, "%s: decode/CRC error\n", in); return 1; }
    FILE *f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "%s: cannot write\n", out); free(o); return 1; }
    fwrite(o, 1, on, f); fclose(f); free(o);
    printf("%s -> %s (%s, %zu bytes)\n", in, out, name, on);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "-d")) {
        int fails = 0, count = 0;
#ifdef _WIN32
        _mkdir(argv[3]);
        char pat[1024]; snprintf(pat, sizeof pat, "%s\\*.MDZ", argv[2]);
        struct _finddata_t fd; intptr_t h = _findfirst(pat, &fd);
        if (h == -1) { fprintf(stderr, "no MDZ files in %s\n", argv[2]); return 1; }
        do {
            char in[1024], out[1024];
            snprintf(in, sizeof in, "%s\\%s", argv[2], fd.name);
            snprintf(out, sizeof out, "%s\\%.*s.MDT", argv[3], (int)(strlen(fd.name) - 4), fd.name);
            fails += one(in, out); count++;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
#endif
        printf("%d archives, %d failed\n", count, fails);
        return fails != 0;
    }
    if (argc == 3) return one(argv[1], argv[2]);
    fprintf(stderr, "usage: mdz_extract <in.MDZ> <out.MDT> | -d <indir> <outdir>\n");
    return 2;
}
