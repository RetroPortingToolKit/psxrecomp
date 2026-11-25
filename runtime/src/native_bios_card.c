#include "cpu_state.h"
#include "native_bios_services.h"
#include "psx_cycles.h"
#include "interrupts.h"
#include "memcard.h"
#include "sio.h"
#include "psx_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CARD_EVENT = 0xf0000011u,
    BACKUP_EVENT = 0xf4000001u,
    CARD_SUCCESS = 4,
    CARD_ERROR = 0x8000,
    CARD_NEW = 0x2000,
    CARD_REQ_DEVICE = 0,
    CARD_REQ_SECTOR = 4,
    CARD_REQ_BUFFER = 8,
    CARD_REQ_INVALID = 12,
    CARD_REQ_BACKUP_INFO = 16,
    CARD_REQ_FILE = 20,
    CARD_REQ_BYTES = 24,
    CARD_REQ_STRIDE = 32,
    CARD_FILE_READ = 8,
    CARD_FILE_WRITE = 10,
};

static CPUState *bound_cpu;

static uint32_t card_request(unsigned port)
{
    return PSX_NATIVE_CARD_REQUEST + port * CARD_REQ_STRIDE;
}

static void cards_replaced(void)
{
    /* Replay installs its sandbox backing before the first frame.
     * Before InitCARD, zeroed status words are not pending requests and
     * must not change the power-on guest RAM or replay digest. */
    if (!bound_cpu || !bound_cpu->read_word(PSX_NATIVE_CARD_INIT)) return;
    for (unsigned port = 0; port < 2; ++port) {
        if (!(bound_cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u))
            bound_cpu->write_word(card_request(port) + CARD_REQ_INVALID, 1);
        bound_cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4, 0);
    }
    for (unsigned fd = 2; fd < 16; ++fd)
        bound_cpu->write_word(PSX_NATIVE_FILE_TABLE + fd * PSX_NATIVE_FILE_BYTES, 0);
    bound_cpu->write_word(PSX_NATIVE_CARD_FIND_FILE, 0);
}

void psx_native_card_bind(CPUState *cpu)
{
    bound_cpu = cpu;
    sio_set_cards_replaced_hook(cpu ? cards_replaced : NULL);
}

static int card_checksum_valid(const uint8_t *data)
{
    uint8_t checksum = 0;
    for (unsigned i = 0; i < MEMCARD_SECTOR_SIZE - 1; ++i)
        checksum ^= data[i];
    return checksum == data[MEMCARD_SECTOR_SIZE - 1];
}

static int write_card_sector(unsigned port, unsigned sector, const uint8_t *data)
{
    if (memcard_write_sector(port, sector, data) != 0) return 0;
    memcard_flush(port);
    if (memcard_last_flush_result(port) != 0) return 0;
    sio_card_mark_written(port);
    return 1;
}

/* The backup unit's scan operates on the live card directory rather than
 * retaining a second, stale copy of it. Preserve its successful header
 * write-test (sector 63), new-card state, and directory checksum checks. */
static int initialize_volume(unsigned port)
{
    uint8_t data[MEMCARD_SECTOR_SIZE];
    if (memcard_read_sector(port, 0, data) != 0 ||
        data[0] != 'M' || data[1] != 'C') return 0;
    if (!write_card_sector(port, 63, data)) return 0;
    for (unsigned sector = 1; sector <= 35; ++sector) {
        if (memcard_read_sector(port, sector, data) != 0 ||
            !card_checksum_valid(data)) return 0;
    }
    return 1;
}

/* The backup-unit filesystem owns the complete File/Device consumer path.
 * Read/write transfer host card sectors directly, avoiding firmware's serial
 * and filesystem polling loops. All descriptor/cursor state remains guest RAM;
 * raw card bytes and atomic persistent flushing stay in memcard.c. */
