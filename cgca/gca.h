/////////////////////////////////////////////////////////////////
// gca.h -- GCA archive format, portable C implementation.
//
// A clean-room reimplementation of the container and codec that
// gcac.exe (GCA GCompressionArchiver v0.9k, SYN 2000/2001) reads and
// writes.  See GCA_FORMAT.md for the format description.
//
// The code is C89 and compiles with Visual C++ 6 as well as with any
// modern C compiler.  No 64 bit integers are used: every 64 bit field
// of the format is carried as two 32 bit halves.
/////////////////////////////////////////////////////////////////
#ifndef GCA_H
#define GCA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* growable byte buffer                                             */
typedef struct {
    unsigned char* data;
    size_t len;
    size_t cap;
} gca_buf;

void gca_buf_init(gca_buf* b);
void gca_buf_free(gca_buf* b);
int  gca_buf_reserve(gca_buf* b, size_t extra);
int  gca_buf_put(gca_buf* b, const void* p, size_t n);
int  gca_buf_putc(gca_buf* b, int c);
int  gca_buf_puts(gca_buf* b, const char* s);
int  gca_buf_put32(gca_buf* b, unsigned long v);
int  gca_buf_put64(gca_buf* b, unsigned long lo, unsigned long hi);
int  gca_buf_set(gca_buf* b, size_t off, unsigned long v);

/* ---------------------------------------------------------------- */
/* little endian accessors                                          */
unsigned long gca_rd32(const unsigned char* p);
unsigned long gca_rd16(const unsigned char* p);
void         gca_wr32(unsigned char* p, unsigned long v);
void         gca_wr16(unsigned char* p, unsigned long v);

/* ---------------------------------------------------------------- */
/* CRC-32 (zlib) and SHA-1                                          */
unsigned long gca_crc32(const unsigned char* data, size_t len,
                        unsigned long seed);
void gca_sha1(const unsigned char* data, size_t len,
              unsigned char digest[20]);
void gca_sha1_hex(const unsigned char* data, size_t len, char* out41);

/* ---------------------------------------------------------------- */
/* the "GflGCACryptograph" keystream                                */
typedef struct {
    unsigned long state[624];
    int          idx;
    unsigned long word;
    unsigned long counter;
} gca_crypt;

void gca_crypt_setkey(gca_crypt* c, const unsigned char* key, size_t len);
void gca_crypt_xor(gca_crypt* c, unsigned char* buf, size_t len);

/* key strings (see GCA_FORMAT.md 6.1) */
int gca_key_payload(const char* spell, const char* name, gca_buf* out);
int gca_key_dir(const char* spell, gca_buf* out);
int gca_key_solid(const char* spell, gca_buf* out);

/* ---------------------------------------------------------------- */
/* the codec                                                         */

/* range decoder (gcac.exe sub_411740/411760/411790) */
typedef struct {
    const unsigned char* data;
    size_t len;
    size_t pos;
    unsigned long code;
    unsigned long range;
    unsigned long buf;
} gca_rc_dec;

void gca_rc_dec_init(gca_rc_dec* d, const unsigned char* data, size_t len,
                     size_t pos);
void gca_rc_dec_normalize(gca_rc_dec* d);
int  gca_rc_dec_bit(gca_rc_dec* d, unsigned long prob, unsigned long* bound);
unsigned long gca_rc_dec_tree(gca_rc_dec* d, unsigned long* model, int node,
                              int nbits, int shift);
unsigned long gca_rc_dec_number(gca_rc_dec* d, unsigned long* model, int nbits,
                                int shift);

/* range encoder (gcac.exe sub_411770/411B80/411800/411CA0/412000) */
typedef struct {
    gca_buf* out;
    unsigned long code;
    unsigned long range;
    unsigned long cache;
    int          cache_size;
} gca_rc_enc;

void gca_rc_enc_init(gca_rc_enc* e, gca_buf* out);
void gca_rc_enc_normalize(gca_rc_enc* e);
void gca_rc_enc_bit(gca_rc_enc* e, unsigned long prob, int bit);
void gca_rc_enc_rawbits(gca_rc_enc* e, int nbits, unsigned long value);
void gca_rc_enc_tree(gca_rc_enc* e, unsigned long* model, int node,
                     unsigned long value, int nbits, int shift);
void gca_rc_enc_number(gca_rc_enc* e, unsigned long* model, unsigned long value,
                       int nbits, int shift);
void gca_rc_enc_flush(gca_rc_enc* e);

/* stage 1: MTF + run length (gcac.exe sub_40FC20 / sub_40FE20) */
int gca_stage1_encode(const unsigned char* in, size_t n, gca_buf* out);
int gca_stage1_decode(const unsigned char* in, size_t len, size_t total,
                       gca_buf* out);

