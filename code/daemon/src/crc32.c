#include "crc32.h"

/* IEEE 802.3 CRC32, polynomial 0xEDB88320 (reflected) */

static uint32_t crc32_table[256];
static int table_initialized = 0;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    table_initialized = 1;
}

uint32_t crc32_ieee(const void *data, size_t len)
{
    if (!table_initialized)
        build_table();

    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
