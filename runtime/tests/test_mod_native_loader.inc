/* Independent synthetic-disc fixture: do not hide an earlier unrelated
 * scheduler-unwind failure by claiming that the full runtime suite ran. */
const auto root = fs::temp_directory_path() / "psxrecomp-native-loader-bridge-test";
std::error_code ec;
fs::remove_all(root, ec);
std::string error;
std::vector<uint8_t> iso(24 * 2048);
iso[16 * 2048] = 1;
std::copy_n("CD001", 5, iso.begin() + 16 * 2048 + 1);
iso[16 * 2048 + 6] = 1;
const auto disc_root = root / "disc";
const auto iso_path = disc_root / "original.iso";
write_bytes(iso_path, iso);
check(psx_mod_register_activation_plugin("runtime.test-vblank", test_activation_plugin),
      "register bridge simulation plugin");
const auto loader_root = root / "native-loader-bridge";
write_text(loader_root / "packages/runtime.loading/1.0.0/manifest.toml",
    "format_version = 5\nid = \"runtime.loading\"\nversion = \"1.0.0\"\n"
    "name = \"Resident Loading\"\n[[target]]\ngame_id = \"SLUS-RUNTIME\"\n"
    "disc_sha256 = \"" + sha256_hex(iso) + "\"\n"
    "[[feature]]\nid = \"resident\"\nname = \"Resident Loading\"\n"
    "default_enabled = true\n[[plugin]]\nfeature = \"resident\"\n"
    "id = \"runtime.net-loader\"\n");
/* Offline co-op may remain enabled in the owner's defaults. The negotiated
 * plan must select only its declared loader and executable-owned simulation. */
write_text(loader_root / "packages/runtime.offline/1.0.0/manifest.toml",
    "format_version = 5\nid = \"runtime.offline\"\nversion = \"1.0.0\"\n"
    "name = \"Offline Co-op\"\n[[target]]\ngame_id = \"SLUS-RUNTIME\"\n"
    "[[feature]]\nid = \"coop\"\nname = \"Offline Co-op\"\ndefault_enabled = true\n"
    "[[plugin]]\nfeature = \"coop\"\nid = \"runtime.unregistered-offline\"\n");
static const PSXModNetplayProfile loader_profile = {
    "runtime.test-vblank", "runtime-loader-bridge-v1", 0, 0, 1, 7u,
    "runtime.net-renderer", nullptr, 0, nullptr, nullptr, nullptr, nullptr,
    "runtime.loading", "resident", "runtime.net-loader"
};
check(psx_mod_register_netplay_profile(&loader_profile), "loader bridge profile registration");
test_match_caps = {};
check(PSXRecompV4::mod_runtime_initialize(loader_root, "SLUS-RUNTIME", 0x80002000, {}, &error),
      "fresh default-on exact-disc loader catalog");
check(PSXRecompV4::mod_runtime_plan_fingerprint_portable().empty(),
      "loader offer refuses unknown image before first commit");
check(!PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error) && !psx_mod_netplay_is_active(),
      "loader plugin must be registered before activation");
static unsigned loader_activations;
check(psx_mod_register_activation_plugin("runtime.net-loader", []() { ++loader_activations; }),
      "register loader bridge native plugin");
check(PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error), error.c_str());
check(psx_mod_netplay_loading() && !psx_mod_netplay_savestates_supported(),
      "default-on loader preserves modern active snapshot policy");
mod_runtime_activate_plugins();
check(loader_activations == 1, "native loader activates exactly once");
const auto direct_fingerprint = PSXRecompV4::mod_runtime_session_plan_fp();
check(direct_fingerprint.size() == 64 &&
      PSXRecompV4::mod_runtime_plan_fingerprint_portable() == direct_fingerprint,
      "default direct commit and portable identity agree");
char enabled_identity[65], disabled_identity[65];
check(psx_mod_netplay_content_identity("", enabled_identity), "loader SHA content identity");
PSXRecompV4::mod_runtime_end_netplay();
check(!psx_mod_netplay_loading(), "ending loader session clears loading policy");
check(PSXRecompV4::mod_runtime_initialize(loader_root, "SLUS-RUNTIME", 0x80002000, {}, &error),
      "fresh lobby catalog");
const auto offer = PSXRecompV4::mod_runtime_plan_fingerprint_portable(iso_path, &error);
check(offer == direct_fingerprint, "lobby resolves selected disc before offline commit");
test_match_caps.valid = 1;
test_match_caps.mod_count = 1;
std::strcpy(test_match_caps.mods[0].id, "runtime.loading");
std::strcpy(test_match_caps.mods[0].ver, "1.0.0");
std::strcpy(test_match_caps.mods[0].feats, "resident");
std::strcpy(test_match_caps.mod_plan_fp, offer.c_str());
check(PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error), "verified loader offer activates");
test_match_caps.mod_plan_fp[0] ^= 1;
check(!PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error) &&
      !psx_mod_netplay_is_active(), "tampered loader offer rejected");
test_match_caps.mod_plan_fp[0] ^= 1;
std::strcpy(test_match_caps.mods[0].id, "runtime.test");
check(!PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error), "unrelated package rejected");
std::strcpy(test_match_caps.mods[0].id, "runtime.loading");
std::strcpy(test_match_caps.mods[0].feats, "resident,dynamic-main");
check(!PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error), "extra loader features rejected");
std::strcpy(test_match_caps.mods[0].feats, "resident");
auto wrong_iso = iso;
wrong_iso.back() ^= 1;
const auto wrong_iso_path = disc_root / "wrong-loader-revision.iso";
write_bytes(wrong_iso_path, wrong_iso);
check(PSXRecompV4::mod_runtime_plan_fingerprint_portable(wrong_iso_path, &error).empty() &&
      !PSXRecompV4::mod_runtime_commit_netplay(wrong_iso_path, &error),
      "wrong actual revision cannot offer or activate loader");
check(PSXRecompV4::mod_runtime_plan_fingerprint_portable(iso_path, &error) == offer,
      "correct image recovers after refused offer");
write_bytes(iso_path, wrong_iso);
check(PSXRecompV4::mod_runtime_plan_fingerprint_portable().empty() &&
      !PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error),
      "same-path replacement is remeasured, never stale identity");
check(!PSXRecompV4::mod_runtime_commit_netplay({}, &error) &&
      PSXRecompV4::mod_runtime_plan_fingerprint_portable(disc_root / "missing.iso", &error).empty(),
      "missing image cannot reuse prior loader identity");
write_bytes(iso_path, iso);
test_match_caps = {};
test_match_caps.valid = 1;
check(PSXRecompV4::mod_runtime_commit_netplay(iso_path, &error), "host disables loader on guest");
check(!psx_mod_netplay_loading(), "disabled loader stays disabled");
mod_runtime_activate_plugins();
check(loader_activations == 1, "disabled loader does not activate again");
check(psx_mod_netplay_content_identity("", disabled_identity) &&
      std::strcmp(enabled_identity, disabled_identity), "loader policies cannot handshake as equal");
PSXRecompV4::mod_runtime_end_netplay();
check(!fs::exists(loader_root / "state.toml"), "loader negotiation never rewrites offline preferences");
fs::remove_all(root, ec);
if (failures) return 1;
std::cout << "native loader bridge tests passed\n";
return 0;
