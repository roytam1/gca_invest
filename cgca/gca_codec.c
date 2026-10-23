/////////////////////////////////////////////////////////////////
// gca_codec.c -- the GCA range coder, stages and frames.
//
// Transcribed from gcac.exe:
//   sub_411740/411760/411790  range decoder state
//   sub_412380/412720        bit / number decoding
//   sub_411770/411B80/411800/411CA0/412000/411970  range encoder
//   sub_40FC20/sub_40FE20    stage 1: MTF + run length
//   sub_40EEC0/sub_40F1F0    Burrows-Wheeler transform
//   sub_40FFD0/sub_4101B0    LZ stage
//   sub_410410               frame writer
/////////////////////////////////////////////////////////////////
#include "gca.h"
#include <math.h>

/* Sanity limit for one decoded frame.  Payload blocks are 1 MiB; the
   1/8 MiB directory frames are far below this as well.  It exists only
   so that a corrupt header cannot ask the decoder to loop for hours. */
#define GCA_MAX_FRAME (256UL * 1024UL * 1024UL)

/* ---------------------------------------------------------------- */
/* range decoder                                                     */
void gca_rc_dec_init(gca_rc_dec* d, const unsigned char* data, size_t len,
                     size_t pos)
{
    unsigned b;
    d->data = data;
    d->len = len;
    d->pos = pos;
    d->code = 0;
    d->range = 0x80;
    d->buf = 0;
    /* one byte is discarded, then one byte is read */
    if (d->pos < d->len)
        d->pos++;
    b = 0;
    if (d->pos < d->len)
        b = data[d->pos++];
    d->buf = b;
    d->code = b >> 1;
}

void gca_rc_dec_normalize(gca_rc_dec* d)
{
    /* A corrupt stream can drive the range to zero.  Without this the
       loop below would never terminate. */
    if (d->range == 0)
        d->range = 1;
    while (d->range <= 0x800000UL) {
        unsigned b = 0;
        d->code = (((d->code << 1) | (d->buf & 1)) << 7) & 0xFFFFFFFFUL;
        if (d->pos < d->len)
            b = d->data[d->pos];
        d->pos++;
        d->buf = b;
        d->code |= (b >> 1);
        d->range = (d->range << 8) & 0xFFFFFFFFUL;
    }
}

int gca_rc_dec_bit(gca_rc_dec* d, unsigned long prob, unsigned long* bound)
{
    gca_rc_dec_normalize(d);
    *bound = (prob * (d->range >> 12)) & 0xFFFFFFFFUL;
    if (d->code < *bound) {
        d->range = *bound;
        return 0;
    }
    d->code = (d->code - *bound) & 0xFFFFFFFFUL;
    d->range = (d->range - *bound) & 0xFFFFFFFFUL;
    return 1;
}

unsigned long gca_rc_dec_tree(gca_rc_dec* d, unsigned long* model, int node,
                              int nbits, int shift)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < nbits; i++) {
        unsigned long p = model[node];
        unsigned long bound;
        int b = gca_rc_dec_bit(d, p, &bound);
        if (b) {
            model[node] = p - (p >> shift);
        } else {
            model[node] = p + ((0x1000UL - p) >> shift);
            d->range = bound;
        }
        node = b + 2 * node;
        v = b + 2 * v;
    }
    return v;
}

unsigned long gca_rc_dec_number(gca_rc_dec* d, unsigned long* model, int nbits,
                                int shift)
{
    int i = 0;
    unsigned long raw = 0;
    for (;;) {
        unsigned long p = model[i];
        unsigned long bound;
        int b = gca_rc_dec_bit(d, p, &bound);
        if (b) {
            model[i] = p - (p >> shift);
            break;
        }
        model[i] = p + ((0x1000UL - p) >> shift);
        d->range = bound;
        i++;
        if (i >= nbits)
            break;
    }
    if (i <= 1)
        return (unsigned long)i;
    {
        int nb = i - 1;
        while (nb > 0) {
            unsigned long half;
            gca_rc_dec_normalize(d);
            half = d->range >> 1;
            raw <<= 1;
            if (d->code < half) {
                d->range = half;
            } else {
                raw |= 1;
                d->code = (d->code - half) & 0xFFFFFFFFUL;
                d->range = (d->range - half) & 0xFFFFFFFFUL;
            }
            nb--;
        }
    }
    return (1UL << ((unsigned long)(i - 1) & 31)) + raw;
}

/* ---------------------------------------------------------------- */
/* range encoder                                                     */
void gca_rc_enc_init(gca_rc_enc* e, gca_buf* out)
{
    e->out = out;
    e->code = 0;
    e->range = 0x80000000UL;
    e->cache = 0;
    e->cache_size = 0;
}

void gca_rc_enc_normalize(gca_rc_enc* e)
{
    while (e->range <= 0x800000UL) {
        unsigned long c = e->code;
        if (c < 0x7F800000UL) {
            gca_buf_putc(e->out, (int)(e->cache & 0xFF));
            while (e->cache_size > 0) {
                gca_buf_putc(e->out, 0xFF);
                e->cache_size--;
            }
            e->cache = (c >> 23) & 0xFF;
        } else if (c < 0x80000000UL) {
            e->cache_size++;
        } else {
            gca_buf_putc(e->out, (int)((e->cache + 1) & 0xFF));
            while (e->cache_size > 0) {
                gca_buf_putc(e->out, 0x00);
                e->cache_size--;
            }
            e->cache = (c >> 23) & 0xFF;
        }
        e->range = (e->range << 8) & 0xFFFFFFFFUL;
        e->code = (c & 0x7FFFFFUL) << 8;
    }
}

