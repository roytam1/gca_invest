/////////////////////////////////////////////////////////////////
// gca_arc.c -- the GCA container: reader, writer and packer.
//
// Container layout (see GCA_FORMAT.md):
//
//   "GCAX" | u32 0 | u64 dirOffset(relative) | payload | GCA3 dir
//
/////////////////////////////////////////////////////////////////
#include "gca.h"
#include <sys/stat.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#define GCA_BLOCK_SIZE 0x100000UL   /* the 1 MiB chunk size */

/* ---------------------------------------------------------------- */
/* helpers                                                           */
static int gca_read_file(const char* path, unsigned char** out, size_t* outlen)
{
    FILE* fp;
    long len;
    unsigned char* buf;
    *out = NULL;
    *outlen = 0;
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len < 0) { fclose(fp); return -1; }
    buf = (unsigned char*)malloc((size_t)len ? (size_t)len : 1);
    if (!buf) { fclose(fp); return -1; }
    if (len && fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *out = buf;
    *outlen = (size_t)len;
    return 0;
}

/* Find the GCAX header.  Returns the offset or -1. */
static long gca_find_gcax(const unsigned char* image, size_t len)
{
    size_t i;
    size_t limit = len < (1u << 20) ? len : (size_t)(1u << 20);
    if (len < 16)
        return -1;
    for (i = 0; i + 16 <= limit; i++) {
        if (memcmp(image + i, "GCAX", 4) == 0)
            return (long)i;
    }
    return -1;
}

/* ---------------------------------------------------------------- */
/* directory parsing                                                 */
/* Read the entry table and the names pool out of a directory body.
 *
 * The body is either
 *      entries (numFiles*32) | names
 * or, when the directory is compressed,
 *      u32 rawSize | u32 0 | GCF1(entries) | GCF1(names)
 */
static int gca_read_dir_body(const unsigned char* body, size_t bodyLen,
                             unsigned long flags, unsigned long numFiles,
                             gca_member* members)
{
    gca_buf ents, names;
    size_t bodyBase = (flags & GCA_FLAG_DIRCOMPRESSED) ? 8 : 0;
    size_t p, i;

    gca_buf_init(&ents);
    gca_buf_init(&names);

    if (flags & GCA_FLAG_DIRCOMPRESSED) {
        size_t consumed, orig;
        if (bodyLen < bodyBase + 24)
            goto fail;
        if (gca_frame_decode(body + bodyBase, bodyLen - bodyBase,
                             &consumed, &orig, &ents) != 0)
            goto fail;
        if (ents.len != (size_t)(numFiles * 32))
            goto fail;
        p = bodyBase + consumed;
        if (p + 24 > bodyLen)
            goto fail;
        if (gca_frame_decode(body + p, bodyLen - p, &consumed, &orig,
                             &names) != 0)
            goto fail;
    } else {
        if (bodyLen < (size_t)(numFiles * 32))
            goto fail;
        gca_buf_put(&ents, body, (size_t)(numFiles * 32));
        gca_buf_put(&names, body + numFiles * 32, bodyLen - numFiles * 32);
    }

    for (i = 0; i < numFiles; i++) {
        const unsigned char* e = ents.data + i * 32;
        members[i].crc        = gca_rd32(e + 0x00);
        members[i].attr       = gca_rd32(e + 0x04);
        members[i].ft_lo      = gca_rd32(e + 0x08);
        members[i].ft_hi      = gca_rd32(e + 0x0C);
        members[i].orig_size  = gca_rd32(e + 0x10);
        members[i].stored_size = gca_rd32(e + 0x18);
        members[i].data_off   = 0;
    }

    /* the names pool: u16 length + the raw bytes */
    p = 0;
    for (i = 0; i < numFiles; i++) {
        unsigned long n;
        if (p + 2 > names.len)
            goto fail;
        n = gca_rd16(names.data + p);
        p += 2;
        if (n == 0 || p + n > names.len)
            goto fail;
        members[i].name = (char*)malloc(n + 1);
        if (!members[i].name)
            goto fail;
        memcpy(members[i].name, names.data + p, n);
        members[i].name[n] = '\0';
        p += n;
    }

    gca_buf_free(&ents);
    gca_buf_free(&names);
    return 0;

fail:
    gca_buf_free(&ents);
    gca_buf_free(&names);
    return -1;
}

