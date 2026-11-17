/* Minimal RAM-only MS_BIND helper for Beta custom shutter sound. Never touches Flash. */
#include "rt.h"

#define NR_MOUNT 21
#define MS_BIND 4096

struct bling_bind_pair {
    const char *src;
    const char *dst;
    long expected_bytes;
};

static const struct bling_bind_pair PAIRS[] = {
    { "/tmp_bt/a6400-bling-ka.pcm",      "/usr/share/avsystemsound/Dslr_Emnt_Shutter_Ka.bin",    640L },
    { "/tmp_bt/a6400-bling-sha.pcm",     "/usr/share/avsystemsound/Dslr_Emnt_Shutter_Sha.bin",   1728L },
    { "/tmp_bt/a6400-bling-kasha.pcm",   "/usr/share/avsystemsound/Dslr_Emnt_10FPS_Kasha.bin",   2368L },
    { "/tmp_bt/a6400-bling-shutter.pcm", "/usr/share/avsystemsound/Dslr_Emnt_WPS_PBC.bin",       29760L },
};

static int contains(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; ++p)
        if (strncmp(p, needle, n) == 0) return 1;
    return 0;
}

static long file_size(const char *path) {
    int fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long sz = sys_lseek(fd, 0, 2);
    sys_close(fd);
    return sz;
}

static int is_mounted(const char *mounts, const char *dst) {
    char pat[128];
    fmt(pat, sizeof pat, " %s ", dst);
    return contains(mounts, pat);
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    static char mounts[32768];
    int len = read_file("/proc/mounts", mounts, sizeof mounts);
    if (len <= 0 || len >= (int)sizeof mounts - 1) return 1;
    mounts[len] = 0;

    int failed = 0;
    for (unsigned i = 0; i < sizeof(PAIRS) / sizeof(PAIRS[0]); ++i) {
        const struct bling_bind_pair *p = &PAIRS[i];
        if (is_mounted(mounts, p->dst)) {
            printf("BLING_BIND_ALREADY dst=%s bytes=%d\n", p->dst, (int)file_size(p->dst));
            continue;
        }
        long src_sz = file_size(p->src);
        long dst_sz = file_size(p->dst);
        if (src_sz != p->expected_bytes || dst_sz != p->expected_bytes) {
            printf("BLING_BIND_SIZE_MISMATCH dst=%s src=%d dst_sz=%d exp=%d\n",
                   p->dst, (int)src_sz, (int)dst_sz, (int)p->expected_bytes);
            failed = 2;
            continue;
        }
        long rc = sys_call(NR_MOUNT, (long)p->src, (long)p->dst, 0, MS_BIND, 0, 0);
        if (rc) {
            printf("BLING_BIND_MOUNT_ERR dst=%s rc=%d\n", p->dst, (int)rc);
            failed = 3;
            continue;
        }
        printf("BLING_BIND_OK dst=%s bytes=%d\n", p->dst, (int)file_size(p->dst));
    }
    return failed;
}