void gca_rc_enc_bit(gca_rc_enc* e, unsigned long prob, int bit)
{
    unsigned long bound;
    gca_rc_enc_normalize(e);
    bound = (prob * (e->range >> 12)) & 0xFFFFFFFFUL;
    if (bit) {
        e->code = (e->code + bound) & 0xFFFFFFFFUL;
        e->range = (e->range - bound) & 0xFFFFFFFFUL;
    } else {
        e->range = bound;
    }
}

void gca_rc_enc_rawbits(gca_rc_enc* e, int nbits, unsigned long value)
{
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        unsigned long half;
        gca_rc_enc_normalize(e);
        half = e->range >> 1;
        if ((value >> i) & 1) {
            e->code = (e->code + half) & 0xFFFFFFFFUL;
            e->range = (e->range - half) & 0xFFFFFFFFUL;
        } else {
            e->range = half;
        }
    }
}

void gca_rc_enc_tree(gca_rc_enc* e, unsigned long* model, int node,
                     unsigned long value, int nbits, int shift)
{
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        int b = (int)((value >> i) & 1);
        unsigned long p = model[node];
        gca_rc_enc_bit(e, p, b);
        if (b)
            model[node] = p - (p >> shift);
        else
            model[node] = p + ((0x1000UL - p) >> shift);
        node = b + 2 * node;
    }
}

void gca_rc_enc_number(gca_rc_enc* e, unsigned long* model, unsigned long value,
                       int nbits, int shift)
{
    int i = 0;
    while ((value >> i) != 0) {
        unsigned long p = model[i];
        gca_rc_enc_bit(e, p, 0);
        model[i] = p + ((0x1000UL - p) >> shift);
        i++;
        if (i >= nbits)
            return;                     /* the scan ran out of levels */
    }
    {
        unsigned long p = model[i];
        gca_rc_enc_bit(e, p, 1);
        model[i] = p - (p >> shift);
    }
    if (i > 1)
        gca_rc_enc_rawbits(e, i - 1, value & ((1UL << (i - 1)) - 1));
}

void gca_rc_enc_flush(gca_rc_enc* e)
{
    int i;
    while (e->range <= 0x800000UL)
        gca_rc_enc_normalize(e);
    if ((e->code & 0xFF800000UL) > 0x7F800000UL) {
        gca_buf_putc(e->out, (int)((e->cache + 1) & 0xFF));
        while (e->cache_size > 0) {
            gca_buf_putc(e->out, 0x00);
            e->cache_size--;
        }
    } else {
        gca_buf_putc(e->out, (int)(e->cache & 0xFF));
        while (e->cache_size > 0) {
            gca_buf_putc(e->out, 0xFF);
            e->cache_size--;
        }
    }
    e->cache_size = 0;
    e->cache = (e->code >> 23) & 0xFF;
    gca_buf_putc(e->out, (int)(e->cache & 0xFF));
    e->cache_size = 4;
    for (i = 0; i < 4; i++)
        gca_buf_putc(e->out, 0xFF);
}

/* ---------------------------------------------------------------- */
/* stage 1: MTF + run length                                         */
int gca_stage1_encode(const unsigned char* in, size_t n, gca_buf* out)
{
    unsigned long t256[256], t32[32];
    unsigned char mtf[256];
    gca_rc_enc e;
    size_t i;
    int j;

    for (j = 0; j < 256; j++) {
        t256[j] = 0x800;
        mtf[j] = (unsigned char)j;
    }
    for (j = 0; j < 32; j++)
        t32[j] = 0x800;

    gca_rc_enc_init(&e, out);
    if (n == 0) {
        gca_rc_enc_flush(&e);
        return 0;
    }
    {
        unsigned char prev = in[0];
        unsigned long run = 0;
        for (i = 1; i < n; i++) {
            unsigned char cur = in[i];
            if (prev == cur) {
                run++;
                continue;
            }
            {
                int pos = 0;
                if (prev != mtf[0]) {
                    for (pos = 0; pos < 256; pos++)
                        if (mtf[pos] == prev) break;
                    if (pos == 256) pos = 0;
                }
                gca_rc_enc_tree(&e, t256, 1, (unsigned long)((pos - 1) & 0xFF),
                                8, 5);
                gca_rc_enc_number(&e, t32, run, 32, 6);
                while (pos > 0) {
                    mtf[pos] = mtf[pos - 1];
                    pos--;
                }
                mtf[0] = prev;
            }
            run = 0;
            prev = cur;
        }
        {
            int pos = 0;
            if (prev != mtf[0]) {
                for (pos = 0; pos < 256; pos++)
                    if (mtf[pos] == prev) break;
                if (pos == 256) pos = 0;
            }
            gca_rc_enc_tree(&e, t256, 1, (unsigned long)((pos - 1) & 0xFF),
                            8, 5);
            gca_rc_enc_number(&e, t32, run, 32, 6);
        }
    }
    gca_rc_enc_flush(&e);
    return 0;
}

