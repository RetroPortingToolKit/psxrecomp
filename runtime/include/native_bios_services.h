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
#define PSX_NATIVE_RANDOM_SEED 0x0000b050u
#define PSX_NATIVE_USER_HEAP_HEAD 0x0000b054u
#define PSX_NATIVE_USER_HEAP_BEGIN 0x0000b058u
#define PSX_NATIVE_USER_HEAP_END 0x0000b05cu
#define PSX_NATIVE_USER_HEAP_MARKER 0x0000b060u
#define PSX_NATIVE_KERNEL_HEAP_HEAD 0x0000b070u
#define PSX_NATIVE_KERNEL_HEAP_BEGIN 0x0000b074u
#define PSX_NATIVE_KERNEL_HEAP_END 0x0000b078u
#define PSX_NATIVE_KERNEL_HEAP_MARKER 0x0000b080u
#define PSX_NATIVE_KERNEL_CONFIG 0x0000b090u
#define PSX_NATIVE_KERNEL_NAME 0x0000b0b0u
#define PSX_NATIVE_INFO_STUB 0x0000a180u
#define PSX_NATIVE_IRQ_NODES 0x0000b500u
#define PSX_NATIVE_IRQ_TOKENS 0x0000a120u
#define PSX_NATIVE_BOOT_IRQ_NODE_COUNT 6u
#define PSX_NATIVE_PAD_CARD_IRQ_NODE 6u
#define PSX_NATIVE_IRQ_NODE_COUNT 7u
#define PSX_NATIVE_PAD_BUFFERS 0x0000b0e0u
#define PSX_NATIVE_PAD_INITIALIZED 0x0000b0e8u
#define PSX_NATIVE_PAD_LEGACY_DEST 0x0000b0ecu
#define PSX_NATIVE_PAD_LEGACY_BUFFER0 0x0000b600u
#define PSX_NATIVE_PAD_LEGACY_BUFFER1 0x0000b640u
#define PSX_NATIVE_IRQ_NODE_BYTES 16u
/* The legacy SDK card fast-track compatibility region is guest-writable.
 * Keep the native exception stack below 0xdf80..0xdffc. */
#define PSX_NATIVE_EXCEPTION_STACK 0x8000df70u
#define PSX_NATIVE_CARD_INIT 0x0000b100u
#define PSX_NATIVE_CARD_STARTED 0x0000b104u
#define PSX_NATIVE_CARD_PAD_STARTED 0x0000b108u
#define PSX_NATIVE_CARD_PORT 0x0000b10cu
#define PSX_NATIVE_CARD_ALLOW_NEW 0x0000b110u
#define PSX_NATIVE_CARD_LAST_DEVICE 0x0000b114u
#define PSX_NATIVE_CARD_STATUS 0x0000b118u
#define PSX_NATIVE_CARD_REQUEST 0x0000b120u
#define PSX_NATIVE_CARD_VOLUME_VALID 0x0000b160u
#define PSX_NATIVE_FILE_TABLE 0x0000b180u
#define PSX_NATIVE_FILE_BYTES 44u
#define PSX_NATIVE_DEVICE_TABLE 0x0000b440u
#define PSX_NATIVE_DEVICE_NAME 0x0000b498u
#define PSX_NATIVE_CARD_FIND_FILE 0x0000b4a0u
#define PSX_NATIVE_CARD_FIND_PATTERN 0x0000b4b0u
#define PSX_NATIVE_CARD_FASTTRACK_SCRATCH 0x0000ad00u
#define PSX_NATIVE_IRQ_CALLBACK_LINK 0x8000a100u
#define PSX_NATIVE_BOOT_RETURN_PHYS 0x0000a104u
#define PSX_NATIVE_EXCEPTION_PATCH_RESUME 0x0000a108u
#define PSX_NATIVE_WAIT_EVENT_RESUME 0x0000a10cu
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
    if (table->vector == 0xa0 && fn == 0x9d)
        return 0x80000000u | PSX_NATIVE_INFO_STUB;
    return 0x80000000u + table->stubs + fn * 12u;
}

/* Older SDK PAD patches derive these two callbacks from ChangeClearPAD's
 * table entry. Keep their ordinary RAM addresses and patch storage usable. */
static inline uint32_t psx_native_pad_patch_helper(int started) {
    return psx_native_bios_stub(&psx_native_bios_tables[1], 0x5b) +
           (started ? 0x884u : 0x894u);
}

/* Intact BREAK markers at these addresses belong to the native dispatcher.
 * A copied guest exception prefix may branch here inside one compiled unit;
 * it must leave that unit so the runtime can check the live marker first. */
