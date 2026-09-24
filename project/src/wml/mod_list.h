// Whompay's Mod Loader - mod discovery and the enabled/load-order list.
// Shared by the game and the launcher.
//
// Layout of the mods folder (next to saintsrow.exe):
//
//   mods/
//     modlist.ini          load order and on/off state, written by the launcher
//     wml.log              log of the last run
//     <ModFolder>/
//       mod.ini            name, author, version, description
//       files/...          replacement game files (mirrors the game folder)
//       *.dll              native mod(s) exporting wml_mod_init
//       main.lua           script mod entry point
//       patch.lua          script that edits game files before the game starts
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace wml {

// UTF-8 string of a path (works with and without char8_t).
inline std::string PathToUtf8(const std::filesystem::path& path) {
  auto s = path.u8string();
  return std::string(s.begin(), s.end());
}

struct ModInfo {
  std::string id;  // folder name
  std::string name;
  std::string author;
  std::string version;
  std::string description;
  std::filesystem::path folder;
  std::filesystem::path files_folder;  // empty if the mod replaces no files
  std::vector<std::filesystem::path> dlls;
  std::filesystem::path script;  // empty if the mod has no Lua script
  std::filesystem::path patch_script;  // patch.lua: edits game files before start
  // [settings] section of mod.ini, in file order.
  std::vector<std::pair<std::string, std::string>> settings;
  bool enabled = false;

  bool has_files() const { return !files_folder.empty(); }
  bool has_code() const { return !dlls.empty(); }
  bool has_script() const { return !script.empty(); }
  bool has_patch() const { return !patch_script.empty(); }
};

// All mods in `mods_dir`, in load order (as saved in modlist.ini, then any
// mods not listed yet, disabled, sorted by name).
std::vector<ModInfo> LoadMods(const std::filesystem::path& mods_dir);

// Saves the load order and on/off state to modlist.ini.
bool SaveModList(const std::filesystem::path& mods_dir, const std::vector<ModInfo>& mods);

}  // namespace wml
