#pragma once
#include <filesystem>
#include <string>

namespace PSXRecompV4 {
/* Preserve the effective choices of the retired built-in setting packages.
 * Only absent Settings keys are filled; state.toml and native saves are read
 * only. No catalog/manifests are needed. Repeated calls do not rewrite a file
 * after its choices have been preserved. An optional title config may declare
 * its former PGXP-disabled baseline when moving that baseline to Settings. */
bool preserve_legacy_player_settings(const std::filesystem::path& settings,
                                     std::string* error = nullptr,
                                     const std::filesystem::path& title_config = {});
}