/* stage 1 of a GCF2 frame: the MTF/RLE stage with the mode selected
   coders (gcac.exe sub_40F460 / sub_40F8E0).  The mode word is
   documented in GCA_FORMAT.md. */
int gca_var_decode(const unsigned char* in, size_t len, size_t total,
                   unsigned long mode, gca_buf* out);

/* Burrows-Wheeler transform (gcac.exe sub_40EEC0 / sub_40F1F0) */
int gca_bwt_encode(const unsigned char* in, size_t n, gca_buf* out,
                   unsigned long* prim);
int gca_bwt_decode(const unsigned char* in, size_t n, unsigned long prim,
                   gca_buf* out);

/* LZ stage (gcac.exe sub_40FFD0 / sub_4101B0) */
int gca_lz_encode(const unsigned char* in, size_t n, gca_buf* lits,
                  gca_buf* stream);
int gca_lz_decode(const unsigned char* stream, size_t len,
                  const unsigned char* lits, size_t litlen, size_t total,
                  gca_buf* out);

/* ---------------------------------------------------------------- */
/* frames                                                            */
#define GCA_FRAME_GCF0 0
#define GCA_FRAME_GCF1 1

/* Build one frame for `n` bytes of `in` and append it to `out`.
   A GCF0 (stored) frame is used when compression does not pay off. */
int gca_frame_write(gca_buf* out, const unsigned char* in, size_t n);

/* Decode the frame at `p`.  *consumed receives the frame length,
   *orig receives the expanded size.  Returns 0 on success. */
int gca_frame_decode(const unsigned char* p, size_t avail, size_t* consumed,
                     size_t* orig, gca_buf* out);

/* ---------------------------------------------------------------- */
/* the container                                                     */
#define GCA_FLAG_DIRCOMPRESSED 0x001u
#define GCA_FLAG_DIRENCRYPTED  0x002u
#define GCA_FLAG_PAYCOMPRESSED 0x010u
#define GCA_FLAG_CRYPTO        0x020u
#define GCA_FLAG_SOLID         0x100u

typedef struct {
    char*          name;        /* stored name */
    unsigned long  crc;         /* CRC-32 of the original data */
    unsigned long  attr;        /* Win32 file attributes */
    unsigned long  ft_lo;       /* FILETIME low  */
    unsigned long  ft_hi;       /* FILETIME high */
    unsigned long  orig_size;   /* original size */
    unsigned long  stored_size; /* bytes in the payload region */
    unsigned long  data_off;    /* offset of the payload */
} gca_member;

typedef struct {
    unsigned char* image;       /* the whole archive file */
    size_t         image_len;
    unsigned char* plain;       /* decrypted copy, when encrypted */
    size_t         plain_len;
    unsigned long  flags;
    int            solid;
    /* the payload is stored verbatim instead of being framed */
    int            rawPayload;
    /* 0 = the modern GCAX container, 1 = a legacy container
       (GCA0 / GCA1 / a standalone GCA3) which is not supported */
    int            legacy;
    unsigned long  num_files;
    gca_member*    members;
    size_t         payload_off; /* start of the payload region */
    size_t         payload_len;
} gca_archive;

/* Read and parse an archive.  `spell` may be NULL for plain archives.
   Returns 0 on success. */
int  gca_arc_open(const char* path, const char* spell, gca_archive* arc);
void gca_arc_close(gca_archive* arc);

/* Extract member `index` into `outdir` (a directory path). */
int  gca_arc_extract(gca_archive* arc, unsigned long index, const char* outdir);

/* ---------------------------------------------------------------- */
/* packing                                                           */
typedef struct {
    char*          name;      /* the name as stored in the archive */
    unsigned char* data;      /* the file content (owned) */
    size_t         len;
    unsigned long  attr;
    unsigned long  ft_lo;
    unsigned long  ft_hi;
    unsigned long  crc;       /* CRC-32 of the content */
} gca_src;

typedef struct {
    int          solid;        /* concatenate all members into one stream */
    int          compress;     /* 0 = store, 1 = compress */
    int          dir_compress; /* compress the directory block */
    const char*  spell;        /* password, or NULL */
    gca_src*     srcs;
    size_t       nsrc;
    size_t       cap;
} gca_writer;

void gca_writer_init(gca_writer* w, int solid, int compress,
                     int dir_compress, const char* spell);
void gca_writer_free(gca_writer* w);

/* Add one member.  `data` is copied. */
int gca_pack_add(gca_writer* w, const char* name,
                 const unsigned char* data, size_t len, unsigned long attr,
                 unsigned long ft_lo, unsigned long ft_hi);

/* Finish the archive and write it to `path`. */
int gca_pack_finish(gca_writer* w, const char* path);

#endif /* GCA_H */
