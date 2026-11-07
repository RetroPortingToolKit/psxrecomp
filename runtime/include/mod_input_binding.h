#pragma once
#include <stdlib.h>
#include <string.h>

/* Portable SDL scancode/button tokens used by mod binding options. Display
 * names belong to the UI; saved values never depend on keyboard locale. */
enum { PSX_MOD_BIND_NONE, PSX_MOD_BIND_KEY, PSX_MOD_BIND_MOUSE, PSX_MOD_BIND_WHEEL };
typedef struct PSXModInputBinding { int kind, code; } PSXModInputBinding;
typedef struct PSXModInputBindings { PSXModInputBinding input[3]; int count; } PSXModInputBindings;

static inline int psx_mod_parse_input_binding(const char* value, PSXModInputBinding* out) {
    PSXModInputBinding binding = {0, 0};
    if (!value || !out) return 0;
    if (!strcmp(value, "none")) { *out = binding; return 1; }
    if (!strcmp(value, "wheel:up") || !strcmp(value, "wheel:down")) {
        binding.kind = PSX_MOD_BIND_WHEEL;
        binding.code = !strcmp(value, "wheel:up") ? 1 : -1;
        *out = binding; return 1;
    }
    const char* number;
    int maximum;
    if (!strncmp(value, "key:", 4)) {
        binding.kind = PSX_MOD_BIND_KEY; number = value + 4; maximum = 511;
    } else if (!strncmp(value, "mouse:", 6)) {
        binding.kind = PSX_MOD_BIND_MOUSE; number = value + 6; maximum = 5;
    } else return 0;
    if (*number < '1' || *number > '9') return 0;
    for (const char* p = number; *p; ++p) if (*p < '0' || *p > '9') return 0;
    if (strlen(number) > 3) return 0;
    binding.code = (int)strtol(number, NULL, 10);
    if (binding.code < 1 || binding.code > maximum) return 0;
    *out = binding; return 1;
}

static inline int psx_mod_parse_input_bindings(const char* value, PSXModInputBindings* out) {
    PSXModInputBindings bindings = {{{0, 0}}, 0};
    if (!value || !out) return 0;
    do {
        const char* comma = strchr(value, ',');
        const size_t length = comma ? (size_t)(comma - value) : strlen(value);
        char token[24];
        if (!length || length >= sizeof(token) || bindings.count == 3) return 0;
        memcpy(token, value, length); token[length] = 0;
        if (!psx_mod_parse_input_binding(token, &bindings.input[bindings.count++])) return 0;
        if (!comma) break;
        value = comma + 1;
    } while (1);
    *out = bindings;
    return 1;
}
