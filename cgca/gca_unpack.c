/////////////////////////////////////////////////////////////////
// gca_unpack.c -- extract GCA archives.
//
//   gcac_unpack [options] <archive.gca>
//
//   -o <dir>  output directory (default: the current directory)
//   -p <pw>   password for encryption
//   -k <file> key file: the password becomes hex(SHA1(file))
//
// Also accepts an archive that is embedded in a larger file (an SFX
// stub): the GCAX header is located automatically.
/////////////////////////////////////////////////////////////////
#include "gca.h"

static int gca_read_content(const char* path, unsigned char** data, size_t* len)
{
    FILE* fp;
    long n;
    unsigned char* buf;
    *data = NULL;
    *len = 0;
    fp = fopen(path, "rb");
    if (!fp)
        return -1;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return -1; }
    buf = (unsigned char*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(fp); return -1; }
    if (n && fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    *data = buf;
    *len = (size_t)n;
    return 0;
}

static int gca_load_keyfile(const char* path, char* out41)
{
    unsigned char* data = NULL;
    size_t len = 0;
    if (gca_read_content(path, &data, &len) != 0)
        return -1;
    gca_sha1_hex(data, len, out41);
    free(data);
    return 0;
}

static void gca_usage(void)
{
    fprintf(stderr,
        "usage: gcac_unpack [options] <archive.gca>\n"
        "\n"
        "  -o <dir>   output directory (default: the current directory)\n"
        "  -p <pw>    password for encryption\n"
        "  -k <file>  key file: the password becomes hex(SHA1(file))\n");
}

int main(int argc, char** argv)
{
    const char* archive = NULL;
    const char* outdir = NULL;
    const char* spell = NULL;
    char keyhex[41];
    gca_archive arc;
    unsigned long i;
    int rc = 0;

    for (i = 1; (int)i < argc; i++) {
        const char* a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (strcmp(a, "-o") == 0) {
                if (++i < (unsigned long)argc) outdir = argv[i];
                continue;
            }
            if (strcmp(a, "-p") == 0) {
                if (++i < (unsigned long)argc) spell = argv[i];
                continue;
            }
            if (strcmp(a, "-k") == 0) {
                if (++i >= (unsigned long)argc) { gca_usage(); return 2; }
                if (gca_load_keyfile(argv[i], keyhex) != 0) {
                    fprintf(stderr, "cannot read key file %s\n", argv[i]);
                    return 1;
                }
                spell = keyhex;
                continue;
            }
            gca_usage();
            return 2;
        }
        if (!archive)
            archive = a;
    }
    if (!archive) {
        gca_usage();
        return 2;
    }

    if (gca_arc_open(archive, spell, &arc) != 0) {
        if (arc.legacy) {
            fprintf(stderr,
                "%s is a standalone GCA3 container.\n"
                "That variant has no GCAX envelope and is not supported.\n",
                archive);
        } else {
            fprintf(stderr, "cannot open %s%s\n", archive,
                    (spell || "") ? "" : " (encrypted archives need -p or -k)");
        }
        return 1;
    }

    printf("%s: %u member%s, flags 0x%lx\n", archive, arc.num_files,
           arc.num_files == 1 ? "" : "s", arc.flags);
    for (i = 0; i < arc.num_files; i++) {
        gca_member* m = &arc.members[i];
        printf("  %-40s %10lu -> %10lu  crc %08lx\n",
               m->name, m->orig_size, m->stored_size, m->crc);
    }

    for (i = 0; i < arc.num_files; i++) {
        if (gca_arc_extract(&arc, i, outdir) != 0) {
            fprintf(stderr, "cannot extract %s\n", arc.members[i].name);
            rc = 1;
        }
    }
    gca_arc_close(&arc);
    return rc;
}