static uint32_t file_word(CPUState *cpu, uint32_t file, unsigned offset)
{
    return cpu->read_word(file + offset);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void directory_checksum(uint8_t *data)
{
    data[127] = 0;
    for (unsigned i = 0; i < 127; ++i) data[127] ^= data[i];
}

static int guest_name(CPUState *cpu, uint32_t address, char name[21])
{
    for (unsigned i = 0; i <= 20; ++i) {
        if (!psx_ram_resolve(address + i, 1, NULL)) return 0;
        uint8_t c = cpu->read_byte(address + i);
        if (!c) { name[i] = 0; return 1; }
        if (i == 20 || c == ':' || c == '/' || c == '\\') return 0;
        name[i] = (char)c;
    }
    return 0;
}

static uint32_t card_path(CPUState *cpu, uint32_t address, unsigned *port)
{
    if (!psx_ram_resolve(address, 5, NULL) || cpu->read_byte(address) != 'b' ||
        cpu->read_byte(address + 1) != 'u' || cpu->read_byte(address + 3) != '0' ||
        cpu->read_byte(address + 4) != ':') return 0;
    uint8_t slot = cpu->read_byte(address + 2);
    if (slot != '0' && slot != '1') return 0;
    *port = slot - '0';
    return address + 5;
}

static int card_ready(CPUState *cpu, unsigned port)
{
    uint8_t header[128];
    return memcard_is_present(port) &&
        (cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u) &&
        memcard_read_sector(port, 0, header) == 0 && header[0] == 'M' && header[1] == 'C';
}

static uint32_t free_file(CPUState *cpu)
{
    for (unsigned fd = 2; fd < 16; ++fd) {
        uint32_t file = PSX_NATIVE_FILE_TABLE + fd * PSX_NATIVE_FILE_BYTES;
        if (!file_word(cpu, file, 0)) {
            for (unsigned at = 0; at < PSX_NATIVE_FILE_BYTES; at += 4) cpu->write_word(file + at, 0);
            cpu->write_word(file + 40, fd);
            return file;
        }
    }
    return 0;
}

static uint32_t descriptor(CPUState *cpu, uint32_t fd)
{
    if (fd < 2 || fd >= 16) return 0;
    uint32_t file = PSX_NATIVE_FILE_TABLE + fd * PSX_NATIVE_FILE_BYTES;
    return file_word(cpu, file, 0) && file_word(cpu, file, 28) == PSX_NATIVE_DEVICE_TABLE ? file : 0;
}

static int commit_card(unsigned port)
{
    memcard_flush(port);
    if (memcard_last_flush_result(port)) return 0;
    sio_card_mark_written(port);
    return 1;
}

static int open_card_file(CPUState *cpu, uint32_t file, uint32_t path, uint32_t mode)
{
    char name[21];
    unsigned port = file_word(cpu, file, 4) >> 4;
    if (port > 1 || !card_ready(cpu, port) || !guest_name(cpu, path, name) || !name[0]) return -1;
    uint8_t directory[16][128];
    unsigned free_blocks[15], free_count = 0, found = 0;
    for (unsigned block = 1; block <= 15; ++block) {
        uint8_t *entry = directory[block];
        if (memcard_read_sector(port, block, entry) || !card_checksum_valid(entry)) return -1;
        if (entry[0] == 0x51 && !strncmp((char *)entry + 10, name, 20)) found = block;
        if ((entry[0] & 0xf0) == 0xa0) free_blocks[free_count++] = block;
    }
    if (mode & 0x200) {
        unsigned blocks = mode >> 16;
        if (found || !blocks || blocks > free_count) return -1;
        for (unsigned i = 0; i < blocks; ++i) {
            unsigned block = free_blocks[i];
            uint8_t *entry = directory[block];
            memset(entry, 0, 128);
            entry[0] = i == 0 ? 0x51 : i + 1 == blocks ? 0x53 : 0x52;
            if (!i) {
                uint32_t bytes = blocks * 8192;
                for (unsigned b = 0; b < 4; ++b) entry[4 + b] = (uint8_t)(bytes >> (8 * b));
                memcpy(entry + 10, name, strlen(name));
            }
            unsigned next = i + 1 < blocks ? free_blocks[i + 1] - 1 : 0xffff;
            entry[8] = (uint8_t)next;
            entry[9] = (uint8_t)(next >> 8);
            directory_checksum(entry);
            if (memcard_write_sector(port, block, entry)) return -1;
        }
        if (!commit_card(port)) return -1;
        found = free_blocks[0];
    }
    if (!found) return -1;
    cpu->write_word(file + 16, 0);
    cpu->write_word(file + 32, le32(directory[found] + 4));
    cpu->write_word(file + 36, found * 64);
    return 0;
}

static int32_t transfer_file_at(CPUState *cpu, uint32_t file, uint32_t buffer,
                                uint32_t bytes, int writing, uint32_t offset)
{
    unsigned port = file_word(cpu, file, 4) >> 4;
    uint32_t cursor = file_word(cpu, file, 16), length = file_word(cpu, file, 32);
    unsigned block = file_word(cpu, file, 36) / 64;
    if (port > 1 || !card_ready(cpu, port) || offset > length || bytes > INT32_MAX ||
        (bytes && !psx_ram_resolve(buffer, bytes, NULL))) return -1;
    if (bytes > length - offset) bytes = length - offset;
    unsigned chain[15], count = 0, seen = 0;
    int ended = 0;
    while (block >= 1 && block <= 15 && !(seen & (1u << block))) {
        uint8_t entry[128];
        if (memcard_read_sector(port, block, entry) || !card_checksum_valid(entry) ||
            (entry[0] & 0xf0) != 0x50) return -1;
        seen |= 1u << block;
        chain[count++] = block;
        unsigned next = (unsigned)entry[8] | (unsigned)entry[9] << 8;
        if (next == 0xffff) { ended = 1; break; }
        block = next + 1;
    }
    if (!ended || length > count * 8192u || offset + bytes > count * 8192u) return -1;
    uint32_t done = 0;
    while (done < bytes) {
        uint32_t position = offset + done;
        unsigned sector = chain[position / 8192] * 64 + (position % 8192) / 128;
        unsigned begin = position % 128, take = 128 - begin;
        if (take > bytes - done) take = bytes - done;
        uint8_t data[128];
        if (memcard_read_sector(port, sector, data)) return -1;
        for (unsigned i = 0; i < take; ++i) {
            if (writing) data[begin + i] = cpu->read_byte(buffer + done + i);
            else cpu->write_byte(buffer + done + i, data[begin + i]);
        }
        if (writing && memcard_write_sector(port, sector, data)) return -1;
        done += take;
    }
    if (writing && bytes && !commit_card(port)) return -1;
    cpu->write_word(file + 16, cursor + bytes);
    return (int32_t)bytes;
}

static int32_t transfer_file(CPUState *cpu, uint32_t file, uint32_t buffer, uint32_t bytes, int writing)
{
    return transfer_file_at(cpu, file, buffer, bytes, writing, file_word(cpu, file, 16));
}

static int32_t transfer_or_enqueue_file(CPUState *cpu, uint32_t file, uint32_t buffer,
                                        uint32_t bytes, int writing)
{
    if (!(file_word(cpu, file, 0) & 0x8000u))
        return transfer_file(cpu, file, buffer, bytes, writing);
    unsigned port = file_word(cpu, file, 4) >> 4;
    uint32_t offset = file_word(cpu, file, 16), length = file_word(cpu, file, 32);
    if (port > 1 || !(cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u) ||
        (offset & 127u) || offset >= length || bytes > INT32_MAX ||
        (bytes && !psx_ram_resolve(buffer, bytes, NULL))) return -1;
    /* The backup-unit async interface transfers whole card sectors and
     * returns zero when accepted. Keep pending work in guest RAM, not a host
     * closure, so current-build snapshots retain the actual operation. */
    bytes &= ~127u;
    if (bytes > length - offset) return -1;
    if (!bytes) {
        psx_native_bios_event(cpu, BACKUP_EVENT, CARD_SUCCESS, 1);
        return 0;
    }
    uint32_t request = card_request(port);
    cpu->write_word(request + CARD_REQ_DEVICE, port << 4);
    cpu->write_word(request + CARD_REQ_SECTOR, offset / MEMCARD_SECTOR_SIZE);
    cpu->write_word(request + CARD_REQ_BUFFER, buffer);
    cpu->write_word(request + CARD_REQ_INVALID, 0);
    cpu->write_word(request + CARD_REQ_BACKUP_INFO, 0);
    cpu->write_word(request + CARD_REQ_FILE, file);
    cpu->write_word(request + CARD_REQ_BYTES, bytes);
    cpu->write_word(PSX_NATIVE_CARD_STATUS + port * 4,
                    writing ? CARD_FILE_WRITE : CARD_FILE_READ);
    return 0;
}

static int pattern_matches(const char *pattern, const uint8_t *name)
{
    for (unsigned i = 0; i < 20; ++i) {
        if (pattern[i] == '*') return 1;
        if (!pattern[i]) return !name[i];
        if (pattern[i] != '?' && (uint8_t)pattern[i] != name[i]) return 0;
    }
    return pattern[20] == 0;
}

static uint32_t find_file(CPUState *cpu, uint32_t file, uint32_t output, int first)
{
    unsigned port = file_word(cpu, file, 4) >> 4;
    if (port > 1 || !psx_ram_resolve(output, 40, NULL) ||
        !(cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u)) return 0;
    uint8_t header[MEMCARD_SECTOR_SIZE];
    int read = memcard_is_present(port) && memcard_read_sector(port, 0, header) == 0;
    if (first) {
        /* FirstFile probes sector zero before scanning the directory. The SDK
         * observes this raw-card completion even when no filename matches.
         * Keep that callback while replacing the firmware's serial/wait loop;
         * NextFile does not start another observable header-read operation. */
        cpu->write_word(PSX_NATIVE_CARD_LAST_DEVICE, port << 4);
        cpu->write_word(PSX_NATIVE_CARD_STATUS + port * 4, read ? 1 : 0x21);
        psx_native_bios_event(cpu, CARD_EVENT, read ? CARD_SUCCESS : CARD_ERROR, 1);
    }
    if (!read || header[0] != 'M' || header[1] != 'C') return 0;
    char pattern[21];
    if (!guest_name(cpu, PSX_NATIVE_CARD_FIND_PATTERN, pattern)) return 0;
    for (unsigned block = file_word(cpu, file, 16) + 1; block <= 15; ++block) {
        uint8_t entry[128];
        if (memcard_read_sector(port, block, entry) || !card_checksum_valid(entry)) return 0;
        if (entry[0] != 0x51 || !pattern_matches(pattern, entry + 10)) continue;
        for (unsigned i = 0; i < 40; ++i) cpu->write_byte(output + i, 0);
        for (unsigned i = 0; i < 20; ++i) cpu->write_byte(output + i, entry[10 + i]);
        cpu->write_word(output + 20, entry[0] & 0xf0);
        cpu->write_word(output + 24, le32(entry + 4));
        cpu->write_word(output + 32, block * 64);
        cpu->write_word(file + 16, block);
        return output;
    }
    return 0;
}

static uint32_t device_call(CPUState *cpu, unsigned method, uint32_t file, uint32_t arg1, uint32_t arg2)
{
    uint32_t target = cpu->read_word(PSX_NATIVE_DEVICE_TABLE + method);
    if (!target) return UINT32_MAX;
    uint32_t saved_ra = cpu->gpr[31];
    cpu->gpr[4] = file;
    cpu->gpr[5] = arg1;
    cpu->gpr[6] = arg2;
    cpu->gpr[31] = PSX_NATIVE_IRQ_CALLBACK_LINK;
    psx_dispatch_call(cpu, target, PSX_NATIVE_IRQ_CALLBACK_LINK);
    cpu->gpr[31] = saved_ra;
    return cpu->gpr[2];
}

static int filesystem_service(CPUState *cpu, uint32_t vector, uint32_t fn)
{
    if (vector == 0xa0 && fn == 0x6d) {
        if (!psx_ram_resolve(cpu->gpr[4], PSX_NATIVE_FILE_BYTES, NULL))
            return PSX_NATIVE_BIOS_UNSUPPORTED;
        unsigned port = cpu->read_word(cpu->gpr[4] + 4) >> 4;
        uint32_t result = 1;
        if (port <= 1 && memcard_is_present(port)) {
            result = 0;
            for (unsigned sector = 0; sector < 64; ++sector) {
                uint8_t data[128] = {0};
                if (sector == 0 || sector == 63) { data[0] = 'M'; data[1] = 'C'; }
                else if (sector <= 15) { data[0] = 0xa0; data[8] = data[9] = 0xff; }
                else if (sector <= 35) {
                    memset(data, 0xff, 4);
                    data[8] = data[9] = 0xff;
                }
                directory_checksum(data);
                if (memcard_write_sector(port, sector, data)) { result = 1; break; }
            }
            if (!result && !commit_card(port)) result = 1;
            cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4, result == 0);
        }
        cpu->gpr[2] = result;
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xa0 && fn >= 0x65 && fn <= 0x6a) {
        uint32_t file = cpu->gpr[4], result;
        if (!psx_ram_resolve(file, PSX_NATIVE_FILE_BYTES, NULL)) return PSX_NATIVE_BIOS_UNSUPPORTED;
        switch (fn) {
        case 0x65: result = (uint32_t)open_card_file(cpu, file, cpu->gpr[5], cpu->gpr[6]); break;
        case 0x66: case 0x67:
            result = (uint32_t)transfer_or_enqueue_file(cpu, file, cpu->gpr[5], cpu->gpr[6], fn == 0x67); break;
        case 0x68: {
            unsigned port = file_word(cpu, file, 4) >> 4;
            result = port > 1 || !(cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u);
            break;
        }
        case 0x69: {
            char pattern[21] = {0};
            if (!guest_name(cpu, cpu->gpr[5], pattern)) { result = 0; break; }
            for (unsigned i = 0; i <= 20; ++i)
                cpu->write_byte(PSX_NATIVE_CARD_FIND_PATTERN + i, (uint8_t)pattern[i]);
            cpu->write_word(file + 16, 0);
            result = find_file(cpu, file, cpu->gpr[6], 1);
            break;
        }
        default: result = find_file(cpu, file, cpu->gpr[5], 0); break;
        }
        cpu->gpr[2] = result;
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xa0 && fn <= 4) fn += 0x32;
    else if (vector != 0xb0) return PSX_NATIVE_BIOS_UNSUPPORTED;
    uint32_t a0 = cpu->gpr[4], a1 = cpu->gpr[5], a2 = cpu->gpr[6], result = UINT32_MAX;
    uint32_t file = descriptor(cpu, a0);
    if (fn == 0x32 || fn == 0x42 || fn == 0x41) {
        unsigned port;
        uint32_t name = card_path(cpu, a0, &port);
        if (!name || (fn == 0x32 && !a1)) return PSX_NATIVE_BIOS_UNSUPPORTED;
        if (fn == 0x42) {
            file = cpu->read_word(PSX_NATIVE_CARD_FIND_FILE);
            if (!file) file = free_file(cpu);
        } else file = free_file(cpu);
        if (file) {
            cpu->write_word(file, fn == 0x32 ? a1 : 1);
            cpu->write_word(file + 4, port << 4);
            cpu->write_word(file + 28, PSX_NATIVE_DEVICE_TABLE);
            if (fn == 0x41) {
                result = device_call(cpu, 0x3c, file, 0, 0) == 0;
                cpu->write_word(file, 0);
            } else if (fn == 0x42) {
                cpu->write_word(PSX_NATIVE_CARD_FIND_FILE, file);
                result = device_call(cpu, 0x34, file, name, a1);
            } else {
                result = device_call(cpu, 0x14, file, name, a1);
                if (!result) result = file_word(cpu, file, 40);
                else cpu->write_word(file, 0);
            }
        } else if (fn == 0x42) result = 0;
    } else if (fn >= 0x33 && fn <= 0x36) {
        if (file) {
            if (fn == 0x33) {
                int64_t position = (int32_t)a1;
                if (a2 == 1) position += file_word(cpu, file, 16);
                else if (a2 == 2) position += file_word(cpu, file, 32);
                if (a2 <= 2 && position >= 0 && position <= UINT32_MAX) {
                    result = (uint32_t)position;
                    cpu->write_word(file + 16, result);
                }
            } else {
                result = device_call(cpu, fn == 0x34 ? 0x24 : fn == 0x35 ? 0x28 : 0x1c, file, a1, a2);
                if (fn == 0x36) {
                    if (!result) cpu->write_word(file, 0);
                    else result = UINT32_MAX;
                }
            }
        }
    } else if (fn == 0x43) {
        file = cpu->read_word(PSX_NATIVE_CARD_FIND_FILE);
        result = file ? device_call(cpu, 0x38, file, a0, 0) : 0;
    } else return PSX_NATIVE_BIOS_UNSUPPORTED;
    cpu->gpr[2] = result;
    psx_cpu_charge(32);
    return PSX_NATIVE_BIOS_RETURN;
}

