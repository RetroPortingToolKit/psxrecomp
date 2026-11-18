/* text_source_guard.h — which pages of the loaded boot executable differ from
 * the image the static game code was generated from.
 *
 * The static game functions are generated from one boot executable. A
 * multi-disc set can carry that program on every disc with a per-disc
 * difference: Star Wars: Rebel Assault II loads the disc number with a
 * different immediate on each disc. When the player boots a disc other than
 * the one the build was generated from, the console loads that disc's bytes,
 * and a static function compiled from the other disc's bytes is wrong for it.
 *
 * The text-image guard cannot see this by itself: an installed product takes
 * its reference image from the mounted disc, so RAM and reference agree.
 * The emitter therefore records a CRC-32 per 4 KiB RAM page of the image it
 * generated from (before [[recompiler.patch]] edits), and this function says
 * which pages of the loaded image differ. Code on such a page must run from
 * RAM in the interpreter.
 */
#ifndef PSXRECOMP_TEXT_SOURCE_GUARD_H
#define PSXRECOMP_TEXT_SOURCE_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_TEXT_SOURCE_PAGE_SHIFT 12u

/* Set one bit per 4 KiB RAM page (bit index = phys >> 12) where `image`
 * differs from the generated-from image.
 *
 *   image, phys_lo, len      the boot executable image the console loads, and
 *                            where it loads (physical address, byte length)
 *   source_crc32, count      per-page CRC-32 of the generated-from image; page
 *                            0 starts at source_phys_lo, later pages at RAM
 *                            page boundaries, the last page may be partial
 *   source_phys_lo, len      where the generated-from image loads, and its size
 *
 * A different load address means a different program: every page of `image`
 * is marked. A loaded image shorter than the generated-from image marks the
 * pages it does not cover. Bytes of `image` past the generated-from image are
 * not compared; no static code exists for them.
 *
 * With no table (count == 0 or source_crc32 == NULL) nothing is marked: a
 * dispatcher generated before the table existed keeps today's behaviour.
 *
 * Returns the number of pages newly marked. *first_phys, when not NULL,
 * receives the physical address of the lowest marked page (0 if none). */
uint32_t psx_text_source_mismatch(const uint8_t *image, uint32_t phys_lo,
                                  uint32_t len,
                                  const uint32_t *source_crc32,
                                  uint32_t source_count,
                                  uint32_t source_phys_lo, uint32_t source_len,
                                  uint32_t *bitmap, uint32_t bitmap_words,
                                  uint32_t *first_phys);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_TEXT_SOURCE_GUARD_H */