/* ---------------------------------------------------------------- */
/* reader                                                            */
void gca_arc_close(gca_archive* arc)
{
    size_t i;
    if (arc->members) {
        for (i = 0; i < arc->num_files; i++) {
            if (arc->members[i].name)
                free(arc->members[i].name);
        }
        free(arc->members);
        arc->members = NULL;
    }
    if (arc->image) free(arc->image);
    if (arc->plain) free(arc->plain);
    arc->image = NULL;
    arc->plain = NULL;
    arc->num_files = 0;
}

/* Decrypt an encrypted archive into arc->plain.  The directory is
 * decrypted first (it carries the member names), then the payload. */
static int gca_arc_decrypt(gca_archive* arc, const char* spell,
                           long start, size_t dirAbs)
{
    const unsigned char* image = arc->image;
    unsigned char* plain;
    unsigned long flags, blockSize, numFiles;
    size_t bodyLen, payloadOff, payloadLen, off, i;
    gca_buf key, body;
    gca_crypt ks;
    gca_member* members = NULL;

    flags     = gca_rd32(image + dirAbs + 4);
    blockSize = gca_rd32(image + dirAbs + 8);
    numFiles  = gca_rd32(image + dirAbs + 0x18);
    if (blockSize < 0x20)
        return -1;
    bodyLen = blockSize - 0x20;
    if (dirAbs + 0x20 + bodyLen > arc->image_len)
        return -1;

    plain = (unsigned char*)malloc(arc->image_len);
    if (!plain)
        return -1;
    memcpy(plain, image, arc->image_len);

    /* the directory: keystream bytes 0..15 then 16.. */
    gca_buf_init(&key);
    if (gca_key_dir(spell, &key) != 0) { free(plain); gca_buf_free(&key); return -1; }
    gca_crypt_setkey(&ks, key.data, key.len);
    gca_crypt_xor(&ks, plain + dirAbs + 0x10, 16);

    gca_buf_init(&body);
    gca_buf_put(&body, image + dirAbs + 0x20, bodyLen);
    gca_crypt_xor(&ks, body.data, body.len);
    memcpy(plain + dirAbs + 0x20, body.data, body.len);
    gca_buf_free(&key);
    gca_buf_free(&body);

    /* clear the crypto bits so the plain parser does not try again */
    {
        unsigned long pf = flags & ~(unsigned long)(GCA_FLAG_CRYPTO |
                                                   GCA_FLAG_DIRENCRYPTED);
        gca_wr32(plain + dirAbs + 4, pf);
    }
    numFiles = gca_rd32(plain + dirAbs + 0x18);
    if (numFiles == 0 || numFiles > 100000) { free(plain); return -1; }

    /* the entries and names come from the decrypted body */
    members = (gca_member*)calloc(numFiles, sizeof(gca_member));
    if (!members) { free(plain); return -1; }
    {
        size_t bodyBase = (flags & GCA_FLAG_DIRCOMPRESSED) ? 8 : 0;
        gca_buf ents, names;
        size_t p, k;
        gca_buf_init(&ents);
        gca_buf_init(&names);
        if (flags & GCA_FLAG_DIRCOMPRESSED) {
            size_t consumed, orig;
            if (gca_frame_decode(plain + dirAbs + 0x20 + bodyBase,
                                 bodyLen - bodyBase, &consumed, &orig,
                                 &ents) != 0)
                goto fail;
            p = bodyBase + consumed;
            if (gca_frame_decode(plain + dirAbs + 0x20 + p, bodyLen - p,
                                 &consumed, &orig, &names) != 0)
                goto fail;
        } else {
            if (bodyLen < (size_t)(numFiles * 32))
                goto fail;
            gca_buf_put(&ents, plain + dirAbs + 0x20,
                        (size_t)(numFiles * 32));
            gca_buf_put(&names, plain + dirAbs + 0x20 + numFiles * 32,
                        bodyLen - numFiles * 32);
        }
        for (k = 0; k < numFiles; k++) {
            const unsigned char* e = ents.data + k * 32;
            members[k].crc         = gca_rd32(e + 0x00);
            members[k].attr        = gca_rd32(e + 0x04);
            members[k].ft_lo       = gca_rd32(e + 0x08);
            members[k].ft_hi       = gca_rd32(e + 0x0C);
            members[k].orig_size   = gca_rd32(e + 0x10);
            members[k].stored_size = gca_rd32(e + 0x18);
        }
        p = 0;
        for (k = 0; k < numFiles; k++) {
            unsigned long n;
            if (p + 2 > names.len) goto fail;
            n = gca_rd16(names.data + p);
            p += 2;
            if (n == 0 || p + n > names.len) goto fail;
            members[k].name = (char*)malloc(n + 1);
            if (!members[k].name) goto fail;
            memcpy(members[k].name, names.data + p, n);
            members[k].name[n] = '\0';
            p += n;
        }
        gca_buf_free(&ents);
        gca_buf_free(&names);
    }

    /* the payload */
    payloadOff = (size_t)start + 0x10;
    if (dirAbs < payloadOff) goto fail;
    payloadLen = dirAbs - payloadOff;
    if (flags & GCA_FLAG_SOLID) {
        gca_buf_init(&key);
        if (gca_key_solid(spell, &key) != 0) goto fail;
        gca_crypt_setkey(&ks, key.data, key.len);
        gca_crypt_xor(&ks, plain + payloadOff, payloadLen);
        gca_buf_free(&key);
    } else {
        off = payloadOff;
        for (i = 0; i < numFiles; i++) {
            unsigned long stored = members[i].stored_size;
            gca_buf_init(&key);
            if (gca_key_payload(spell, members[i].name, &key) != 0) goto fail;
            gca_crypt_setkey(&ks, key.data, key.len);
            if (stored) {
                if (off + stored > dirAbs) goto fail;
                gca_crypt_xor(&ks, plain + off, stored);
            }
            off += stored;
            gca_buf_free(&key);
        }
    }

    /* hand the members over */
    if (arc->members)
        gca_arc_close(arc);
    arc->plain = plain;
    arc->plain_len = arc->image_len;
    arc->members = members;
    arc->num_files = numFiles;
    arc->flags = flags;
    arc->solid = (flags & GCA_FLAG_SOLID) ? 1 : 0;
    return 0;

fail:
    {
        size_t k;
        for (k = 0; k < numFiles; k++)
            if (members[k].name) free(members[k].name);
        free(members);
    }
    free(plain);
    return -1;
}