static uint32_t enqueue(CPUState *cpu, uint32_t status, int backup_info)
{
    uint32_t device = cpu->gpr[4];
    if (device >= 32) return 0;
    unsigned port = device >> 4;
    uint32_t address = card_request(port);
    if (!(cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u)) return 0;
    int raw = status == 2 || status == 4;
    if (raw && (cpu->gpr[5] >= MEMCARD_SECTORS ||
        !psx_ram_resolve(cpu->gpr[6], MEMCARD_SECTOR_SIZE, NULL))) return 0;
    cpu->write_word(address + CARD_REQ_DEVICE, device);
    cpu->write_word(address + CARD_REQ_SECTOR, raw ? cpu->gpr[5] : 0);
    cpu->write_word(address + CARD_REQ_BUFFER, raw ? cpu->gpr[6] : 0);
    cpu->write_word(address + CARD_REQ_INVALID, 0);
    cpu->write_word(address + CARD_REQ_BACKUP_INFO, backup_info);
    cpu->write_word(PSX_NATIVE_CARD_STATUS + port * 4, status);
    return 1;
}

static uint32_t complete_request(CPUState *cpu, unsigned port, uint32_t status)
{
    uint32_t request = card_request(port);
    uint32_t device = cpu->read_word(request + CARD_REQ_DEVICE);
    uint32_t sector = cpu->read_word(request + CARD_REQ_SECTOR);
    uint32_t buffer = cpu->read_word(request + CARD_REQ_BUFFER);
    if (cpu->read_word(request + CARD_REQ_INVALID) ||
        (device & 0xfu) || !memcard_is_present(port)) return CARD_ERROR;
    if ((sio_card_flag(port) & 8) &&
        !cpu->read_word(PSX_NATIVE_CARD_ALLOW_NEW)) return CARD_NEW;
    if (status == CARD_FILE_READ || status == CARD_FILE_WRITE) {
        uint32_t file = cpu->read_word(request + CARD_REQ_FILE);
        if (!psx_ram_resolve(file, PSX_NATIVE_FILE_BYTES, NULL) ||
            descriptor(cpu, file_word(cpu, file, 40)) != psx_ram_canonical_offset(file) ||
            file_word(cpu, file, 4) != device || !(file_word(cpu, file, 0) & 0x8000u))
            return CARD_ERROR;
        /* The owning operation is completing now. Release its busy state
         * before reusing the synchronous host sector transfer, then publish
         * the normal backup/file/card events after its final result. */
        cpu->write_word(PSX_NATIVE_CARD_STATUS + port * 4, 1);
        if (sector > UINT32_MAX / MEMCARD_SECTOR_SIZE) return CARD_ERROR;
        int32_t transferred = transfer_file_at(cpu, file, buffer,
            cpu->read_word(request + CARD_REQ_BYTES), status == CARD_FILE_WRITE,
            sector * MEMCARD_SECTOR_SIZE);
        return transferred < 0 ? CARD_ERROR : CARD_SUCCESS;
    }
    if (status == 0) return CARD_SUCCESS;
    if (status == 6) {
        int valid = initialize_volume(port);
        cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4, valid);
        return valid ? CARD_SUCCESS : CARD_ERROR;
    }
    if (!psx_ram_resolve(buffer, MEMCARD_SECTOR_SIZE, NULL)) return CARD_ERROR;
    uint8_t data[MEMCARD_SECTOR_SIZE];
    if (status == 2) {
        if (memcard_read_sector(port, sector, data) != 0) return CARD_ERROR;
        for (unsigned i = 0; i < sizeof data; ++i)
            cpu->write_byte(buffer + i, data[i]);
    } else if (status == 4) {
        for (unsigned i = 0; i < sizeof data; ++i)
            data[i] = cpu->read_byte(buffer + i);
        if (!write_card_sector(port, sector, data)) return CARD_ERROR;
    } else {
        return CARD_ERROR;
    }
    return CARD_SUCCESS;
}

