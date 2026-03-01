#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define HAMMER2_RAID6_POLY      0x1d
#define HAMMER2_PBUFSIZE        65536

uint8_t gf_exp[256];
uint8_t gf_log[256];
uint8_t gf_inv[256];
uint8_t gf_mul_table[256][256];

uint8_t gf_mul2(uint8_t x) {
    return (x << 1) ^ ((x & 0x80) ? HAMMER2_RAID6_POLY : 0);
}

void gf_init() {
    uint8_t x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = x;
        gf_log[x] = (uint8_t)i;
        x = gf_mul2(x);
    }
    gf_log[0] = 0;
    gf_exp[255] = gf_exp[0];
    gf_inv[0] = 0;
    for (int i = 1; i < 256; i++) {
        gf_inv[i] = gf_exp[255 - gf_log[i]];
    }
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 256; j++) {
            if (i == 0 || j == 0) gf_mul_table[i][j] = 0;
            else gf_mul_table[i][j] = gf_exp[(gf_log[i] + gf_log[j]) % 255];
        }
    }
}

uint8_t gf_mul(uint8_t a, uint8_t b) { return gf_mul_table[a][b]; }

void gen_syndrome(int ndisks, size_t bytes, void **ptrs) {
    int ndata = ndisks - 2;
    uint8_t *p = ptrs[ndisks - 2];
    uint8_t *q = ptrs[ndisks - 1];
    for (size_t d = 0; d < bytes; d++) {
        uint8_t pv = 0;
        uint8_t qv = 0;
        for (int i = 0; i < ndata; i++) {
            uint8_t c = ((uint8_t **)ptrs)[i][d];
            pv ^= c;
            qv ^= gf_mul(gf_exp[i], c);
        }
        p[d] = pv;
        q[d] = qv;
    }
}

// Simplified versions of recovery for testing
void recover_2data(int ndisks, size_t bytes, int faila, int failb, void **ptrs) {
    int ndata = ndisks - 2;
    uint8_t *p = ptrs[ndisks - 2];
    uint8_t *q = ptrs[ndisks - 1];
    uint8_t g_a = gf_exp[faila];
    uint8_t g_b = gf_exp[failb];
    uint8_t inv_gapb = gf_inv[g_a ^ g_b];

    for (size_t d = 0; d < bytes; d++) {
        uint8_t Px = p[d];
        uint8_t Qx = q[d];
        for (int i = 0; i < ndata; i++) {
            if (i == faila || i == failb) continue;
            uint8_t c = ((uint8_t **)ptrs)[i][d];
            Px ^= c;
            Qx ^= gf_mul(gf_exp[i], c);
        }
        // Da = (g_b*Px ^ Qx) / (g_a ^ g_b)
        // Db = Px ^ Da
        ((uint8_t *)ptrs[faila])[d] = gf_mul(gf_mul(g_b, Px) ^ Qx, inv_gapb);
        ((uint8_t *)ptrs[failb])[d] = Px ^ ((uint8_t *)ptrs[faila])[d];
    }
}

int main() {
    gf_init();
    int ndisks = 4;
    size_t bytes = 16;
    uint8_t *d0 = malloc(bytes);
    uint8_t *d1 = malloc(bytes);
    uint8_t *p  = malloc(bytes);
    uint8_t *q  = malloc(bytes);
    uint8_t *r0 = malloc(bytes);
    uint8_t *r1 = malloc(bytes);
    void *ptrs[4] = { d0, d1, p, q };

    for(int i=0; i<bytes; i++) { d0[i] = rand()%256; d1[i] = rand()%256; }
    
    gen_syndrome(ndisks, bytes, ptrs);
    printf("Generated P and Q\n");

    // Test dual data recovery
    memset(r0, 0, bytes); memset(r1, 0, bytes);
    void *rptrs[4] = { r0, r1, p, q };
    recover_2data(ndisks, bytes, 0, 1, rptrs);
    
    if (memcmp(d0, r0, bytes) == 0 && memcmp(d1, r1, bytes) == 0) {
        printf("PASS: Dual data recovery\n");
    } else {
        printf("FAIL: Dual data recovery\n");
    }
    return 0;
}
