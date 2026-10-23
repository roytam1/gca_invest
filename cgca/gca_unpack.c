///////////////////////////////////////////////////////////////
// gca_unpack.c -- list or extract GCA archives.
//
//   gcac_unpack [options] <archive.gca> [extract folder]
//   gcac_unpack e [options] <archive.gca> [extract folder]
//   gcac_unpack l [options] <archive.gca>
//
//   -l         list only (same as the "l" command)
//   -o <dir>   output directory (default: the current directory)
//   -p <pw>    password for encryption (or -p<pw>)
//   -k <file>  key file (or -k<file>)
//
// The archive name follows gcac.exe: "foo" also finds "foo.gca".
///////////////////////////////////////////////////////////////
#include "gca.h"
#include <time.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

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
        "usage: gcac_unpack [options] <archive.gca> [extract folder]\n"
        "\n"
        "  e, l       optional gcac.exe style command (extract / list)\n"
        "  -l         list only\n"
        "  -o <dir>   output directory (default: the current directory)\n"
        "  -p <pw>    password for encryption (or -p<pw>)\n"
        "  -k <file>  key file (or -k<file>)\n");
}

static int gca_push_arg(const char*** args, size_t* n, size_t* cap,
                        const char* p)
{
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 8;
        const char** v = (const char**)realloc((void*)*args,
                                               sizeof(const char*) * c);
        if (!v)
            return -1;
        *args = v;
        *cap = c;
    }
    (*args)[(*n)++] = p;
    return 0;
}

static void gca_format_time(unsigned long lo, unsigned long hi, char* out)
{
#ifdef _WIN32
    FILETIME ft, lt;
    SYSTEMTIME st;
    ft.dwLowDateTime = lo;
    ft.dwHighDateTime = hi;
    if (FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &st))
        sprintf(out, "%04u/%02u/%02u %02u:%02u:%02u",
                (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    else
        strcpy(out, "                   ");
#else
    double secs = ((double)hi * 4294967296.0 + (double)lo)
                  / 10000000.0 - 11644473600.0;
    time_t t = (time_t)secs;
    struct tm* tm = localtime(&t);
    if (tm)
        strftime(out, 32, "%Y/%m/%d %H:%M:%S", tm);
    else
        strcpy(out, "                   ");
#endif
}

static void gca_print_list(gca_archive* arc)
{
    unsigned long i;
    printf("\nfilename                           date & time      "
           "original compressed   ratio\n");
    printf("----------------------------- ------------------- "
           "---------- ---------- -------\n");
    for (i = 0; i < arc->num_files; i++) {
        gca_member* m = &arc->members[i];
        char date[32];
        char disp[32];
        size_t nlen = strlen(m->name);
        unsigned long comp = arc->solid ? 0 : m->stored_size;
        double ratio = 0.0;
        if (nlen > 29) {
            strcpy(disp, "...");
            strcpy(disp + 3, m->name + nlen - 26);
        } else {
            strcpy(disp, m->name);
        }
        if (!arc->solid && comp != 0 && m->orig_size != 0 &&
            comp < m->orig_size)
            ratio = 100.0 * (1.0 - (double)comp / (double)m->orig_size);
        gca_format_time(m->ft_lo, m->ft_hi, date);
        printf("%-29s %19s %10lu %10lu %6.1f%%\n",
               disp, date, m->orig_size, comp, ratio);
    }
}

int main(int argc, char** argv)
{
    const char** pos = NULL;
    size_t npos = 0, cpos = 0;
    const char* archive = NULL;
    const char* outdir = NULL;
    const char* spell = NULL;
    char keyhex[41];
    char norm[1024];
    const char* openpath;
    gca_archive arc;
    unsigned long i;
    int list_only = 0;
    int cmd = 0;
    int rc = 0;
    int j;

    for (j = 1; j < argc; j++) {
        const char* a = argv[j];
        int isopt = 0;
        char opt = 0;
        const char* val = NULL;

        if (a[0] == '-' && a[1]) {
            isopt = 1;
            opt = a[1];
            if (a[2])
                val = a + 2;
        }
#ifdef _WIN32
        else if (a[0] == '/' && a[1] &&
                 strchr("lLoOpPkK", a[1]) != NULL) {
            isopt = 1;
            opt = a[1];
            if (a[2])
                val = a + 2;
        }
#endif
        if (isopt) {
            int need = 0;
            char lower = (opt >= 'A' && opt <= 'Z') ?
                         (char)(opt - 'A' + 'a') : opt;
            if (lower == 'o' || lower == 'p' || lower == 'k')
                need = 1;
            if (need && !val) {
                if (j + 1 >= argc) { gca_usage(); free((void*)pos); return 2; }
                val = argv[++j];
            }
            if (lower == 'l') {
                list_only = 1;
            } else if (lower == 'o') {
                outdir = val;
            } else if (lower == 'p') {
                spell = val;
            } else if (lower == 'k') {
                if (gca_load_keyfile(val, keyhex) != 0) {
                    fprintf(stderr, "cannot read key file %s\n", val);
                    free((void*)pos);
                    return 1;
                }
                spell = keyhex;
            } else {
                gca_usage();
                free((void*)pos);
                return 2;
            }
            continue;
        }
        if (gca_push_arg(&pos, &npos, &cpos, a) != 0) {
            fprintf(stderr, "out of memory\n");
            free((void*)pos);
            return 1;
        }
    }

    if (npos >= 1 && pos[0][0] && !pos[0][1]) {
        char c = pos[0][0];
        if (c == 'e' || c == 'E') cmd = 'e';
        else if (c == 'l' || c == 'L') { cmd = 'l'; list_only = 1; }
    }

    if (cmd) {
        if (npos < 2) {
            gca_usage();
            free((void*)pos);
            return 2;
        }
        archive = pos[1];
        if (!outdir && npos >= 3)
            outdir = pos[2];
    } else {
        if (npos < 1) {
            gca_usage();
            free((void*)pos);
            return 2;
        }
        archive = pos[0];
        if (!outdir && npos >= 2)
            outdir = pos[1];
    }
    free((void*)pos);

    openpath = archive;
    if (gca_arc_open(openpath, spell, &arc) != 0) {
        /* gcac.exe replaces the extension with .gca; fall back to that
           when the name as given is a folder or does not exist. */
        if (gca_archive_path(archive, norm, sizeof(norm)) == 0 &&
            strcmp(norm, archive) != 0 && gca_path_exists(norm) &&
            gca_arc_open(norm, spell, &arc) == 0) {
            openpath = norm;
        }
    }
    if (arc.image == NULL) {
        if (arc.crc_error) {
            fprintf(stderr, "CRC Error\n");
        } else if (arc.legacy_encrypted) {
            fprintf(stderr,
                "%s is an encrypted legacy GCA0/GCA1 container.\n"
                "Use -p or -k to decode it.\n",
                archive);
        } else if (arc.legacy) {
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

    if (list_only) {
        gca_print_list(&arc);
        gca_arc_close(&arc);
        return 0;
    }

    for (i = 0; i < arc.num_files; i++) {
        printf("  extracting %s\n", arc.members[i].name);
        if (gca_arc_extract(&arc, i, outdir) != 0) {
            fprintf(stderr, "cannot extract %s\n", arc.members[i].name);
            rc = 1;
        }
    }
    gca_arc_close(&arc);
    return rc;
}