int gca_arc_open(const char* path, const char* spell, gca_archive* arc)
{
    unsigned char* image = NULL;
    size_t image_len = 0;
    long start;
    size_t dirAbs, bodyLen, payloadOff, payloadLen, pos;
    unsigned long flags, blockSize, numFiles;
    const unsigned char* view;
    size_t view_len;

    memset(arc, 0, sizeof(*arc));
    if (gca_read_file(path, &image, &image_len) != 0)
        return -1;
    arc->image = image;
    arc->image_len = image_len;

    start = gca_find_gcax(image, image_len);
    if (start < 0)
        goto fail;
    if (gca_rd32(image + start + 4) != 0)
        goto fail;                       /* the reader requires +4 == 0 */
    if (gca_rd32(image + start + 12) != 0)
        goto fail;                       /* dirOffset must fit in 32 bits */
    {
        unsigned long dirOff = gca_rd32(image + start + 8);
        if (dirOff < 16)
            goto fail;
        dirAbs = (size_t)start + dirOff;
    }
    if (dirAbs + 0x20 > image_len)
        goto fail;
    if (memcmp(image + dirAbs, "GCA3", 4) != 0)
        goto fail;

    flags     = gca_rd32(image + dirAbs + 4);
    blockSize = gca_rd32(image + dirAbs + 8);
    if (blockSize < 0x20)
        goto fail;
    bodyLen = blockSize - 0x20;
    if (dirAbs + 0x20 + bodyLen > image_len)
        goto fail;

    if (flags & (GCA_FLAG_CRYPTO | GCA_FLAG_DIRENCRYPTED)) {
        if (!spell || !*spell)
            goto fail;                   /* no password */
        if (gca_arc_decrypt(arc, spell, start, dirAbs) != 0)
            goto fail;
        view = arc->plain;
        view_len = arc->plain_len;
        flags = gca_rd32(view + dirAbs + 4);
    } else {
        view = image;
        view_len = image_len;
    }

    numFiles = gca_rd32(view + dirAbs + 0x18);
    if (numFiles == 0 || numFiles > 100000)
        goto fail;
    arc->members = (gca_member*)calloc(numFiles, sizeof(gca_member));
    if (!arc->members)
        goto fail;
    if (gca_read_dir_body(view + dirAbs + 0x20, bodyLen, flags, numFiles,
                          arc->members) != 0)
        goto fail;
    arc->num_files = numFiles;
    arc->flags = flags;
    arc->solid = (flags & GCA_FLAG_SOLID) ? 1 : 0;

    /* the payload offsets */
    payloadOff = (size_t)start + 0x10;
    if (dirAbs < payloadOff)
        goto fail;
    payloadLen = dirAbs - payloadOff;
    arc->payload_off = payloadOff;
    arc->payload_len = payloadLen;
    pos = payloadOff;
    if (arc->solid) {
        size_t i;
        for (i = 0; i < numFiles; i++) {
            arc->members[i].data_off = (unsigned long)pos;
            arc->members[i].stored_size = (unsigned long)payloadLen;
        }
    } else {
        size_t i;
        for (i = 0; i < numFiles; i++) {
            arc->members[i].data_off = (unsigned long)pos;
            pos += arc->members[i].stored_size;
            if (pos > dirAbs)
                goto fail;
        }
    }
    return 0;

fail:
    gca_arc_close(arc);
    return -1;
}