static inline int psx_native_bios_dispatch_marker(uint32_t address) {
    const uint32_t phys = address & 0x1fffffffu;
    if (phys == PSX_NATIVE_EXCEPTION_ENTRY ||
        phys == PSX_NATIVE_EXCEPTION_PATCH_RESUME ||
        phys == PSX_NATIVE_WAIT_EVENT_RESUME ||
        phys == PSX_NATIVE_BOOT_RETURN_PHYS ||
        phys == (psx_native_pad_patch_helper(1) & 0x1fffffffu) ||
        phys == (psx_native_pad_patch_helper(0) & 0x1fffffffu)) return 1;
    for (unsigned i = 0; i < sizeof(psx_native_bios_tables) / sizeof(psx_native_bios_tables[0]); ++i)
        if (phys == psx_native_bios_tables[i].token) return 1;
    return 0;
}

static inline uint32_t psx_native_bios_stub_words(const PsxNativeBiosTable *table,
                                                 uint32_t fn) {
    return table->vector == 0xa0 && fn == 0x9d ? 5u : 3u;
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
    if (table->vector == 0xa0 && fn == 0x9d) {
        /* SDK kernel initialization locates the writable TCB/EvCB/stack
         * configuration from GetSystemInfo's first two instructions. Retain
         * that observed ABI while the actual call remains a native service. */
        const uint32_t address = PSX_NATIVE_KERNEL_CONFIG + 8;
        const uint32_t info[] = {
            0x3c020000u | ((address + 0x8000u) >> 16),
            0x8c420000u | (address & 0xffffu),
            0x2409009du, 0x08000000u | (table->token >> 2), 0
        };
        return info[word];
    }
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
            for (uint32_t word = 0; word < psx_native_bios_stub_words(table, fn); ++word)
                write_word(stub + word * 4, psx_native_bios_stub_word(table, fn, word));
        }
    }
    const uint32_t pad_patch_base =
        psx_native_bios_stub(&psx_native_bios_tables[1], 0x5b);
    for (uint32_t word = 0; word < 11; ++word)
        write_word(pad_patch_base + 0x594u + word * 4, 0);
    write_word(psx_native_pad_patch_helper(1), PSX_NATIVE_EXCEPTION_MARKER);
    write_word(psx_native_pad_patch_helper(0), PSX_NATIVE_EXCEPTION_MARKER);
    write_word(PSX_NATIVE_C0_TABLE + 6 * 4, 0x80000000u | PSX_NATIVE_EXCEPTION_ENTRY);
    for (uint32_t word = 0; word < 0x100; word += 4)
        write_word(PSX_NATIVE_EXCEPTION_ENTRY + word, 0);
    write_word(PSX_NATIVE_EXCEPTION_ENTRY, PSX_NATIVE_EXCEPTION_MARKER);
    /* Some normal SDK exception patches copy the first 0x38 bytes of
     * the legacy handler into this writable address. Their prologue saves
     * AT/V0/V1/RA into the TCB, then reads Cause into V0. Continue into the
     * host-native executive after that actual guest prologue instead of
     * requiring an immutable entry or letting it run into filler RAM. */
    write_word(PSX_NATIVE_EXCEPTION_ENTRY + 0x38,
               0x08000000u | (PSX_NATIVE_EXCEPTION_PATCH_RESUME >> 2));
    write_word(PSX_NATIVE_EXCEPTION_ENTRY + 0x3c, 0);
    write_word(PSX_NATIVE_EXCEPTION_PATCH_RESUME, PSX_NATIVE_EXCEPTION_MARKER);
    write_word(PSX_NATIVE_WAIT_EVENT_RESUME, PSX_NATIVE_EXCEPTION_MARKER);
    for (uint32_t node = 0; node < PSX_NATIVE_IRQ_NODE_COUNT; ++node) {
        const uint32_t address = PSX_NATIVE_IRQ_NODES + node * PSX_NATIVE_IRQ_NODE_BYTES;
        const uint32_t token = PSX_NATIVE_IRQ_TOKENS + node * 8;
        write_word(address, 0);
        write_word(address + 4, node < 4 || node == PSX_NATIVE_PAD_CARD_IRQ_NODE
                                  ? 0x80000000u | token : 0);
        write_word(address + 8, 0x80000000u | (token + 4));
        write_word(address + 12, 0);
        write_word(token, PSX_NATIVE_EXCEPTION_MARKER);
        write_word(token + 4, PSX_NATIVE_EXCEPTION_MARKER);
    }
    write_word(PSX_NATIVE_PAD_BUFFERS, 0);
    write_word(PSX_NATIVE_PAD_BUFFERS + 4, 0);
    write_word(PSX_NATIVE_PAD_INITIALIZED, 0);
    write_word(PSX_NATIVE_PAD_LEGACY_DEST, 0);
    const char native_name[32] = "PSXRecomp native kernel";
    for (uint32_t offset = 0; offset < sizeof native_name; offset += 4) {
        uint32_t word = 0;
        for (uint32_t byte = 0; byte < 4; ++byte)
            word |= (uint32_t)(uint8_t)native_name[offset + byte] << (byte * 8);
        write_word(PSX_NATIVE_KERNEL_NAME + offset, word);
    }
    /* Published File/Device ABI: SDKs inspect and patch these ordinary RAM
     * records. Device methods use the same guarded native A0 thunks. */
    for (uint32_t at = PSX_NATIVE_FILE_TABLE; at < PSX_NATIVE_CARD_FIND_PATTERN + 24; at += 4)
        write_word(at, 0);
    write_word(0x140, PSX_NATIVE_FILE_TABLE);
    write_word(0x144, 16 * PSX_NATIVE_FILE_BYTES);
    write_word(0x150, PSX_NATIVE_DEVICE_TABLE);
    write_word(0x154, 0x50);
    write_word(PSX_NATIVE_DEVICE_NAME, 0x00007562u); /* bu */
    write_word(PSX_NATIVE_DEVICE_TABLE, PSX_NATIVE_DEVICE_NAME);
    write_word(PSX_NATIVE_DEVICE_TABLE + 8, 128);
    const uint32_t offsets[] = {0x14, 0x24, 0x28, 0x1c, 0x34, 0x38, 0x3c};
    const uint32_t functions[] = {0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6d};
    for (uint32_t i = 0; i < sizeof offsets / sizeof offsets[0]; ++i)
        write_word(PSX_NATIVE_DEVICE_TABLE + offsets[i],
                   psx_native_bios_stub(&psx_native_bios_tables[0], functions[i]));
    write_word(PSX_NATIVE_FILE_TABLE, 1);
    write_word(PSX_NATIVE_FILE_TABLE + PSX_NATIVE_FILE_BYTES, 1);
    /* Psy-Q's legacy card patch obtains a writable fast-track code address
     * by decoding the two immediate fields at exception-handler +70/+74.
     * Native card operations do not execute that serial loop. Expose its
     * legitimate patch storage instead of letting the SDK write through a
     * fabricated null address or denying its RAM writes. */
    write_word(PSX_NATIVE_EXCEPTION_ENTRY + 0x70, 0x3c1a0000u |
               (PSX_NATIVE_CARD_FASTTRACK_SCRATCH >> 16));
    write_word(PSX_NATIVE_EXCEPTION_ENTRY + 0x74, 0x375a0000u |
               (PSX_NATIVE_CARD_FASTTRACK_SCRATCH & 0xffffu));
}

