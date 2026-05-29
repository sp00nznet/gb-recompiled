#include "recompiler/custom_names.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace gbrecomp {

static std::map<std::uint32_t, std::string> g_names;

static std::uint32_t key_of(std::uint8_t bank, std::uint16_t addr) {
    return (static_cast<std::uint32_t>(bank) << 16) | addr;
}

// Fixed GB vectors whose canonical names the codegen relies on; never renamed.
static bool is_reserved_vector(std::uint8_t bank, std::uint16_t addr) {
    if (bank != 0) return false;
    switch (addr) {
        case 0x0000: case 0x0008: case 0x0010: case 0x0018:
        case 0x0020: case 0x0028: case 0x0030: case 0x0038:
        case 0x0040: case 0x0048: case 0x0050: case 0x0058: case 0x0060:
        case 0x0100:
            return true;
        default:
            return false;
    }
}

std::size_t load_custom_names(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[names] could not open %s\n", path.c_str());
        return 0;
    }
    g_names.clear();
    std::string line;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream ls(line);
        std::string key, name;
        if (!(ls >> key >> name)) continue;  // need both fields
        auto colon = key.find(':');
        if (colon == std::string::npos) continue;
        unsigned bank = 0, addr = 0;
        try {
            bank = std::stoul(key.substr(0, colon), nullptr, 10);
            addr = std::stoul(key.substr(colon + 1), nullptr, 16);
        } catch (...) {
            continue;
        }
        if (name.empty()) continue;
        g_names[key_of(static_cast<std::uint8_t>(bank), static_cast<std::uint16_t>(addr))] = name;
    }
    std::fprintf(stderr, "[names] loaded %zu custom function names from %s\n",
                 g_names.size(), path.c_str());
    return g_names.size();
}

const std::string* lookup_custom_name(std::uint8_t bank, std::uint16_t addr) {
    if (g_names.empty()) return nullptr;
    if (is_reserved_vector(bank, addr)) return nullptr;
    auto it = g_names.find(key_of(bank, addr));
    return it == g_names.end() ? nullptr : &it->second;
}

}  // namespace gbrecomp
