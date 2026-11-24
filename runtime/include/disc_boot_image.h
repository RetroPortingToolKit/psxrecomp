/* disc_boot_image.h — the boot executable's image on the mounted disc.
 *
 * The text image guard (main.cpp arm_text_image_guard) takes its reference
 * bytes from here in an installed product, which has no local EXE. */
#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct PsxDiscBootHeader {
    uint32_t entry_pc;
    uint32_t global_pointer;
    uint32_t text_address;
    uint32_t text_size;
    uint32_t fill_address;
    uint32_t fill_size;
    uint32_t stack_start;
    uint32_t stack_size;
};

/* The boot EXE image (without its 2048-byte header) on the mounted disc, or
 * nullptr. The caller owns the malloc'd buffer. `exe_path` is the game.toml
 * exe field as resolved by the caller and may be empty. `out_name` receives
 * the path of the file on the disc. If requested, out_header receives the
 * decoded launch fields when the header and declared text are readable and
 * describe a PS-X EXE in the RAM window. The text guard can still use readable
 * bytes from files with a nonstandard or incomplete launch header. */
uint8_t *psx_read_disc_boot_image(const std::string &exe_path,
                                  const std::string &disc_path,
                                  uint32_t *out_len,
                                  std::string *out_name,
                                  std::optional<PsxDiscBootHeader> *out_header = nullptr);
