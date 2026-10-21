#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace PSXRecompV4 {
// Language-independent authoring data. Stable IDs identify a message's role;
// original text and source digest detect stale adapters/catalogs independently.
struct NativeTextEntry {
    std::string id, original, translation, source_sha256;
};
class NativeTextCatalog {
public:
    bool load(const std::string& json, const std::string& language, std::string& error);
    const NativeTextEntry* find(const std::string& id) const;
    const std::map<std::string, NativeTextEntry>& entries() const { return entries_; }
private:
    std::map<std::string, NativeTextEntry> entries_;
};

using NativeGlyphMap = std::map<uint32_t,uint16_t>;
bool native_text_utf8(const std::string& text, std::vector<uint32_t>& codepoints,
                      std::string& error);

struct NativeTextLayout {
    uint32_t columns=20, rows=3;
    uint16_t space=0, line_end=0, page_end=0, text_end=0;
    uint16_t style=0, progressive=0;
};
// Encodes into the original game's glyph/control codes. Wraps at word
// boundaries; pages are explicit \f or generated when the native box fills.
// Unsupported glyphs and overlong words fail, never silently lose accents.
bool native_text_encode(const std::string& text, const NativeGlyphMap& glyphs,
                         const NativeTextLayout& layout, std::vector<uint16_t>& words,
                         std::string& error);

// Native 4-bpp indexed atlas. New Latin glyphs copy the source font's cells,
// including its palette indices and outline; marks use that font's own ink.
// Destination cells must be blank. No host font or presentation overlay.
struct NativeFontAtlas {
    uint32_t width=256,height=256,cell_width=16,cell_height=16;
    std::vector<uint8_t> packed;
};
enum class NativeGlyphMark { Acute, Tilde, Diaeresis, Inverted };
bool native_font_compose(NativeFontAtlas& atlas, uint16_t base, uint16_t destination,
                          NativeGlyphMark mark, std::string& error);
} // namespace PSXRecompV4