/* Native card executive state is guest-resident, so snapshots and rewinds
 * retain the same initialization, VBlank registration and slot state. The
 * host card backing remains under the shared memcard runtime's ownership. */
int psx_native_card_service(CPUState *cpu, uint32_t vector, uint32_t fn)
{
    int filesystem = filesystem_service(cpu, vector, fn);
    if (filesystem) return filesystem;
    if (vector == 0xa0 && (fn == 0x55 || fn == 0x70)) {
        for (unsigned port = 0; port < 2; ++port)
            cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4,
                            initialize_volume(port));
        cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 0);
        cpu->gpr[2] = 0;
        psx_cpu_charge(128);
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xa0 && fn == 0xab) {
        cpu->gpr[2] = enqueue(cpu, 0, 1);
        psx_cpu_charge(32);
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xa0 && fn == 0xac) {
        cpu->gpr[2] = enqueue(cpu, 6, 1);
        psx_cpu_charge(32);
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector != 0xb0) return PSX_NATIVE_BIOS_UNSUPPORTED;
    switch (fn) {
    case 0x4a: /* InitCARD */
        psx_native_padcard_init(cpu);
        cpu->gpr[2] = cpu->read_word(PSX_NATIVE_CARD_INIT);
        cpu->write_word(PSX_NATIVE_CARD_INIT, 1);
        cpu->write_word(PSX_NATIVE_CARD_PAD_STARTED, cpu->gpr[4]);
        cpu->write_word(PSX_NATIVE_CARD_PORT, 0);
        cpu->write_word(PSX_NATIVE_CARD_STATUS, 1);
        cpu->write_word(PSX_NATIVE_CARD_STATUS + 4, 1);
        break;
    case 0x4b: /* StartCARD */
        if (!psx_native_padcard_set_active(cpu, 1)) return PSX_NATIVE_BIOS_UNSUPPORTED;
        /* Preserve device register state without the old select/delay loop. */
        cpu->write_half(0x1f80104au, 0x40);
        cpu->write_half(0x1f80104eu, 2073600 / 15200);
        cpu->write_half(0x1f801048u, 13);
        cpu->write_half(0x1f80104au, 0);
        cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 0);
        cpu->write_word(PSX_NATIVE_CARD_STARTED, 1);
        cpu->write_word(PSX_NATIVE_SIO_AUTO_ACK, 1);
        cpu->write_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4, 0);
        cpu->write_word(0x1f801074u, cpu->read_word(0x1f801074u) | (1u << IRQ_VBLANK));
        cpu->cop0[12] |= 0x401u;
        cpu->gpr[2] = 1;
        break;
    case 0x4c: /* StopCARD */
        if (!psx_native_padcard_set_active(cpu, 0)) return PSX_NATIVE_BIOS_UNSUPPORTED;
        cpu->write_word(PSX_NATIVE_CARD_STARTED, 0);
        cpu->write_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4, 1);
        cpu->cop0[12] |= 0x401u;
        cpu->gpr[2] = 1;
        break;
    case 0x4d: /* cardInfoInternal */
        cpu->gpr[2] = enqueue(cpu, 0, 0);
        break;
    case 0x4e: /* _card_write */
        cpu->gpr[2] = enqueue(cpu, 4, 0);
        break;
    case 0x4f: /* _card_read */
        cpu->gpr[2] = enqueue(cpu, 2, 0);
        break;
    case 0x58: /* _card_chan */
        cpu->gpr[2] = cpu->read_word(PSX_NATIVE_CARD_LAST_DEVICE);
        break;
    case 0x50: /* _card_allowNew */
        cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 1);
        break;
    case 0x5c: /* card status: bit 0 means idle */
        if (cpu->gpr[4] > 1) return PSX_NATIVE_BIOS_UNSUPPORTED;
        cpu->gpr[2] = cpu->read_word(PSX_NATIVE_CARD_STATUS + cpu->gpr[4] * 4);
        break;
    default:
        return PSX_NATIVE_BIOS_UNSUPPORTED;
    }
    psx_cpu_charge(32);
    return PSX_NATIVE_BIOS_RETURN;
}

