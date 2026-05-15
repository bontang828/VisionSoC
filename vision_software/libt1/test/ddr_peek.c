#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: ddr_peek <pa_hex> [count=64]\n");
        return 1;
    }
    uint64_t pa = strtoull(argv[1], NULL, 0);
    size_t count = argc > 2 ? strtoull(argv[2], NULL, 0) : 64;

    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) { perror("open /dev/mem"); return 1; }

    long page = sysconf(_SC_PAGESIZE);
    uint64_t pa_page = pa & ~(page - 1);
    size_t map_len = ((pa - pa_page) + count + page - 1) & ~(page - 1);
    void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, pa_page);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    uint8_t *p = (uint8_t*)map + (pa - pa_page);
    int nonzero = 0, total = 0;
    for (size_t i = 0; i < count; i++) {
        if (i % 16 == 0) printf("%08lx: ", (unsigned long)(pa + i));
        printf("%02x ", p[i]);
        if (p[i] != 0) nonzero++;
        total++;
        if (i % 16 == 15) printf("\n");
    }
    if (count % 16) printf("\n");
    printf("--- %d/%d non-zero bytes ---\n", nonzero, total);

    munmap(map, map_len);
    close(fd);
    return 0;
}