int gca_stage1_decode(const unsigned char* in, size_t len, size_t total,
                       gca_buf* out)
{
    unsigned long t256[256], t32[32];
    unsigned char mtf[256];
    gca_rc_dec d;
    size_t produced = 0;
    int j;

    for (j = 0; j < 256; j++) {
        t256[j] = 0x800;
        mtf[j] = (unsigned char)j;
    }
    for (j = 0; j < 32; j++)
        t32[j] = 0x800;

    gca_rc_dec_init(&d, in, len, 0);
    while (produced < total) {
        unsigned long idx = gca_rc_dec_tree(&d, t256, 1, 8, 5);
        unsigned long run = gca_rc_dec_number(&d, t32, 32, 6);
        unsigned mi = (idx + 1) & 0xFF;
        unsigned char sym = mtf[mi];
        unsigned long n = run + 1;
        while (mi > 0) {
            mtf[mi] = mtf[mi - 1];
            mi--;
        }
        mtf[0] = sym;
        while (n > 0 && produced < total) {
            gca_buf_putc(out, sym);
            produced++;
            n--;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* the GCF2 stage 1: MTF/RLE with the mode selected coders           */
/*                                                                     */
/* The mode word (gcac.exe sub_40F8E0):                               */
/*   bit 28   MTF enable                                               */
/*   bit 24   run length enable                                        */
/*   >>20 &F  the symbol coder: 0 plain, 2 averaged, else weighted     */
/*   >>16 &F  the number coder: 0 plain, else weighted                 */
/*   >>12 &F  the adaptation shift of the symbol tree                 */
/*   >>8  &F  the adaptation shift of the number coder                */
/*   >>4  &F  the adaptation shift of the context tables              */
/*   bit 31   the literals were noised by the encoder                 */
/* ---------------------------------------------------------------- */

/* the mixed symbol tree: t1 adapts with shift, t2 with x */
static unsigned long gca_rc_dec_tree_mixed(gca_rc_dec* d, unsigned long* t1,
                                           unsigned long* t2, int node,
                                           int nbits, int shift, int x,
                                           int w)
{
    unsigned sc = ((unsigned)shift) & 31;
    unsigned xc = ((unsigned)x) & 31;
    unsigned long v = 0;
    int i;
    if (nbits <= 0)
        return 0;
    for (i = 0; i < nbits; i++) {
        unsigned long mixed, bound;
        int b;
        gca_rc_dec_normalize(d);
        mixed = (t1[node] * (unsigned long)(4 - w) +
                 t2[node] * (unsigned long)w) >> 2;
        bound = (d->range >> 12) * mixed;
        if (d->code < bound) {
            unsigned long a, b2;
            d->range = bound;
            a = t1[node]; t1[node] = a + ((0x1000UL - a) >> sc);
            b2 = t2[node]; t2[node] = b2 + ((0x1000UL - b2) >> xc);
            b = 0;
        } else {
            unsigned long a, b2;
            d->code = (d->code - bound) & 0xFFFFFFFFUL;
            d->range = (d->range - bound) & 0xFFFFFFFFUL;
            a = t1[node]; t1[node] = a - (a >> sc);
            b2 = t2[node]; t2[node] = b2 - (b2 >> xc);
            b = 1;
        }
        node = b + 2 * node;
        v = b + 2 * v;
    }
    return v;
}

/* the averaged symbol tree: (t1[node] + t2[node]) >> 1 */
static unsigned long gca_rc_dec_tree_mixed2(gca_rc_dec* d, unsigned long* t1,
                                            unsigned long* t2, int node,
                                            int nbits, int shift, int x)
{
    unsigned sc = ((unsigned)shift) & 31;
    unsigned xc = ((unsigned)x) & 31;
    unsigned long v = 0;
    int i;
    if (nbits <= 0)
        return 0;
    for (i = 0; i < nbits; i++) {
        unsigned long mixed, bound;
        int b;
        gca_rc_dec_normalize(d);
        mixed = (t1[node] + t2[node]) >> 1;
        bound = (d->range >> 12) * mixed;
        if (d->code < bound) {
            unsigned long a, b2;
            d->range = bound;
            a = t1[node]; t1[node] = a + ((0x1000UL - a) >> sc);
            b2 = t2[node]; t2[node] = b2 + ((0x1000UL - b2) >> xc);
            b = 0;
        } else {
            unsigned long a, b2;
            d->code = (d->code - bound) & 0xFFFFFFFFUL;
            d->range = (d->range - bound) & 0xFFFFFFFFUL;
            a = t1[node]; t1[node] = a - (a >> sc);
            b2 = t2[node]; t2[node] = b2 - (b2 >> xc);
            b = 1;
        }
        node = b + 2 * node;
        v = b + 2 * v;
    }
    return v;
}

/* the mixed number coder: t1 adapts with shift, t2 with x */
static unsigned long gca_rc_dec_number_mixed(gca_rc_dec* d, unsigned long* t1,
                                              unsigned long* t2, int n,
                                              int shift, int x, int w)
{
    unsigned sc = ((unsigned)shift) & 31;
    unsigned xc = ((unsigned)x) & 31;
    unsigned long raw = 0;
    int i = 0;
    if (n <= 0)
        return 0;
    for (;;) {
        unsigned long mixed, bound;
        int b;
        gca_rc_dec_normalize(d);
        mixed = (t1[i] * (unsigned long)(4 - w) +
                 t2[i] * (unsigned long)w) >> 2;
        bound = (d->range >> 12) * mixed;
        if (d->code < bound) {
            unsigned long a, b2;
            d->range = bound;
            a = t1[i]; t1[i] = a + ((0x1000UL - a) >> sc);
            b2 = t2[i]; t2[i] = b2 + ((0x1000UL - b2) >> xc);
            b = 0;
        } else {
            unsigned long a, b2;
            d->code = (d->code - bound) & 0xFFFFFFFFUL;
            d->range = (d->range - bound) & 0xFFFFFFFFUL;
            a = t1[i]; t1[i] = a - (a >> sc);
            b2 = t2[i]; t2[i] = b2 - (b2 >> xc);
            b = 1;
        }
        if (b)
            break;
        i++;
        if (i >= n)
            break;
    }
    if (i <= 1)
        return (unsigned long)i;
    {
        int nb = i - 1;
        while (nb > 0) {
            unsigned long half;
            gca_rc_dec_normalize(d);
            half = d->range >> 1;
            raw <<= 1;
            if (d->code < half) {
                d->range = half;
            } else {
                raw |= 1;
                d->code = (d->code - half) & 0xFFFFFFFFUL;
                d->range = (d->range - half) & 0xFFFFFFFFUL;
            }
            nb--;
        }
    }
    return (1UL << ((unsigned long)(i - 1) & 31)) + raw;
}

/* ---------------------------------------------------------------- */
/* the variable stage 1 encoder (GCF2)                               */
/* The model updates below mirror gca_var_decode exactly.            */
static void gca_rc_enc_tree_mixed(gca_rc_enc* e, unsigned long* t1,
                                  unsigned long* t2, int node,
                                  unsigned long value, int nbits,
                                  int shift, int x, int w)
{
    unsigned sc = ((unsigned)shift) & 31;
    unsigned xc = ((unsigned)x) & 31;
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        int b = (int)((value >> i) & 1);
        unsigned long mixed = (t1[node] * (unsigned long)(4 - w) +
                               t2[node] * (unsigned long)w) >> 2;
        gca_rc_enc_bit(e, mixed, b);
        if (b) {
            t1[node] -= t1[node] >> sc;
            t2[node] -= t2[node] >> xc;
        } else {
            t1[node] += (0x1000UL - t1[node]) >> sc;
            t2[node] += (0x1000UL - t2[node]) >> xc;
        }
        node = b + 2 * node;
    }
}

static void gca_rc_enc_tree_mixed2(gca_rc_enc* e, unsigned long* t1,
                                   unsigned long* t2, int node,
                                   unsigned long value, int nbits,
                                   int shift, int x)
{
    unsigned sc = ((unsigned)shift) & 31;
    unsigned xc = ((unsigned)x) & 31;
    int i;
    for (i = nbits - 1; i >= 0; i--) {
        int b = (int)((value >> i) & 1);
        unsigned long mixed = (t1[node] + t2[node]) >> 1;
        gca_rc_enc_bit(e, mixed, b);
        if (b) {
            t1[node] -= t1[node] >> sc;
            t2[node] -= t2[node] >> xc;
        } else {
            t1[node] += (0x1000UL - t1[node]) >> sc;
            t2[node] += (0x1000UL - t2[node]) >> xc;
        }
        node = b + 2 * node;
    }
}

static void gca_rc_enc_number_mixed(gca_rc_enc* e, unsigned long* t1,
                                    unsigned long* t2, unsigned long value,
                                    int nbits, int shift, int x, int w)
{
    unsigned sc = ((unsigned)shift) & 31;
    unsigned xc = ((unsigned)x) & 31;
    int i = 0;
    while ((value >> i) != 0) {
        unsigned long mixed = (t1[i] * (unsigned long)(4 - w) +
                               t2[i] * (unsigned long)w) >> 2;
        gca_rc_enc_bit(e, mixed, 0);
        t1[i] += (0x1000UL - t1[i]) >> sc;
        t2[i] += (0x1000UL - t2[i]) >> xc;
        i++;
        if (i >= nbits)
            return;
    }
    {
        unsigned long mixed = (t1[i] * (unsigned long)(4 - w) +
                               t2[i] * (unsigned long)w) >> 2;
        gca_rc_enc_bit(e, mixed, 1);
        t1[i] -= t1[i] >> sc;
        t2[i] -= t2[i] >> xc;
    }
    if (i > 1)
        gca_rc_enc_rawbits(e, i - 1, value & ((1UL << (i - 1)) - 1));
}

int gca_var_encode(const unsigned char* in, size_t len, gca_buf* out,
                   unsigned long mode)
{
    unsigned long* t256, *t32, *t64k, *t8k;
    unsigned char mtf[256];
    gca_rc_enc e;
    size_t pos = 0;
    unsigned prev = 0;
    unsigned useMtf, useRun, vSel, lSel, f12, lnShift, f4;
    int i;

    t256 = (unsigned long*)malloc(sizeof(unsigned long) * 256);
    t32  = (unsigned long*)malloc(sizeof(unsigned long) * 32);
    t64k = (unsigned long*)malloc(sizeof(unsigned long) * 65536);
    t8k  = (unsigned long*)malloc(sizeof(unsigned long) * 8192);
    if (!t256 || !t32 || !t64k || !t8k) {
        free(t256); free(t32); free(t64k); free(t8k);
        return -1;
    }
    useMtf   = (mode >> 28) & 1;
    useRun   = (mode >> 24) & 1;
    vSel     = (mode >> 20) & 0xF;
    lSel     = (mode >> 16) & 0xF;
    f12      = (mode >> 12) & 0xF;
    lnShift  = (mode >> 8) & 0xF;
    f4       = (mode >> 4) & 0xF;

    for (i = 0; i < 256; i++) t256[i] = 0x800;
    for (i = 0; i < 32; i++) t32[i] = 0x800;
    if (vSel != 0 || lSel != 0) {
        for (i = 0; i < 65536; i++) t64k[i] = 0x800;
        for (i = 0; i < 8192; i++) t8k[i] = 0x800;
    }
    for (i = 0; i < 256; i++) mtf[i] = (unsigned char)i;

    gca_rc_enc_init(&e, out);
    while (pos < len) {
        unsigned long sym = in[pos];
        unsigned long run = 0;
        unsigned long idx;

        if (useRun) {
            while (pos + run + 1 < len && in[pos + run + 1] == sym)
                run++;
        }
        if (useMtf) {
            unsigned long k = 0;
            while (mtf[k] != (unsigned char)sym)
                k++;
            idx = useRun ? ((k - 1) & 0xFF) : k;
            while (k > 0) {
                mtf[k] = mtf[k - 1];
                k--;
            }
            mtf[0] = (unsigned char)sym;
        } else {
            idx = sym & 0xFF;
        }

        if (vSel == 0)
            gca_rc_enc_tree(&e, t256, 1, idx, 8, (int)f12);
        else if (vSel == 2)
            gca_rc_enc_tree_mixed2(&e, t256,
                                   t64k + ((prev & 0xFF) << 8), 1, idx, 8,
                                   (int)f12, (int)f4);
        else
            gca_rc_enc_tree_mixed(&e, t256,
                                  t64k + ((prev & 0xFF) << 8), 1, idx, 8,
                                  (int)f12, (int)f4, (int)vSel);

        if (useRun) {
            if (lSel)
                gca_rc_enc_number_mixed(&e, t8k + ((prev & 0xFF) << 5),
                                        t32, run, 32, (int)lnShift,
                                        (int)f4, (int)lSel);
            else
                gca_rc_enc_number(&e, t32, run, 32, (int)lnShift);
        }
        pos += useRun ? (run + 1) : 1;
        prev = (unsigned)sym;
    }
    gca_rc_enc_flush(&e);
    free(t256); free(t32); free(t64k); free(t8k);
    return 0;
}

int gca_var_decode(const unsigned char* in, size_t len, size_t total,
                   unsigned long mode, gca_buf* out)
{
    unsigned long* t256, *t32, *t64k, *t8k;
    unsigned char mtf[256];
    gca_rc_dec d;
    size_t produced = 0;
    unsigned prev = 0;
    unsigned useMtf, useRun, vSel, lSel, f12, lnShift, f4;
    int i;

    t256 = (unsigned long*)malloc(sizeof(unsigned long) * 256);
    t32  = (unsigned long*)malloc(sizeof(unsigned long) * 32);
    t64k = (unsigned long*)malloc(sizeof(unsigned long) * 65536);
    t8k  = (unsigned long*)malloc(sizeof(unsigned long) * 8192);
    if (!t256 || !t32 || !t64k || !t8k) {
        free(t256); free(t32); free(t64k); free(t8k);
        return -1;
    }
    useMtf   = (mode >> 28) & 1;
    useRun   = (mode >> 24) & 1;
    vSel     = (mode >> 20) & 0xF;
    lSel     = (mode >> 16) & 0xF;
    f12      = (mode >> 12) & 0xF;
    lnShift  = (mode >> 8) & 0xF;
    f4       = (mode >> 4) & 0xF;

    if (vSel == 0 && lSel == 0) {
        for (i = 0; i < 256; i++) t256[i] = 0x800;
        for (i = 0; i < 32; i++) t32[i] = 0x800;
    } else {
        for (i = 0; i < 256; i++) t256[i] = 0x800;
        for (i = 0; i < 32; i++) t32[i] = 0x800;
        for (i = 0; i < 65536; i++) t64k[i] = 0x800;
        for (i = 0; i < 8192; i++) t8k[i] = 0x800;
    }
    for (i = 0; i < 256; i++) mtf[i] = (unsigned char)i;

    gca_rc_dec_init(&d, in, len, 0);
    while (produced < total) {
        unsigned long idx, run = 0, sym;
        unsigned mi;
        if (vSel == 0) {
            idx = gca_rc_dec_tree(&d, t256, 1, 8, (int)f12);
        } else if (vSel == 2) {
            idx = gca_rc_dec_tree_mixed2(&d, t256,
                                         t64k + ((prev & 0xFF) << 8), 1, 8,
                                         (int)f12, (int)f4);
        } else {
            idx = gca_rc_dec_tree_mixed(&d, t256,
                                        t64k + ((prev & 0xFF) << 8), 1, 8,
                                        (int)f12, (int)f4, (int)vSel);
        }
        if (useRun) {
            if (lSel)
                run = gca_rc_dec_number_mixed(&d,
                                              t8k + ((prev & 0xFF) << 5), t32,
                                              32, (int)lnShift, (int)f4,
                                              (int)lSel);
            else
                run = gca_rc_dec_number(&d, t32, 32, (int)lnShift);
        }
        mi = useRun ? ((idx + 1) & 0xFF) : (idx & 0xFF);
        if (useMtf) {
            unsigned k;
            sym = mtf[mi];
            for (k = mi; k > 0; k--)
                mtf[k] = mtf[k - 1];
            mtf[0] = (unsigned char)sym;
        } else {
            sym = idx & 0xFF;
        }
        if (useRun) {
            unsigned long n = run + 1;
            while (n > 0 && produced < total) {
                gca_buf_putc(out, (int)sym);
                produced++;
                n--;
            }
        } else {
            if (produced < total) {
                gca_buf_putc(out, (int)sym);
                produced++;
            }
        }
        prev = sym;
    }
    free(t256); free(t32); free(t64k); free(t8k);
    return 0;
}

/* ---------------------------------------------------------------- */
/* Burrows-Wheeler transform                                         */
static int gca_cmp_ulong(const void* a, const void* b)
{
    unsigned long x = *(const unsigned long*)a;
    unsigned long y = *(const unsigned long*)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

int gca_bwt_encode(const unsigned char* in, size_t n, gca_buf* out,
                   unsigned long* prim)
{
    unsigned long *p, *pn, *c, *cn, *cnt;
    size_t i, k;
    int found = 0;

    *prim = 0;
    if (n == 0)
        return 0;
    if (n == 1) {
        gca_buf_putc(out, in[0]);
        return 0;
    }

    p   = (unsigned long*)malloc(sizeof(unsigned long) * n);
    pn  = (unsigned long*)malloc(sizeof(unsigned long) * n);
    c   = (unsigned long*)malloc(sizeof(unsigned long) * n);
    cn  = (unsigned long*)malloc(sizeof(unsigned long) * n);
    /* the counting array is indexed by byte value in the first sort and
       by class afterwards, so it must hold max(n, 256) entries */
    cnt = (unsigned long*)malloc(sizeof(unsigned long) *
                                 (n > 256 ? n + 1 : 256));
    if (!p || !pn || !c || !cn || !cnt) {
        free(p); free(pn); free(c); free(cn); free(cnt);
        return -1;
    }

    /* initial sort by the first character */
    for (i = 0; i < 256; i++) cnt[i] = 0;
    for (i = 0; i < n; i++) cnt[in[i]]++;
    for (i = 1; i < 256; i++) cnt[i] += cnt[i - 1];
    for (i = n; i > 0; i--) p[--cnt[in[i - 1]]] = (unsigned long)(i - 1);
    c[p[0]] = 0;
    for (i = 1; i < n; i++)
        c[p[i]] = c[p[i - 1]] + (in[p[i]] != in[p[i - 1]] ? 1 : 0);

    /* prefix doubling */
    for (k = 1; k < n; k <<= 1) {
        unsigned long cls, maxc;
        for (i = 0; i < n; i++) {
            unsigned long v = p[i] - (unsigned long)k;
            pn[i] = (p[i] >= (unsigned long)k) ? v : v + (unsigned long)n;
        }
        maxc = c[p[n - 1]] + 1;
        for (i = 0; i < maxc; i++) cnt[i] = 0;
        for (i = 0; i < n; i++) cnt[c[pn[i]]]++;
        for (i = 1; i < maxc; i++) cnt[i] += cnt[i - 1];
        for (i = n; i > 0; i--) p[--cnt[c[pn[i - 1]]]] = pn[i - 1];
        cn[p[0]] = 0;
        cls = 0;
        for (i = 1; i < n; i++) {
            unsigned long a = p[i], b = p[i - 1];
            if (c[a] != c[b] ||
                c[(a + (unsigned long)k) % (unsigned long)n] !=
                c[(b + (unsigned long)k) % (unsigned long)n])
                cls++;
            cn[a] = cls;
        }
        { unsigned long* t = c; c = cn; cn = t; }
    }

    /* Identical rotations have the same class but no defined order in
       the doubling sort.  gcac.exe's merge sort is stable and keeps the
       smaller rotation start first; do the same so the primary index
       matches. */
    {
        size_t s = 0;
        while (s < n) {
            size_t e = s + 1;
            unsigned long cls = c[p[s]];
            while (e < n && c[p[e]] == cls)
                e++;
            if (e - s > 1)
                qsort(p + s, e - s, sizeof(unsigned long), gca_cmp_ulong);
            s = e;
        }
    }
    for (i = 0; i < n; i++) {
        unsigned long idx = p[i];
        unsigned long prev = (idx == 0) ? (unsigned long)(n - 1) : idx - 1;
        gca_buf_putc(out, in[prev]);
        if (idx == 0 && !found) {
            *prim = (unsigned long)i;
            found = 1;
        }
    }
    free(p); free(pn); free(c); free(cn); free(cnt);
    return 0;
}

int gca_bwt_decode(const unsigned char* in, size_t n, unsigned long prim,
                   gca_buf* out)
{
    unsigned count[256];
    unsigned long* tmp;
    unsigned long j;
    size_t i;

    if (n == 0)
        return 0;
    if (prim >= n)
        return -1;
    tmp = (unsigned long*)malloc(sizeof(unsigned long) * n);
    if (!tmp)
        return -1;
    for (i = 0; i < 256; i++) count[i] = 0;
    for (i = 0; i < n; i++) count[in[i]]++;
    {
        unsigned sum = 0;
        for (i = 0; i < 256; i++) {
            unsigned t = count[i];
            count[i] = sum;
            sum += t;
        }
    }
    for (i = 0; i < n; i++) {
        unsigned c = in[i];
        tmp[count[c]] = (unsigned long)i;
        count[c]++;
    }
    j = prim;
    for (i = 0; i < n; i++) {
        j = tmp[j];
        gca_buf_putc(out, in[j]);
    }
    free(tmp);
    return 0;
}

/* ---------------------------------------------------------------- */
/* the LZ stage                                                      */
#define GCA_LZ_MIN_MATCH 64

int gca_lz_encode(const unsigned char* in, size_t n, gca_buf* lits,
                  gca_buf* stream)
{
    unsigned long locA[32], locB[32];
    unsigned long* table;
    gca_rc_enc e;
    size_t i = 0;
    unsigned long litcount = 0;
    unsigned h = 0;
    int j;

    for (j = 0; j < 32; j++) locA[j] = locB[j] = 0x800;
    table = (unsigned long*)malloc(sizeof(unsigned long) * 65536);
    if (!table)
        return -1;
    for (j = 0; j < 65536; j++) table[j] = 0xFFFFFFFFUL;

    gca_rc_enc_init(&e, stream);
    while (i < n) {
        unsigned long cand = table[h & 0xFFFF];
        size_t matchlen = 0;
        if (cand != 0xFFFFFFFFUL && cand < i && in[cand] == in[i]) {
            size_t j2 = 0;
            while (i + j2 < n && in[cand + j2] == in[i + j2])
                j2++;
            matchlen = j2;
        }
        if (matchlen >= GCA_LZ_MIN_MATCH) {
            gca_rc_enc_number(&e, locA, litcount, 32, 6);
            gca_rc_enc_number(&e, locB,
                              (unsigned long)(matchlen - GCA_LZ_MIN_MATCH),
                              32, 6);
            litcount = 0;
            while (matchlen > 0) {
                table[h & 0xFFFF] = (unsigned long)i;
                h = ((h & 0xFF) << 8) | in[i];
                i++;
                matchlen--;
            }
        } else {
            gca_buf_putc(lits, in[i]);
            litcount++;
            table[h & 0xFFFF] = (unsigned long)i;
            h = ((h & 0xFF) << 8) | in[i];
            i++;
        }
    }
    gca_rc_enc_number(&e, locA, litcount, 32, 6);
    gca_rc_enc_flush(&e);
    free(table);
    return 0;
}

int gca_lz_decode(const unsigned char* stream, size_t len,
                  const unsigned char* lits, size_t litlen, size_t total,
                  gca_buf* out)
{
    unsigned long locA[32], locB[32];
    unsigned long* hist;
    gca_rc_dec d;
    size_t outpos = 0, litpos = 0;
    unsigned h = 0;
    int j;

    for (j = 0; j < 32; j++) locA[j] = locB[j] = 0x800;
    hist = (unsigned long*)malloc(sizeof(unsigned long) * 65536);
    if (!hist)
        return -1;
    for (j = 0; j < 65536; j++) hist[j] = 0xFFFFFFFFUL;

    gca_rc_dec_init(&d, stream, len, 0);
    while (outpos < total) {
        unsigned long v = gca_rc_dec_number(&d, locA, 32, 6);
        unsigned long mlen, src;
        while (v > 0 && outpos < total) {
            unsigned char b;
            if (litpos >= litlen)
                break;
            b = lits[litpos++];
            gca_buf_putc(out, b);
            hist[h & 0xFFFF] = (unsigned long)outpos;
            h = ((h & 0xFF) << 8) | b;
            outpos++;
            v--;
        }
        if (outpos >= total)
            break;
        /* the match length is coded as (length - 64) and is used as is */
        mlen = gca_rc_dec_number(&d, locB, 32, 6) + 64;
        src = hist[h & 0xFFFF];
        if (src == 0xFFFFFFFFUL || src > outpos)
            break;
        while (mlen > 0 && outpos < total) {
            unsigned char b;
            if (src >= outpos)
                break;
            b = out->data[src];
            src++;
            gca_buf_putc(out, b);
            hist[h & 0xFFFF] = (unsigned long)outpos;
            h = ((h & 0xFF) << 8) | b;
            outpos++;
            mlen--;
        }
    }
    free(hist);
    return 0;
}

/* ---------------------------------------------------------------- */
/* the frame writer chooses GCF1 or GCF2 from the statistics of the
 * BWT output, exactly like gcac.exe sub_40F2F0.  GCF1 is requested by
 * the special mode 0x11005600. */
static unsigned long gca_choose_mode(double a1, double a2, size_t n)
{
    int v3 = 0, v4 = 0, v5 = 2, v6 = 0, v7 = 2, v8 = 4;
    double v10;

    if (a2 < 0.2) {
        v7 = 3;
        v8 = 5;
    }
    if (a2 > 0.7) {
        v7 = 1;
        v8 = 3;
        if (a1 > 7.0)
            v5 = 3;
    }
    v10 = a2 - (log((double)(n ? n : 1)) / log(2.0) - 17.0) * 0.1;
    if ((a1 < 4.0 && v10 > 0.6) ||
        (a1 >= 4.0 && (v10 - 0.2) * 10.0 > a1)) {
        v4 = 1;
        v5 = 0;
        v3 = 1;
        v7 = 5;
        v8 = 0;
        v6 = 6;
    }
    return ((unsigned long)v3 << 28) | ((unsigned long)v4 << 24) |
           ((unsigned long)v5 << 20) | ((unsigned long)v7 << 12) |
           ((unsigned long)v6 << 8) | ((unsigned long)v8 << 4);
}

int gca_frame_write(gca_buf* out, const unsigned char* in, size_t n)
{
    gca_buf lits, stream, bwt, s1;
    unsigned long prim = 0;
    unsigned long mode;
    unsigned long counts[256];
    size_t frame_len;
    size_t i;
    unsigned long same = 0;
    unsigned prev = 0;
    double a1 = 0.0, a2 = 0.0;
    int hdr;

    gca_buf_init(&lits);
    gca_buf_init(&stream);
    gca_buf_init(&bwt);
    gca_buf_init(&s1);

    if (gca_lz_encode(in, n, &lits, &stream) != 0) {
        gca_buf_free(&lits); gca_buf_free(&stream);
        gca_buf_free(&bwt); gca_buf_free(&s1);
        return -1;
    }
    if (gca_bwt_encode(lits.data, lits.len, &bwt, &prim) != 0) {
        gca_buf_free(&lits); gca_buf_free(&stream);
        gca_buf_free(&bwt); gca_buf_free(&s1);
        return -1;
    }

    for (i = 0; i < 256; i++)
        counts[i] = 0;
    for (i = 0; i < bwt.len; i++) {
        counts[bwt.data[i]]++;
        if (bwt.data[i] == prev)
            same++;
        prev = bwt.data[i];
    }
    if (bwt.len) {
        double sum = 0.0;
        for (i = 0; i < 256; i++) {
            double x = (double)counts[i] * 256.0 / (double)bwt.len - 1.0;
            sum += x * x;
        }
        a1 = sqrt(sum * 0.00392156862745098);
        a2 = (double)same / (double)bwt.len;
    }
    mode = gca_choose_mode(a1, a2, bwt.len);

    if (mode == 0x11005600UL) {
        hdr = 24;
        if (gca_stage1_encode(bwt.data, bwt.len, &s1) != 0) {
            gca_buf_free(&lits); gca_buf_free(&stream);
            gca_buf_free(&bwt); gca_buf_free(&s1);
            return -1;
        }
    } else {
        hdr = 28;
        if (gca_var_encode(bwt.data, bwt.len, &s1, mode) != 0) {
            gca_buf_free(&lits); gca_buf_free(&stream);
            gca_buf_free(&bwt); gca_buf_free(&s1);
            return -1;
        }
    }

    frame_len = (size_t)hdr + s1.len + stream.len;
    if (n <= 8 || frame_len >= n + 8) {
        /* compression did not pay off: store the block */
        gca_buf_puts(out, "GCF0");
        gca_buf_put32(out, (unsigned long)n);
        gca_buf_put(out, in, n);
    } else if (hdr == 24) {
        gca_buf_puts(out, "GCF1");
        gca_buf_put32(out, (unsigned long)n);          /* original size   */
        gca_buf_put32(out, (unsigned long)lits.len);   /* literal count   */
        gca_buf_put32(out, prim);                     /* BWT primary     */
        gca_buf_put32(out, (unsigned long)s1.len);    /* stage 1 length  */
        gca_buf_put32(out, (unsigned long)stream.len);/* LZ stream length*/
        gca_buf_put(out, s1.data, s1.len);
        gca_buf_put(out, stream.data, stream.len);
    } else {
        gca_buf_puts(out, "GCF2");
        gca_buf_put32(out, (unsigned long)n);          /* original size   */
        gca_buf_put32(out, (unsigned long)lits.len);   /* literal count   */
        gca_buf_put32(out, prim);                     /* BWT primary     */
        gca_buf_put32(out, (unsigned long)s1.len);    /* stage 1 length  */
        gca_buf_put32(out, (unsigned long)stream.len);/* LZ stream length*/
        gca_buf_put32(out, mode);                     /* coder selection */
        gca_buf_put(out, s1.data, s1.len);
        gca_buf_put(out, stream.data, stream.len);
    }

    gca_buf_free(&lits); gca_buf_free(&stream);
    gca_buf_free(&bwt); gca_buf_free(&s1);
    return 0;
}

int gca_frame_decode(const unsigned char* p, size_t avail, size_t* consumed,
                     size_t* orig, gca_buf* out)
{
    if (avail < 8)
        return -1;
    if (memcmp(p, "GCF0", 4) == 0) {
        unsigned long n = gca_rd32(p + 4);
        if (n > avail - 8)
            return -1;
        *consumed = 8 + n;
        *orig = n;
        return gca_buf_put(out, p + 8, n);
    }
    if (avail >= 24 && memcmp(p, "GCF1", 4) == 0) {
        unsigned long ex   = gca_rd32(p + 4);
        unsigned long lit  = gca_rd32(p + 8);
        unsigned long prim = gca_rd32(p + 12);
        unsigned long s1   = gca_rd32(p + 16);
        unsigned long s2   = gca_rd32(p + 20);
        gca_buf bwt, lits, dec;
        int rc = -1;

        if (ex > GCA_MAX_FRAME || lit > GCA_MAX_FRAME || lit > ex)
            return -1;
        if (lit && prim >= lit)
            return -1;
        if (s1 > avail - 24 || s2 > avail - 24 - s1)
            return -1;
        gca_buf_init(&bwt);
        gca_buf_init(&lits);
        gca_buf_init(&dec);

        if (gca_stage1_decode(p + 24, s1, lit, &bwt) == 0 &&
            bwt.len == lit &&
            gca_bwt_decode(bwt.data, bwt.len, prim, &lits) == 0 &&
            gca_lz_decode(p + 24 + s1, s2, lits.data, lits.len, ex, &dec) == 0 &&
            dec.len == ex) {
            gca_buf_put(out, dec.data, dec.len);
            *consumed = 24 + s1 + s2;
            *orig = ex;
            rc = 0;
        }
        gca_buf_free(&bwt);
        gca_buf_free(&lits);
        gca_buf_free(&dec);
        return rc;
    }
    if (avail >= 28 && memcmp(p, "GCF2", 4) == 0) {
        unsigned long ex   = gca_rd32(p + 4);
        unsigned long lit  = gca_rd32(p + 8);
        unsigned long prim = gca_rd32(p + 12);
        unsigned long s1   = gca_rd32(p + 16);
        unsigned long s2   = gca_rd32(p + 20);
        unsigned long mode = gca_rd32(p + 24);
        gca_buf bwt, lits, dec;
        int rc = -1;

        if (ex > GCA_MAX_FRAME || lit > GCA_MAX_FRAME || lit > ex)
            return -1;
        if (lit && prim >= lit)
            return -1;
        if (s1 > avail - 28 || s2 > avail - 28 - s1)
            return -1;
        gca_buf_init(&bwt);
        gca_buf_init(&lits);
        gca_buf_init(&dec);

        if (gca_var_decode(p + 28, s1, lit, mode, &bwt) == 0 &&
            bwt.len == lit &&
            gca_bwt_decode(bwt.data, bwt.len, prim, &lits) == 0) {
            /* the encoder may have noised the literal stream */
            if (mode & 0x80000000UL) {
                size_t x;
                for (x = 0; x < lits.len; x += 0x3FF)
                    lits.data[x] ^= (unsigned char)(x & 0xFF);
            }
            if (gca_lz_decode(p + 28 + s1, s2, lits.data, lits.len, ex,
                              &dec) == 0 && dec.len == ex) {
                gca_buf_put(out, dec.data, dec.len);
                *consumed = 28 + s1 + s2;
                *orig = ex;
                rc = 0;
            }
        }
        gca_buf_free(&bwt);
        gca_buf_free(&lits);
        gca_buf_free(&dec);
        return rc;
    }
    return -1;
}