/* ---------------------------------------------------------------- */
/* extraction                                                        */
static int gca_mkpath(const char* path)
{
    char tmp[1024];
    char* p;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char sv = *p;
            *p = 0;
#ifdef _WIN32
            CreateDirectoryA(tmp, NULL);
#else
            mkdir(tmp, 0777);
#endif
            *p = sv;
        }
    }
#ifdef _WIN32
    CreateDirectoryA(tmp, NULL);
#else
    mkdir(tmp, 0777);
#endif
    return 0;
}

static int gca_write_file(const char* path, const unsigned char* data, size_t len)
{
    FILE* fp = fopen(path, "wb");
    if (!fp)
        return -1;
    if (len && fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

int gca_arc_extract(gca_archive* arc, unsigned long index, const char* outdir)
{
    gca_member* m;
    const unsigned char* view;
    size_t view_len;
    gca_buf out;
    char path[1024];
    unsigned long crc;

    if (index >= arc->num_files)
        return -1;
    m = &arc->members[index];
    view = arc->plain ? arc->plain : arc->image;
    view_len = arc->plain ? arc->plain_len : arc->image_len;
    (void)view_len;

    if (outdir && *outdir) {
        size_t n = strlen(outdir);
        if (n + 1 + strlen(m->name) + 1 >= sizeof(path))
            return -1;
        strcpy(path, outdir);
        if (path[n - 1] != '\\' && path[n - 1] != '/')
            strcat(path, "\\");
        strcat(path, m->name);
    } else {
        if (strlen(m->name) + 1 >= sizeof(path))
            return -1;
        strcpy(path, m->name);
    }
    /* create the directories */
    {
        char tmp[1024];
        char* q;
        strncpy(tmp, path, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = 0;
        q = strrchr(tmp, '\\');
        if (!q) q = strrchr(tmp, '/');
        if (q) { *q = 0; gca_mkpath(tmp); }
    }

    gca_buf_init(&out);
    if (arc->solid) {
        /* solid archives share one stream: decode it and slice */
        gca_buf whole;
        size_t pos = 0, i;
        gca_buf_init(&whole);
        {
            gca_member* f = &arc->members[0];
            size_t consumed, orig;
            size_t p = f->data_off;
            size_t left = arc->payload_len;
            while (left > 0) {
                if (gca_frame_decode(view + p, left, &consumed, &orig,
                                     &whole) != 0)
                    break;
                p += consumed;
                left -= consumed;
            }
        }
        for (i = 0; i < index; i++)
            pos += arc->members[i].orig_size;
        if (pos + m->orig_size > whole.len) {
            gca_buf_free(&whole);
            gca_buf_free(&out);
            return -1;
        }
        gca_buf_put(&out, whole.data + pos, m->orig_size);
        gca_buf_free(&whole);
    } else {
        size_t p = m->data_off;
        size_t left = m->stored_size;
        unsigned long produced = 0;
        if (!(arc->flags & GCA_FLAG_PAYCOMPRESSED)) {
            /* the "no compression" mode stores the member verbatim */
            if (left > arc->image_len - p)
                left = arc->image_len - p;
            gca_buf_put(&out, view + p, left);
        } else {
            while (left > 0 && produced < m->orig_size) {
                size_t consumed, orig;
                if (gca_frame_decode(view + p, left, &consumed, &orig,
                                     &out) != 0)
                    break;
                p += consumed;
                left -= consumed;
                produced += (unsigned long)orig;
            }
        }
        if (out.len != m->orig_size) {
            gca_buf_free(&out);
            return -1;
        }
    }

    crc = gca_crc32(out.data, out.len, 0);
    if (crc != m->crc) {
        fprintf(stderr, "warning: CRC mismatch for %s (%08lx != %08lx)\n",
                m->name, crc, m->crc);
    }
    if (gca_write_file(path, out.data, out.len) != 0) {
        gca_buf_free(&out);
        return -1;
    }
    gca_buf_free(&out);
    return 0;
}

/* ---------------------------------------------------------------- */
/* writer                                                            */
void gca_writer_init(gca_writer* w, int solid, int compress,
                     int dir_compress, const char* spell)
{
    memset(w, 0, sizeof(*w));
    w->solid = solid;
    w->compress = compress;
    w->dir_compress = dir_compress;
    w->spell = spell;
}

void gca_writer_free(gca_writer* w)
{
    size_t i;
    for (i = 0; i < w->nsrc; i++) {
        if (w->srcs[i].name) free(w->srcs[i].name);
        if (w->srcs[i].data) free(w->srcs[i].data);
    }
    if (w->srcs) free(w->srcs);
    memset(w, 0, sizeof(*w));
}

int gca_pack_add(gca_writer* w, const char* name,
                 const unsigned char* data, size_t len, unsigned long attr,
                 unsigned long ft_lo, unsigned long ft_hi)
{
    gca_src* s;
    size_t n = strlen(name);
    if (w->nsrc == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 8;
        gca_src* p = (gca_src*)realloc(w->srcs, sizeof(gca_src) * cap);
        if (!p)
            return -1;
        w->srcs = p;
        w->cap = cap;
    }
    s = &w->srcs[w->nsrc++];
    memset(s, 0, sizeof(*s));
    s->name = (char*)malloc(n + 1);
    if (!s->name)
        return -1;
    memcpy(s->name, name, n + 1);
    s->data = (unsigned char*)malloc(len ? len : 1);
    if (!s->data)
        return -1;
    if (len) memcpy(s->data, data, len);
    s->len = len;
    s->attr = attr;
    s->ft_lo = ft_lo;
    s->ft_hi = ft_hi;
    s->crc = gca_crc32(data, len, 0);
    return 0;
}

/* Build the payload region and the directory, then write the file. */
int gca_pack_finish(gca_writer* w, const char* path)
{
    gca_buf payload, entries, names, body;
    unsigned long flags = 0;
    unsigned long* stored = NULL;
    size_t i;
    FILE* fp;
    unsigned char hdr[16], dhdr[32];
    unsigned long crc, numFiles = (unsigned long)w->nsrc;
    unsigned long blockSize;

    gca_buf_init(&payload);
    gca_buf_init(&entries);
    gca_buf_init(&names);
    gca_buf_init(&body);

    stored = (unsigned long*)calloc(numFiles ? numFiles : 1,
                                    sizeof(unsigned long));
    if (!stored)
        return -1;

    if (w->compress)
        flags |= GCA_FLAG_PAYCOMPRESSED;
    if (w->solid)
        flags |= GCA_FLAG_SOLID;
    /* the flags must be final before the directory CRC is computed: the
       CRC covers the header, and an encrypted archive carries the crypto
       bits in it */
    if (w->spell && *w->spell)
        flags |= GCA_FLAG_CRYPTO | GCA_FLAG_DIRENCRYPTED;

    if (w->solid) {
        /* concatenate every member and cut the result into 1 MiB blocks */
        gca_buf all;
        size_t pos = 0;
        gca_buf_init(&all);
        for (i = 0; i < w->nsrc; i++) {
            gca_buf_put(&all, w->srcs[i].data, w->srcs[i].len);
        }
        while (pos < all.len) {
            size_t n = all.len - pos;
            if (n > GCA_BLOCK_SIZE)
                n = GCA_BLOCK_SIZE;
            if (gca_frame_write(&payload, all.data + pos, n) != 0)
                goto fail;
            pos += n;
        }
        gca_buf_free(&all);
        /* solid members report a stored size of 0 */
        for (i = 0; i < w->nsrc; i++)
            stored[i] = 0;
    } else if (w->compress) {
        for (i = 0; i < w->nsrc; i++) {
            size_t pos = 0;
            size_t before = payload.len;
            while (pos < w->srcs[i].len) {
                size_t n = w->srcs[i].len - pos;
                if (n > GCA_BLOCK_SIZE)
                    n = GCA_BLOCK_SIZE;
                if (gca_frame_write(&payload, w->srcs[i].data + pos, n) != 0)
                    goto fail;
                pos += n;
            }
            stored[i] = (unsigned long)(payload.len - before);
        }
    } else {
        /* stored: the raw bytes, no framing at all */
        for (i = 0; i < w->nsrc; i++) {
            gca_buf_put(&payload, w->srcs[i].data, w->srcs[i].len);
            stored[i] = (unsigned long)w->srcs[i].len;
        }
        flags &= ~(unsigned long)GCA_FLAG_PAYCOMPRESSED;
    }

    /* the entry table */
    for (i = 0; i < w->nsrc; i++) {
        unsigned char e[32];
        memset(e, 0, sizeof(e));
        gca_wr32(e + 0x00, w->srcs[i].crc);
        gca_wr32(e + 0x04, w->srcs[i].attr);
        gca_wr32(e + 0x08, w->srcs[i].ft_lo);
        gca_wr32(e + 0x0C, w->srcs[i].ft_hi);
        gca_wr32(e + 0x10, (unsigned long)w->srcs[i].len);
        gca_wr32(e + 0x14, 0);
        gca_wr32(e + 0x18, stored[i]);
        gca_wr32(e + 0x1C, 0);
        gca_buf_put(&entries, e, 32);
    }
    /* the names pool */
    for (i = 0; i < w->nsrc; i++) {
        size_t n = strlen(w->srcs[i].name);
        unsigned char l[2];
        gca_wr16(l, (unsigned long)n);
        gca_buf_put(&names, l, 2);
        gca_buf_put(&names, w->srcs[i].name, n);
    }
    gca_buf_put(&body, entries.data, entries.len);
    gca_buf_put(&body, names.data, names.len);

    if (w->dir_compress) {
        /* u32 rawSize | u32 0 | GCF1(entries) | GCF1(names) */
        gca_buf cbody;
        gca_buf_init(&cbody);
        gca_buf_put32(&cbody, (unsigned long)body.len);
        gca_buf_put32(&cbody, 0);
        gca_frame_write(&cbody, entries.data, entries.len);
        gca_frame_write(&cbody, names.data, names.len);
        if (cbody.len < body.len) {
            gca_buf_free(&body);
            body = cbody;
            flags |= GCA_FLAG_DIRCOMPRESSED;
        } else {
            gca_buf_free(&cbody);
        }
    }

    /* the GCA3 header */
    memset(dhdr, 0, sizeof(dhdr));
    memcpy(dhdr, "GCA3", 4);
    blockSize = (unsigned long)(0x20 + body.len);
    gca_wr32(dhdr + 0x04, flags);
    gca_wr32(dhdr + 0x08, blockSize);
    gca_wr32(dhdr + 0x0C, 0);
    gca_wr32(dhdr + 0x10, 0);
    gca_wr32(dhdr + 0x14, 0);          /* the CRC placeholder */
    gca_wr32(dhdr + 0x18, numFiles);
    gca_wr32(dhdr + 0x1C, 0);

    /* the CRC covers the header with the CRC field and the 16 byte
       block zeroed (but the file count in place) plus the body */
    {
        unsigned char tmp[32];
        memcpy(tmp, dhdr, 32);
        gca_wr32(tmp + 0x14, 0);
        gca_wr32(tmp + 0x10, 0);
        gca_wr32(tmp + 0x1C, 0);
        crc = gca_crc32(tmp, 32, 0);
        crc = gca_crc32(body.data, body.len, crc);
    }
    gca_wr32(dhdr + 0x14, crc);

    /* encryption */
    if (w->spell && *w->spell) {
        gca_buf key;
        gca_crypt ks;

        if (w->solid) {
            gca_buf_init(&key);
            gca_key_solid(w->spell, &key);
            gca_crypt_setkey(&ks, key.data, key.len);
            gca_crypt_xor(&ks, payload.data, payload.len);
            gca_buf_free(&key);
        } else {
            size_t pos = 0;
            for (i = 0; i < w->nsrc; i++) {
                gca_buf_init(&key);
                gca_key_payload(w->spell, w->srcs[i].name, &key);
                gca_crypt_setkey(&ks, key.data, key.len);
                if (stored[i])
                    gca_crypt_xor(&ks, payload.data + pos, stored[i]);
                pos += stored[i];
                gca_buf_free(&key);
            }
        }

        /* the directory: 16 key bytes then the body */
        gca_buf_init(&key);
        gca_key_dir(w->spell, &key);
        gca_crypt_setkey(&ks, key.data, key.len);
        gca_crypt_xor(&ks, dhdr + 0x10, 16);
        gca_crypt_xor(&ks, body.data, body.len);
        gca_buf_free(&key);
    }

    /* write it out */
    fp = fopen(path, "wb");
    if (!fp)
        goto fail;
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "GCAX", 4);
    gca_wr32(hdr + 4, 0);
    gca_wr32(hdr + 8, 0);
    gca_wr32(hdr + 12, 0);
    if (fwrite(hdr, 1, 16, fp) != 16) { fclose(fp); goto fail; }
    if (payload.len && fwrite(payload.data, 1, payload.len, fp) != payload.len) {
        fclose(fp); goto fail;
    }
    if (fwrite(dhdr, 1, 32, fp) != 32) { fclose(fp); goto fail; }
    if (body.len && fwrite(body.data, 1, body.len, fp) != body.len) {
        fclose(fp); goto fail;
    }
    fclose(fp);

    /* patch dirOffset (relative to the GCAX header) */
    {
        unsigned long dirOff = 0x10 + (unsigned long)payload.len;
        FILE* f2 = fopen(path, "r+b");
        if (!f2)
            goto fail;
        if (fseek(f2, 8, SEEK_SET) != 0) { fclose(f2); goto fail; }
        gca_wr32(hdr + 8, dirOff);
        gca_wr32(hdr + 12, 0);
        if (fwrite(hdr + 8, 1, 8, f2) != 8) { fclose(f2); goto fail; }
        fclose(f2);
    }

    free(stored);
    gca_buf_free(&payload);
    gca_buf_free(&entries);
    gca_buf_free(&names);
    gca_buf_free(&body);
    return 0;

fail:
    free(stored);
    gca_buf_free(&payload);
    gca_buf_free(&entries);
    gca_buf_free(&names);
    gca_buf_free(&body);
    return -1;
}
