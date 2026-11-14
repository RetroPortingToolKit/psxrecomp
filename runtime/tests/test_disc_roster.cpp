#include "disc_roster.h"
#include <cstdio>

using namespace PSXRecompV4;
static int failures;
#define CHECK(test) do { if (!(test)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); ++failures; } } while (0)

int main() {
    const std::vector<std::filesystem::path> duplicate = {
        "cache/a/disc.cue", "cache/b/disc.cue"};
    const std::vector<std::string> serials = {"SLUS-00544", "SLUS-00556"};
    CHECK(disc_roster_index(duplicate, "cache/a/disc.bin") == 0);
    CHECK(disc_roster_index(duplicate, "cache/b/disc.bin") == 1);
    CHECK(disc_roster_index(duplicate, "cache/b/../a/disc.bin") == 0);
    CHECK(disc_roster_index(duplicate, "relocated/disc.bin") == -1);
    CHECK(disc_roster_value(duplicate, serials, "cache/a/disc.bin", "") == serials[0]);
    CHECK(disc_roster_value(duplicate, serials, "cache/b/disc.bin", "") == serials[1]);
    CHECK(disc_roster_value(duplicate, serials, "relocated/disc.bin", "") == "");
    CHECK(disc_roster_selected(duplicate, 1, "cache/b/disc.bin") == duplicate[0]);
    CHECK(disc_roster_selected(duplicate, 2, "cache/a/disc.bin") == duplicate[1]);
    CHECK(disc_roster_selected(duplicate, 2, "cache/b/disc.bin") == "cache/b/disc.bin");
    CHECK(disc_roster_selected(duplicate, 0, "custom/disc.bin") == "custom/disc.bin");
    CHECK(disc_roster_selected(duplicate, 3, "custom/disc.bin") == "custom/disc.bin");
    CHECK(disc_roster_selected({duplicate[0]}, 1, "custom/disc.bin") == "custom/disc.bin");
    const std::vector<std::filesystem::path> named = {"a/Game (Disc 1).cue", "b/Game (Disc 2).cue"};
    CHECK(disc_roster_selected(named, 2, "moved/Game (Disc 2).bin") == "moved/Game (Disc 2).bin");
    CHECK(disc_roster_index(named, "moved/Game (Disc 1).chd") == 0);
    CHECK(disc_roster_value(duplicate, {"fp1", "fp2"}, "cache/b/disc.bin", "flat") == "fp2");
    CHECK(disc_roster_value(duplicate, {"fp1"}, "cache/b/disc.bin", "flat") == "flat");
    CHECK(disc_roster_value(duplicate, {"fp1", ""}, "cache/b/disc.bin", "flat") == "flat");
    if (failures) return 1;
    std::puts("PASS duplicate paths, serial/fingerprint identity, index selection and relocation controls");
}
