#include "mod_packages.h"
#include "psx_sha256.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include <cstdlib>

namespace fs = std::filesystem;
using namespace PSXRecompV4;

static int failures;
static int session_resets;

static void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

static void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

static void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
}

static std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    uint8_t digest[32];
    psx_sha256_compute(bytes.data(), bytes.size(), digest);
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 15];
    }
    return out;
}

static void write_deflated_package(const fs::path& path) {
    static const char* compressed_hex =
        "4bcb2fca4d2c892f4b2d2acecccf53b05530e4ca4c01524a5599057ab9f929"
        "4a5c082925433d033d0325aebcc4dc541037ca3340c117a4a428b5383f07a80"
        "e2498929a9c93589458925996aac4151d5d9258949e5a121bcb950ed4140f313"
        "ad827345837c4353844890b00";
    std::vector<uint8_t> compressed;
    for (const char* p = compressed_hex; *p; p += 2)
        compressed.push_back((uint8_t)std::stoul(std::string(p, 2), nullptr, 16));
    std::vector<uint8_t> zip;
    auto le16 = [&](uint16_t v) {
        zip.push_back((uint8_t)v); zip.push_back((uint8_t)(v >> 8));
    };
    auto le32 = [&](uint32_t v) {
        le16((uint16_t)v); le16((uint16_t)(v >> 16));
    };
    const std::string name = "manifest.toml";
    le32(0x04034b50); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0);
    zip.insert(zip.end(), name.begin(), name.end());
    zip.insert(zip.end(), compressed.begin(), compressed.end());
    const uint32_t central_offset = (uint32_t)zip.size();
    le32(0x02014b50); le16(20); le16(20); le16(0); le16(8); le16(0); le16(0);
    le32(0x7d8454e1); le32((uint32_t)compressed.size()); le32(127);
    le16((uint16_t)name.size()); le16(0); le16(0); le16(0); le16(0);
    le32(0); le32(0);
    zip.insert(zip.end(), name.begin(), name.end());
    const uint32_t central_size = (uint32_t)zip.size() - central_offset;
    le32(0x06054b50); le16(0); le16(0); le16(1); le16(1);
    le32(central_size); le32(central_offset); le16(0);
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)zip.data(), (std::streamsize)zip.size());
}

