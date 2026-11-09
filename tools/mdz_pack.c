// mdz_pack: pack an MDT into a Grandia III MDZ (the counterpart of mdz_extract).
// An MDZ is a level-2 LHA archive with method "MDZ\0\0" = lh6: LZ77 with a 32 KB window (matches of
// 3..256 bytes) and static Huffman blocks. The header (file name, directory) is taken from the
// original MDZ; sizes and the data CRC16 are updated.
// usage: mdz_pack <original.MDZ> <in.MDT> <out.MDZ>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

enum { NC = 510, NT = 19, TBIT = 5, CBIT = 9, NP = 16, PBIT = 5, DICSIZ = 1 << 15, MAXMATCH = 256, THRESHOLD = 3 };
enum { HASHBITS = 15, HASHSIZE = 1 << HASHBITS, CHAIN = 128, BLOCKSYM = 16384 };

// ---- bit output ----
static uint8_t *out; static size_t outlen, outcap;
static unsigned bitbuf, bitcnt;
static void putbyte(uint8_t b) {
    if (outlen == outcap) { outcap = outcap ? outcap * 2 : 1 << 20; out = realloc(out, outcap); }
    out[outlen++] = b;
}
static void putbits(int n, unsigned x) {   // n <= 16
    for (int i = n - 1; i >= 0; i--) {
        bitbuf = (bitbuf << 1) | ((x >> i) & 1);
        if (++bitcnt == 8) { putbyte((uint8_t)bitbuf); bitbuf = 0; bitcnt = 0; }
    }
}
static void flushbits(void) { while (bitcnt) putbits(1, 0); }

// ---- Huffman code lengths (max 16) and canonical codes as the decoder's make_table builds them ----
static void make_lengths(int n, const unsigned *freq, uint8_t *len) {
    // plain Huffman via repeated minimum search (n <= 510), then limit to 16 bits
    int parent[2 * NC], used = 0, node = n;
    unsigned w[2 * NC];
    int alive[2 * NC], na = 0;
    memset(len, 0, n);
    for (int i = 0; i < n; i++) { w[i] = freq[i]; parent[i] = -1; if (freq[i]) alive[na++] = i, used++; }
    if (used < 2) return;
    while (na > 1) {
        int a = -1, b = -1;
        for (int k = 0; k < na; k++) {
            int x = alive[k];
            if (a < 0 || w[x] < w[alive[a]]) { b = a; a = k; }
            else if (b < 0 || w[x] < w[alive[b]]) b = k;
        }
        int x = alive[a], y = alive[b];
        w[node] = w[x] + w[y]; parent[node] = -1; parent[x] = parent[y] = node;
        if (a < b) { alive[b] = alive[--na]; alive[a] = node; }
        else { alive[a] = alive[--na]; alive[b] = node; }
        node++;
    }
    unsigned count[64] = {0};
    for (int i = 0; i < n; i++) if (freq[i]) {
        int d = 0;
        for (int p = i; parent[p] >= 0; p = parent[p]) d++;
        len[i] = (uint8_t)(d > 63 ? 63 : d);
        count[len[i]]++;
    }
    // limit to 16 bits (Kraft sum kept at exactly 1)
    for (int i = 17; i < 64; i++) { count[16] += count[i]; count[i] = 0; }
    unsigned long cum = 0;
    for (int i = 16; i > 0; i--) cum += (unsigned long)count[i] << (16 - i);
    while (cum != (1UL << 16)) {
        count[16]--;
        for (int i = 15; i > 0; i--) if (count[i]) { count[i]--; count[i + 1] += 2; break; }
        cum--;
    }
    // give the shortest codes to the most frequent symbols
    int order[NC], m = 0;
    for (int i = 0; i < n; i++) if (freq[i]) order[m++] = i;
    for (int i = 1; i < m; i++) for (int j = i; j > 0 && freq[order[j]] > freq[order[j - 1]]; j--) {
        int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
    }
    int k = 0;
    for (int l = 1; l <= 16; l++) for (unsigned c = 0; c < count[l]; c++) len[order[k++]] = (uint8_t)l;
}
static void make_codes(int n, const uint8_t *len, uint16_t *code) {
    unsigned count[17] = {0}, start[18];
    for (int i = 0; i < n; i++) count[len[i]]++;
    start[1] = 0;
    for (int i = 1; i <= 16; i++) start[i + 1] = (start[i] + count[i]) << 1;
    for (int i = 0; i < n; i++) code[i] = (uint16_t)(len[i] ? start[len[i]]++ : 0);
}

