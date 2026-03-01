#include <stdio.h>
#include <stdint.h>

#define HAMMER2_ZONE_SEG64 (4 * 1024 * 1024)
#define HAMMER2_PBUFSIZE 65536
#define HAMMER2_ZONE_MASK64 (2LLU * 1024 * 1024 * 1024 - 1)

void map_kernel(uint64_t logical_off, int ndata, int ndisks, uint64_t *stripe_num, int *column, uint64_t *phys_off) {
    uint64_t loff = logical_off & HAMMER2_ZONE_MASK64;
    *stripe_num = loff / ((uint64_t)ndata * HAMMER2_PBUFSIZE);
    *column = (int)((loff / HAMMER2_PBUFSIZE) % ndata);
    *phys_off = HAMMER2_ZONE_SEG64 + (*stripe_num) * HAMMER2_PBUFSIZE;
}

int main() {
    int ndata = 2;
    int ndisks = 4;
    uint64_t l_offs[] = { 0x0000000008801000, 0x0000000008800000, 0x400000 };
    for (int i=0; i<3; i++) {
        uint64_t sn, po;
        int col;
        map_kernel(l_offs[i], ndata, ndisks, &sn, &col, &po);
        printf("Loff: %016jx -> Stripe: %lu Col: %d Phys: %016jx\n", l_offs[i], sn, col, po);
    }
    return 0;
}