static std::string manifest(const std::string& id, const std::string& version,
                            const std::string& extra = {}) {
    return
        "format_version = 1\n"
        "id = \"" + id + "\"\n"
        "version = \"" + version + "\"\n"
        "name = \"" + id + "\"\n"
        "author = \"Test Author\"\n"
        "source_name = \"Upstream project\"\n"
        "source_url = \"https://example.com/project\"\n"
        "resolver = \"declarative\"\n"
        "[[author_link]]\n"
        "name = \"Test Author\"\n"
        "url = \"https://example.com/author\"\n"
        "[[target]]\n"
        "game_id = \"SLUS-TEST\"\n" + extra;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "psxrecomp-mod-package-test";
    std::error_code ec;
    fs::remove_all(root, ec);

    {
        const uint8_t abc[] = {'a', 'b', 'c'};
        uint8_t one_shot[32], streamed[32];
        psx_sha256_compute(abc, sizeof(abc), one_shot);
        psx_sha256_ctx hash;
        psx_sha256_init(&hash);
        psx_sha256_update(&hash, abc, 1);
        psx_sha256_update(&hash, abc + 1, 2);
        psx_sha256_final(&hash, streamed);
        check(std::equal(one_shot, one_shot + 32, streamed),
              "streaming SHA-256 must match one-shot hashing");
    }

    write_text(root / "packages/base.mod/1.0.0/manifest.toml",
               manifest("base.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"difficulty\"\n"
                   "label = \"Difficulty\"\n"
                   "type = \"choice\"\n"
                   "default = \"normal\"\n"
                   "[[option.choice]]\nvalue = \"normal\"\nlabel = \"Normal\"\n"
                   "[[option.choice]]\nvalue = \"hard\"\nlabel = \"Hard\"\n"
                   "[[patch]]\n"
                   "target = \"main_exe\"\n"
                   "address = 2147487744\n"
                   "expected = \"01 02 03 04\"\n"
                   "replace = \"05 06 07 08\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"
                   "[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/base.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 123456\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"
                   "when_option = \"difficulty\"\n"
                   "when_value = \"hard\"\n"));
    write_text(root / "packages/base.mod/1.0.0/assets/base.xdelta3", "test");
    write_text(root / "packages/addon.mod/2.0.0/manifest.toml",
               manifest("addon.mod", "2.0.0",
                   "\n[[dependency]]\nid = \"base.mod\"\nversion = \"^1.0.0\"\n"));

    ModPackageManager manager(root);
    std::string error;
    check(manager.scan(&error), error.c_str());
    const ModPackage& metadata_package =
        manager.packages().at("base.mod").at("1.0.0");
    check(metadata_package.source_url == "https://example.com/project",
          "package source URL must be retained");
    check(metadata_package.author_links.size() == 1 &&
              metadata_package.author_links[0].name == "Test Author" &&
              metadata_package.author_links[0].url == "https://example.com/author",
          "package author links must be retained");
    write_deflated_package(root / "zip.psxmod");
    check(manager.install_archive(root / "zip.psxmod", nullptr, nullptr, &error),
          error.c_str());
    check(manager.packages().count("zip.mod") == 1,
          "deflated .psxmod must install");
    if (const char* external = std::getenv("PSXMOD_TEST_ARCHIVE");
        external && external[0]) {
        std::string installed_id, installed_version;
        check(manager.install_archive(external, &installed_id, &installed_version,
                                      &error),
              error.c_str());
        check(!installed_id.empty() && !installed_version.empty(),
              "external package must report installed identity");
    }
    check(manager.load_state(&error), error.c_str());
    check(manager.set_enabled("addon.mod", true, &error), error.c_str());
    ModResolution missing = manager.resolve("SLUS-TEST");
    check(!missing.ok, "missing dependency must fail resolution");
    check(manager.set_enabled("base.mod", true, &error), error.c_str());
    check(manager.set_option("base.mod", "difficulty", "hard", &error), error.c_str());
    check(!manager.set_option("base.mod", "difficulty", "impossible", &error),
          "invalid choice must be rejected");

    ModResolution resolved = manager.resolve("SLUS-TEST");
    check(resolved.ok, "valid dependency graph must resolve");
    check(resolved.ordered.size() == 2, "two packages should resolve");
    check(resolved.ordered.size() == 2 && resolved.ordered[0]->id == "base.mod",
          "dependency must precede dependent");
    check(resolved.writes.size() == 1, "selected declarative patch must resolve");
    check(resolved.writes.size() == 1 &&
              resolved.writes[0].location == 0x80001000ull &&
              resolved.writes[0].replacement[0] == 5,
          "resolved write must retain guest address and bytes");
    check(resolved.derived_discs.size() == 1 &&
              resolved.derived_discs[0].output_size == 123456,
          "selected derived-disc recipe must resolve");
    check(resolved.fingerprint.size() == 64, "plan fingerprint must be SHA-256 hex");
    const std::string fingerprint = resolved.fingerprint;

    check(manager.save_state(&error), error.c_str());
    ModPackageManager reload(root);
    check(reload.scan(&error), error.c_str());
    check(reload.load_state(&error), error.c_str());
    check(reload.resolve("SLUS-TEST").fingerprint == fingerprint,
          "saved state must resolve deterministically");
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "active package cannot be removed");
    check(reload.set_enabled("base.mod", false, &error), error.c_str());
    check(!reload.remove_version("base.mod", "1.0.0", &error),
          "enabled dependent must protect required version");
    check(reload.set_enabled("addon.mod", false, &error), error.c_str());
    check(reload.remove_version("base.mod", "1.0.0", &error), error.c_str());

    write_text(root / "packages/conflict.a/1.0.0/manifest.toml",
               "format_version = 1\n"
               "id = \"conflict.a\"\n"
               "version = \"1.0.0\"\n"
               "name = \"conflict.a\"\n"
               "resolver = \"declarative\"\n"
               "conflicts = [\"conflict.b\"]\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n");
    write_text(root / "packages/conflict.b/1.0.0/manifest.toml",
               manifest("conflict.b", "1.0.0"));
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", true, &error), error.c_str());
    check(reload.set_enabled("conflict.b", true, &error), error.c_str());
    check(reload.selections().at("conflict.a").enabled &&
              reload.selections().at("conflict.b").enabled,
          "enabling a package must not silently disable another package");
    check(!reload.resolve("SLUS-TEST").ok,
          "declared conflicts must fail resolution");

    write_text(root / "packages/matrix.mod/1.0.0/manifest.toml",
               manifest("matrix.mod", "1.0.0",
                   "\n[[option]]\n"
                   "id = \"title\"\n"
                   "label = \"Title\"\n"
                   "type = \"choice\"\n"
                   "default = \"mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"mega\"\n"
                   "label = \"Mega\"\n"
                   "[[option.choice]]\n"
                   "value = \"rockman\"\n"
                   "label = \"Rockman\"\n"
                   "\n[[option]]\n"
                   "id = \"script\"\n"
                   "label = \"Script\"\n"
                   "type = \"choice\"\n"
                   "default = \"original\"\n"
                   "[[option.choice]]\n"
                   "value = \"original\"\n"
                   "label = \"Original\"\n"
                   "[[option.choice]]\n"
                   "value = \"retranslation\"\n"
                   "label = \"Retranslation\"\n"
                   "\n[[derived_disc]]\n"
                   "kind = \"vcdiff\"\n"
                   "patch = \"assets/matrix.xdelta3\"\n"
                   "patch_sha256 = \"2222222222222222222222222222222222222222222222222222222222222222\"\n"
                   "output_size = 222222\n"
                   "output_sha256 = \"3333333333333333333333333333333333333333333333333333333333333333\"\n"
                   "when = { title = \"rockman\", script = \"retranslation\" }\n"));
    write_text(root / "packages/matrix.mod/1.0.0/assets/matrix.xdelta3", "test");
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("conflict.a", false, &error), error.c_str());
    check(reload.set_enabled("conflict.b", false, &error), error.c_str());
    check(reload.set_enabled("matrix.mod", true, &error), error.c_str());
    check(reload.set_option("matrix.mod", "title", "rockman", &error), error.c_str());
    check(reload.set_option("matrix.mod", "script", "retranslation", &error), error.c_str());
    ModResolution matrix = reload.resolve("SLUS-TEST");
    check(matrix.ok && matrix.derived_discs.size() == 1 &&
              matrix.derived_discs[0].output_size == 222222,
          "multi-option derived-disc condition must match selected values");

    mod_clear_builtin_resolvers_for_tests();
    bool resolver_context_seen = false;
    check(mod_register_builtin_resolver(
              "context-test",
              [&](const ModPackage& package, const ModSelection& selection,
                  const ModBuiltinResolverContext& context,
                  std::vector<ModResolution::Write>& writes,
                  std::vector<std::string>& errors) {
                  (void)writes;
                  (void)errors;
                  resolver_context_seen =
                      package.id == "context.consumer" &&
                      selection.enabled &&
                      context.active_packages &&
                      context.selections &&
                      context.active_packages->count("context.provider") == 1 &&
                      context.active_packages->count("context.consumer") == 1 &&
                      context.selections->at("context.provider").enabled &&
                      context.selections->at("context.consumer").enabled;
                  return resolver_context_seen;
              }),
          "test resolver must register");
    write_text(root / "packages/context.provider/1.0.0/manifest.toml",
               manifest("context.provider", "1.0.0"));
    write_text(root / "packages/context.consumer/1.0.0/manifest.toml",
               "format_version = 1\n"
               "id = \"context.consumer\"\n"
               "version = \"1.0.0\"\n"
               "name = \"context.consumer\"\n"
               "resolver = \"builtin:context-test\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_enabled("matrix.mod", false, &error), error.c_str());
    check(reload.set_enabled("context.provider", true, &error), error.c_str());
    check(reload.set_enabled("context.consumer", true, &error), error.c_str());
    ModResolution context_resolution = reload.resolve("SLUS-TEST");
    check(context_resolution.ok && resolver_context_seen,
          "built-in resolver must receive active package selection context");
    check(reload.set_enabled("context.consumer", false, &error), error.c_str());
    check(reload.set_enabled("context.provider", false, &error), error.c_str());
    mod_clear_builtin_resolvers_for_tests();

    mod_clear_plugins_for_tests();
    check(mod_register_vblank_plugin(
              "test.vblank", +[]() {}),
          "test plugin must register");
    check(mod_register_session_reset_plugin(
              "test.vblank", +[]() { ++session_resets; }),
          "session cleanup must register independently of active features");
    check(!mod_register_session_reset_plugin("test.vblank", +[]() {}),
          "duplicate session cleanup must be rejected");
    // No manifest or active plan exists yet: deselected plugins still clean
    // up the host state left by the previous session.
    mod_reset_plugin_sessions();
    mod_reset_plugin_sessions();
    check(session_resets == 2, "each session boundary must invoke registered cleanup");
    write_text(root / "packages/plugin.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"plugin.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Plugin Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"vblank\"\n"
               "name = \"VBlank Plugin\"\n"
               "[[plugin]]\n"
               "feature = \"vblank\"\n"
               "id = \"test.vblank\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", true, &error), error.c_str());
    ModResolution plugin_resolution = reload.resolve("SLUS-TEST");
    check(plugin_resolution.ok && plugin_resolution.plugins.size() == 1 &&
              plugin_resolution.plugins[0].id == "test.vblank" &&
              plugin_resolution.plugins[0].package_id == "plugin.mod" &&
              plugin_resolution.plugins[0].feature_id == "vblank",
          "enabled trusted plugin must resolve with feature ownership");
    const std::string plugin_fingerprint = plugin_resolution.fingerprint;
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", false, &error), error.c_str());
    ModResolution plugin_disabled = reload.resolve("SLUS-TEST");
    check(plugin_disabled.ok && plugin_disabled.plugins.empty() &&
              plugin_disabled.fingerprint != plugin_fingerprint,
          "disabling a plugin feature must remove its activation and change "
          "the fingerprint");
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", true, &error), error.c_str());
    mod_clear_plugins_for_tests();
    ModResolution plugin_unavailable = reload.resolve("SLUS-TEST");
    check(!plugin_unavailable.ok &&
              std::any_of(
                  plugin_unavailable.errors.begin(),
                  plugin_unavailable.errors.end(),
                  [](const std::string& item) {
                      return item.find(
                          "trusted plugin is unavailable: test.vblank") !=
                          std::string::npos;
                  }),
          "enabled plugin must fail closed when its trusted implementation "
          "is unavailable");
    check(reload.set_feature_enabled(
              "plugin.mod", "vblank", false, &error), error.c_str());

    /* A function-entry hook is selected by id exactly like activation and
     * VBlank plugins; a manifest naming only such an implementation must
     * resolve, and must still fail closed once it is gone. */
    const PSXModFunctionEntryCallback entry_hook = +[](CPUState*, uint32_t) {};
    check(mod_register_function_entry_plugin("test.entry", 0x80010000u, entry_hook),
          "function-entry plugin must register");
    check(!mod_register_function_entry_plugin(
              "test.entry", 0x80010000u, +[](CPUState*, uint32_t) {}),
          "duplicate function-entry hook must be rejected");
    check(!mod_register_function_entry_plugin(
              "test.entry", 0x00010000u, +[](CPUState*, uint32_t) {}),
          "a KUSEG alias of a hooked function is the same hook");
    write_text(root / "packages/entry.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"entry.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Entry Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"entry\"\n"
               "name = \"Entry Plugin\"\n"
               "[[plugin]]\n"
               "feature = \"entry\"\n"
               "id = \"test.entry\"\n");
    check(reload.scan(&error), error.c_str());
    check(reload.set_feature_enabled("entry.mod", "entry", true, &error),
          error.c_str());
    ModResolution entry_resolution = reload.resolve("SLUS-TEST");
    check(entry_resolution.ok && entry_resolution.plugins.size() == 1 &&
              entry_resolution.plugins[0].id == "test.entry",
          "enabled function-entry plugin must resolve");
    const std::vector<ModFunctionEntryHook> entry_hooks =
        mod_function_entry_hooks("test.entry");
    check(entry_hooks.size() == 1 && entry_hooks[0].address == 0x80010000u &&
              entry_hooks[0].callback == entry_hook,
          "an implementation exposes exactly the hooks it registered");
    check(mod_function_entry_hooks("test.missing").empty(),
          "an unregistered implementation exposes no hooks");
    /* Audit: a hook registered under a sub-id of the package's declared
     * plugin can never be activated by a plan and must be reported. */
    check(mod_register_function_entry_plugin("test.entry.hud", 0x80020000u, entry_hook),
          "sub-id function-entry plugin must register");
    {
        const fs::path audit_root = root / "audit-authored";
        write_text(audit_root / "entry.mod/1.0.0/manifest.toml",
                   "format_version = 5\n"
                   "id = \"entry.mod\"\n"
                   "version = \"1.0.0\"\n"
                   "name = \"Entry Mod\"\n"
                   "resolver = \"declarative\"\n"
                   "[[target]]\n"
                   "game_id = \"SLUS-TEST\"\n"
                   "[[feature]]\n"
                   "id = \"entry\"\n"
                   "name = \"Entry Plugin\"\n"
                   "[[plugin]]\n"
                   "feature = \"entry\"\n"
                   "id = \"test.entry\"\n");
        const ModPluginAudit audit = mod_audit_registered_plugins({audit_root});
        check(audit.errors.empty() && audit.manifests == 1,
              "audit must read every authored manifest");
        check(std::find(audit.declared.begin(), audit.declared.end(),
                        "test.entry") != audit.declared.end(),
              "audit must collect declared [[plugin]] ids");
        check(audit.undeclared.size() == 1 && audit.undeclared[0] == "test.entry.hud",
              "audit must report exactly the registered-but-undeclared id");
        const ModPluginAudit missing = mod_audit_registered_plugins({root / "absent"});
        check(!missing.errors.empty(), "audit must report an unreadable root");
    }
    mod_clear_plugins_for_tests();
    ModResolution entry_unavailable = reload.resolve("SLUS-TEST");
    check(!entry_unavailable.ok,
          "enabled function-entry plugin must fail closed when unregistered");
    check(reload.set_feature_enabled("entry.mod", "entry", false, &error),
          error.c_str());
    write_text(root / "packages/features.mod/1.0.0/manifest.toml",
               manifest("features.mod", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"title-screen\"\n"
                   "name = \"Title Screen\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"retranslation\"\n"
                   "name = \"Retranslation\"\n"
                   "group = \"Localization\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-collision\"\n"
                   "name = \"Title Collision\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-identical\"\n"
                   "name = \"Title Identical\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-partial-compatible\"\n"
                   "name = \"Title Partial Compatible\"\n"
                   "\n[[feature]]\n"
                   "id = \"title-partial-conflict\"\n"
                   "name = \"Title Partial Conflict\"\n"
                   "\n[[option]]\n"
                   "feature = \"title-screen\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"choice\"\n"
                   "default = \"usa\"\n"
                   "[[option.choice]]\n"
                   "value = \"usa\"\n"
                   "label = \"Mega Man X6\"\n"
                   "[[option.choice]]\n"
                   "value = \"japan\"\n"
                   "label = \"Rockman X6\"\n"
                   "\n[[option]]\n"
                   "feature = \"retranslation\"\n"
                   "id = \"variant\"\n"
                   "label = \"Variant\"\n"
                   "type = \"boolean\"\n"
                   "default = \"true\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-screen\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"
                   "when = { variant = \"japan\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"retranslation\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 23520\n"
                   "expected = \"03\"\n"
                   "replace = \"b3\"\n"
                   "when = { variant = \"true\" }\n"
                   "\n[[patch]]\n"
                   "feature = \"title-collision\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"02\"\n"
                   "replace = \"ff\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-identical\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495936\n"
                   "expected = \"0102\"\n"
                   "replace = \"a1a2\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-partial-compatible\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"0209\"\n"
                   "replace = \"a2c9\"\n"
                   "\n[[patch]]\n"
                   "feature = \"title-partial-conflict\"\n"
                   "target = \"main_exe\"\n"
                   "address = 2147495937\n"
                   "expected = \"ff09\"\n"
                   "replace = \"a2c9\"\n"));
    check(reload.scan(&error), error.c_str());
    check(!reload.set_enabled("features.mod", true, &error),
          "feature-style package must not expose package enablement");
    check(reload.set_feature_option(
              "features.mod", "title-screen", "variant", "japan", &error),
          error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-screen", true, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "retranslation", true, &error), error.c_str());
    ModResolution features = reload.resolve("SLUS-TEST");
    check(features.ok && features.writes.size() == 2,
          "independently enabled features must compose their operations");
    check(features.ok && features.writes[0].feature_id == "title-screen" &&
              features.writes[1].feature_id == "retranslation",
          "resolved writes must retain feature ownership");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", true, &error), error.c_str());
    ModResolution collision = reload.resolve("SLUS-TEST");
    check(!collision.ok && collision.diagnostics.size() == 1,
          "overlapping feature writes must produce a structured diagnostic");
    check(!collision.diagnostics.empty() &&
              collision.diagnostics[0].feature_id == "title-collision" &&
              collision.diagnostics[0].other_feature_id == "title-screen" &&
              !collision.diagnostics[0].resource.empty(),
          "collision diagnostic must identify both features and the resource");
    check(reload.set_feature_enabled(
              "features.mod", "title-collision", false, &error), error.c_str());
    check(reload.set_feature_enabled(
              "features.mod", "title-identical", true, &error), error.c_str());
    ModResolution identical = reload.resolve("SLUS-TEST");
    check(identical.ok && identical.writes.size() == 2,
          "truly identical writes must coalesce deterministically");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-compatible", true, &error),
          error.c_str());
    ModResolution partial_compatible = reload.resolve("SLUS-TEST");
    check(partial_compatible.ok && partial_compatible.writes.size() == 3,
          "partially overlapping writes with matching expected and replacement "
          "bytes must compose");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-conflict", true, &error),
          error.c_str());
    ModResolution partial_conflict = reload.resolve("SLUS-TEST");
    check(!partial_conflict.ok && !partial_conflict.diagnostics.empty() &&
              partial_conflict.diagnostics[0].resource ==
                  "main_exe:0x80003001-0x80003002",
          "one differing expected byte in a partial overlap must identify "
          "the exact contested byte");
    check(reload.set_feature_enabled(
              "features.mod", "title-partial-conflict", false, &error),
          error.c_str());
    check(reload.save_state(&error), error.c_str());
    ModPackageManager feature_reload(root);
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.load_state(&error), error.c_str());
    check(feature_reload.feature_enabled("features.mod", "title-screen") &&
              feature_reload.feature_enabled("features.mod", "retranslation") &&
              !feature_reload.feature_enabled("features.mod", "title-collision"),
          "per-feature enabled state must survive save/reload");
    check(feature_reload.feature_option_value(
              "features.mod", "title-screen", "variant") == "japan",
          "feature-scoped option values must survive save/reload");
    check(feature_reload.resolve("SLUS-TEST").fingerprint ==
              partial_compatible.fingerprint,
          "feature state must resolve deterministically after reload");

    fs::create_directories(root / "selected", ec);
    write_text(root / "selected/bezel.png", "not a decoded image");
    write_text(root / "packages/resource.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"resource.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Resource Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"bezel\"\n"
               "name = \"Bezel\"\n"
               "[[resource]]\n"
               "feature = \"bezel\"\n"
               "id = \"artwork\"\n"
               "label = \"Artwork\"\n"
               "description = \"Pick image\"\n"
               "file_patterns = \"*.png,*.jpg\"\n"
               "file_description = \"Image files\"\n"
               "required = false\n");
    write_text(root / "packages/required-resource.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"required-resource.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Required Resource Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"bezel\"\n"
               "name = \"Bezel\"\n"
               "[[resource]]\n"
               "feature = \"bezel\"\n"
               "id = \"artwork\"\n"
               "label = \"Artwork\"\n"
               "required = true\n");
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "resource.mod", "bezel", true, &error), error.c_str());
    ModResolution resource_unset = feature_reload.resolve("SLUS-TEST");
    check(resource_unset.ok && resource_unset.resources.empty(),
          "optional resources must be omitted when no path is selected");
    check(feature_reload.set_feature_resource_path(
              "resource.mod", "bezel", "artwork",
              root / "selected/bezel.png", &error), error.c_str());
    ModResolution resource_selected = feature_reload.resolve("SLUS-TEST");
    check(resource_selected.ok && resource_selected.resources.size() == 1 &&
              resource_selected.resources[0].id == "artwork" &&
              resource_selected.resources[0].path ==
                  root / "selected/bezel.png",
          "selected feature resource path must enter the committed plan");
    check(feature_reload.save_state(&error), error.c_str());
    ModPackageManager resource_reload(root);
    check(resource_reload.scan(&error), error.c_str());
    check(resource_reload.load_state(&error), error.c_str());
    check(resource_reload.feature_resource_path(
              "resource.mod", "bezel", "artwork") ==
              root / "selected/bezel.png",
          "feature resource paths must survive save/reload");
    check(resource_reload.set_feature_enabled(
              "required-resource.mod", "bezel", true, &error), error.c_str());
    ModResolution resource_required = resource_reload.resolve("SLUS-TEST");
    check(!resource_required.ok,
          "required enabled resources must reject launch while unset");
    check(resource_reload.set_feature_resource_path(
              "required-resource.mod", "bezel", "artwork",
              root / "selected/bezel.png", &error), error.c_str());
    check(resource_reload.resolve("SLUS-TEST").ok,
          "required enabled resources must resolve after selecting a path");

    /* A package-relative default supplies a required folder the package
     * ships; a player selection still wins, and clearing it restores it. */
    const fs::path default_pack = root / "installed/default-resource.mod/1.0.0/pack";
    fs::create_directories(default_pack, ec);
    fs::create_directories(root / "selected/pack", ec);
    write_text(root / "installed/default-resource.mod/1.0.0/manifest.toml",
               "format_version = 5\n"
               "id = \"default-resource.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Default Resource Mod\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"pack\"\n"
               "name = \"Pack\"\n"
               "default_enabled = true\n"
               "[[resource]]\n"
               "feature = \"pack\"\n"
               "id = \"pack\"\n"
               "label = \"Pack folder\"\n"
               "format = \"directory\"\n"
               "default = \"pack\"\n"
               "required = true\n");
    ModPackageManager default_resource(root);
    check(default_resource.scan(&error), error.c_str());
    check(default_resource.feature_resource_path(
              "default-resource.mod", "pack", "pack") == default_pack,
          "an unselected resource must use its package-relative default");
    ModResolution default_resolved = default_resource.resolve("SLUS-TEST");
    const bool default_in_plan = std::any_of(
        default_resolved.resources.begin(), default_resolved.resources.end(),
        [&](const ModResolution::Resource& r) {
            return r.package_id == "default-resource.mod" && r.path == default_pack;
        });
    check(default_in_plan,
          "a required resource with a default must resolve without a selection");
    check(default_resource.set_feature_resource_path(
              "default-resource.mod", "pack", "pack",
              root / "selected/pack", &error), error.c_str());
    check(default_resource.feature_resource_path(
              "default-resource.mod", "pack", "pack") == root / "selected/pack",
          "a selected resource path must override the package default");
    {
        /* Clearing the selection (state.toml holding an empty path, or no
         * entry at all) restores the package default. */
        auto sel = default_resource.selections();
        sel["default-resource.mod"].features["pack"].resources["pack"] = "";
        auto kept = default_resource.exchange_selections(sel);
        check(default_resource.feature_resource_path(
                  "default-resource.mod", "pack", "pack") == default_pack,
              "a cleared selection must restore the package default");
        sel["default-resource.mod"].features["pack"].resources.erase("pack");
        default_resource.exchange_selections(sel);
        check(default_resource.feature_resource_path(
                  "default-resource.mod", "pack", "pack") == default_pack,
              "a removed selection must restore the package default");
        default_resource.exchange_selections(kept);
    }
    {
        /* The default must exist, and its real path must stay inside the
         * package: a shipped symlink pointing outside is not followed. */
        ModPackageManager probe(root);
        fs::remove_all(default_pack, ec);
        check(probe.scan(&error), error.c_str());
        check(probe.feature_resource_path("default-resource.mod", "pack", "pack").empty(),
              "a missing default folder must not count as resolved");
        check(!probe.resolve("SLUS-TEST").ok,
              "a required resource whose default is missing must block launch");
        const fs::path outside = root / "outside-pack";
        fs::create_directories(outside, ec);
        fs::create_directory_symlink(outside, default_pack, ec);
        if (!ec) {
            ModPackageManager linked(root);
            check(linked.scan(&error), error.c_str());
            check(linked.feature_resource_path("default-resource.mod", "pack", "pack").empty(),
                  "a default that is a symlink out of the package must be rejected");
            check(!linked.resolve("SLUS-TEST").ok,
                  "a default escaping the package must not resolve");
            fs::remove(default_pack, ec);
        }
        fs::remove_all(outside, ec);
    }
    fs::remove_all(root / "installed/default-resource.mod", ec);
    {
        const fs::path unsafe_root = root / "unsafe-default";
        write_text(unsafe_root / "installed/unsafe.mod/1.0.0/manifest.toml",
                   "format_version = 5\n"
                   "id = \"unsafe.mod\"\n"
                   "version = \"1.0.0\"\n"
                   "name = \"Unsafe\"\n"
                   "resolver = \"declarative\"\n"
                   "[[target]]\n"
                   "game_id = \"SLUS-TEST\"\n"
                   "[[feature]]\n"
                   "id = \"pack\"\n"
                   "name = \"Pack\"\n"
                   "[[resource]]\n"
                   "feature = \"pack\"\n"
                   "id = \"pack\"\n"
                   "label = \"Pack folder\"\n"
                   "format = \"directory\"\n"
                   "default = \"../outside\"\n");
        ModPackageManager unsafe(unsafe_root);
        unsafe.scan(&error);
        check(!unsafe.scan_errors().empty(),
              "an unsafe resource default must be reported by the scan");
        check(unsafe.feature_resource_path("unsafe.mod", "pack", "pack").empty(),
              "a resource default outside the package must be rejected");
    }

    write_text(root / "packages/parametric.mod/1.0.0/manifest.toml",
               "format_version = 3\n"
               "id = \"parametric.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Parametric\"\n"
               "resolver = \"declarative\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"numeric\"\n"
               "name = \"Numeric\"\n"
               "[[feature]]\n"
               "id = \"numeric-collision\"\n"
               "name = \"Numeric Collision\"\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"byte\"\n"
               "label = \"Byte\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 255\n"
               "step = 1\n"
               "default = 7\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"word\"\n"
               "label = \"Word\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 65534\n"
               "step = 1\n"
               "default = 4660\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"dword\"\n"
               "label = \"Dword\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 4294967295\n"
               "step = 1\n"
               "default = 305419896\n"
               "[[option]]\n"
               "feature = \"numeric\"\n"
               "id = \"split\"\n"
               "label = \"Split\"\n"
               "type = \"integer\"\n"
               "min = 200000\n"
               "max = 600000\n"
               "step = 1\n"
               "default = 425984\n"
               "[[option]]\n"
               "feature = \"numeric-collision\"\n"
               "id = \"byte\"\n"
               "label = \"Byte\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 255\n"
               "default = 8\n"
               "[[constraint]]\n"
               "feature = \"numeric\"\n"
               "kind = \"ordered_integer\"\n"
               "direction = \"nondecreasing\"\n"
               "options = [\"byte\", \"word\", \"dword\"]\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500032\n"
               "expected = \"00\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500033\n"
               "expected = \"07\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500034\n"
               "expected = \"0000\"\n"
               "replace_from = { option = \"word\", encoding = \"u16le\", addend = 1 }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500036\n"
               "expected = \"00000000\"\n"
               "replace_from = { option = \"dword\", encoding = \"u32le\" }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500040\n"
               "expected = \"0000aabb\"\n"
               "replace_from = { option = \"word\", encoding = \"u16le\", offset = 0 }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500048\n"
               "expected = \"0600013c00802134\"\n"
               "replace_from = { option = \"split\", encoding = \"mips_lui_ori_u32\", omit_when_default = true }\n"
               "[[patch]]\n"
               "feature = \"numeric\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500056\n"
               "expected = \"0400013c00202134\"\n"
               "replace_from = { option = \"split\", encoding = \"mips_lui_ori_u32\", omit_when_default = true }\n"
               "[[patch]]\n"
               "feature = \"numeric-collision\"\n"
               "target = \"main_exe\"\n"
               "address = 2147500032\n"
               "expected = \"00\"\n"
               "replace_from = { option = \"byte\", encoding = \"u8\" }\n");
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error), error.c_str());
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "+7", &error),
          "integer options must reject a leading plus");
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "07", &error),
          "integer options must reject noncanonical leading zeroes");
    ModResolution parametric = feature_reload.resolve("SLUS-TEST");
    const auto numeric_write = [&](uint64_t location)
        -> const ModResolution::Write* {
        const auto found = std::find_if(
            parametric.writes.begin(), parametric.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "parametric.mod" &&
                       write.location == location;
            });
        return found == parametric.writes.end() ? nullptr : &*found;
    };
    const ModResolution::Write* byte_write = numeric_write(0x80004000ull);
    const ModResolution::Write* noop_write = numeric_write(0x80004001ull);
    const ModResolution::Write* word_write = numeric_write(0x80004002ull);
    const ModResolution::Write* dword_write = numeric_write(0x80004004ull);
    const ModResolution::Write* guarded_word_write =
        numeric_write(0x80004008ull);
    check(parametric.ok && byte_write &&
              byte_write->replacement == std::vector<uint8_t>({7}),
          "u8 replace_from must encode the selected value");
    check(!noop_write,
          "replace_from equal to the stock guard must elide the no-op write");
    check(word_write &&
              word_write->replacement == std::vector<uint8_t>({0x35, 0x12}),
          "u16le replace_from must apply addend and encode little-endian");
    check(dword_write &&
              dword_write->replacement ==
                  std::vector<uint8_t>({0x78, 0x56, 0x34, 0x12}),
          "u32le replace_from must encode little-endian");
    check(guarded_word_write &&
              guarded_word_write->replacement ==
                  std::vector<uint8_t>({0x34, 0x12, 0xaa, 0xbb}),
          "replace_from must preserve guarded bytes outside its value field");
    check(!numeric_write(0x80004010ull) &&
              !numeric_write(0x80004018ull),
          "omit_when_default must suppress every split-immediate site");
    const std::string parametric_fingerprint = parametric.fingerprint;
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "byte", "9", &error),
          error.c_str());
    check(!feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "5", &error),
          "enabled ordered integer features must reject inverted values");
    ModResolution changed_parametric = feature_reload.resolve("SLUS-TEST");
    check(changed_parametric.ok &&
              changed_parametric.fingerprint != parametric_fingerprint,
          "changing a generated integer must change the plan fingerprint");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "200000", &error),
          error.c_str());
    ModResolution split_parametric = feature_reload.resolve("SLUS-TEST");
    const auto split_write = [&](uint64_t location)
        -> const ModResolution::Write* {
        const auto found = std::find_if(
            split_parametric.writes.begin(), split_parametric.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "parametric.mod" &&
                       write.location == location;
            });
        return found == split_parametric.writes.end() ? nullptr : &*found;
    };
    check(split_write(0x80004010ull) &&
              split_write(0x80004010ull)->replacement ==
                  std::vector<uint8_t>({
                      0x03, 0x00, 0x01, 0x3c,
                      0x40, 0x0d, 0x21, 0x34}) &&
              split_write(0x80004018ull) &&
              split_write(0x80004018ull)->replacement ==
                  std::vector<uint8_t>({
                      0x03, 0x00, 0x01, 0x3c,
                      0x40, 0x0d, 0x21, 0x34}),
          "typed MIPS split encodings must update every guarded pair");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "270336", &error),
          error.c_str());
    ModResolution partial_stock_split =
        feature_reload.resolve("SLUS-TEST");
    check(std::count_if(
              partial_stock_split.writes.begin(),
              partial_stock_split.writes.end(),
              [](const ModResolution::Write& write) {
                  return write.package_id == "parametric.mod" &&
                         (write.location == 0x80004010ull ||
                          write.location == 0x80004018ull);
              }) == 2,
          "a nondefault split value must retain ownership of a pair whose "
          "replacement happens to equal stock");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "split", "425984", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric-collision", true, &error),
          error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "different generated values at one guarded byte must collide");
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric-collision", false, &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", false, &error), error.c_str());
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "0", &error),
          "disabled features may retain an invalid draft");
    check(!feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error),
          "an invalid ordered integer draft must block feature enablement");
    check(feature_reload.set_feature_option(
              "parametric.mod", "numeric", "word", "4660", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "parametric.mod", "numeric", true, &error), error.c_str());
    check(feature_reload.save_state(&error), error.c_str());
    ModPackageManager parametric_reload(root);
    check(parametric_reload.scan(&error), error.c_str());
    check(parametric_reload.load_state(&error), error.c_str());
    check(parametric_reload.feature_option_value(
              "parametric.mod", "numeric", "byte") == "9" &&
              parametric_reload.resolve("SLUS-TEST").fingerprint ==
                  changed_parametric.fingerprint,
          "generated integer state and fingerprint must survive reload");

    write_text(root / "packages/requires.mod/1.0.0/manifest.toml",
               "format_version = 4\n"
               "id = \"requires.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Requires\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"prereq\"\n"
               "name = \"Prerequisite\"\n"
               "[[feature]]\n"
               "id = \"dependent\"\n"
               "name = \"Dependent\"\n"
               "[[feature]]\n"
               "id = \"optioned-dependent\"\n"
               "name = \"Optioned Dependent\"\n"
               "[[option]]\n"
               "feature = \"prereq\"\n"
               "id = \"availability\"\n"
               "label = \"Available in\"\n"
               "type = \"choice\"\n"
               "default = \"main\"\n"
               "[[option.choice]]\n"
               "value = \"main\"\n"
               "label = \"Main Stages\"\n"
               "[[option.choice]]\n"
               "value = \"everywhere\"\n"
               "label = \"Everywhere\"\n"
               "[[constraint]]\n"
               "feature = \"dependent\"\n"
               "kind = \"requires_feature\"\n"
               "requires_feature = \"prereq\"\n"
               "[[constraint]]\n"
               "feature = \"optioned-dependent\"\n"
               "kind = \"requires_feature\"\n"
               "requires_feature = \"prereq\"\n"
               "requires_option = \"availability\"\n"
               "requires_value = \"everywhere\"\n");
    check(parametric_reload.scan(&error), error.c_str());
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "dependent", true, &error), error.c_str());
    check(parametric_reload.feature_enabled("requires.mod", "prereq") &&
              parametric_reload.feature_enabled("requires.mod", "dependent"),
          "enabling a dependent feature must auto-enable its prerequisite");
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "optioned-dependent", true, &error),
          error.c_str());
    check(parametric_reload.feature_enabled(
              "requires.mod", "optioned-dependent") &&
              parametric_reload.feature_option_value(
                  "requires.mod", "prereq", "availability") == "everywhere",
          "enabling an optioned dependent must auto-select the required "
          "prerequisite value");
    check(parametric_reload.set_feature_option(
              "requires.mod", "prereq", "availability", "main", &error),
          error.c_str());
    check(parametric_reload.feature_enabled("requires.mod", "prereq") &&
              parametric_reload.feature_enabled("requires.mod", "dependent") &&
              !parametric_reload.feature_enabled(
                  "requires.mod", "optioned-dependent"),
          "weakening a prerequisite option must disable invalid dependents");
    check(parametric_reload.set_feature_enabled(
              "requires.mod", "prereq", false, &error), error.c_str());
    check(!parametric_reload.feature_enabled("requires.mod", "prereq") &&
              !parametric_reload.feature_enabled("requires.mod", "dependent"),
          "disabling a prerequisite must disable downstream dependents");

    const auto reject_parametric_manifest =
        [&](const std::string& name, const std::string& body) {
            const fs::path path = root / (name + ".toml");
            write_text(path, body);
            ModPackage rejected;
            return !ModPackageManager::read_manifest(path, rejected, &error);
        };
    const std::string dynamic_prelude =
        "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
        "[[target]]\ngame_id=\"SLUS-TEST\"\n"
        "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
        "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
        "type=\"integer\"\nmin=0\nmax=255\ndefault=1\n";
    check(reject_parametric_manifest(
              "dynamic-v1",
              "format_version=1\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\"}\n"),
          "format 1 manifests must reject replace_from");
    check(reject_parametric_manifest(
              "dynamic-both",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\nreplace=\"01\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\"}\n"),
          "a patch must reject simultaneous replace and replace_from");
    check(reject_parametric_manifest(
              "dynamic-width",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0000\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",offset=2}\n"),
          "replace_from value must stay inside the expected guard");
    check(reject_parametric_manifest(
              "dynamic-overflow",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",addend=1}\n"),
          "the full option range plus addend must fit its encoding");
    check(reject_parametric_manifest(
              "dynamic-unknown",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"00\"\n"
                  "replace_from={option=\"value\",encoding=\"u8\",shift=1}\n"),
          "replace_from must reject unknown transform fields");
    check(reject_parametric_manifest(
              "dynamic-mips-v2",
              "format_version=2\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0600013c00802134\"\n"
                  "replace_from={option=\"value\","
                  "encoding=\"mips_lui_ori_u32\"}\n"),
          "typed MIPS pairs must require package format 3");
    check(reject_parametric_manifest(
              "dynamic-mips-unlinked",
              "format_version=3\n" + dynamic_prelude +
                  "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
                  "address=2147487744\nexpected=\"0600013c00802234\"\n"
                  "replace_from={option=\"value\","
                  "encoding=\"mips_lui_ori_u32\"}\n"),
          "typed MIPS pairs must reject unlinked registers");
    check(reject_parametric_manifest(
              "constraint-inverted-default",
              "format_version=3\n"
              "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-TEST\"\n"
              "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
              "[[option]]\nfeature=\"bad\"\nid=\"low\"\nlabel=\"Low\"\n"
              "type=\"integer\"\nmin=0\nmax=10\ndefault=8\n"
              "[[option]]\nfeature=\"bad\"\nid=\"high\"\nlabel=\"High\"\n"
              "type=\"integer\"\nmin=0\nmax=10\ndefault=2\n"
              "[[constraint]]\nfeature=\"bad\"\n"
              "kind=\"ordered_integer\"\ndirection=\"nondecreasing\"\n"
              "options=[\"low\",\"high\"]\n"),
          "ordered integer defaults must satisfy their constraint");
    check(reject_parametric_manifest(
              "dynamic-step-default",
              "format_version=2\n"
              "id=\"bad.dynamic\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
              "[[target]]\ngame_id=\"SLUS-TEST\"\n"
              "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
              "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
              "type=\"integer\"\nmin=0\nmax=10\nstep=2\ndefault=3\n"),
          "integer defaults must align to their declared step");

    write_text(root / "packages/sparse.mod/1.0.0/manifest.toml",
               "format_version = 4\n"
               "id = \"sparse.mod\"\n"
               "version = \"1.0.0\"\n"
               "name = \"Sparse Fields\"\n"
               "[[target]]\n"
               "game_id = \"SLUS-TEST\"\n"
               "[[feature]]\n"
               "id = \"timing\"\n"
               "name = \"Timing\"\n"
               "[[feature]]\n"
               "id = \"cancellable\"\n"
               "name = \"Cancellable\"\n"
               "[[feature]]\n"
               "id = \"collision\"\n"
               "name = \"Collision\"\n"
               "[[feature]]\n"
               "id = \"guard-mismatch\"\n"
               "name = \"Guard Mismatch\"\n"
               "[[feature]]\n"
               "id = \"predicates\"\n"
               "name = \"Predicates\"\n"
               "[[option]]\n"
               "feature = \"timing\"\n"
               "id = \"frames\"\n"
               "label = \"Frames\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 99\n"
               "default = 2\n"
               "[[option]]\n"
               "feature = \"predicates\"\n"
               "id = \"value\"\n"
               "label = \"Value\"\n"
               "type = \"integer\"\n"
               "min = 0\n"
               "max = 10\n"
               "default = 5\n"
               "[[patch]]\n"
               "feature = \"timing\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, option = \"frames\", encoding = \"u8\" }]\n"
               "when_integer = { option = \"frames\", op = \"gt\", value = 0 }\n"
               "[[patch]]\n"
               "feature = \"timing\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, replace = \"01\" }, "
               "{ offset = 2, replace = \"00\" }]\n"
               "when_integer = { option = \"frames\", op = \"eq\", value = 0 }\n"
               "[[patch]]\n"
               "feature = \"cancellable\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 1, replace = \"42\" }]\n"
               "[[patch]]\n"
               "feature = \"collision\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"02000132\"\n"
               "fields = [{ offset = 0, replace = \"09\" }]\n"
               "[[patch]]\n"
               "feature = \"guard-mismatch\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508224\n"
               "expected = \"03000132\"\n"
               "fields = [{ offset = 3, replace = \"33\" }]\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508480\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"eq\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508481\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"ne\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508482\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"lt\", value = 6 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508483\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"le\", value = 5 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508484\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"gt\", value = 4 }\n"
               "[[patch]]\n"
               "feature = \"predicates\"\n"
               "target = \"main_exe\"\n"
               "address = 2147508485\n"
               "expected = \"00\"\n"
               "fields = [{ replace = \"01\" }]\n"
               "when_integer = { option = \"value\", op = \"ge\", value = 5 }\n");
    check(feature_reload.scan(&error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "timing", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "cancellable", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "predicates", true, &error), error.c_str());
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "5", &error),
          error.c_str());
    ModResolution sparse_positive = feature_reload.resolve("SLUS-TEST");
    const auto sparse_writes_at = [&](const ModResolution& plan,
                                      uint64_t location) {
        return std::count_if(
            plan.writes.begin(), plan.writes.end(),
            [&](const ModResolution::Write& write) {
                return write.package_id == "sparse.mod" &&
                       write.location == location;
            });
    };
    check(sparse_positive.ok &&
              sparse_writes_at(sparse_positive, 0x80006000ull) == 2,
          "adjacent sparse fields in one guarded record must compose");
    const auto timing_write = std::find_if(
        sparse_positive.writes.begin(), sparse_positive.writes.end(),
        [](const ModResolution::Write& write) {
            return write.package_id == "sparse.mod" &&
                   write.feature_id == "timing" &&
                   write.location == 0x80006000ull;
        });
    check(timing_write != sparse_positive.writes.end() &&
              timing_write->expected ==
                  std::vector<uint8_t>({2, 0, 1, 0x32}) &&
              timing_write->replacement.empty() &&
              timing_write->fields.size() == 1 &&
              timing_write->fields[0].offset == 0 &&
              timing_write->fields[0].replacement ==
                  std::vector<uint8_t>({5}),
          "sparse resolution must retain the complete guard but own only "
          "declared fields");
    check(std::count_if(
              sparse_positive.writes.begin(),
              sparse_positive.writes.end(),
              [](const ModResolution::Write& write) {
                  return write.package_id == "sparse.mod" &&
                         write.feature_id == "predicates";
              }) == 5,
          "eq/ne/lt/le/gt/ge predicates must resolve with typed integer "
          "semantics");
    const std::string sparse_positive_fingerprint =
        sparse_positive.fingerprint;
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "0", &error),
          error.c_str());
    ModResolution sparse_zero = feature_reload.resolve("SLUS-TEST");
    const auto zero_timing = std::find_if(
        sparse_zero.writes.begin(), sparse_zero.writes.end(),
        [](const ModResolution::Write& write) {
            return write.package_id == "sparse.mod" &&
                   write.feature_id == "timing";
        });
    check(sparse_zero.ok && zero_timing != sparse_zero.writes.end() &&
              zero_timing->fields.size() == 2 &&
              zero_timing->fields[0].offset == 0 &&
              zero_timing->fields[0].replacement ==
                  std::vector<uint8_t>({1}) &&
              zero_timing->fields[1].offset == 2 &&
              zero_timing->fields[1].replacement ==
                  std::vector<uint8_t>({0}) &&
              sparse_zero.fingerprint != sparse_positive_fingerprint,
          "zero and nonzero conditional sparse plans must own their exact "
          "distinct fields and fingerprints");
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "2", &error),
          error.c_str());
    ModResolution sparse_stock = feature_reload.resolve("SLUS-TEST");
    check(sparse_stock.ok &&
              sparse_writes_at(sparse_stock, 0x80006000ull) == 1,
          "stock-equal sparse fields must elide only their own no-op while "
          "an adjacent feature remains active");
    check(feature_reload.set_feature_option(
              "sparse.mod", "timing", "frames", "5", &error),
          error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "collision", true, &error), error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "different sparse replacements for one owned byte must collide");
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "collision", false, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "guard-mismatch", true, &error), error.c_str());
    check(!feature_reload.resolve("SLUS-TEST").ok,
          "overlapping complete guards with incompatible expected bytes "
          "must fail before runtime");
    check(feature_reload.set_feature_enabled(
              "sparse.mod", "guard-mismatch", false, &error),
          error.c_str());

    const std::string sparse_prelude =
        "id=\"bad.sparse\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
        "[[target]]\ngame_id=\"SLUS-TEST\"\n"
        "[[feature]]\nid=\"bad\"\nname=\"Bad\"\n"
        "[[option]]\nfeature=\"bad\"\nid=\"value\"\nlabel=\"Value\"\n"
        "type=\"integer\"\nmin=0\nmax=10\nstep=2\ndefault=2\n";
    const std::string sparse_patch =
        "[[patch]]\nfeature=\"bad\"\ntarget=\"main_exe\"\n"
        "address=2147487744\nexpected=\"00000000\"\n";
    check(reject_parametric_manifest(
              "sparse-v3",
              "format_version=3\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"),
          "sparse fields must require format 4");
    check(reject_parametric_manifest(
              "sparse-empty",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[]\n"),
          "sparse fields must not be empty");
    check(reject_parametric_manifest(
              "sparse-both",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "replace=\"01000000\"\n"
                  "fields=[{offset=0,replace=\"01\"}]\n"),
          "sparse fields must be mutually exclusive with full replace");
    check(reject_parametric_manifest(
              "sparse-overlap",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"0102\"},"
                  "{offset=1,replace=\"03\"}]\n"),
          "sparse fields must reject overlapping owned ranges");
    check(reject_parametric_manifest(
              "sparse-bounds",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=4,replace=\"01\"}]\n"),
          "sparse fields must stay inside the complete guard");
    check(reject_parametric_manifest(
              "sparse-mixed",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\",option=\"value\","
                  "encoding=\"u8\"}]\n"),
          "one sparse field must not mix literal and dynamic forms");
    check(reject_parametric_manifest(
              "sparse-overflow",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,option=\"value\",encoding=\"u8\","
                  "addend=250}]\n"),
          "sparse dynamic field ranges plus addends must fit encoding");
    check(reject_parametric_manifest(
              "sparse-predicate-op",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"between\",value=2}\n"),
          "typed integer predicates must reject unknown operations");
    check(reject_parametric_manifest(
              "sparse-predicate-feature",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"missing\",op=\"eq\",value=2}\n"),
          "typed integer predicates must reference same-feature integers");
    check(reject_parametric_manifest(
              "sparse-predicate-bounds",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"gt\",value=11}\n"),
          "typed integer predicate constants must stay in option bounds");
    check(reject_parametric_manifest(
              "sparse-predicate-step",
              "format_version=4\n" + sparse_prelude + sparse_patch +
                  "fields=[{offset=0,replace=\"01\"}]\n"
                  "when_integer={option=\"value\",op=\"eq\",value=3}\n"),
          "typed equality predicates must use selectable values");

    const std::vector<uint8_t> overlay_a = {1, 2, 3, 4};
    const std::vector<uint8_t> overlay_b = {8, 9};
    const std::vector<uint8_t> overlay_c = {3, 4, 7};
    const std::string overlay_disc_hash(64, '4');
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/a.bin", overlay_a);
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/b.bin", overlay_b);
    write_bytes(root / "packages/overlay.mod/1.0.0/assets/c.bin", overlay_c);
    write_text(root / "packages/overlay.mod/1.0.0/manifest.toml",
               manifest("overlay.mod", "1.0.0",
                   "disc_sha256 = \"" + overlay_disc_hash + "\"\n"
                   "[[feature]]\n"
                   "id = \"asset-a\"\n"
                   "name = \"Asset A\"\n"
                   "[[feature]]\n"
                   "id = \"asset-b\"\n"
                   "name = \"Asset B\"\n"
                   "[[feature]]\n"
                   "id = \"asset-c\"\n"
                   "name = \"Asset C\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-a\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 100\n"
                   "file = \"assets/a.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_a) + "\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-b\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 102\n"
                   "file = \"assets/b.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_b) + "\"\n"
                   "[[overlay]]\n"
                   "feature = \"asset-c\"\n"
                   "target = \"disc_raw\"\n"
                   "offset = 102\n"
                   "file = \"assets/c.bin\"\n"
                   "sha256 = \"" + sha256_hex(overlay_c) + "\"\n"));
    check(feature_reload.scan(&error), error.c_str());
    const ModPackage* overlay_package =
        feature_reload.selected_package("overlay.mod");
    check(overlay_package && overlay_package->overlays.size() == 3 &&
              overlay_package->overlays[0].size == overlay_a.size(),
          "manifest scan must verify and retain overlay metadata");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-a", true, &error), error.c_str());
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-c", true, &error), error.c_str());
    ModResolution overlay_compatible =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(overlay_compatible.ok && overlay_compatible.overlays.size() == 2,
          "partially overlapping file overlays with matching payload bytes "
          "must compose");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", true, &error), error.c_str());
    ModResolution overlay_collision =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(!overlay_collision.ok &&
              overlay_collision.diagnostics.size() == 1 &&
              overlay_collision.diagnostics[0].feature_id == "asset-b" &&
              overlay_collision.diagnostics[0].other_feature_id == "asset-a",
          "overlapping file overlays must identify both owning features");
    check(feature_reload.set_feature_enabled(
              "overlay.mod", "asset-b", false, &error), error.c_str());
    ModResolution one_overlay =
        feature_reload.resolve("SLUS-TEST", {}, overlay_disc_hash);
    check(one_overlay.ok && one_overlay.overlays.size() == 2 &&
              one_overlay.overlays[0].payload == overlay_a &&
              one_overlay.overlays[1].payload == overlay_c,
          "compatible enabled overlay payloads must remain in the plan");

    write_text(root / "feature-derived.toml",
               manifest("bad.derived", "1.0.0",
                   "\n[[feature]]\n"
                   "id = \"bad\"\n"
                   "name = \"Bad\"\n"
                   "[[derived_disc]]\n"
                   "patch = \"bad.xdelta3\"\n"
                   "patch_sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
                   "output_size = 1\n"
                   "output_sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"));
    ModPackage feature_derived;
    check(!ModPackageManager::read_manifest(
              root / "feature-derived.toml", feature_derived, &error),
          "feature-style packages must reject derived-disc operations");

    ModPackage invalid;
    write_text(root / "bad.toml",
               "format_version=1\nid=\"../bad\"\nversion=\"1.0.0\"\nname=\"Bad\"\n"
               "[[target]]\ngame_id=\"SLUS-TEST\"\n");
    check(!ModPackageManager::read_manifest(root / "bad.toml", invalid, &error),
          "unsafe package id must be rejected");

    /* Catalog roots. mods/bundled is build output that every build wipes and
     * re-stages; mods/installed belongs to the launcher and a build never
     * touches it. Before the split both lived in mods/packages, so a rebuild
     * deleted every mod the player had installed. */
    {
        const fs::path split_root = root / "split";
        /* Pre-split layout: a build-staged package and a player install,
         * indistinguishable in the same tree. */
        write_text(split_root / "packages/psx.builtin/1.0.0/manifest.toml",
                   manifest("psx.builtin", "1.0.0"));
        write_text(split_root / "packages/player.mod/1.0.0/manifest.toml",
                   manifest("player.mod", "1.0.0"));
        /* The current build has already staged its own copy of the builtin. */
        write_text(split_root / "bundled/psx.builtin/1.0.0/manifest.toml",
                   manifest("psx.builtin", "1.0.0"));

        ModPackageManager split(split_root);
        check(split.scan(&error), error.c_str());
        check(!fs::exists(split_root / "packages"),
              "the legacy mods/packages tree must be migrated away");
        check(fs::exists(split_root / "installed/player.mod/1.0.0/manifest.toml"),
              "a player-installed package must migrate into mods/installed");
        check(!fs::exists(split_root / "installed/psx.builtin"),
              "a package the build also staged must not migrate to installed");
        check(split.packages().at("psx.builtin").at("1.0.0").origin ==
                  ModPackageOrigin::Bundled,
              "a package under mods/bundled must be Bundled");
        check(split.packages().at("player.mod").at("1.0.0").origin ==
                  ModPackageOrigin::Installed,
              "a migrated player package must be Installed");

        check(!split.remove_version("psx.builtin", "1.0.0", &error),
              "a bundled package must not be removable");

        /* What a rebuild does: wipe bundled/ and re-stage it. The player's
         * installed catalog must survive untouched. */
        fs::remove_all(split_root / "bundled", ec);
        write_text(split_root / "bundled/psx.builtin/1.0.0/manifest.toml",
                   manifest("psx.builtin", "1.0.0"));
        ModPackageManager rebuilt(split_root);
        check(rebuilt.scan(&error), error.c_str());
        check(rebuilt.packages().count("player.mod") == 1,
              "a rebuild must not delete a player-installed package");
        check(rebuilt.packages().count("psx.builtin") == 1,
              "a rebuild must re-stage the bundled catalog");

        /* An installed package of the same id shadows the bundled one, and
         * says so rather than winning silently. */
        write_text(split_root / "installed/psx.builtin/1.0.0/manifest.toml",
                   manifest("psx.builtin", "1.0.0"));
        ModPackageManager shadowed(split_root);
        check(shadowed.scan(&error), error.c_str());
        const ModPackage& shadow =
            shadowed.packages().at("psx.builtin").at("1.0.0");
        check(shadow.origin == ModPackageOrigin::Installed &&
                  shadow.shadows_bundled,
              "an installed package must shadow the bundled one and record it");

        /* A manifest that cannot be parsed must be named, never skipped in
         * silence -- an author's typo used to produce a mod that simply did
         * not exist, with nothing anywhere explaining why. */
        write_text(split_root / "installed/broken.mod/1.0.0/manifest.toml",
                   "format_version = 5\nid = \"broken.mod\"\n");
        ModPackageManager broken(split_root);
        check(broken.scan(&error), error.c_str());
        check(broken.packages().count("broken.mod") == 0,
              "an unparseable package must not load");
        check(std::any_of(broken.scan_errors().begin(),
                          broken.scan_errors().end(),
                          [](const std::string& e) {
                              return e.find("broken.mod") != std::string::npos;
                          }),
              "an unparseable manifest must be reported by scan_errors()");
    }

    /* Channels. A feature carries its own maturity: a package is the trust and
     * installation boundary, and tying the two forced a whole catalog down to
     * the maturity of its least finished feature. */
    {
        const fs::path chan_root = root / "channel";
        const std::string mixed =
            "format_version = 6\n"
            "id = \"chan.mod\"\n"
            "version = \"1.0.0\"\n"
            "name = \"Channel Mod\"\n"
            "[[target]]\n"
            "game_id = \"SLUS-TEST\"\n"
            "[[feature]]\n"
            "id = \"solid\"\n"
            "name = \"Solid\"\n"
            "[[feature]]\n"
            "id = \"rough\"\n"
            "name = \"Rough\"\n"
            "channel = \"experimental\"\n"
            "[[feature]]\n"
            "id = \"probe\"\n"
            "name = \"Probe\"\n"
            "channel = \"developer\"\n"
            "[[option]]\n"
            "feature = \"probe\"\n"
            "id = \"depth\"\n"
            "label = \"Depth\"\n"
            "type = \"boolean\"\n"
            "default = \"false\"\n";
        write_text(chan_root / "bundled/chan.mod/1.0.0/manifest.toml", mixed);

        /* A local build sees all three. */
        ModPackageManager local(chan_root);
        local.set_developer_channel_visible(true);
        check(local.scan(&error), error.c_str());
        const ModPackage& all = local.packages().at("chan.mod").at("1.0.0");
        check(all.features.size() == 3, "a local build must see every channel");
        check(all.features[0].channel == ModChannel::Stable &&
                  all.features[1].channel == ModChannel::Experimental &&
                  all.features[2].channel == ModChannel::Developer,
              "each feature must carry its own channel");

        /* A release build must not carry the developer feature at all --
         * absent, not hidden -- nor the options that only served it. */
        ModPackageManager shipped(chan_root);
        shipped.set_developer_channel_visible(false);
        check(shipped.scan(&error), error.c_str());
        const ModPackage& published = shipped.packages().at("chan.mod").at("1.0.0");
        check(published.features.size() == 2,
              "a release build must drop developer-channel features");
        check(std::none_of(published.features.begin(), published.features.end(),
                           [](const ModFeature& f) { return f.id == "probe"; }),
              "the developer feature must be absent from a release build");
        check(std::none_of(published.options.begin(), published.options.end(),
                           [](const ModOption& o) { return o.feature_id == "probe"; }),
              "options serving a dropped feature must go with it");
        check(published.features[1].channel == ModChannel::Experimental,
              "an experimental feature still ships");

        /* A package whose every feature is developer-channel simply is not
         * there on a release build, rather than listing with nothing in it. */
        write_text(chan_root / "bundled/dev.only/1.0.0/manifest.toml",
                   "format_version = 6\n"
                   "id = \"dev.only\"\n"
                   "version = \"1.0.0\"\n"
                   "name = \"Dev Only\"\n"
                   "channel = \"developer\"\n"
                   "[[target]]\n"
                   "game_id = \"SLUS-TEST\"\n"
                   "[[feature]]\n"
                   "id = \"instrument\"\n"
                   "name = \"Instrument\"\n");
        ModPackageManager dev_only(chan_root);
        dev_only.set_developer_channel_visible(false);
        check(dev_only.scan(&error), error.c_str());
        check(dev_only.packages().count("dev.only") == 0,
              "a developer-only package must not load in a release build");
        ModPackageManager dev_seen(chan_root);
        dev_seen.set_developer_channel_visible(true);
        check(dev_seen.scan(&error), error.c_str());
        check(dev_seen.packages().count("dev.only") == 1,
              "a developer-only package must load in a local build");
        check(dev_seen.packages().at("dev.only").at("1.0.0")
                  .features[0].channel == ModChannel::Developer,
              "a feature must inherit its package's channel by default");

        /* The key is version-gated like every other format addition, and only
         * the three names are accepted. */
        ModPackage rejected;
        write_text(chan_root / "v5-feature-channel.toml",
                   "format_version = 5\n"
                   "id = \"old.mod\"\n"
                   "version = \"1.0.0\"\n"
                   "name = \"Old\"\n"
                   "[[target]]\n"
                   "game_id = \"SLUS-TEST\"\n"
                   "[[feature]]\n"
                   "id = \"f\"\n"
                   "name = \"F\"\n"
                   "channel = \"experimental\"\n");
        check(!ModPackageManager::read_manifest(
                  chan_root / "v5-feature-channel.toml", rejected, &error),
              "a per-feature channel must require format_version 6");
        write_text(chan_root / "bad-channel.toml",
                   "format_version = 6\n"
                   "id = \"bad.mod\"\n"
                   "version = \"1.0.0\"\n"
                   "name = \"Bad\"\n"
                   "channel = \"beta\"\n"
                   "[[target]]\n"
                   "game_id = \"SLUS-TEST\"\n"
                   "[[feature]]\n"
                   "id = \"f\"\n"
                   "name = \"F\"\n");
        check(!ModPackageManager::read_manifest(
                  chan_root / "bad-channel.toml", rejected, &error),
              "an unknown channel name must be rejected");
    }

    /* A stale state.toml naming a package the catalog no longer holds: a
     * framework builtin the title now excludes (EXCLUDE_BUILTIN_MODS), a
     * developer package a release stripped, or one the player deleted. The
     * selection must be dormant -- not a launch error the player cannot clear
     * from a Mods page that no longer lists the package, never applied, and
     * never silently thrown away. */
    {
        const fs::path stale_root = root / "stale";
        const std::string kept =
            "format_version = 5\n"
            "id = \"kept.mod\"\n"
            "version = \"1.0.0\"\n"
            "name = \"Kept\"\n"
            "[[target]]\n"
            "game_id = \"*\"\n"
            "[[feature]]\n"
            "id = \"on\"\n"
            "name = \"On\"\n"
            "[[patch]]\n"
            "feature = \"on\"\n"
            "target = \"main_exe\"\n"
            "address = 2147487744\n"
            "expected = \"01 02 03 04\"\n"
            "replace = \"05 06 07 08\"\n";
        write_text(stale_root / "bundled/kept.mod/1.0.0/manifest.toml", kept);
        write_text(stale_root / "state.toml",
                   "format_version = 2\n"
                   "\n[[package]]\nid = \"kept.mod\"\n"
                   "\n[[package]]\nid = \"psx.enhancement.fast-loading\"\n"
                   "\n[[package]]\nid = \"psx.presentation.bezel\"\n"
                   "version = \"1.0.0\"\n"
                   "\n[[feature]]\npackage_id = \"kept.mod\"\nid = \"on\"\n"
                   "enabled = true\n"
                   "\n[[feature]]\npackage_id = \"psx.enhancement.fast-loading\"\n"
                   "id = \"fast-loading\"\nenabled = true\n"
                   "[feature.values]\nmultiplier = \"8\"\n"
                   "\n[[feature]]\npackage_id = \"psx.presentation.bezel\"\n"
                   "id = \"bezel\"\nenabled = true\n"
                   "[feature.resources]\nartwork = \"C:/art.png\"\n");

        ModPackageManager stale(stale_root);
        check(stale.scan(&error), error.c_str());
        check(stale.load_state(&error), error.c_str());
        const ModResolution plan = stale.resolve("SLUS-TEST");
        check(plan.ok, "a selection naming an absent package must not fail resolution");
        check(plan.ordered.size() == 1 && plan.ordered[0]->id == "kept.mod",
              "only the present, enabled package may enter the plan");
        check(plan.writes.size() == 1,
              "the present package's operations must still resolve");
        const std::vector<std::string> dormant = stale.dormant_selections();
        check(dormant.size() == 2 &&
                  std::find(dormant.begin(), dormant.end(),
                            "psx.enhancement.fast-loading") != dormant.end() &&
                  std::find(dormant.begin(), dormant.end(),
                            "psx.presentation.bezel") != dormant.end(),
              "both absent packages must be reported as dormant selections");
        check(std::find(dormant.begin(), dormant.end(), "kept.mod") == dormant.end(),
              "a present package is never dormant");
        // Strong title policy also suppresses stale bundles and installed
        // copies. Preserve the user's choice and files for a later re-enable.
        write_text(stale_root / "installed/kept.mod/1.0.0/manifest.toml", kept);
        stale.set_disabled_packages({"kept.mod"});
        check(stale.scan(&error), error.c_str());
        const auto disabled = stale.resolve("SLUS-TEST");
        check(disabled.ok && disabled.ordered.empty() && disabled.writes.empty(),
              "a title-disabled package must not resolve from either catalog");
        check(stale.packages().count("kept.mod") == 0 && stale.dormant_selections().size() == 3,
              "title-disabled packages must be hidden and their saved choices dormant");
        check(fs::exists(stale_root / "bundled/kept.mod/1.0.0/manifest.toml") &&
              fs::exists(stale_root / "installed/kept.mod/1.0.0/manifest.toml"),
              "disabling never deletes catalog files");
        stale.set_disabled_packages({});
        check(stale.scan(&error), error.c_str());

        /* Saving (every launch commit does) must keep the dormant choices. */
        check(stale.save_state(&error), error.c_str());
        ModPackageManager reread(stale_root);
        check(reread.scan(&error), error.c_str());
        check(reread.load_state(&error), error.c_str());
        const auto& sel = reread.selections();
        const auto fast = sel.find("psx.enhancement.fast-loading");
        check(fast != sel.end() &&
                  fast->second.features.count("fast-loading") == 1 &&
                  fast->second.features.at("fast-loading").enabled &&
                  fast->second.features.at("fast-loading").values.at("multiplier") == "8",
              "a dormant selection must survive save_state verbatim");
        const auto bezel = sel.find("psx.presentation.bezel");
        check(bezel != sel.end() && bezel->second.version == "1.0.0" &&
                  bezel->second.features.count("bezel") == 1 &&
                  bezel->second.features.at("bezel").resources.at("artwork") ==
                      "C:/art.png",
              "a dormant pinned version and resource must survive save_state");
        check(reread.resolve("SLUS-TEST").fingerprint == plan.fingerprint,
              "dormant selections must not perturb the plan fingerprint");

        /* When the package comes back, the preserved choice applies again. */
        write_text(stale_root / "bundled/psx.enhancement.fast-loading/1.0.0/manifest.toml",
                   "format_version = 5\n"
                   "id = \"psx.enhancement.fast-loading\"\n"
                   "version = \"1.0.0\"\n"
                   "name = \"Fast\"\n"
                   "[[target]]\n"
                   "game_id = \"*\"\n"
                   "[[feature]]\n"
                   "id = \"fast-loading\"\n"
                   "name = \"Fast\"\n");
        ModPackageManager restored(stale_root);
        check(restored.scan(&error), error.c_str());
        check(restored.load_state(&error), error.c_str());
        check(restored.feature_enabled("psx.enhancement.fast-loading", "fast-loading"),
              "a restored package must pick its preserved selection back up");
        check(restored.dormant_selections().size() == 1,
              "only the still-absent package remains dormant");
    }

    /* Cross-package implicit requirements ([[requirement]], format 7). A
     * feature's option selection needs a feature of ANOTHER package: the
     * required feature is activated for the session even though it is hidden
     * and off in state.toml, state.toml is never rewritten to include it, and
     * an unmet requirement fails the plan loudly instead of running without
     * it (WipEout 3's Extras = Full needs psx.enhancement.8mb-ram). */
    {
        const fs::path req_root = root / "requirements";
        mod_clear_plugins_for_tests();
        check(mod_register_activation_plugin("test.big-ram", +[]() {}),
              "requirement test plugin must register");
        check(mod_register_activation_plugin("test.mode", +[]() {}),
              "requirement test plugin must register");
        const std::string provider =
            "format_version = 5\n"
            "id = \"test.ram\"\n"
            "version = \"1.2.0\"\n"
            "name = \"Big RAM\"\n"
            "[[target]]\n"
            "game_id = \"*\"\n"
            "[[feature]]\n"
            "id = \"big-ram\"\n"
            "name = \"Big RAM\"\n"
            "default_enabled = false\n"
            "hidden = true\n"
            "[[plugin]]\n"
            "feature = \"big-ram\"\n"
            "id = \"test.big-ram\"\n";
        const auto requiring = [](const std::string& extra) {
            return std::string(
                "format_version = 7\n"
                "id = \"test.mode\"\n"
                "version = \"1.0.0\"\n"
                "name = \"Mode\"\n"
                "[[target]]\n"
                "game_id = \"SLUS-TEST\"\n"
                "[[feature]]\n"
                "id = \"mode\"\n"
                "name = \"Mode\"\n"
                "[[plugin]]\n"
                "feature = \"mode\"\n"
                "id = \"test.mode\"\n"
                "[[option]]\n"
                "feature = \"mode\"\n"
                "id = \"extras\"\n"
                "label = \"Extras\"\n"
                "type = \"choice\"\n"
                "default = \"none\"\n"
                "[[option.choice]]\nvalue = \"none\"\nlabel = \"None\"\n"
                "[[option.choice]]\nvalue = \"enhanced\"\nlabel = \"Enhanced\"\n"
                "[[option.choice]]\nvalue = \"full\"\nlabel = \"Full\"\n"
                "[[requirement]]\n"
                "feature = \"mode\"\n"
                "package = \"test.ram\"\n"
                "requires_feature = \"big-ram\"\n"
                "when = { extras = \"enhanced\" }\n"
                "[[requirement]]\n"
                "feature = \"mode\"\n"
                "package = \"test.ram\"\n"
                "requires_feature = \"big-ram\"\n"
                "when = { extras = \"full\" }\n") + extra;
        };
        write_text(req_root / "bundled/test.ram/1.2.0/manifest.toml", provider);
        write_text(req_root / "bundled/test.mode/1.0.0/manifest.toml",
                   requiring(""));
        /* The player chose Mode = Full and explicitly left Big RAM off. */
        const std::string state_text =
            "format_version = 2\n"
            "\n[[package]]\nid = \"test.mode\"\n"
            "\n[[feature]]\npackage_id = \"test.mode\"\nid = \"mode\"\n"
            "enabled = true\n"
            "[feature.values]\nextras = \"full\"\n"
            "\n[[feature]]\npackage_id = \"test.ram\"\nid = \"big-ram\"\n"
            "enabled = false\n";
        write_text(req_root / "state.toml", state_text);

        const auto has_plugin = [](const ModResolution& plan,
                                   const std::string& id) {
            return std::any_of(plan.plugins.begin(), plan.plugins.end(),
                               [&](const ModResolution::Plugin& plugin) {
                                   return plugin.id == id;
                               });
        };
        const auto has_error = [](const ModResolution& plan,
                                  const std::string& text) {
            return std::any_of(plan.errors.begin(), plan.errors.end(),
                               [&](const std::string& item) {
                                   return item.find(text) != std::string::npos;
                               });
        };

        ModPackageManager req(req_root);
        check(req.scan(&error), error.c_str());
        check(req.scan_errors().empty(), "requirement manifests must parse");
        check(req.load_state(&error), error.c_str());
        const ModResolution full = req.resolve("SLUS-TEST");
        check(full.ok, "an active requirement on a present package must resolve");
        check(has_plugin(full, "test.big-ram"),
              "an active requirement must activate the hidden required "
              "feature even though state.toml disables it");
        check(has_plugin(full, "test.mode"),
              "the requiring feature itself must stay active");
        check(full.implicit_features.size() == 1 &&
                  full.implicit_features[0].package_id == "test.ram" &&
                  full.implicit_features[0].feature_id == "big-ram" &&
                  full.implicit_features[0].required_by_package_id ==
                      "test.mode" &&
                  full.implicit_features[0].required_by_feature_id == "mode",
              "the plan must name the derived activation and what required it");
        check(std::any_of(full.ordered.begin(), full.ordered.end(),
                          [](const ModPackage* p) { return p->id == "test.ram"; }),
              "the required package must enter the ordered plan");
        check(!req.feature_enabled("test.ram", "big-ram"),
              "feature_enabled must keep reporting the player's own choice");
        check(req.feature_implicitly_enabled("test.ram", "big-ram"),
              "feature_implicitly_enabled must report the derived activation");
        check(req.feature_option_value(full, "test.mode", "mode", "extras") ==
                  "full",
              "a plan must answer option values from its own selection");

        /* state.toml is not a record of derived activations. */
        check(req.save_state(&error), error.c_str());
        {
            ModPackageManager reread(req_root);
            check(reread.scan(&error), error.c_str());
            check(reread.load_state(&error), error.c_str());
            const auto ram_sel = reread.selections().find("test.ram");
            check(ram_sel != reread.selections().end() &&
                      ram_sel->second.features.count("big-ram") == 1 &&
                      !ram_sel->second.features.at("big-ram").enabled,
                  "save_state must persist the player's explicit Big RAM = off, "
                  "not the derived activation");
            check(reread.resolve("SLUS-TEST").fingerprint == full.fingerprint,
                  "a re-read state must reproduce the same plan");
        }
        /* With no prior entry at all, save_state must not invent one. */
        write_text(req_root / "state.toml",
                   "format_version = 2\n"
                   "\n[[feature]]\npackage_id = \"test.mode\"\nid = \"mode\"\n"
                   "enabled = true\n"
                   "[feature.values]\nextras = \"enhanced\"\n");
        {
            ModPackageManager fresh(req_root);
            check(fresh.scan(&error), error.c_str());
            check(fresh.load_state(&error), error.c_str());
            const ModResolution enhanced = fresh.resolve("SLUS-TEST");
            check(enhanced.ok && has_plugin(enhanced, "test.big-ram"),
                  "each `when` entry must activate the requirement on its own");
            check(fresh.save_state(&error), error.c_str());
            std::ifstream in(req_root / "state.toml");
            const std::string saved((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            check(saved.find("test.ram") == std::string::npos,
                  "state.toml must never gain a derived activation");
        }

        /* Condition false: nothing is derived and the plan stays retail. */
        check(req.set_feature_option("test.mode", "mode", "extras", "none",
                                     &error),
              error.c_str());
        const ModResolution none = req.resolve("SLUS-TEST");
        check(none.ok && has_plugin(none, "test.mode") &&
                  !has_plugin(none, "test.big-ram") &&
                  none.implicit_features.empty(),
              "a requirement whose condition is false must not activate");
        check(std::none_of(none.ordered.begin(), none.ordered.end(),
                           [](const ModPackage* p) { return p->id == "test.ram"; }),
              "an unrequired, disabled package must stay out of the plan");
        check(none.fingerprint != full.fingerprint,
              "a derived activation must change the plan fingerprint");
        check(!req.feature_implicitly_enabled("test.ram", "big-ram"),
              "feature_implicitly_enabled must follow the condition");

        /* Requiring feature disabled: its requirements are inert. */
        check(req.set_feature_option("test.mode", "mode", "extras", "full",
                                     &error),
              error.c_str());
        check(req.set_feature_enabled("test.mode", "mode", false, &error),
              error.c_str());
        const ModResolution off = req.resolve("SLUS-TEST");
        check(off.ok && off.plugins.empty() && off.implicit_features.empty(),
              "a disabled feature's requirements must not activate anything");
        check(req.set_feature_enabled("test.mode", "mode", true, &error),
              error.c_str());

        /* The player already enabled it: nothing is derived. */
        check(req.set_feature_enabled("test.ram", "big-ram", true, &error),
              error.c_str());
        const ModResolution chosen = req.resolve("SLUS-TEST");
        check(chosen.ok && has_plugin(chosen, "test.big-ram") &&
                  chosen.implicit_features.empty(),
              "an already-enabled required feature is not an implicit one");
        check(req.set_feature_enabled("test.ram", "big-ram", false, &error),
              error.c_str());

        /* Unmet requirements fail loudly -- never "run the 8 MB build on
         * 2 MB". Package absent (removed, or a title excluded the builtin): */
        std::error_code rm_ec;
        fs::remove_all(req_root / "bundled/test.ram", rm_ec);
        {
            ModPackageManager missing(req_root);
            check(missing.scan(&error), error.c_str());
            check(missing.load_state(&error), error.c_str());
            check(missing.set_feature_enabled("test.mode", "mode", true, &error),
                  error.c_str());
            check(missing.set_feature_option("test.mode", "mode", "extras",
                                             "full", &error),
                  error.c_str());
            const ModResolution plan = missing.resolve("SLUS-TEST");
            check(!plan.ok && plan.plugins.empty() && plan.ordered.empty(),
                  "a requirement on an absent package must reject the plan");
            check(has_error(plan,
                            "test.mode/mode requires test.ram/big-ram, but "
                            "package test.ram is not in this build's mod "
                            "catalog"),
                  "the error must name the requiring feature and the "
                  "missing package");
            check(std::any_of(
                      plan.diagnostics.begin(), plan.diagnostics.end(),
                      [](const ModResolution::Diagnostic& d) {
                          return d.package_id == "test.mode" &&
                                 d.feature_id == "mode" &&
                                 d.other_package_id == "test.ram" &&
                                 d.other_feature_id == "big-ram";
                      }),
                  "the launcher must be able to mark the requiring feature row");
            check(missing.set_feature_option("test.mode", "mode", "extras",
                                             "none", &error),
                  error.c_str());
            check(missing.resolve("SLUS-TEST").ok,
                  "with the condition false an absent provider is irrelevant");
        }
        /* Version outside the declared range: */
        write_text(req_root / "bundled/test.ram/1.2.0/manifest.toml", provider);
        write_text(req_root / "bundled/test.mode/1.0.0/manifest.toml",
                   std::string(requiring("")).replace(
                       requiring("").find("requires_feature = \"big-ram\"\n"
                                          "when = { extras = \"full\" }"),
                       std::string("requires_feature = \"big-ram\"\n").size(),
                       "requires_feature = \"big-ram\"\nversion = \">=2.0.0\"\n"));
        {
            ModPackageManager versioned(req_root);
            check(versioned.scan(&error), error.c_str());
            check(versioned.scan_errors().empty(),
                  "a versioned requirement must parse");
            check(versioned.load_state(&error), error.c_str());
            check(versioned.set_feature_option("test.mode", "mode", "extras",
                                               "full", &error),
                  error.c_str());
            const ModResolution plan = versioned.resolve("SLUS-TEST");
            check(!plan.ok &&
                      has_error(plan, "test.ram 1.2.0 does not satisfy >=2.0.0"),
                  "a provider outside the version range must reject the plan");
        }
        /* Provider present but without the named feature: */
        write_text(req_root / "bundled/test.mode/1.0.0/manifest.toml",
                   requiring(""));
        write_text(req_root / "bundled/test.ram/1.2.0/manifest.toml",
                   "format_version = 6\n"
                   "id = \"test.ram\"\n"
                   "version = \"1.2.0\"\n"
                   "name = \"Big RAM\"\n"
                   "[[target]]\n"
                   "game_id = \"*\"\n"
                   "[[feature]]\n"
                   "id = \"big-ram\"\n"
                   "name = \"Big RAM\"\n"
                   "hidden = true\n"
                   "channel = \"developer\"\n"
                   "[[feature]]\n"
                   "id = \"other\"\n"
                   "name = \"Other\"\n");
        {
            /* A release build strips the developer feature, so the provider
             * has no big-ram: the requirement is unmet, not silently dropped. */
            ModPackageManager stripped(req_root);
            stripped.set_developer_channel_visible(false);
            check(stripped.scan(&error), error.c_str());
            check(stripped.load_state(&error), error.c_str());
            check(stripped.set_feature_option("test.mode", "mode", "extras",
                                              "full", &error),
                  error.c_str());
            const ModResolution plan = stripped.resolve("SLUS-TEST");
            check(!plan.ok &&
                      has_error(plan, "test.ram 1.2.0 has no feature big-ram"),
                  "a provider lacking the required feature must reject the plan");
        }

        /* Fixed point: a derived feature's own requirements, and its
         * in-package requires_feature constraints, are derived too. */
        write_text(req_root / "bundled/test.ram/1.2.0/manifest.toml",
                   "format_version = 7\n"
                   "id = \"test.ram\"\n"
                   "version = \"1.2.0\"\n"
                   "name = \"Big RAM\"\n"
                   "[[target]]\n"
                   "game_id = \"*\"\n"
                   "[[feature]]\n"
                   "id = \"big-ram\"\n"
                   "name = \"Big RAM\"\n"
                   "hidden = true\n"
                   "[[feature]]\n"
                   "id = \"map\"\n"
                   "name = \"Map\"\n"
                   "hidden = true\n"
                   "[[constraint]]\n"
                   "feature = \"big-ram\"\n"
                   "kind = \"requires_feature\"\n"
                   "requires_feature = \"map\"\n"
                   "[[requirement]]\n"
                   "feature = \"big-ram\"\n"
                   "package = \"test.bus\"\n"
                   "requires_feature = \"wide\"\n");
        write_text(req_root / "bundled/test.bus/1.0.0/manifest.toml",
                   "format_version = 5\n"
                   "id = \"test.bus\"\n"
                   "version = \"1.0.0\"\n"
                   "name = \"Bus\"\n"
                   "[[target]]\n"
                   "game_id = \"*\"\n"
                   "[[feature]]\n"
                   "id = \"wide\"\n"
                   "name = \"Wide\"\n"
                   "hidden = true\n");
        {
            ModPackageManager chain(req_root);
            check(chain.scan(&error), error.c_str());
            check(chain.scan_errors().empty(), "chained manifests must parse");
            check(chain.load_state(&error), error.c_str());
            check(chain.set_feature_option("test.mode", "mode", "extras",
                                           "full", &error),
                  error.c_str());
            const ModResolution plan = chain.resolve("SLUS-TEST");
            check(plan.ok, "a chained requirement must resolve");
            check(plan.implicit_features.size() == 2 &&
                      plan.implicit_features[0].package_id == "test.ram" &&
                      plan.implicit_features[1].package_id == "test.bus" &&
                      plan.implicit_features[1].required_by_package_id ==
                          "test.ram",
                  "a derived feature's own requirement must be derived too");
            const auto ram_sel = plan.selections.find("test.ram");
            check(ram_sel != plan.selections.end() &&
                      ram_sel->second.features.count("map") == 1 &&
                      ram_sel->second.features.at("map").enabled,
                  "a derived feature's in-package requires_feature must hold "
                  "in the plan's selection");
            check(!chain.feature_enabled("test.ram", "map") &&
                      !chain.feature_enabled("test.bus", "wide"),
                  "chained derivations must not leak into the player's state");

            /* A package another feature requires is in use: not removable. */
            fs::create_directories(req_root / "installed");
            fs::rename(req_root / "bundled/test.bus",
                       req_root / "installed/test.bus");
            ModPackageManager in_use(req_root);
            check(in_use.scan(&error), error.c_str());
            check(in_use.load_state(&error), error.c_str());
            check(in_use.set_feature_option("test.mode", "mode", "extras",
                                            "full", &error),
                  error.c_str());
            std::string remove_error;
            check(!in_use.remove_version("test.bus", "1.0.0", &remove_error) &&
                      remove_error.find("required by test.ram/big-ram") !=
                          std::string::npos,
                  "a version an active requirement needs must not be removable");
            check(in_use.set_feature_option("test.mode", "mode", "extras",
                                            "none", &error),
                  error.c_str());
            check(in_use.remove_version("test.bus", "1.0.0", &remove_error),
                  remove_error.c_str());
        }

        /* Schema: version-gated, owned, closed. */
        const auto rejects = [&](const std::string& name,
                                 const std::string& text, const char* why) {
            ModPackage parsed;
            std::string parse_error;
            write_text(req_root / name, text);
            check(!ModPackageManager::read_manifest(req_root / name, parsed,
                                                    &parse_error),
                  why);
        };
        std::string v6 = requiring("");
        v6.replace(0, std::string("format_version = 7").size(),
                   "format_version = 6");
        rejects("v6.toml", v6, "[[requirement]] must require format_version 7");
        rejects("self.toml",
                requiring("[[requirement]]\nfeature = \"mode\"\n"
                          "package = \"test.mode\"\n"
                          "requires_feature = \"mode\"\n"),
                "a requirement naming its own package must be rejected");
        rejects("unknown-field.toml",
                requiring("[[requirement]]\nfeature = \"mode\"\n"
                          "package = \"test.ram\"\n"
                          "requires_feature = \"big-ram\"\n"
                          "enabled = true\n"),
                "a requirement with an unknown field must be rejected");
        rejects("unknown-owner.toml",
                requiring("[[requirement]]\nfeature = \"nope\"\n"
                          "package = \"test.ram\"\n"
                          "requires_feature = \"big-ram\"\n"),
                "a requirement owned by an unknown feature must be rejected");
        rejects("bad-when.toml",
                requiring("[[requirement]]\nfeature = \"mode\"\n"
                          "package = \"test.ram\"\n"
                          "requires_feature = \"big-ram\"\n"
                          "when = { extras = \"ludicrous\" }\n"),
                "a requirement condition must name a declared choice");
        mod_clear_plugins_for_tests();
    }

    {
        const auto media_root = root / "media-catalog";
        const auto manifest_path = media_root / "packages/media.mod/1.0.0/manifest.toml";
        std::vector<uint8_t> rom(64);
        rom[0] = 0x80; rom[1] = 0x37; rom[2] = 0x12; rom[3] = 0x40;
        const auto owner = media_root / "owner.z64";
        const auto moved = media_root / "moved.v64";
        write_bytes(owner, rom);
        const std::string declaration =
            "id = \"media.mod\"\nversion = \"1.0.0\"\nname = \"Media\"\n"
            "[[target]]\ngame_id = \"SLUS-TEST\"\n"
            "[[feature]]\nid = \"arena\"\nname = \"Arena\"\n"
            "[[resource]]\nfeature = \"arena\"\nid = \"rom\"\nlabel = \"ROM\"\n"
            "format = \"n64-rom\"\nrequired = true\nsize = 64\nsha256 = \"" + sha256_hex(rom) + "\"\n";
        write_text(manifest_path, "format_version = 8\n" + declaration);
        ModPackageManager media(media_root);
        check(media.scan(&error) && media.scan_errors().empty(), "verified-media manifest parses");
        const auto stock_plan = media.resolve("SLUS-TEST");
        check(stock_plan.ok && stock_plan.resources.empty(), "disabled donor mod needs no media");
        check(media.set_feature_enabled("media.mod", "arena", true, &error), error.c_str());
        check(!media.resolve("SLUS-TEST").ok, "enabled donor mod without media cannot launch");
        check(media.set_feature_resource_path("media.mod", "arena", "rom", owner, &error), error.c_str());
        auto pinned = media.resolve("SLUS-TEST");
        check(pinned.ok && pinned.resources.size() == 1 && pinned.resources[0].bytes &&
              *pinned.resources[0].bytes == rom, "plan contains verified canonical bytes");
        auto swapped = rom;
        for (size_t i = 0; i < swapped.size(); i += 2) std::swap(swapped[i], swapped[i + 1]);
        write_bytes(moved, swapped);
        check(media.set_feature_resource_path("media.mod", "arena", "rom", moved, &error), error.c_str());
        check(media.resolve("SLUS-TEST").fingerprint == pinned.fingerprint,
              "donor fingerprint depends on canonical identity rather than path or byte order");
        swapped[24] ^= 1;
        write_bytes(moved, swapped);
        check(!media.resolve("SLUS-TEST").ok, "wrong same-size media blocks plan");
        fs::remove(moved);
        check(!media.resolve("SLUS-TEST").ok, "removed media blocks a new plan");
        check(media.set_feature_enabled("media.mod", "arena", false, &error), error.c_str());
        const auto restored = media.resolve("SLUS-TEST");
        check(restored.ok && restored.resources.empty() && restored.fingerprint == stock_plan.fingerprint,
              "disabling donor mod restores the exact stock plan");
        write_text(manifest_path, "format_version = 7\n" + declaration);
        ModPackage invalid;
        check(!ModPackageManager::read_manifest(manifest_path, invalid, &error),
              "old manifest version cannot silently ignore donor verification");
        auto negative_size = declaration;
        negative_size.replace(negative_size.find("size = 64"), 9, "size = -1");
        write_text(manifest_path, "format_version = 8\n" + negative_size);
        check(!ModPackageManager::read_manifest(manifest_path, invalid, &error), "negative media size rejected");
    }

    {
        const auto folder = root / "shared-preparation";
        const auto source = folder / "source.iso", replacement = folder / "replacement.iso";
        const auto asset = folder / "cache/asset.dat";
        const std::vector<uint8_t> data{1,2,3,4};
        write_bytes(source, data);write_bytes(replacement, data);write_bytes(asset, data);
        for(const char* id : {"cars", "music"}) {
            std::string manifest = "format_version = 9\nid = \"" + std::string(id) +
                "\"\nversion = \"1.0.0\"\nname = \"Import\"\nprepare = \"test.media\"\n"
                "[[target]]\ngame_id = \"SLUS-TEST\"\n"
                "[[feature]]\nid = \"content\"\nname = \"Content\"\n"
                "[[resource]]\nfeature = \"content\"\nid = \"source\"\nlabel = \"Disc\"\n"
                "input_only = true\nshared_source = \"game.original\"\nrequired = true\n"
                "[[resource]]\nfeature = \"content\"\nid = \"asset\"\nlabel = \"Asset\"\n"
                "hidden = true\nrequired = true\nsize = 4\nsha256 = \"" + sha256_hex(data) + "\"\n";
            write_text(folder / "bundled" / id / "1.0.0/manifest.toml", manifest);
        }
        ModPackageManager manager(folder);
        check(manager.scan(&error) && manager.scan_errors().empty(), "source/preparation catalog parses");
        check(manager.set_feature_resource_path("cars", "content", "source", source, &error), error.c_str());
        check(manager.feature_resource_path("music", "content", "source") == source, "source picker prefills sibling package");
        check(manager.set_feature_resource_path("music", "content", "source", replacement, &error), error.c_str());
        check(manager.feature_resource_path("cars", "content", "source") == replacement, "source sharing works in both directions");
        check(manager.save_state(&error), error.c_str());
        ModPackageManager reload(folder);check(reload.scan(&error) && reload.load_state(&error), error.c_str());
        check(reload.feature_resource_path("cars", "content", "source") == replacement, "shared binding survives restart");
        int calls = 0;bool fail = false;
        check(mod_register_media_preparer("test.media",[&](const ModPrepareContext& context,
            std::map<std::string,fs::path>& output,std::string& reason){
            ++calls;
            check(context.disc_path == source, "preparer inherits main disc picker");
            check(context.inputs.at("source") == replacement, "preparer receives shared source");
            if(fail){reason="invalid input";return false;}
            output["asset"] = asset;return true;
        }), "trusted preparer registered");
        check(reload.prepare_resources("SLUS-TEST", source, folder/"cache", &error) && calls==0, "disabled imports do not prepare media");
        check(reload.set_feature_enabled("cars", "content", true, &error), error.c_str());
        check(reload.resolve("SLUS-TEST", {}, {}, true).ok,
              "launcher preview does not request generated asset paths");
        check(!reload.resolve("SLUS-TEST").ok,
              "runtime still rejects content before preparation");
        check(reload.prepare_resources("SLUS-TEST", source, folder/"cache", &error), error.c_str());
        auto prepared = reload.resolve("SLUS-TEST");
        check(prepared.ok && prepared.resources.size()==1 && prepared.resources[0].id=="asset", "only derived assets enter runtime plan");
        const auto before = reload.feature_resource_path("cars", "content", "asset");
        int progress_calls=0;
        check(!reload.prepare_resources("SLUS-TEST",source,folder/"cache",&error,false,
            [&](float,const std::string&){return ++progress_calls<2;}),"cancel after provider prevents selection publication");
        check(reload.feature_resource_path("cars","content","asset")==before,"cancel retains prior derived binding");
        fail=true;check(!reload.prepare_resources("SLUS-TEST", source, folder/"cache", &error), "failed preparation refuses launch");
        check(reload.feature_resource_path("cars", "content", "asset")==before, "failed preparation preserves prior bindings");
        write_bytes(asset, {4,3,2,1});check(!reload.resolve("SLUS-TEST").ok, "corrupt derived output fails engine verification");
    }

    {
        const auto folder=root/"resource-overlays";
        const auto asset=folder/"local"/"derived.dat";
        const auto manifest_path=folder/"bundled/prepared.overlay/1.0.0/manifest.toml";
        const std::vector<uint8_t> bytes={1,2,3,4,5,6};
        const std::vector<uint8_t> slice={3,4,5};
        const std::string disc_hash(64,'a');
        auto make_manifest=[&](const std::string& source_fields,const std::string& feature="active") {
            return "format_version = 10\nid = \"prepared.overlay\"\nversion = \"1.0.0\"\nname = \"Prepared overlay\"\nprepare = \"test.overlay\"\n"
                "[[target]]\ngame_id = \"SLUS-TEST\"\ndisc_sha256 = \""+disc_hash+"\"\n"
                "[[feature]]\nid = \"active\"\nname = \"Active\"\ndefault_enabled = true\n"
                "[[feature]]\nid = \"other\"\nname = \"Other\"\n"
                "[[resource]]\nfeature = \""+feature+"\"\nid = \"asset\"\nlabel = \"Derived asset\"\nformat = \"file\"\nrequired = true\nhidden = true\nsize = 6\nsha256 = \""+sha256_hex(bytes)+"\"\n"
                "[[overlay]]\nfeature = \"active\"\ntarget = \"disc_raw\"\noffset = 2340\n"+source_fields+
                "sha256 = \""+sha256_hex(slice)+"\"\nexpected_sha256 = \""+sha256_hex({0,0,0})+"\"\n";
        };
        const std::string fields="resource = \"asset\"\nresource_offset = 2\nsize = 3\n";
        write_text(manifest_path,make_manifest(fields));
        ModPackageManager manager(folder);
        check(manager.scan(&error) && manager.scan_errors().empty(),"resource overlay scans before generated file exists");
        check(manager.resolve("SLUS-TEST",{},disc_hash,true).ok,"preview defers prepared resource overlay");
        check(!manager.resolve("SLUS-TEST",{},disc_hash).ok,"runtime refuses missing overlay source");
        write_bytes(asset,bytes);
        check(manager.set_feature_resource_path("prepared.overlay","active","asset",asset,&error),error.c_str());
        const auto plan=manager.resolve("SLUS-TEST",{},disc_hash);
        check(plan.ok && plan.overlays.size()==1 && plan.resources.size()==1,"verified resource overlay resolves");
        if(plan.overlays.size()==1) {
            check(plan.overlays[0].payload==slice && plan.overlays[0].location==2340,"overlay selects exact declared slice and target offset");
        }
        write_bytes(asset,{1,2,3,4,5,7});
        check(!manager.resolve("SLUS-TEST",{},disc_hash).ok,"mutated resource rejected even when selected slice did not change");
        write_text(manifest_path,make_manifest("resource = \"asset\"\nresource_offset = 4\nsize = 3\n"));
        ModPackageManager bounds(folder);bounds.scan(&error);
        check(!bounds.scan_errors().empty(),"resource slice beyond declared extent rejected");
        write_text(manifest_path,make_manifest(fields+"file = \"payload.dat\"\n"));
        ModPackageManager both(folder);both.scan(&error);
        check(!both.scan_errors().empty(),"ambiguous file and resource sources rejected");
        write_text(manifest_path,make_manifest(fields,"other"));
        ModPackageManager ownership(folder);ownership.scan(&error);
        check(!ownership.scan_errors().empty(),"cross-feature overlay source rejected");
        write_text(manifest_path,make_manifest("resource = \"asset\"\nresource_offset = -1\nsize = 3\n"));
        ModPackageManager negative(folder);negative.scan(&error);
        check(!negative.scan_errors().empty(),"negative resource slice rejected");
    }
    fs::remove_all(root, ec);
    if (failures) {
        std::cerr << failures << " mod package test(s) failed\n";
        return 1;
    }
    std::cout << "mod package tests passed\n";
    return 0;
}