/* The same writable HandlerInfo nodes are used by boot and C0 enqueue calls.
 * Later guest registrations prepend; unlinking or editing a native node has
 * the ordinary effect on the actual interrupt chain. */
static inline void psx_native_bios_boot_irq_handlers(
    void (*write_word)(uint32_t, uint32_t), uint32_t handlers) {
    uint32_t heads[4] = {0, 0, 0, 0};
    for (uint32_t node = 0; node < PSX_NATIVE_BOOT_IRQ_NODE_COUNT; ++node) {
        const uint32_t priority = node < 4 ? 1 : node == 4 ? 3 : 0;
        const uint32_t address = PSX_NATIVE_IRQ_NODES + node * PSX_NATIVE_IRQ_NODE_BYTES;
        write_word(address, heads[priority]);
        heads[priority] = address;
    }
    for (uint32_t priority = 0; priority < 4; ++priority)
        write_word(handlers + priority * 8, heads[priority]);
}

enum {
    PSX_NATIVE_BIOS_UNSUPPORTED = 0,
    PSX_NATIVE_BIOS_RETURN = 1,
    PSX_NATIVE_BIOS_TRANSFER = 2,
};
int psx_native_bios_service(struct CPUState *cpu, uint32_t vector, uint32_t fn);
int psx_native_card_service(struct CPUState *cpu, uint32_t vector, uint32_t fn);
void psx_native_card_vblank(struct CPUState *cpu);
void psx_native_padcard_init(struct CPUState *cpu);
int psx_native_padcard_set_active(struct CPUState *cpu, int active);
int psx_native_pad_read_legacy(struct CPUState *cpu, uint32_t *result);
void psx_native_card_bind(struct CPUState *cpu);
void psx_native_bios_event(struct CPUState *cpu, uint32_t cls, uint32_t spec, int deliver);

#ifdef __cplusplus
}
#endif
