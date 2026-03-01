#!/bin/bash
set -e
# Path to the file
FILE="/usr/src/sys/vfs/hammer2/hammer2_raid6.c"

# Replace the gen_syndrome function
sed -i '124,150c\
hammer2_raid6_gen_syndrome(int ndisks, size_t bytes, void **ptrs)\
{\
        int ndata = ndisks - 2;\
        uint8_t *p = ptrs[ndisks - 2];\
        uint8_t *q = ptrs[ndisks - 1];\
        size_t d;\
        int z;\
\
        for (d = 0; d < bytes; d++) {\
                uint8_t pv = 0;\
                uint8_t qv = 0;\
                for (z = 0; z < ndata; z++) {\
                        uint8_t c = ((uint8_t **)ptrs)[z][d];\
                        pv ^= c;\
                        qv ^= hammer2_gf_mul(hammer2_gf_exp[z], c);\
                }\
                p[d] = pv;\
                q[d] = qv;\
        }\
}' "$FILE"
