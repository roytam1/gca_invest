/////////////////////////////////////////////////////////////////
// gca_util.c -- buffers, CRC-32, SHA-1 and little endian accessors.
/////////////////////////////////////////////////////////////////
#include "gca.h"
#include <sys/stat.h>

/* ---------------------------------------------------------------- */
/* path helpers                                                      */
int gca_path_exists(const char* path)
{
    struct stat st;
    return path && stat(path, &st) == 0;
}

int gca_archive_path(const char* in, char* out, size_t cap)
{
    size_t n, i, sep = 0, cut;
    if (!in || !out || cap == 0)
        return -1;
    n = strlen(in);
    if (n + 5 > cap)
        return -1;
    for (i = 0; i < n; i++)
        if (in[i] == '\\' || in[i] == '/')
            sep = i + 1;
    cut = n;
    for (i = sep; i < n; i++)
        if (in[i] == '.')
            cut = i;
    if (cut > sep && cut < n) {
        memcpy(out, in, cut);
        out[cut] = 0;
    } else {
        memcpy(out, in, n);
        out[n] = 0;
    }
    strcat(out, ".gca");
    return 0;
}

/* ---------------------------------------------------------------- */
/* buffers                                                           */
void gca_buf_init(gca_buf* b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void gca_buf_free(gca_buf* b)
{
    if (b->data) free(b->data);
    gca_buf_init(b);
}

int gca_buf_reserve(gca_buf* b, size_t extra)
{
    size_t need = b->len + extra;
    size_t cap;
    unsigned char* p;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > (size_t)-1 / 2)
            return -1;
        cap *= 2;
    }
    p = (unsigned char*)realloc(b->data, cap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

int gca_buf_put(gca_buf* b, const void* p, size_t n)
{
    if (n == 0)
        return 0;
    if (gca_buf_reserve(b, n) != 0)
        return -1;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

int gca_buf_putc(gca_buf* b, int c)
{
    if (gca_buf_reserve(b, 1) != 0)
        return -1;
    b->data[b->len++] = (unsigned char)c;
    return 0;
}

int gca_buf_puts(gca_buf* b, const char* s)
{
    return gca_buf_put(b, s, strlen(s));
}

int gca_buf_put32(gca_buf* b, unsigned long v)
{
    unsigned char t[4];
    gca_wr32(t, v);
    return gca_buf_put(b, t, 4);
}

int gca_buf_put64(gca_buf* b, unsigned long lo, unsigned long hi)
{
    unsigned char t[8];
    gca_wr32(t, lo);
    gca_wr32(t + 4, hi);
    return gca_buf_put(b, t, 8);
}

int gca_buf_set(gca_buf* b, size_t off, unsigned long v)
{
    if (off + 4 > b->len)
        return -1;
    gca_wr32(b->data + off, v);
    return 0;
}

/* ---------------------------------------------------------------- */
/* little endian accessors                                          */
unsigned long gca_rd32(const unsigned char* p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

unsigned long gca_rd16(const unsigned char* p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

void gca_wr32(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

void gca_wr16(unsigned char* p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

/* ---------------------------------------------------------------- */
/* CRC-32 (reflected, poly 0xEDB88320, init/final XOR 0xFFFFFFFF)  */
static unsigned long gca_crc_table[256];
static int gca_crc_ready = 0;

static void gca_crc_make(void)
{
    int i, j;
    for (i = 0; i < 256; i++) {
        unsigned long c = (unsigned long)i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        gca_crc_table[i] = c;
    }
    gca_crc_ready = 1;
}

unsigned long gca_crc32(const unsigned char* data, size_t len,
                        unsigned long seed)
{
    unsigned long c;
    if (!gca_crc_ready)
        gca_crc_make();
    c = ~seed;
    while (len--)
        c = gca_crc_table[(c ^ *data++) & 0xFF] ^ (c >> 8);
    return ~c;
}

/* ---------------------------------------------------------------- */
/* SHA-1 (FIPS 180-1)                                                */
#define GCA_SHA1_ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void gca_sha1_block(unsigned long state[5], const unsigned char blk[64])
{
    unsigned long w[80];
    unsigned long a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = ((unsigned long)blk[i * 4] << 24) |
               ((unsigned long)blk[i * 4 + 1] << 16) |
               ((unsigned long)blk[i * 4 + 2] << 8) |
               ((unsigned long)blk[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++)
        w[i] = GCA_SHA1_ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      t = ((b & c) | ((~b) & d)) + 0x5A827999UL;
        else if (i < 40) t = (b ^ c ^ d) + 0x6ED9EBA1UL;
        else if (i < 60) t = ((b & c) | (b & d) | (c & d)) + 0x8F1BBCDCUL;
        else             t = (b ^ c ^ d) + 0xCA62C1D6UL;
        t += GCA_SHA1_ROL(a, 5) + e + w[i];
        e = d; d = c; c = GCA_SHA1_ROL(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

void gca_sha1(const unsigned char* data, size_t len,
              unsigned char digest[20])
{
    unsigned long state[5];
    unsigned char blk[64];
    size_t i, n, rem;
    unsigned long lo, hi;

    state[0] = 0x67452301UL;
    state[1] = 0xEFCDAB89UL;
    state[2] = 0x98BADCFEUL;
    state[3] = 0x10325476UL;
    state[4] = 0xC3D2E1F0UL;

    n = len / 64;
    for (i = 0; i < n; i++)
        gca_sha1_block(state, data + i * 64);

    rem = len % 64;
    memset(blk, 0, sizeof(blk));
    if (rem) memcpy(blk, data + n * 64, rem);
    blk[rem] = 0x80;
    if (rem >= 56) {
        gca_sha1_block(state, blk);
        memset(blk, 0, sizeof(blk));
    }
    /* the message length as a 64 bit big endian bit count */
    lo = ((unsigned long)(len & 0x1FFFFFFFUL)) << 3;
    hi = (unsigned long)(len >> 29);
    blk[63] = (unsigned char)(lo & 0xFF);
    blk[62] = (unsigned char)((lo >> 8) & 0xFF);
    blk[61] = (unsigned char)((lo >> 16) & 0xFF);
    blk[60] = (unsigned char)((lo >> 24) & 0xFF);
    blk[59] = (unsigned char)(hi & 0xFF);
    blk[58] = (unsigned char)((hi >> 8) & 0xFF);
    blk[57] = (unsigned char)((hi >> 16) & 0xFF);
    blk[56] = (unsigned char)((hi >> 24) & 0xFF);
    gca_sha1_block(state, blk);

    for (i = 0; i < 5; i++) {
        digest[i * 4]     = (unsigned char)((state[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = (unsigned char)((state[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = (unsigned char)((state[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = (unsigned char)(state[i] & 0xFF);
    }
}

void gca_sha1_hex(const unsigned char* data, size_t len, char* out41)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[20];
    int i;
    gca_sha1(data, len, digest);
    for (i = 0; i < 20; i++) {
        out41[i * 2]     = hex[(digest[i] >> 4) & 0xF];
        out41[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out41[40] = '\0';
}
