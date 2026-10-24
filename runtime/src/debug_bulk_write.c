#include "debug_bulk_write.h"
#include "psx_memory.h"

#include <stddef.h>
#include <string.h>

/* Quoted wire strings are ASCII. Reject escapes and unterminated strings
 * instead of silently writing a truncated run. */
static const char *bulk_value(const char *json, const char *key)
{
    const char *p = strstr(json, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p++ != ':') return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}

static const char *bulk_string(const char *json, const char *key, size_t *len)
{
    const char *p = bulk_value(json, key);
    if (!p) return NULL;
    if (*p++ != '"') return NULL;
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' || (unsigned char)*p < 0x20) return NULL;
        ++p;
    }
    if (*p != '"') return NULL;
    *len = (size_t)(p - start);
    return start;
}

static int bulk_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char *debug_bulk_write(const char *json)
{
    size_t addr_len = 0, hex_len = 0;
    const char *addr_text = bulk_string(json, "\"addr\"", &addr_len);
    const char *hex = bulk_string(json, "\"hex\"", &hex_len);
    if (!hex) return "missing or invalid hex string";
    uint32_t addr = 0;
    if (!addr_text) {
        /* Match read_ram/write_ram: a bare JSON address is decimal. */
        const char *p = bulk_value(json, "\"addr\"");
        if (!p || *p < '0' || *p > '9') return "missing or invalid addr";
        const char *start = p;
        uint64_t number = 0;
        while (*p >= '0' && *p <= '9') {
            number = number * 10u + (unsigned)(*p++ - '0');
            if (number > UINT32_MAX) return "bad addr";
        }
        if ((p - start > 1 && *start == '0') ||
            (*p != ',' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'))
            return "bad addr";
        addr = (uint32_t)number;
    } else {
        if (addr_len >= 2 && addr_text[0] == '0' &&
            (addr_text[1] == 'x' || addr_text[1] == 'X')) {
            addr_text += 2;
            addr_len -= 2;
        }
        if (addr_len == 0 || addr_len > 8) return "bad addr";
        for (size_t i = 0; i < addr_len; ++i) {
            int n = bulk_nibble(addr_text[i]);
            if (n < 0) return "bad addr";
            addr = (addr << 4) | (uint32_t)n;
        }
    }
    if (!hex_len || (hex_len & 1u) || hex_len > PSX_DEBUG_BULK_MAX_BYTES * 2u)
        return "hex must contain 1 to 1024 complete bytes";
    const uint32_t count = (uint32_t)(hex_len / 2u);
    if (!psx_ram_resolve(addr, count, NULL)) return "write outside contiguous main RAM";

    unsigned char bytes[PSX_DEBUG_BULK_MAX_BYTES];
    for (uint32_t i = 0; i < count; ++i) {
        int hi = bulk_nibble(hex[i * 2u]), lo = bulk_nibble(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return "bad hex";
        bytes[i] = (unsigned char)((hi << 4) | lo);
    }
    /* Keep host-store semantics: cache isolation, executable-RAM dirty
     * tracking and the existing write observers all follow write_ram. */
    for (uint32_t i = 0; i < count; ++i)
        psx_host_write_byte(addr + i, bytes[i]);
    return NULL;
}
