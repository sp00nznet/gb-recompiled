#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace gbrecomp {

// Load a names file produced by tools/ghidra/build_names.py. Each line is
//   <bank>:<hexaddr> <c_identifier>
// with '#' comments and blank lines ignored. Returns how many names loaded.
// Replaces any previously loaded set.
std::size_t load_custom_names(const std::string& path);

// Returns the custom name for (bank, addr), or nullptr when there is none.
// Reserved GB vectors (rst/interrupt entries and the 0x0100 entry point) are
// never overridden, so their canonical names (int_vblank, rst_00, gb_main…)
// are preserved for both the analyzer's naming and Program::make_function_name.
const std::string* lookup_custom_name(std::uint8_t bank, std::uint16_t addr);

}  // namespace gbrecomp
