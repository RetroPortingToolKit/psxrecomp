#include "debug_bulk_write.h"
#include "psx_memory.h"
#include <stdio.h>
#include <string.h>

uint32_t g_psx_ram_size = PSX_MAIN_RAM_RETAIL_BYTES;
uint32_t g_psx_ram_mask = PSX_MAIN_RAM_RETAIL_BYTES - 1u;
static unsigned char ram[PSX_MAIN_RAM_BACKING_BYTES];
static unsigned writes;
static unsigned failures;

void psx_host_write_byte(uint32_t addr, uint8_t val)
{
    ram[psx_ram_map_write(addr)] = val;
    ++writes;
}

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void reject(const char *json)
{
    unsigned before = writes;
    CHECK(debug_bulk_write(json) != NULL);
    CHECK(writes == before);
}

int main(void)
{
    CHECK(!debug_bulk_write("{\"addr\":\"0x80000100\",\"hex\":\"0011aaFF\"}"));
    CHECK(writes == 4 && ram[0x100] == 0 && ram[0x101] == 0x11 &&
          ram[0x102] == 0xaa && ram[0x103] == 0xff);
    CHECK(!debug_bulk_write("{\"hex\":\"42\",\"addr\":\"A0200100\"}"));
    CHECK(ram[0x100] == 0x42); /* KSEG1 and retail RAM mirrors */
    CHECK(!debug_bulk_write("{\"addr\":2147483904,\"hex\":\"99\"}"));
    CHECK(ram[0x100] == 0x99); /* bare decimal uses normal JSON semantics */
    CHECK(!debug_bulk_write("{\"addr\":\"801fffff\",\"hex\":\"fe\"}"));
    CHECK(ram[PSX_MAIN_RAM_RETAIL_BYTES - 1] == 0xfe);

    reject("{}");
    reject("{\"addr\":\"100\"}");
    reject("{\"addr\":\"100\",\"hex\":\"\"}");
    reject("{\"addr\":\"100\",\"hex\":\"001\"}");
    reject("{\"addr\":\"100\",\"hex\":\"001122gg\"}");
    reject("{\"addr\":\"100\",\"hex\":\"001122");
    reject("{\"addr\":\"100\",\"hex\":\"00\\u0031\"}");
    reject("{\"addr\":\"100\",\"hex\":12}");
    reject("{\"addr\":\"0x\",\"hex\":\"00\"}");
    reject("{\"addr\":\"100oops\",\"hex\":\"00\"}");
    reject("{\"addr\":\"100000000\",\"hex\":\"00\"}");
    reject("{\"addr\":4294967296,\"hex\":\"00\"}");
    reject("{\"addr\":0100,\"hex\":\"00\"}");
    reject("{\"addr\":-1,\"hex\":\"00\"}");
    reject("{\"addr\":\"ffffffff\",\"hex\":\"0011\"}");
    reject("{\"addr\":\"1f801800\",\"hex\":\"0011\"}"); /* MMIO */
    reject("{\"addr\":\"1f800000\",\"hex\":\"0011\"}"); /* scratchpad */
    reject("{\"addr\":\"1fc00000\",\"hex\":\"0011\"}"); /* BIOS */
    reject("{\"addr\":\"801fffff\",\"hex\":\"0011\"}");

    char json[PSX_DEBUG_BULK_MAX_BYTES * 2u + 100u];
    char hex[PSX_DEBUG_BULK_MAX_BYTES * 2u + 3u];
    memset(hex, 'a', sizeof(hex) - 1u);
    hex[PSX_DEBUG_BULK_MAX_BYTES * 2u] = 0;
    snprintf(json, sizeof(json), "{\"addr\":\"80001000\",\"hex\":\"%s\"}", hex);
    unsigned before = writes;
    CHECK(!debug_bulk_write(json));
    CHECK(writes == before + PSX_DEBUG_BULK_MAX_BYTES);
    CHECK(ram[0x1000] == 0xaa && ram[0x13ff] == 0xaa);
    hex[PSX_DEBUG_BULK_MAX_BYTES * 2u] = 'a';
    snprintf(json, sizeof(json), "{\"addr\":\"80001000\",\"hex\":\"%s\"}", hex);
    reject(json); /* no prefix write on oversize */

    g_psx_ram_size = PSX_MAIN_RAM_EXPANDED_BYTES;
    g_psx_ram_mask = g_psx_ram_size - 1u;
    CHECK(!debug_bulk_write("{\"addr\":\"a07ffffe\",\"hex\":\"1234\"}"));
    CHECK(ram[0x7ffffe] == 0x12 && ram[0x7fffff] == 0x34);
    reject("{\"addr\":\"807fffff\",\"hex\":\"1234\"}");
    if (!failures) puts("PASS debug bulk writes (atomic validation, RAM bounds, mirrors, 8 MB)");
    return failures ? 1 : 0;
}