// ---- one block ----
static uint16_t sym_c[BLOCKSYM], sym_p[BLOCKSYM];
static int nsym;

static void write_pt_len(int n, int nbit, int special, const uint8_t *len) {
    while (n > 0 && len[n - 1] == 0) n--;
    putbits(nbit, n);
    for (int i = 0; i < n;) {
        int k = len[i++];
        if (k <= 6) putbits(3, k); else putbits(k - 3, (1u << (k - 3)) - 2);
        if (i == special) {
            while (i < 6 && len[i] == 0) i++;
            putbits(2, (i - 3) & 3);
        }
    }
}

static void flush_block(void) {
    if (!nsym) return;
    unsigned cfreq[NC] = {0}, pfreq[NP] = {0}, tfreq[NT] = {0};
    uint8_t clen[NC], plen[NP], tlen[NT];
    uint16_t ccode[NC], pcode[NP], tcode[NT];
    for (int i = 0; i < nsym; i++) {
        cfreq[sym_c[i]]++;
        if (sym_c[i] >= 256) { unsigned p = sym_p[i], b = 0; while (p) { b++; p >>= 1; } pfreq[b]++; }
    }
    int cused = 0, cone = 0;
    for (int i = 0; i < NC; i++) if (cfreq[i]) { cused++; cone = i; }
    make_lengths(NC, cfreq, clen);
    // run-length coding of the c lengths through the t table
    int n = NC;
    while (n > 0 && clen[n - 1] == 0) n--;
    if (cused >= 2) {
        for (int i = 0; i < n;) {
            int k = clen[i++];
            if (k == 0) {
                int cnt = 1;
                while (i < n && clen[i] == 0) { i++; cnt++; }
                if (cnt <= 2) tfreq[0] += cnt;
                else if (cnt <= 18) tfreq[1]++;
                else if (cnt == 19) { tfreq[0]++; tfreq[1]++; }
                else tfreq[2]++;
            } else tfreq[k + 2]++;
        }
    }
    putbits(16, nsym);
    if (cused >= 2) {
        int tused = 0, tone = 0;
        for (int i = 0; i < NT; i++) if (tfreq[i]) { tused++; tone = i; }
        if (tused >= 2) { make_lengths(NT, tfreq, tlen); make_codes(NT, tlen, tcode); write_pt_len(NT, TBIT, 3, tlen); }
        else { memset(tlen, 0, NT); putbits(TBIT, 0); putbits(TBIT, tone); }
        putbits(CBIT, n);
        for (int i = 0; i < n;) {
            int k = clen[i++];
            if (k == 0) {
                int cnt = 1;
                while (i < n && clen[i] == 0) { i++; cnt++; }
                if (cnt <= 2) { for (int j = 0; j < cnt; j++) if (tlen[0]) putbits(tlen[0], tcode[0]); }
                else if (cnt <= 18) { if (tlen[1]) putbits(tlen[1], tcode[1]); putbits(4, cnt - 3); }
                else if (cnt == 19) { if (tlen[0]) putbits(tlen[0], tcode[0]); if (tlen[1]) putbits(tlen[1], tcode[1]); putbits(4, 15); }
                else { if (tlen[2]) putbits(tlen[2], tcode[2]); putbits(CBIT, cnt - 20); }
            } else if (tlen[k + 2]) putbits(tlen[k + 2], tcode[k + 2]);
        }
        make_codes(NC, clen, ccode);
    } else {
        // one symbol only: empty t and c tables, the symbol itself
        putbits(TBIT, 0); putbits(TBIT, 0);
        putbits(CBIT, 0); putbits(CBIT, cone);
        memset(clen, 0, NC);
    }
    int pused = 0, pone = 0;
    for (int i = 0; i < NP; i++) if (pfreq[i]) { pused++; pone = i; }
    if (pused >= 2) { make_lengths(NP, pfreq, plen); make_codes(NP, plen, pcode); write_pt_len(NP, PBIT, -1, plen); }
    else { memset(plen, 0, NP); putbits(PBIT, 0); putbits(PBIT, pone); }
    for (int i = 0; i < nsym; i++) {
        int c = sym_c[i];
        if (clen[c]) putbits(clen[c], ccode[c]);
        if (c >= 256) {
            unsigned p = sym_p[i], b = 0, q = p;
            while (q) { b++; q >>= 1; }
            if (plen[b]) putbits(plen[b], pcode[b]);
            if (b > 1) putbits(b - 1, p & ((1u << (b - 1)) - 1));
        }
    }
    nsym = 0;
}

