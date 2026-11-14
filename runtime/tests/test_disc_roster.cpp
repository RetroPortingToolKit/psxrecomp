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

    // An image the set's disc list does not know. A copy of one of
    // the set's discs stays accepted, whatever it is called and whatever its
    // place in the list; another game's disc is not listed.
    const std::vector<std::string> two_discs = {"SLUS-00544", "SLUS-00556"};
    const std::vector<std::string> four_discs = {"SLPS-00700", "SLPS-00701", "SLPS-00702", "SLPS-00703"};
    for (const auto& set_serials : {two_discs, four_discs}) {
        // Every order of the list: the judgement does not depend on a disc's place.
        std::vector<std::string> order = set_serials;
        for (size_t turn = 0; turn < order.size(); ++turn) {
            for (const std::string& disc : set_serials) {
                CHECK(disc_roster_judge_serial(order, order.size(), disc) == DiscSetSerial::Listed);
                std::string lower = disc;
                for (char& c : lower) c = (char)std::tolower((unsigned char)c);
                CHECK(disc_roster_judge_serial(order, order.size(), lower) == DiscSetSerial::Listed);
            }
            CHECK(disc_roster_judge_serial(order, order.size(), "SLES-03398") == DiscSetSerial::NotListed);
            CHECK(disc_roster_judge_serial(order, order.size(), "SLUS-0054") == DiscSetSerial::NotListed);
            // Nothing read: not listed, and so the wrong disc.
            CHECK(disc_roster_judge_serial(order, order.size(), "") == DiscSetSerial::NotListed);
            order.push_back(order.front());
            order.erase(order.begin());
        }
    }
    CHECK(disc_roster_serial_list(two_discs) == "SLUS-00544, SLUS-00556");
    CHECK(disc_roster_serial_list(four_discs) == "SLPS-00700, SLPS-00701, SLPS-00702, SLPS-00703");
    CHECK(disc_roster_serial_list({"SLUS-00544", ""}) == "SLUS-00544");
    // A kit built for a single-disc game: game.toml carries `discs` and `disc_serials` of ONE entry
    // beside `id`. It takes the list path, so every other file the player
    // selects is "an image the list does not know". Before that image was
    // judged, nothing was expected of it: another game's disc got a tick and
    // PLAY was lit. The lines below are the launcher's own decisions
    // (ae_disc_verify), with that kit's lists and two sample files.
    {
        struct Kit { const char* disc; const char* other; const char* copy; const char* renamed; bool native; };
        const Kit kits[] = {
            // as game.toml has them on Windows
            {"Z:\\Emulators\\PS1 Games\\Diablo (Europe) (En,Fr,De,Sv).chd",
             "Z:\\Emulators\\PS1 Games\\Alien Resurrection (Europe) (En,Fr,De,Es,It).chd",
             "D:\\My discs\\diablo-pal.chd",
             "D:\\My discs\\Diablo (Europe) (En,Fr,De,Sv).cue",
#if defined(_WIN32)
             true},
#else
             false},   // a backslash is not a separator here: the folder is part of the name
#endif
            {"/games/ps1/Diablo (Europe) (En,Fr,De,Sv).chd",
             "/games/ps1/Alien Resurrection (Europe) (En,Fr,De,Es,It).chd",
             "/home/player/diablo-pal.chd",
             "/home/player/Diablo (Europe) (En,Fr,De,Sv).cue", true},
        };
        const std::vector<std::string> one_serial_list = {"SLES-01156"};
        const std::string id = "SLES-01156";
        for (const Kit& kit : kits) {
            const std::vector<std::filesystem::path> one_disc = {kit.disc};
            // What the launcher decides for `file` when `read` is the serial on it.
            struct Decision { std::string expected; DiscSetSerial outside; bool wrong; };
            const auto decide = [&](const char* file, const std::string& read, bool readable) {
                Decision d;
                d.expected = disc_roster_expected_serial(one_disc, one_serial_list, file, id);
                d.outside = disc_roster_outside_list(one_disc, one_serial_list, file,
                                                     !d.expected.empty(), readable, read);
                d.wrong = disc_roster_wrong_disc(d.outside);
                return d;
            };
            // Another game's disc: nothing is expected of it by the list, so it
            // is judged by the serial read. Not listed, the wrong disc.
            Decision d = decide(kit.other, "SLES-02913", true);
            CHECK(d.expected.empty());
            CHECK(d.outside == DiscSetSerial::NotListed);
            CHECK(d.wrong);
            CHECK(disc_roster_serial_list(one_serial_list) == "SLES-01156");   // the sentence's "needs"
            // The kit's own image: its entry's serial is expected, as before.
            d = decide(kit.disc, "SLES-01156", true);
            CHECK(d.expected == "SLES-01156" && d.outside == DiscSetSerial::NotJudged && !d.wrong);
            // A copy of the right disc under another name: listed, accepted.
            d = decide(kit.copy, "SLES-01156", true);
            CHECK(d.expected.empty() && d.outside == DiscSetSerial::Listed && !d.wrong);
            // The same file name in another folder is the list's image: judged
            // by its entry, so another game's disc under that name is refused
            // by the expected serial (identify_disc compares it).
            d = decide(kit.renamed, "SLES-02913", true);
            if (kit.native)
                CHECK(d.expected == "SLES-01156" && d.outside == DiscSetSerial::NotJudged);
            else   // then it is an image outside the list, and refused as one
                CHECK(d.expected.empty() && d.wrong);
            // No serial found (a data disc that is not a game): not listed,
            // and refused, as a build with no list refuses it. The
            // sentence is the second one.
            d = decide(kit.other, "", true);
            CHECK(d.outside == DiscSetSerial::NotListed && d.wrong);
            CHECK(disc_roster_wrong_disc_sentence("", disc_roster_serial_list(one_serial_list))
                  == "No serial was found on this disc. This build needs SLES-01156.");
            // The kit's own image on which no serial is found is judged by its
            // entry (the disc check looks for that serial on the disc), never
            // by this rule.
            d = decide(kit.disc, "", true);
            CHECK(d.expected == "SLES-01156" && d.outside == DiscSetSerial::NotJudged && !d.wrong);
            // A file that did not open says nothing more: its ISO header row
            // is the reason, and the verdict is already "bad".
            d = decide(kit.other, "", false);
            CHECK(d.outside == DiscSetSerial::NotJudged && !d.wrong);
        }
        // A build with no list at all expects its own serial of every image.
        const std::vector<std::filesystem::path> none;
        const std::vector<std::string> no_list;
        CHECK(disc_roster_expected_serial(none, no_list, kits[0].other, id) == id);
        CHECK(disc_roster_outside_list(none, no_list, kits[0].other, true, true, "SLES-02913")
              == DiscSetSerial::NotJudged);
        // A disc of the list expects its own entry's serial, not the boot disc's.
        const std::vector<std::string> re2 = {"SLUS-00748", "SLUS-00756"};
        const std::vector<std::filesystem::path> re2_discs = {"d/RE2 (Disc 1).chd", "d/RE2 (Disc 2).chd"};
        CHECK(disc_roster_expected_serial(re2_discs, re2, re2_discs[1], "SLUS-00748") == "SLUS-00756");
        CHECK(disc_roster_expected_serial(re2_discs, re2, re2_discs[0], "SLUS-00748") == "SLUS-00748");
    }

    // A kit that lists a serial in the boot file's spelling. In its own slot
    // the disc check finds that spelling on the disc and accepts it; a copy of
    // the same disc under another name was compared letter by letter with the
    // serial read ("SLUS-00594") and refused. Both sides now take one form.
    CHECK(disc_roster_serial_key("SLUS_005.94") == "SLUS-00594");
    CHECK(disc_roster_serial_key("slus-00594") == "SLUS-00594");
    CHECK(disc_roster_serial_key("SLUS_005.94;1") == "SLUS-00594");
    CHECK(disc_roster_serial_key("SLUS 00594") == "SLUS-00594");
    CHECK(disc_roster_serial_key("SLUS-0059") == "SLUS-0059");     // not four letters and five digits: as written
    CHECK(disc_roster_serial_key("12345-ABCD") == "12345-ABCD");
    CHECK(disc_roster_serial_key("demo") == "DEMO");
    CHECK(disc_roster_serial_key("").empty());
    CHECK(disc_roster_judge_serial({"SLUS_005.94"}, 1, "SLUS-00594") == DiscSetSerial::Listed);
    CHECK(disc_roster_judge_serial({"SLUS_005.94"}, 1, "SLUS-00595") == DiscSetSerial::NotListed);
    CHECK(disc_roster_judge_serial({"slus_005.94", "SLUS_006.26"}, 2, "SLUS-00626") == DiscSetSerial::Listed);
    CHECK(disc_roster_judge_serial({"SLUS_005.94", "SLUS_006.26"}, 2, "SLES-02913") == DiscSetSerial::NotListed);
    CHECK(disc_roster_judge_serial({"SLUS_005.94"}, 1, "") == DiscSetSerial::NotListed);
    {
        // The whole decision for such a kit: the copy is listed, another game's disc is the wrong disc.
        const std::vector<std::filesystem::path> one_disc = {"/games/ps1/Game (USA).chd"};
        const std::vector<std::string> exe_spelling = {"SLUS_005.94"};
        CHECK(disc_roster_expected_serial(one_disc, exe_spelling, "/games/ps1/Game (USA).chd", "SLUS-00594")
              == "SLUS_005.94");   // its own slot: the spelling the disc check searches the disc for
        const DiscSetSerial copy = disc_roster_outside_list(one_disc, exe_spelling, "/elsewhere/copy.chd",
                                                            false, true, "SLUS-00594");
        CHECK(copy == DiscSetSerial::Listed && !disc_roster_wrong_disc(copy));
        const DiscSetSerial other = disc_roster_outside_list(one_disc, exe_spelling, "/elsewhere/other.chd",
                                                             false, true, "SLES-02913");
        CHECK(other == DiscSetSerial::NotListed && disc_roster_wrong_disc(other));
    }
    // The sentence shows the list in the form the disc's own serial is shown in.
    CHECK(disc_roster_serial_list({"SLUS_005.94", "slus-00626"}) == "SLUS-00594, SLUS-00626");
    CHECK(disc_roster_serial_list({"demo"}) == "demo");

    // For sets: an image outside the list on which no serial is found is
    // the wrong disc when the list has a serial for every disc, whatever the
    // list's length. A list with a gap judges nothing, so it refuses nothing.
    for (const auto& complete : {std::vector<std::string>{"SLES-01156"}, two_discs, four_discs}) {
        const DiscSetSerial none = disc_roster_judge_serial(complete, complete.size(), "");
        CHECK(none == DiscSetSerial::NotListed && disc_roster_wrong_disc(none));
        CHECK(disc_roster_wrong_disc(disc_roster_judge_serial(complete, complete.size(), "SLES-02913")));
        CHECK(!disc_roster_wrong_disc(disc_roster_judge_serial(complete, complete.size(), complete.back())));
    }
    CHECK(!disc_roster_wrong_disc(disc_roster_judge_serial({"SLUS-00544", ""}, 2, "")));
    CHECK(!disc_roster_wrong_disc(disc_roster_judge_serial({"SLUS-00544"}, 2, "")));
    CHECK(!disc_roster_wrong_disc(DiscSetSerial::NotJudged));
    CHECK(!disc_roster_wrong_disc(DiscSetSerial::Listed));
    // A set's own discs in their slots are judged by their entries: the disc
    // list knows them, so this rule is never asked.
    {
        const std::vector<std::filesystem::path> set_discs = {"d/Game (Disc 1).chd", "d/Game (Disc 2).chd"};
        for (const auto& slot : set_discs)
            for (const char* read : {"", "SLUS-00544", "SLES-02913"})
                CHECK(disc_roster_outside_list(set_discs, two_discs, slot, true, true, read)
                      == DiscSetSerial::NotJudged);
        // The same file name in another folder is the list's image too.
        CHECK(disc_roster_outside_list(set_discs, two_discs, "elsewhere/Game (Disc 2).cue",
                                       true, true, "") == DiscSetSerial::NotJudged);
        // Outside the list, with no serial: refused.
        CHECK(disc_roster_wrong_disc(disc_roster_outside_list(set_discs, two_discs, "elsewhere/unknown.chd",
                                                              false, true, "")));
        // A file that did not open is not this rule's: its own message stands.
        CHECK(!disc_roster_wrong_disc(disc_roster_outside_list(set_discs, two_discs, "elsewhere/unknown.chd",
                                                               false, false, "")));
    }

    // The sentences of a refused disc, word for word as the launcher's panel
    // has them.
    CHECK(disc_roster_wrong_disc_sentence("SLES-02913", "SLES-01156")
          == "This disc is SLES-02913. This build needs SLES-01156.");
    CHECK(disc_roster_wrong_disc_sentence("SLES-03398", disc_roster_serial_list(two_discs))
          == "This disc is SLES-03398. This build needs SLUS-00544, SLUS-00556.");
    CHECK(disc_roster_wrong_disc_sentence("", disc_roster_serial_list(two_discs))
          == "No serial was found on this disc. This build needs SLUS-00544, SLUS-00556.");
    CHECK(disc_roster_wrong_disc_sentence("SLES-03398", "")
          == "This disc is SLES-03398. This build is made for another disc.");
    CHECK(disc_roster_wrong_disc_sentence("", "").empty());   // nothing known: nothing to say
    // A build with no list names its one serial in the same form.
    CHECK(disc_roster_wrong_disc_sentence("SLES-02913", disc_roster_serial_list({"sles_011.56"}))
          == "This disc is SLES-02913. This build needs SLES-01156.");

    // A set that does not list a serial for every disc cannot judge: a copy of
    // the unlisted disc would be called wrong. Nor can a build with no list at
    // all: it expects its own serial and never comes here.
    CHECK(disc_roster_judge_serial({"SLUS-00544", ""}, 2, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({"SLUS-00544"}, 2, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({"SLUS-00544"}, 2, "SLUS-00544") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({}, 0, "SLES-03398") == DiscSetSerial::NotJudged);
    CHECK(disc_roster_judge_serial({}, 2, "") == DiscSetSerial::NotJudged);

    if (failures) return 1;
    std::puts("PASS duplicate paths, serial/fingerprint identity, index selection, relocation controls and images outside the set");
}