void psx_native_card_vblank(CPUState *cpu)
{
    const int pads = cpu->read_word(PSX_NATIVE_CARD_PAD_STARTED) != 0;
    if (!pads && !cpu->read_word(PSX_NATIVE_CARD_STARTED)) return;
    if (pads) {
        if (!cpu->read_word(PSX_NATIVE_PAD_INITIALIZED)) {
            fprintf(stderr, "native BIOS PAD provider gap: active driver has no InitPAD buffers\n");
            exit(1);
        }
        for (unsigned port = 0; port < 2; ++port) {
            uint8_t packet[34];
            uint32_t buffer = cpu->read_word(PSX_NATIVE_PAD_BUFFERS + port * 4);
            uint32_t bytes = sio_read_pad_packet((int)port, packet);
            if (!bytes || bytes > sizeof packet || !psx_ram_resolve(buffer, bytes, NULL)) {
                fprintf(stderr, "native BIOS PAD provider gap: invalid packet/buffer on port %u\n", port);
                exit(1);
            }
            for (uint32_t i = 0; i < bytes; ++i) cpu->write_byte(buffer + i, packet[i]);
        }
        if (cpu->read_word(PSX_NATIVE_PAD_LEGACY_DEST) &&
            !psx_native_pad_read_legacy(cpu, NULL)) {
            fprintf(stderr, "native BIOS PAD provider gap: invalid legacy button buffer\n");
            exit(1);
        }
    }
    if (cpu->read_word(PSX_NATIVE_SIO_AUTO_ACK))
        cpu->write_word(0x1f801070u, ~(1u << IRQ_VBLANK));
    if (!cpu->read_word(PSX_NATIVE_CARD_STARTED)) return;
    static const uint32_t specs[] = {CARD_SUCCESS, CARD_ERROR, 0x100, 0x200, CARD_NEW};
    for (unsigned i = 0; i < sizeof specs / sizeof specs[0]; ++i)
        psx_native_bios_event(cpu, CARD_EVENT, specs[i], 0);
    unsigned port = 1 - (cpu->read_word(PSX_NATIVE_CARD_PORT) & 1u);
    cpu->write_word(PSX_NATIVE_CARD_PORT, port);
    uint32_t status_address = PSX_NATIVE_CARD_STATUS + port * 4;
    uint32_t status = cpu->read_word(status_address);
    if (status & 1u) return;

    uint32_t request = card_request(port);
    int file_read = status == CARD_FILE_READ, file_write = status == CARD_FILE_WRITE;
    uint32_t file = cpu->read_word(request + CARD_REQ_FILE);
    uint32_t fd = (file_read || file_write) && psx_ram_resolve(file, PSX_NATIVE_FILE_BYTES, NULL)
        ? file_word(cpu, file, 40) : 0;
    uint32_t spec = complete_request(cpu, port, status);
    int backup_info = cpu->read_word(request + CARD_REQ_BACKUP_INFO) != 0;
    cpu->write_word(PSX_NATIVE_CARD_LAST_DEVICE,
                    cpu->read_word(request + CARD_REQ_DEVICE));
    cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 0);
    cpu->write_word(status_address, spec == CARD_ERROR ? 0x21 : 1);
    cpu->write_word(request + CARD_REQ_BACKUP_INFO, 0);
    if (spec == CARD_NEW)
        cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4, 0);
    if (file_write && spec == CARD_SUCCESS)
        psx_native_bios_event(cpu, fd, CARD_SUCCESS, 1);
    if (backup_info || file_read || file_write)
        psx_native_bios_event(cpu, BACKUP_EVENT, spec, 1);
    if (file_read && spec == CARD_SUCCESS)
        psx_native_bios_event(cpu, fd, CARD_SUCCESS, 1);
    psx_native_bios_event(cpu, CARD_EVENT, spec, 1);
}
