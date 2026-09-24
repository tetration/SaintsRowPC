// Whompay's Mod Loader - patch scripts.
//
// A mod's patch.lua runs before the game starts, in its own Lua state, and can
// change files inside the game's packfiles:
//
//   wml.packfile_files(pack)                -> list of file names
//   wml.packfile_read(pack, name)           -> contents (or nil)
//   wml.packfile_write(pack, name, data)    -> replace a file
//   wml.game_file_read(path)                -> contents of a loose game file (or nil)
//   wml.setting(name, default)              -> value from [settings] in mod.ini
//   wml.settings, wml.log(...), wml.mod_name, wml.mod_folder
//
// `pack` is a packfile name such as "misc.vpp_xbox2" (looked up in the game's
// packfiles folder) or a path relative to the game folder. Changes are applied
// to the player's own copy of the packfile and cached in mods/.cache, which is
// then mounted over the game folder.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "packfile.h"
#include "wml_internal.h"

namespace wml {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct PatchedPack {
  std::string relative_path;  // e.g. "packfiles/misc.vpp_xbox2"
  fs::path source;
  std::unique_ptr<Packfile> pack;
  std::map<std::string, std::string> replacements;
  std::set<std::string> mods;
};

struct PatchState {
  fs::path game_dir;
  std::map<std::string, PatchedPack> packs;  // key: lower-case relative path
};

PatchState* g_state = nullptr;

struct ScriptContext {
  const ModInfo* mod;
};

ScriptContext* ContextOf(lua_State* L) {
  return static_cast<ScriptContext*>(lua_touserdata(L, lua_upvalueindex(1)));
}

std::string NormalizeRelative(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  while (!path.empty() && path[0] == '/') path.erase(0, 1);
  if (path.find('/') == std::string::npos) path = "packfiles/" + path;
  return path;
}

PatchedPack* OpenPack(lua_State* L, const char* name) {
  std::string relative = NormalizeRelative(name);
  if (relative.find("..") != std::string::npos) {
    luaL_error(L, "invalid packfile path '%s'", name);
    return nullptr;
  }
  std::string key = Lower(relative);
  auto it = g_state->packs.find(key);
  if (it != g_state->packs.end()) return &it->second;
  PatchedPack pp;
  pp.relative_path = relative;
  pp.source = g_state->game_dir / fs::u8path(relative);
  pp.pack = std::make_unique<Packfile>();
  std::string error;
  if (!pp.pack->Load(pp.source, &error)) {
    luaL_error(L, "cannot open packfile '%s': %s", relative.c_str(), error.c_str());
    return nullptr;
  }
  return &g_state->packs.emplace(key, std::move(pp)).first->second;
}

int LLog(lua_State* L) {
  std::string text;
  for (int i = 1; i <= lua_gettop(L); ++i) {
    if (i > 1) text += " ";
    text += luaL_tolstring(L, i, nullptr);
    lua_pop(L, 1);
  }
  Log(ContextOf(L)->mod->name, text);
  return 0;
}

int LPackfileFiles(lua_State* L) {
  PatchedPack* pp = OpenPack(L, luaL_checkstring(L, 1));
  lua_newtable(L);
  int i = 1;
  for (const auto& name : pp->pack->Names()) {
    lua_pushstring(L, name.c_str());
    lua_rawseti(L, -2, i++);
  }
  return 1;
}

int LPackfileRead(lua_State* L) {
  PatchedPack* pp = OpenPack(L, luaL_checkstring(L, 1));
  std::string name = luaL_checkstring(L, 2);
  for (const auto& r : pp->replacements) {
    if (Lower(r.first) == Lower(name)) {
      lua_pushlstring(L, r.second.data(), r.second.size());
      return 1;
    }
  }
  std::string data;
  if (!pp->pack->Read(name, data)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushlstring(L, data.data(), data.size());
  return 1;
}

int LPackfileWrite(lua_State* L) {
  PatchedPack* pp = OpenPack(L, luaL_checkstring(L, 1));
  std::string name = luaL_checkstring(L, 2);
  size_t size = 0;
  const char* data = luaL_checklstring(L, 3, &size);
  if (!pp->pack->Contains(name)) {
    return luaL_error(L, "'%s' is not in %s (only existing files can be replaced)", name.c_str(),
                      pp->relative_path.c_str());
  }
  for (auto it = pp->replacements.begin(); it != pp->replacements.end(); ++it) {
    if (Lower(it->first) == Lower(name)) {
      pp->replacements.erase(it);
      break;
    }
  }
  pp->replacements[name] = std::string(data, size);
  pp->mods.insert(ContextOf(L)->mod->name);
  return 0;
}

int LGameFileRead(lua_State* L) {
  std::string relative = luaL_checkstring(L, 1);
  for (char& c : relative) {
    if (c == '\\') c = '/';
  }
  if (relative.find("..") != std::string::npos) return luaL_error(L, "invalid path");
  std::ifstream in(g_state->game_dir / fs::u8path(relative), std::ios::binary);
  if (!in) {
    lua_pushnil(L);
    return 1;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string data = buffer.str();
  lua_pushlstring(L, data.data(), data.size());
  return 1;
}

int LSetting(lua_State* L) { return PushSetting(L, ContextOf(L)->mod->settings); }

void RunPatchScript(const ModInfo& mod) {
  lua_State* L = luaL_newstate();
  if (!L) return;
  luaL_openlibs(L);
  ScriptContext context{&mod};
  static const luaL_Reg kFunctions[] = {
      {"log", LLog},
      {"packfile_files", LPackfileFiles},
      {"packfile_read", LPackfileRead},
      {"packfile_write", LPackfileWrite},
      {"game_file_read", LGameFileRead},
      {"setting", LSetting},
      {nullptr, nullptr}};
  lua_newtable(L);
  lua_pushlightuserdata(L, &context);
  luaL_setfuncs(L, kFunctions, 1);
  lua_pushstring(L, mod.name.c_str());
  lua_setfield(L, -2, "mod_name");
  lua_pushstring(L, PathToUtf8(mod.folder).c_str());
  lua_setfield(L, -2, "mod_folder");
  lua_newtable(L);
  for (const auto& kv : mod.settings) {
    lua_pushstring(L, kv.second.c_str());
    lua_setfield(L, -2, kv.first.c_str());
  }
  lua_setfield(L, -2, "settings");
  lua_setglobal(L, "wml");
  lua_getglobal(L, "package");
  std::string path = PathToUtf8(mod.folder) + "/?.lua";
  lua_pushstring(L, path.c_str());
  lua_setfield(L, -2, "path");
  lua_pop(L, 1);

  std::string script = PathToUtf8(mod.patch_script);
  if (luaL_loadfile(L, script.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
    const char* message = lua_tostring(L, -1);
    Log(mod.name, std::string("patch.lua failed: ") + (message ? message : "?"));
  } else {
    Log(mod.name, "Ran patch.lua");
  }
  lua_close(L);
}

uint64_t Fnv1a(uint64_t hash, const std::string& data) {
  for (unsigned char c : data) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

// wml.setting(name, default): the value from mod.ini, converted to the type of
// the default (number or boolean) when possible.
int PushSetting(lua_State* L, const std::vector<std::pair<std::string, std::string>>& settings) {
  std::string name = Lower(luaL_checkstring(L, 1));
  for (const auto& kv : settings) {
    if (Lower(kv.first) != name) continue;
    const std::string& value = kv.second;
    if (lua_type(L, 2) == LUA_TNUMBER) {
      char* end = nullptr;
      double number = std::strtod(value.c_str(), &end);
      if (end && end != value.c_str()) {
        lua_pushnumber(L, number);
        return 1;
      }
      break;
    }
    if (lua_type(L, 2) == LUA_TBOOLEAN) {
      std::string v = Lower(value);
      lua_pushboolean(L, v == "true" || v == "1" || v == "yes" || v == "on");
      return 1;
    }
    lua_pushstring(L, value.c_str());
    return 1;
  }
  lua_pushvalue(L, 2);
  return 1;
}

fs::path RunPatchScripts(const std::vector<ModInfo>& mods, const fs::path& game_dir,
                         const fs::path& cache_dir) {
  PatchState state;
  state.game_dir = game_dir;
  g_state = &state;
  for (const auto& mod : mods) {
    if (mod.has_patch()) RunPatchScript(mod);
  }
  g_state = nullptr;

  // Build (or reuse) the patched packfiles, and remove outdated ones.
  fs::path files_dir = cache_dir / "files";
  std::error_code ec;
  std::set<fs::path> wanted;
  for (auto& [key, pp] : state.packs) {
    if (pp.replacements.empty()) continue;
    fs::path output = files_dir / fs::u8path(pp.relative_path);
    wanted.insert(output.lexically_normal());

    uint64_t hash = 14695981039346656037ull;
    hash = Fnv1a(hash, std::to_string(fs::file_size(pp.source, ec)));
    auto time = fs::last_write_time(pp.source, ec).time_since_epoch().count();
    hash = Fnv1a(hash, std::to_string(time));
    for (const auto& [name, data] : pp.replacements) {
      hash = Fnv1a(hash, Lower(name));
      hash = Fnv1a(hash, data);
    }
    char key_text[32];
    std::snprintf(key_text, sizeof(key_text), "%016llx", (unsigned long long)hash);
    fs::path key_file = cache_dir / "keys" / (Lower(pp.relative_path) + ".key");
    std::string old_key;
    {
      std::ifstream in(key_file);
      std::getline(in, old_key);
    }
    std::string mods_text;
    for (const auto& m : pp.mods) mods_text += (mods_text.empty() ? "" : ", ") + m;
    if (old_key == key_text && fs::exists(output, ec)) {
      Log("WML", "Using cached " + pp.relative_path + " (changed by " + mods_text + ")");
      continue;
    }
    std::string error;
    if (!pp.pack->Save(output, pp.replacements, &error)) {
      Log("WML", "Could not build " + pp.relative_path + ": " + error);
      fs::remove(output, ec);
      continue;
    }
    fs::create_directories(key_file.parent_path(), ec);
    std::ofstream(key_file, std::ios::trunc) << key_text << "\n";
    Log("WML", "Built " + pp.relative_path + " with " + std::to_string(pp.replacements.size()) +
                   " changed file(s) (by " + mods_text + ")");
  }
  if (fs::exists(files_dir, ec)) {
    std::vector<fs::path> stale;
    for (auto it = fs::recursive_directory_iterator(files_dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it->is_regular_file(ec) && !wanted.count(it->path().lexically_normal())) {
        stale.push_back(it->path());
      }
    }
    for (const auto& path : stale) fs::remove(path, ec);
  }
  return wanted.empty() ? fs::path() : files_dir;
}

}  // namespace wml
