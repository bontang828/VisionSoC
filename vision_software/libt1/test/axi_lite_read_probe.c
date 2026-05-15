/*
 * Signal-guarded AXI-Lite read-shape probe for the 16-byte-aligned-only
 * read failure seen during KV260 bringup.
 *
 * This is intentionally independent of libt1. It checks whether the
 * failure changes with userspace access width, explicit arm64 barriers,
 * or /dev/mem vs UIO mapping paths.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MAP_SIZE 0x10000u

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_sig;

static void sigbus_handler(int sig)
{
    g_sig = sig;
    siglongjmp(g_jmp, 1);
}

static void barrier(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__("dsb sy\nisb" ::: "memory");
#else
    __sync_synchronize();
#endif
}

static int guarded_load(const volatile void *addr, unsigned width, uint64_t *out)
{
    g_sig = 0;
    if (sigsetjmp(g_jmp, 1) != 0) {
        return -g_sig;
    }

    barrier();
    switch (width) {
    case 1:
        *out = *(const volatile uint8_t *)addr;
        break;
    case 2:
        *out = *(const volatile uint16_t *)addr;
        break;
    case 4:
#if defined(__aarch64__)
        {
            uint32_t v;
            __asm__ __volatile__("ldr %w0, [%1]"
                                 : "=r"(v)
                                 : "r"(addr)
                                 : "memory");
            *out = v;
        }
#else
        *out = *(const volatile uint32_t *)addr;
#endif
        break;
    case 8:
        *out = *(const volatile uint64_t *)addr;
        break;
    default:
        return -EINVAL;
    }
    barrier();
    return 0;
}

static void dump_smaps_for(void *map)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f) {
        printf("smaps: open failed: %s\n", strerror(errno));
        return;
    }

    uintptr_t target = (uintptr_t)map;
    char line[512];
    bool in_match = false;
    unsigned extra = 0;

    printf("\n/proc/self/smaps entry for mapped BAR:\n");
    while (fgets(line, sizeof(line), f)) {
        unsigned long start = 0, end = 0;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            in_match = (target >= start && target < end);
            extra = in_match ? 32u : 0u;
        }
        if (in_match || extra > 0) {
            fputs(line, stdout);
            if (!in_match && extra > 0) {
                extra--;
            }
            if (in_match && strncmp(line, "VmFlags:", 8) == 0) {
                break;
            }
        }
    }
    fclose(f);
}

static void probe_offsets(volatile uint8_t *base)
{
    static const uint32_t offsets[] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x08, 0x0c, 0x10,
        0x14, 0x30, 0x34, 0x40,
        0x44, 0x4c, 0x50, 0x54,
    };
    static const unsigned widths[] = {1, 2, 4, 8};

    printf("\nSignal-guarded loads:\n");
    printf("  offset  width  result\n");
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        for (size_t j = 0; j < sizeof(widths) / sizeof(widths[0]); j++) {
            uint32_t off = offsets[i];
            unsigned width = widths[j];
            if ((off & (width - 1u)) != 0) {
                continue;
            }

            uint64_t value = 0;
            int rc = guarded_load(base + off, width, &value);
            if (rc == 0) {
                printf("  0x%04" PRIx32 "  %5u  0x%0*" PRIx64 "\n",
                       off, width, width * 2, value);
            } else {
                printf("  0x%04" PRIx32 "  %5u  signal %d (%s)\n",
                       off, width, -rc, strsignal(-rc));
            }
        }
    }
}

static int parse_phys(const char *s, off_t *phys)
{
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || end == s || *end != '\0') {
        return -1;
    }
    *phys = (off_t)v;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = (argc >= 2) ? argv[1] : "/dev/uio4";
    off_t phys = 0;
    bool use_devmem = false;

    if (strcmp(path, "/dev/mem") == 0) {
        if (argc < 3 || parse_phys(argv[2], &phys) < 0) {
            fprintf(stderr, "usage: %s /dev/mem PHYS_ADDR\n", argv[0]);
            return 2;
        }
        use_devmem = true;
    }

    struct sigaction sa = {0};
    sa.sa_handler = sigbus_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGBUS, &sa, NULL) < 0 ||
        sigaction(SIGSEGV, &sa, NULL) < 0) {
        perror("sigaction");
        return 1;
    }

    int fd = open(path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "open(%s): %s\n", path, strerror(errno));
        return 1;
    }

    off_t map_off = use_devmem ? phys : 0;
    void *map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, map_off);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap(%s): %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    printf("axi_lite_read_probe: %s", path);
    if (use_devmem) {
        printf(" @ 0x%" PRIxMAX, (uintmax_t)phys);
    }
    printf(", va=%p, size=0x%x\n", map, MAP_SIZE);

    dump_smaps_for(map);
    probe_offsets((volatile uint8_t *)map);

    munmap(map, MAP_SIZE);
    close(fd);
    return 0;
}