static void emit(int c, unsigned p) {
    sym_c[nsym] = (uint16_t)c; sym_p[nsym] = (uint16_t)p;
    if (++nsym == BLOCKSYM) flush_block();
}

// ---- LZ77 with hash chains ----
static void compress(const uint8_t *d, size_t n) {
    int32_t *head = malloc(sizeof(int32_t) * HASHSIZE), *prev = malloc(sizeof(int32_t) * DICSIZ);
    for (int i = 0; i < HASHSIZE; i++) head[i] = -1;
    #define HASH(i) ((((unsigned)d[i] << 10) ^ ((unsigned)d[(i) + 1] << 5) ^ d[(i) + 2]) & (HASHSIZE - 1))
    size_t i = 0;
    while (i < n) {
        int best = 0; size_t bestpos = 0;
        if (i + THRESHOLD <= n) {
            int32_t cand = head[HASH(i)];
            int depth = CHAIN;
            size_t maxlen = n - i < MAXMATCH ? n - i : MAXMATCH;
            while (cand >= 0 && depth-- > 0 && i - (size_t)cand <= DICSIZ) {
                const uint8_t *a = d + cand, *b = d + i;
                if (a[best] == b[best]) {
                    size_t l = 0;
                    while (l < maxlen && a[l] == b[l]) l++;
                    if ((int)l > best) { best = (int)l; bestpos = (size_t)cand; if (l == maxlen) break; }
                }
                int32_t nx = prev[cand & (DICSIZ - 1)];
                if (nx >= cand) break;
                cand = nx;
            }
        }
        size_t adv = best >= THRESHOLD ? (size_t)best : 1;
        if (best >= THRESHOLD) emit(256 + best - THRESHOLD, (unsigned)(i - bestpos - 1));
        else emit(d[i], 0);
        for (size_t k = 0; k < adv; k++, i++) {
            if (i + THRESHOLD <= n) {
                unsigned h = HASH(i);
                prev[i & (DICSIZ - 1)] = head[h];
                head[h] = (int32_t)i;
            }
        }
    }
    flush_block();
    flushbits();
    free(head); free(prev);
}

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t crc = 0;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1; }
    return crc;
}

static uint8_t *load(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? n : 1);
    if (fread(b, 1, n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *len = n; return b;
}

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: mdz_pack <original.MDZ> <in.MDT> <out.MDZ>\n"); return 1; }
    size_t tn, n;
    uint8_t *t = load(argv[1], &tn), *d = load(argv[2], &n);
    if (!t || !d || tn < 24 || memcmp(t + 2, "MDZ\0\0", 5) || t[20] != 2) { fprintf(stderr, "bad input\n"); return 1; }
    size_t hdr = t[0] | t[1] << 8;
    compress(d, n);
    wr32(t + 7, (uint32_t)outlen);
    wr32(t + 11, (uint32_t)n);
    uint16_t crc = crc16(d, n);
    t[21] = (uint8_t)crc; t[22] = (uint8_t)(crc >> 8);
    FILE *f = fopen(argv[3], "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", argv[3]); return 1; }
    fwrite(t, 1, hdr, f);
    fwrite(out, 1, outlen, f);
    fputc(0, f);   // end of archive
    fclose(f);
    printf("%s: %zu -> %zu bytes\n", argv[3], n, hdr + outlen + 1);
    return 0;
}
