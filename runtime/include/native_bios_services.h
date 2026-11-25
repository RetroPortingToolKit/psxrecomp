#pragma once
#include <stdint.h>

struct CPUState;

#ifdef __cplusplus
extern "C" {
#endif

/* Native-only guest RAM state, restored together with guest snapshots. */
#define PSX_NATIVE_EXCEPTION_JUMP_PTR 0x0000b000u
#define PSX_NATIVE_SIO_AUTO_ACK 0x0000b004u
#define PSX_NATIVE_TIMER_AUTO_ACK 0x0000b008u
#define PSX_NATIVE_CD_HANDLERS_ACTIVE 0x0000b018u
#define PSX_NATIVE_GENERIC_IRQ_ACK 0x0000b01cu
#define PSX_NATIVE_BOOT_CD_EVENTS 5u
#define PSX_NATIVE_EXCEPTION_STACK 0x8000dff0u
#define PSX_NATIVE_IRQ_CALLBACK_LINK 0x8000a100u
#define PSX_NATIVE_BOOT_RETURN_PHYS 0x0000a104u
#define PSX_NATIVE_EXCEPTION_ENTRY 0x0000a000u
#define PSX_NATIVE_EXCEPTION_MARKER 0x0000000du
#define PSX_NATIVE_A0_TABLE 0x200u
#define PSX_NATIVE_B0_TABLE 0x500u
#define PSX_NATIVE_C0_TABLE 0x680u

typedef struct {
    uint32_t vector;
    uint32_t table;
    uint32_t count;
    uint32_t stubs;
    uint32_t token;
    uint32_t vector_thunk;
} PsxNativeBiosTable;

static const PsxNativeBiosTable psx_native_bios_tables[] = {
    {0xa0, PSX_NATIVE_A0_TABLE, 0xc0, 0x9000, 0x8e00, 0x8f00},
    {0xb0, PSX_NATIVE_B0_TABLE, 0x60, 0x9900, 0x8e10, 0x8f20},
    {0xc0, PSX_NATIVE_C0_TABLE, 0x20, 0x9d80, 0x8e20, 0x8f40},
};

static inline uint32_t psx_native_bios_stub(const PsxNativeBiosTable *table,
                                           uint32_t fn) {
    return 0x80000000u + table->stubs + fn * 12u;
}

/* Guest-visible ordinary vector and per-entry thunks. The native dispatcher
 * recognizes their intact bytes, rather than assuming that this RAM cannot
 * be patched. Table writes or edited thunks may redirect to game/mod code. */
static inline uint32_t psx_native_bios_vector_word(const PsxNativeBiosTable *table,
                                                  uint32_t word) {
    const uint32_t words[] = { 0x08000000u | (table->vector_thunk >> 2), 0, 0, 0 };
    return words[word];
}
static inline uint32_t psx_native_bios_stub_word(const PsxNativeBiosTable *table,
                                                uint32_t fn, uint32_t word) {
    const uint32_t words[] = { 0x24090000u | fn, 0x08000000u | (table->token >> 2), 0 };
    return words[word];
}
static inline void psx_native_bios_init_tables(void (*write_word)(uint32_t, uint32_t)) {
    for (unsigned i = 0; i < sizeof(psx_native_bios_tables) / sizeof(psx_native_bios_tables[0]); ++i) {
        const PsxNativeBiosTable *table = &psx_native_bios_tables[i];
        for (uint32_t word = 0; word < 4; ++word)
            write_word(table->vector + word * 4, psx_native_bios_vector_word(table, word));
        /* Keep a real guest vector thunk for normal patched execution. The
         * native token lets a modified per-call thunk invoke the underlying
         * service without looking itself up again. */
        const uint32_t thunk[] = {0x00094080u, 0x8d080000u | table->table,
                                  0, 0x01000008u, 0};
        for (uint32_t word = 0; word < 5; ++word)
            write_word(table->vector_thunk + word * 4, thunk[word]);
        write_word(table->token, PSX_NATIVE_EXCEPTION_MARKER);
        for (uint32_t fn = 0; fn < table->count; ++fn) {
            const uint32_t stub = psx_native_bios_stub(table, fn);
            write_word(table->table + fn * 4, stub);
            for (uint32_t word = 0; word < 3; ++word)
                write_word(stub + word * 4, psx_native_bios_stub_word(table, fn, word));
        }
    }
    write_word(PSX_NATIVE_C0_TABLE + 6 * 4, 0x80000000u | PSX_NATIVE_EXCEPTION_ENTRY);
    for (uint32_t word = 0; word < 0x100; word += 4)
        write_word(PSX_NATIVE_EXCEPTION_ENTRY + word, 0);
    write_word(PSX_NATIVE_EXCEPTION_ENTRY, PSX_NATIVE_EXCEPTION_MARKER);
}

enum {
    PSX_NATIVE_BIOS_UNSUPPORTED = 0,
    PSX_NATIVE_BIOS_RETURN = 1,
    PSX_NATIVE_BIOS_TRANSFER = 2,
};
int psx_native_bios_service(struct CPUState *cpu, uint32_t vector, uint32_t fn);

#ifdef __cplusplus
}
#endif
