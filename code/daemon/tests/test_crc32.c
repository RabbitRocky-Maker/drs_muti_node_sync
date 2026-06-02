#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/crc32.h"

int main(void)
{
    /* IEEE 802.3 test vector */
    const char *msg = "123456789";
    uint32_t expected = 0xCBF43926u;
    uint32_t got = crc32_ieee(msg, strlen(msg));

    if (got != expected) {
        printf("FAIL test_crc32: expected 0x%08X, got 0x%08X\n", expected, got);
        return 1;
    }

    /* Zero-length input */
    uint32_t zero = crc32_ieee("", 0);
    if (zero != 0x00000000u) {
        printf("FAIL test_crc32 zero-len: got 0x%08X\n", zero);
        return 1;
    }

    printf("PASS test_crc32\n");
    return 0;
}
