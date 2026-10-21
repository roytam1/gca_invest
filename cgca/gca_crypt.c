/////////////////////////////////////////////////////////////////
// gca_crypt.c -- the "GflGCACryptograph" keystream.
//
// Transcribed from gcac.exe (sub_401320 key setup, sub_401490/sub_401560
// keystream, sub_4112D0..sub_4113A0 the Mersenne Twister core).
// See GCA_FORMAT.md section 6.
/////////////////////////////////////////////////////////////////
#include "gca.h"

#define GCA_MT_N    624
#define GCA_MT_M    397
#define GCA_MT_MAG  0x9908B0DFUL
#define GCA_MT_TEMPA 0x9D2C5680UL
#define GCA_MT_TEMPB 0xEFC60000UL

#define GCA_DEFAULT_PASSWORD "GflGCACryptograph"

static void gca_mt_twist(gca_crypt* c)
{
    unsigned long y;
    int k;
    for (k = 0; k < GCA_MT_N - GCA_MT_M; k++) {
        y = (c->state[k] & 0x80000000UL) | (c->state[k + 1] & 0x7FFFFFFFUL);
        c->state[k] = c->state[k + GCA_MT_M] ^ (y >> 1) ^
                      ((c->state[k + 1] & 1) ? GCA_MT_MAG : 0);
    }
    for (; k < GCA_MT_N - 1; k++) {
        y = (c->state[k] & 0x80000000UL) | (c->state[k + 1] & 0x7FFFFFFFUL);
        c->state[k] = c->state[k + (GCA_MT_M - GCA_MT_N)] ^ (y >> 1) ^
                      ((c->state[k + 1] & 1) ? GCA_MT_MAG : 0);
    }
    y = (c->state[GCA_MT_N - 1] & 0x80000000UL) | (c->state[0] & 0x7FFFFFFFUL);
    c->state[GCA_MT_N - 1] = c->state[GCA_MT_M - 1] ^ (y >> 1) ^
                             ((c->state[0] & 1) ? GCA_MT_MAG : 0);
    c->idx = 0;
}

static unsigned long gca_mt_next(gca_crypt* c)
{
    unsigned long y;
    if (c->idx >= GCA_MT_N)
        gca_mt_twist(c);
    y = c->state[c->idx++];
    y ^= (y >> 11);
    y ^= (y << 7) & GCA_MT_TEMPA;
    y ^= (y << 15) & GCA_MT_TEMPB;
    y ^= (y >> 18);
    return y;
}

void gca_crypt_setkey(gca_crypt* c, const unsigned char* key, size_t len)
{
    unsigned long v19[GCA_MT_N];
    unsigned long s, s2, m, ebp;
    size_t i;

    memset(v19, 0, sizeof(v19));
    for (i = 0; i < len; i++) {
        size_t idx = (i + 1) % GCA_MT_N;
        /* the key byte is sign extended (movsx in the original) */
        long sc = (long)(signed char)key[i];
        unsigned long v = (unsigned long)sc;
        v = (v | (v << 8)) & 0xFFFFFFFFUL;
        v = (v | (v << 16)) & 0xFFFFFFFFUL;      /* c * 0x01010101 */
        v19[idx] = ((v19[idx] << 8) ^ v) & 0xFFFFFFFFUL;
    }

    ebp = (unsigned long)len + 1;
    if (ebp > GCA_MT_N)
        ebp = GCA_MT_N;

    s = 4357UL;
    for (i = 0; i < GCA_MT_N; i++) {
        s2 = (69069UL * s + 1UL) & 0xFFFFFFFFUL;
        m = (s & 0xFFFF0000UL) | ((s2 >> 16) & 0xFFFFUL);
        s = (69069UL * s2 + 1UL) & 0xFFFFFFFFUL;
        c->state[i] = (m ^ v19[i % ebp]) & 0xFFFFFFFFUL;
    }
    c->idx = GCA_MT_N;
    c->word = 0;
    c->counter = 0;
    gca_mt_twist(c);
}

static unsigned long gca_rol32(unsigned long v, unsigned n)
{
    n &= 31;
    if (n == 0)
        return v;
    return ((v << n) | (v >> (32 - n))) & 0xFFFFFFFFUL;
}

static unsigned long gca_rotcnt(unsigned long x)
{
    unsigned long t = (x ^ (x >> 16)) & 0xFFFFFFFFUL;
    return ((t & 0x1FUL) ^ ((t >> 8) & 0x1FUL)) & 31UL;
}

static unsigned char gca_crypt_byte(gca_crypt* c)
{
    unsigned long w[8], t;
    int i;
    if ((c->counter & 3) == 0) {
        for (i = 0; i < 8; i++)
            w[i] = gca_mt_next(c);
        t = gca_rol32(w[0] ^ w[2], gca_rotcnt(w[1] ^ w[3])) ^
            gca_rol32(w[4] ^ w[6], gca_rotcnt(w[5] ^ w[7]));
        c->word = t & 0xFFFFFFFFUL;
    }
    i = (int)(c->counter & 3);
    c->counter++;
    return (unsigned char)((c->word >> (i * 8)) & 0xFF);
}

void gca_crypt_xor(gca_crypt* c, unsigned char* buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        buf[i] ^= gca_crypt_byte(c);
}

/* ---------------------------------------------------------------- */
/* key strings                                                       */
static int gca_key_build(gca_buf* out, const char* spell, const char* suffix)
{
    gca_buf_init(out);
    if (!spell || !*spell)
        spell = GCA_DEFAULT_PASSWORD;
    if (gca_buf_puts(out, spell) != 0)
        return -1;
    if (suffix && gca_buf_puts(out, suffix) != 0)
        return -1;
    return 0;
}

int gca_key_payload(const char* spell, const char* name, gca_buf* out)
{
    return gca_key_build(out, spell, name);
}

int gca_key_dir(const char* spell, gca_buf* out)
{
    return gca_key_build(out, spell, "Header");
}

int gca_key_solid(const char* spell, gca_buf* out)
{
    return gca_key_build(out, spell, NULL);
}
