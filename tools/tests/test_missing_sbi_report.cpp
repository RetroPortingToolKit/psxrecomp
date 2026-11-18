#undef NDEBUG
#include "sbi_setup.h"
#include "start_refusal.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    char report[8192];
    // The listed LibCrypt serials, warning metadata only.
    const char* listed_serials[] = {"SLES-02529", "SLES-01301", "SLES-02328",
        "SLES-12328", "SLES-22328", "SLES-03324", "SLES-03530", "SCES-02105"};
    for (const char* serial : listed_serials) {
        const auto missing = PSXRecompV4::missing_sbi_warning("listed.chd", serial, false);
        assert(missing.find("Missing SBI: " + std::string(serial)) == 0);
        assert(missing.find("listed.chd") != std::string::npos);
        assert(PSXRecompV4::missing_sbi_warning("listed.chd", serial, true).empty());
    }
    for (const char* serial : PSXRecompV4::kKnownLibcryptSerials) {
        const auto missing = PSXRecompV4::missing_sbi_warning("disc.cue", serial, false);
        assert(missing.find("Missing SBI: " + std::string(serial)) == 0);
        assert(missing.find('\n') == std::string::npos);
        psx_disc_warning_set(missing.c_str());
        psx_start_refusal_set("fixture", "fixture", "fixture");
        psx_start_refusal_reset(); // first guest instruction must retain warning
        assert(psx_start_refusal_kind() == nullptr);
        assert(psx_disc_warning_json(report, sizeof(report)) > 0);
        assert(std::strstr(report, "Missing SBI") != nullptr);
        std::cout << report << '\n';
        const auto loaded = PSXRecompV4::missing_sbi_warning("disc.cue", serial, true);
        assert(loaded.empty());
        psx_disc_warning_set(loaded.c_str());
        psx_disc_warning_json(report, sizeof(report));
        assert(std::strcmp(report, "null") == 0);
    }
    assert(PSXRecompV4::missing_sbi_warning("disc.cue", "SCUS-99999", false).empty());
    psx_disc_warning_set("Missing SBI: D:\\private\\disc.cue");
    psx_disc_warning_json(report, sizeof(report));
    assert(std::strstr(report, "private") == nullptr);
    char small[5];
    psx_disc_warning_json(small, sizeof(small));
    assert(std::strcmp(small, "null") == 0);
    psx_disc_warning_set("");
    psx_disc_warning_json(report, sizeof(report));
    assert(std::strcmp(report, "null") == 0);
    std::cout << "MISSING_SBI_REPORT_CONTROLS_PASS\n";
}
