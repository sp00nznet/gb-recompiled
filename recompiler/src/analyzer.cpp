/**
 * @file analyzer.cpp
 * @brief Control flow analyzer implementation (stub for MVP)
 */

#include "recompiler/analyzer.h"
#include "recompiler/custom_names.h"
#include <algorithm>
#include <queue>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <set>
#include <map>
#include <fstream>

namespace gbrecomp {

/* ============================================================================
 * Helper Functions
 * ========================================================================== */

static uint32_t make_address(uint8_t bank, uint16_t addr) {
    return (static_cast<uint32_t>(bank) << 16) | addr;
}

static uint8_t get_bank(uint32_t addr) {
    return static_cast<uint8_t>(addr >> 16);
}

static uint16_t get_offset(uint32_t addr) {
    return static_cast<uint16_t>(addr & 0xFFFF);
}

/* ============================================================================
 * AnalysisResult Implementation
 * ========================================================================== */

const Instruction* AnalysisResult::get_instruction(uint8_t bank, uint16_t addr) const {
    uint32_t full_addr = make_addr(bank, addr);
    auto it = addr_to_index.find(full_addr);
    if (it != addr_to_index.end() && it->second < instructions.size()) {
        return &instructions[it->second];
    }
    return nullptr;
}

const BasicBlock* AnalysisResult::get_block(uint8_t bank, uint16_t addr) const {
    uint32_t full_addr = make_addr(bank, addr);
    auto it = blocks.find(full_addr);
    if (it != blocks.end()) {
        return &it->second;
    }
    return nullptr;
}

const Function* AnalysisResult::get_function(uint8_t bank, uint16_t addr) const {
    uint32_t full_addr = make_addr(bank, addr);
    auto it = functions.find(full_addr);
    if (it != functions.end()) {
        return &it->second;
    }
    return nullptr;
}

/* ============================================================================
 * RST Pattern Detection
 * ========================================================================== */

/**
 * @brief Check if a RST vector contains only 0xFF padding (not real code)
 * 
 * Many ROMs have 0xFF padding at unused RST vector locations.
 * This prevents infinite recursion when analyzing these vectors.
 * 
 * @param rom The ROM to check
 * @param vector The RST vector address (0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38)
 * @return true if the vector contains only 0xFF bytes
 */
static bool is_rst_padding(const ROM& rom, uint16_t vector) {
    // RST vectors are 8 bytes apart, check all bytes up to the next vector
    uint16_t end = vector + 8;
    if (end > 0x40) end = 0x40;  // Don't go past RST 38 region
    
    for (uint16_t addr = vector; addr < end; addr++) {
        if (rom.read_banked(0, addr) != 0xFF) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Check if RST 28 is a jump table dispatcher
 * 
 * Tetris and many other GB games use RST 28 as a computed jump table:
 *   ADD A,A       ; Double A (table entries are 2 bytes)
 *   POP HL        ; Get return address (points to table)
 *   ...
 *   JP (HL)       ; Jump to looked-up address
 * 
 * The bytes following RST 28 calls are table data, NOT code.
 */
static bool is_rst28_jump_table(const ROM& rom) {
    // Check for the pattern starting at 0x28:
    // 87 E1 ... E9 (ADD A,A; POP HL; ...; JP (HL))
    if (rom.read_banked(0, 0x28) == 0x87 &&  // ADD A,A
        rom.read_banked(0, 0x29) == 0xE1) {  // POP HL
        // Look for JP (HL) = 0xE9 somewhere in 0x28-0x3F region
        for (uint16_t addr = 0x2A; addr < 0x40; addr++) {
            if (rom.read_banked(0, addr) == 0xE9) {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Check if RST 28 falls through into RST 30
 * 
 * When RST 28 is a jump table dispatcher, it typically continues through
 * RST 30's space to reach JP (HL). In this case, RST 30 should NOT be
 * marked as a separate function entry since it's part of RST 28's routine.
 * 
 * Pattern: RST 28 at 0x28-0x2F falls through to code at 0x30-0x33 ending with JP (HL)
 */
static bool rst28_uses_rst30(const ROM& rom) {
    if (!is_rst28_jump_table(rom)) {
        return false;
    }
    
    // Check if JP (HL) (0xE9) is in the 0x30-0x37 range (RST 30 region)
    for (uint16_t addr = 0x30; addr < 0x38; addr++) {
        if (rom.read_banked(0, addr) == 0xE9) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Extract jump table entries following an RST 28 call site
 * 
 * When RST 28 is a jump table dispatcher, the bytes immediately following
 * the RST 28 opcode are 16-bit addresses (in little-endian format).
 * 
 * Pattern from Tetris:
 *   ldh  a,(0cdh)     ; Load index value
 *   rst  28h          ; Call jump table dispatcher (opcode 0xEF)
 *   .dw  l0078        ; Entry 0
 *   .dw  l009f        ; Entry 1
 *   ...
 * 
 * @param rom The ROM to read from
 * @param rst_call_addr Address of the RST 28 opcode (0xEF)
 * @param bank Bank number for the call site
 * @return Vector of extracted jump table target addresses
 */
static std::vector<uint16_t> extract_rst28_table_entries(const ROM& rom, uint16_t rst_call_addr, uint8_t bank) {
    std::vector<uint16_t> targets;
    
    // Table starts immediately after the RST 28 opcode (1 byte)
    uint16_t table_start = rst_call_addr + 1;
    
    // We don't know the table size statically.
    // Heuristic: Read addresses until we hit:
    // 1. An address that's clearly not code (below 0x0100 except for RST vectors)
    // 2. An address that overlaps with known code
    // 3. An unreasonably large number of entries (e.g., > 64)
    // 4. An address at or past 0x8000 (not ROM)
    
    const int MAX_TABLE_ENTRIES = 64;  // Tetris has up to 44 entries in its main state machine
    
    for (int i = 0; i < MAX_TABLE_ENTRIES; i++) {
        uint16_t entry_addr = table_start + i * 2;
        
        // Make sure we can read 2 bytes
        size_t rom_offset;
        if (entry_addr < 0x4000) {
            rom_offset = entry_addr;
        } else {
            rom_offset = static_cast<size_t>(bank) * 0x4000 + (entry_addr - 0x4000);
        }
        
        if (rom_offset + 1 >= rom.size()) {
            break;  // Past end of ROM
        }
        
        // Read 16-bit address (little-endian)
        // For addresses in bank 0 region (< 0x4000), always use bank 0
        uint8_t read_bank = (entry_addr < 0x4000) ? 0 : bank;
        uint8_t lo = rom.read_banked(read_bank, entry_addr);
        uint8_t hi = rom.read_banked(read_bank, entry_addr + 1);
        uint16_t target = static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
        
        // Validate the target address
        if (target >= 0x8000) {
            // Not ROM - likely end of table
            break;
        }
        
        // Address should be aligned to reasonable code
        // Very low addresses (0x00-0x3F) are RST/INT vectors, which is OK
        // Addresses 0x40-0xFF should be valid only for known interrupt handlers
        // Core code typically starts at 0x100+
        if (target == 0x0000 || target == 0xFFFF) {
            // Invalid entry, likely end of table
            break;
        }
        
        // Add the target if it looks valid
        targets.push_back(target);
    }
    
    return targets;
}

/* ============================================================================
 * Internal State Tracking
 * ========================================================================== */

// Track addresses to explore: (addr, known_a, ..., current_bank_context)
struct AnalysisState {
    uint32_t addr;
    int known_a;  // -1 if unknown
    int known_b;
    int known_c;
    int known_d;
    int known_e;
    int known_h;
    int known_l;
    uint8_t current_bank;
};

/* ============================================================================
 * Bank Switch Detection
 * ========================================================================== */

/**
 * @brief Detect immediate bank values from common patterns
 * 
 * Looks for patterns like:
 *   LD A, n      ; n is bank number
 *   LD (2000), A ; or LD (2100), A, etc.
 */
static std::set<uint8_t> detect_bank_values(const ROM& rom) {
    std::set<uint8_t> banks;
    banks.insert(0);  // Bank 0 is always present
    banks.insert(1);  // Bank 1 is the default switchable bank
    
    // Use ROM header to know how many banks exist
    uint16_t bank_count = rom.header().rom_banks;
    for (uint16_t i = 0; i < bank_count && i < 256; i++) {
        banks.insert(static_cast<uint8_t>(i));
    }
    
    return banks;
}

/**
 * @brief Calculate Shannon entropy of a memory region
 */
static double calculate_entropy(const ROM& rom, uint8_t bank, uint16_t addr, size_t len) {
    if (addr + len > 0x8000) return 0.0;
    
    uint32_t counts[256] = {0};
    for (size_t i = 0; i < len; i++) {
        counts[rom.read_banked(bank, addr + i)]++;
    }
    
    double entropy = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0) {
            double p = (double)counts[i] / len;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

/**
 * @brief Heuristic check if an address looks like valid code start
 * 
 * Checks for:
 * 1. Shannon Entropy (filtering tile data / PCM)
 * 2. Repetitive byte patterns
 * 3. Illegal address access/jumps
 * 4. Instruction density (loads vs math/control flow)
 */
static int is_likely_valid_code(const ROM& rom, uint8_t bank, uint16_t addr) {
    // 1. Shannon Entropy Check
    // Typical code has moderate entropy (3.0-6.0). 
    // Data like tilemaps is very low (< 2.0). PCM is very high (> 7.5).
    double entropy = calculate_entropy(rom, bank, addr, 48);
    if (entropy < 1.8 || entropy > 7.6) return 0;

    // 2. Check for repetitive patterns (e.g. tile data)
    const int PATTERN_CHECK_LEN = 128;
    if (addr + PATTERN_CHECK_LEN < 0x8000) {
        for (int period = 1; period <= 8; period++) {
            const int REQUIRED_REPEATS = 16;
            const int REQUIRED_LEN = period * REQUIRED_REPEATS;
            
            bool matches = true;
            for (int i = 0; i < REQUIRED_LEN; i++) {
                if (rom.read_banked(bank, addr + i) != rom.read_banked(bank, addr + i + period)) {
                    matches = false;
                    break;
                }
            }
            if (matches) return 0;
        }
    }

    // 3. Decode instructions
    Decoder decoder(rom);
    uint16_t curr = addr;
    int instructions_checked = 0;
    const int MAX_CHECK = 64;
    int nop_count = 0;
    int ld_count = 0;
    int control_flow_count = 0;
    int math_count = 0;

    while (instructions_checked < MAX_CHECK) {
        Instruction instr = decoder.decode(curr, bank);
        
        if (instr.type == InstructionType::UNDEFINED || instr.type == InstructionType::INVALID) return 0;
        
        if (instr.type == InstructionType::NOP) {
            nop_count++;
            if (nop_count > 4) return 0; // Too many NOPs
        }
        
        // 4. Illegal address check
        if (instr.type == InstructionType::LD_A_NN || instr.type == InstructionType::LD_NN_A ||
            instr.type == InstructionType::LD_NN_SP || instr.type == InstructionType::LD_RR_NN ||
            instr.is_call || instr.is_jump) {
            
            uint16_t imm = (instr.type == InstructionType::JR_N || instr.type == InstructionType::JR_CC_N) ? 0 : instr.imm16;
            if (imm != 0) {
                // Prohibited memory areas
                if (imm >= 0xFEA0 && imm <= 0xFEFF) return 0;
                // Echo RAM (usually not used by real code)
                if (imm >= 0xE000 && imm <= 0xFDFF) return 0;
            }
        }

        if (instr.reads_memory || instr.writes_memory) ld_count++;
        if (instr.is_call || instr.is_jump || instr.is_return) control_flow_count++;
        
        // Math/Logic
        if (instr.opcode >= 0x80 && instr.opcode <= 0xBF) math_count++;

        // Reject rare/data-like opcodes if too frequent at start
        if (instr.opcode == 0x27 || instr.opcode == 0x2F || instr.opcode == 0x37 || instr.opcode == 0x3F) {
            if (instructions_checked < 4) return 0;
        }
        
        // RST instructions in data are suspicious (0x00 or 0xFF)
        if (instr.type == InstructionType::RST) {
            if (instr.opcode == 0xC7 || instr.opcode == 0xFF) {
                 if (instructions_checked < 2) return 0;
            }
        }

        // Terminator Check
        if (instr.is_return && !instr.is_conditional) {
            if (instructions_checked < 2) return 0; 
            // Avoid load-only functions discovered via scanning
            if (ld_count >= instructions_checked && instructions_checked > 2) return 0;
            return (curr + instr.length - addr);
        }
        
        if (instr.is_jump && !instr.is_conditional) {
             if (instructions_checked >= 3) return (curr + instr.length - addr);
             return 0;
        }
        
        curr += instr.length;
        if (curr >= 0x8000) return 0;
        instructions_checked++;
        
        // High density of loads (indicative of data or large tables)
        if (instructions_checked >= 15 && ld_count == instructions_checked) return 0;
    }

    return 0;
}

/**
 * @brief Scan for 16-bit pointers that likely lead to code
 */
static void find_pointer_entry_points(const ROM& rom, AnalysisResult& result, std::queue<AnalysisState>& work_queue) {
    // Scan Bank 0 for potential 16-bit pointers
    // Typically pointers are found after the header (0x150)
    for (uint16_t addr = 0x0150; addr < 0x3FFE; addr++) {
        uint8_t lo = rom.read_banked(0, addr);
        uint8_t hi = rom.read_banked(0, addr + 1);
        uint16_t target = lo | (hi << 8);
        
        // Target must be in ROM
        if (target >= 0x0150 && target < 0x8000) {
            uint8_t tbank = (target < 0x4000) ? 0 : 1; 
            // If it's a pointer to code, suggest it as an entry point
            if (is_likely_valid_code(rom, tbank, target)) {
                uint32_t full_addr = make_address(tbank, target);
                if (result.call_targets.find(full_addr) == result.call_targets.end()) {
                    result.call_targets.insert(full_addr);
                    work_queue.push({full_addr, -1, -1, -1, -1, -1, -1, -1, (tbank > 0 ? tbank : (uint8_t)1)});
                }
            }
        }
    }
}

/* ============================================================================
 * Analysis Implementation
 * ========================================================================== */

/**
 * @brief Load entry points from a runtime trace file
 */
static void load_trace_entry_points(const std::string& path, std::set<uint32_t>& call_targets) {
    if (path.empty()) return;

    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: Could not open trace file: " << path << "\n";
        return;
    }

    std::string line;
    int count = 0;
    while (std::getline(file, line)) {
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            try {
                int bank = std::stoi(line.substr(0, colon));
                int addr = std::stoi(line.substr(colon + 1), nullptr, 16);
                call_targets.insert(make_address(bank, addr));
                count++;
            } catch (...) {
                continue;
            }
        }
    }
    std::cout << "Loaded " << count << " entry points from trace file: " << path << "\n";
}

AnalysisResult analyze(const ROM& rom, const AnalyzerOptions& options) {
    AnalysisResult result;
    result.rom = &rom;
    result.entry_point = 0x100;
    
    // Add standard GameBoy entry points
    result.interrupt_vectors = {0x40, 0x48, 0x50, 0x58, 0x60};  // Interrupt vectors
    
    Decoder decoder(rom);
    
    // Detect which banks are used
    std::set<uint8_t> known_banks = detect_bank_values(rom);
    
    std::queue<AnalysisState> work_queue;
    std::set<uint32_t> visited;
    // Pointer scanning pass
    find_pointer_entry_points(rom, result, work_queue);
    
    // Entry point is always a function (bank 0)
    result.call_targets.insert(make_address(0, 0x100));
    
    // RST vectors
    bool skip_rst30 = rst28_uses_rst30(rom);
    for (uint16_t vec : {0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38}) {
        if (is_rst_padding(rom, vec)) continue;
        if (vec == 0x30 && skip_rst30) continue;
        result.call_targets.insert(make_address(0, vec));
    }
    
    // Interrupt vectors
    for (uint16_t vec : result.interrupt_vectors) {
        result.call_targets.insert(make_address(0, vec));
    }

    // Load from trace if provided
    load_trace_entry_points(options.trace_file_path, result.call_targets);

    // Initial work queue seeding
    for (uint32_t target : result.call_targets) {
        uint8_t bank = get_bank(target);
        work_queue.push({target, -1, -1, -1, -1, -1, -1, -1, (bank > 0 ? bank : (uint8_t)1)});
    }
    
    // Manual entry points
    for (uint32_t target : options.entry_points) {
        if (result.call_targets.find(target) == result.call_targets.end()) {
            result.call_targets.insert(target);
            uint8_t bank = get_bank(target);
            work_queue.push({target, -1, -1, -1, -1, -1, -1, -1, (bank > 0 ? bank : (uint8_t)1)});
        }
    }
    
    // For MBC games
    if (rom.header().mbc_type != MBCType::NONE && options.analyze_all_banks) {
        std::cerr << "Analyzing all " << known_banks.size() << " banks\n";
        for (uint8_t bank : known_banks) {
                if (bank > 0) {
                    if (is_likely_valid_code(rom, bank, 0x4000)) {
                        work_queue.push({make_address(bank, 0x4000), -1, -1, -1, -1, -1, -1, -1, bank});
                        result.call_targets.insert(make_address(bank, 0x4000));
                    } else {
                        // std::cout << "[INFO] Skipping likely data bank " << (int)bank << " at 0x4000\n";
                    }
                }
        }
    }
    
    // Add overlay entry points
    for (const auto& ov : options.ram_overlays) {
        uint32_t addr = make_address(0, ov.ram_addr);
        result.call_targets.insert(addr);
        work_queue.push({addr, -1, -1, -1, -1, -1, -1, -1, 1});
    }

    // Add manual entry points
    for (uint32_t addr : options.entry_points) {
        result.call_targets.insert(addr);
        uint8_t bank = get_bank(addr);
        uint8_t context = (bank > 0) ? bank : 1;
        work_queue.push({addr, -1, -1, -1, -1, -1, -1, -1, context});
    }
    
    // Multi-pass analysis
    bool scanning_pass = false;

    // Explore all reachable code
    while (true) {
        // Drain work queue
        while (!work_queue.empty()) {
            auto item = work_queue.front();
        work_queue.pop();
        
        uint32_t addr = item.addr;
        int known_a = item.known_a;
        int known_b = item.known_b;
        int known_c = item.known_c;
        int known_d = item.known_d;
        int known_e = item.known_e;
        int known_h = item.known_h;
        int known_l = item.known_l;
        uint8_t current_switchable_bank = item.current_bank;
        
        if (visited.count(addr)) continue;
        
        uint8_t bank = get_bank(addr);
        uint16_t offset = get_offset(addr);
        
        // Check if inside any RAM overlay
        const AnalyzerOptions::RamOverlay* overlay = nullptr;
        for (const auto& ov : options.ram_overlays) {
            if (offset >= ov.ram_addr && offset < ov.ram_addr + ov.size) {
                overlay = &ov;
                break;
            }
        }
        
        // Only analyze ROM space or RAM overlays
        if (offset >= 0x8000 && !overlay) continue;
        
        // Bank mapping rules
        if (offset < 0x4000) {
            bank = 0;  // Force bank 0 for this region
            addr = make_address(0, offset);
            if (visited.count(addr)) continue;
        } else if (offset < 0x8000 && bank == 0) {
            bank = 1;  // Default to bank 1
            addr = make_address(1, offset);
            if (visited.count(addr)) continue;
        } else if (overlay) {
             if (visited.count(addr)) continue;
        }
        
        if (bank > 0) current_switchable_bank = bank;
        
        // Calculate ROM offset
        size_t rom_offset;
        if (overlay) {
            uint8_t src_bank = get_bank(overlay->rom_addr);
            uint16_t src_addr = get_offset(overlay->rom_addr);
            if (src_addr < 0x4000) rom_offset = src_addr;
            else rom_offset = static_cast<size_t>(src_bank) * 0x4000 + (src_addr - 0x4000);
            rom_offset += (offset - overlay->ram_addr);
        } else if (offset < 0x4000) {
            rom_offset = offset;
        } else {
            rom_offset = static_cast<size_t>(bank) * 0x4000 + (offset - 0x4000);
        }
        if (rom_offset >= rom.size()) continue;
        
        visited.insert(addr);
        
        // Decode instruction
        Instruction instr;
        if (overlay) {
             uint8_t src_bank = get_bank(overlay->rom_addr);
             uint16_t src_addr = get_offset(overlay->rom_addr) + (offset - overlay->ram_addr);
             instr = decoder.decode(src_addr, src_bank);
             instr.address = offset; 
             instr.bank = 0; // RAM is bank 0
        } else {
            instr = decoder.decode(offset, bank);
        }

        /* -------------------------------------------------------------
         * Constant Propagation (A and HL)
         * ------------------------------------------------------------- */
         
        // Helper to get combined HL
        auto get_known_hl = [&]() -> int {
            if (known_h != -1 && known_l != -1) return (known_h << 8) | known_l;
            return -1;
        };

        // Helper to get combined registers
        auto get_known_bc = [&]() -> int { if (known_b != -1 && known_c != -1) return (known_b << 8) | known_c; return -1; };
        auto get_known_de = [&]() -> int { if (known_d != -1 && known_e != -1) return (known_d << 8) | known_e; return -1; };

        // 8-bit Loads
        if (instr.opcode == 0x06) known_b = instr.imm8;
        else if (instr.opcode == 0x0E) known_c = instr.imm8;
        else if (instr.opcode == 0x16) known_d = instr.imm8;
        else if (instr.opcode == 0x1E) known_e = instr.imm8;
        else if (instr.opcode == 0x26) known_h = instr.imm8;
        else if (instr.opcode == 0x2E) known_l = instr.imm8;
        else if (instr.opcode == 0x3E) known_a = instr.imm8;
        // 16-bit Loads
        else if (instr.opcode == 0x01) { known_b = (instr.imm16 >> 8); known_c = instr.imm16 & 0xFF; }
        else if (instr.opcode == 0x11) { known_d = (instr.imm16 >> 8); known_e = instr.imm16 & 0xFF; }
        else if (instr.opcode == 0x21) { known_h = (instr.imm16 >> 8); known_l = instr.imm16 & 0xFF; }
        // LD r, r'
        else if (instr.opcode >= 0x40 && instr.opcode <= 0x7F && instr.opcode != 0x76) {
            int* regs[] = {&known_b, &known_c, &known_d, &known_e, &known_h, &known_l, nullptr, &known_a};
            int dst = (instr.opcode >> 3) & 7;
            int src = instr.opcode & 7;
            if (regs[dst]) {
                if (src == 6) { // LD r, (HL)
                    int mhl = get_known_hl();
                    if (mhl != -1 && mhl < 0x8000) *regs[dst] = rom.read_banked(mhl < 0x4000 ? 0 : bank, mhl);
                    else *regs[dst] = -1;
                } else if (regs[src]) *regs[dst] = *regs[src];
                else *regs[dst] = -1;
            } else if (dst == 6) { // LD (HL), r
                 // Memory write - conceptually invalidates ROM values if we were tracking them, 
                 // but we only track constant ROM.
            }
        }
        else if (instr.opcode == 0xAF) known_a = 0; // XOR A
        // ADD HL, rr
        else if (instr.opcode == 0x09 || instr.opcode == 0x19 || instr.opcode == 0x29) {
            int val = -1;
            if (instr.opcode == 0x09) val = get_known_bc();
            if (instr.opcode == 0x19) val = get_known_de();
            if (instr.opcode == 0x29) val = get_known_hl();
            int mhl = get_known_hl();
            if (mhl != -1 && val != -1) {
                int res = (mhl + val) & 0xFFFF;
                known_h = (res >> 8); known_l = res & 0xFF;
            } else { known_h = -1; known_l = -1; }
        }
        // Invalidate A on ALU
        else if ((instr.opcode >= 0x80 && instr.opcode <= 0xBF) || (instr.opcode & 0xC7) == 0x06 || instr.opcode == 0x3C || instr.opcode == 0x3D) {
            known_a = -1;
        }
        // POPs
        else if (instr.opcode == 0xC1) { known_b = -1; known_c = -1; }
        else if (instr.opcode == 0xD1) { known_d = -1; known_e = -1; }
        else if (instr.opcode == 0xE1) { known_h = -1; known_l = -1; }
        else if (instr.opcode == 0xF1) { known_a = -1; }

        /* -------------------------------------------------------------
         * Bank Switching Detection
         * ------------------------------------------------------------- */
        if (instr.opcode == 0xEA) { // LD (nn), A
            if (instr.imm16 >= 0x2000 && instr.imm16 <= 0x3FFF) {
                bool is_dynamic = (known_a == -1);
                uint8_t target_b = is_dynamic ? 1 : (known_a == 0 ? 1 : known_a);
                target_b &= 0xFF;
                
                result.bank_tracker.record_bank_switch(addr, target_b, is_dynamic);
                
                if (!is_dynamic) {
                    current_switchable_bank = target_b;
                }
            }
        }

        // Trace logging
        if (options.trace_log) {
            std::cout << "[TRACE] " << std::hex << std::setfill('0') << std::setw(2) << (int)bank
                      << ":" << std::setw(4) << offset << " " << instr.disassemble() << std::dec << "\n";
        }
        
        // Check padding
        if (bank > 0 && instr.opcode == 0xFF) {
            bool is_padding = true;
            for (int i = 1; i < 16; i++) {
                if (rom.read_banked(bank, offset + i) != 0xFF) { is_padding = false; break; }
            }
            if (is_padding) continue;
        }

        if (instr.type == InstructionType::UNDEFINED) {
             std::cout << "[ERROR] Undefined instruction at " << std::hex << (int)bank << ":" << offset << "\n";
             continue;
        }

        if (options.max_instructions > 0 && result.instructions.size() >= options.max_instructions) {
            break;
        }
        
        size_t idx = result.instructions.size();
        result.instructions.push_back(instr);
        result.addr_to_index[addr] = idx;
        
        auto target_bank = [&](uint16_t target) -> uint8_t {
            if (target < 0x4000) return 0;
            if (rom.header().mbc_type == MBCType::NONE) return 1;
            return current_switchable_bank;
        };
        
        if (instr.type == InstructionType::RST) {
            if (is_rst_padding(rom, instr.rst_vector)) continue;
            
            result.call_targets.insert(make_address(0, instr.rst_vector));
            work_queue.push({make_address(0, instr.rst_vector), -1, -1, -1, -1, -1, -1, -1, 1});
            
            bool is_rst28_jt = (instr.rst_vector == 0x28 && is_rst28_jump_table(rom));
            if (is_rst28_jt) {
                std::vector<uint16_t> table_targets = extract_rst28_table_entries(rom, offset, bank);
                for (uint16_t target : table_targets) {
                    uint8_t tbank = (target < 0x4000) ? 0 : bank;
                    if (tbank == 0 && target >= 0x4000) tbank = 1;

                    if (is_likely_valid_code(rom, tbank, target)) {
                        result.call_targets.insert(make_address(tbank, target));
                        work_queue.push({make_address(tbank, target), -1, -1, -1, -1, -1, -1, -1, tbank});
                        result.label_addresses.insert(make_address(tbank, target));
                    }
                }
            } else {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(fall_through);
                work_queue.push({fall_through, known_a, known_b, known_c, known_d, known_e, known_h, known_l, current_switchable_bank});
            }
        } else if (instr.is_call) {
            uint16_t target = instr.imm16;
            uint8_t tbank = target_bank(target);
            instr.resolved_target_bank = tbank;
            
            if (tbank > 0 && tbank != bank) {
                if (!is_likely_valid_code(rom, tbank, target)) continue;
            }

            result.call_targets.insert(make_address(tbank, target));
            work_queue.push({make_address(tbank, target), -1, -1, -1, -1, -1, -1, -1, tbank});
            
            if (tbank != bank) {
                result.stats.cross_bank_calls++;
                result.bank_tracker.record_cross_bank_call(offset, target, bank, tbank);
            }
            
            uint32_t fall_through = make_address(bank, offset + instr.length);
            result.label_addresses.insert(fall_through);
            work_queue.push({fall_through, known_a, known_b, known_c, known_d, known_e, known_h, known_l, current_switchable_bank});
        } else if (instr.is_jump) {
            if (instr.type == InstructionType::JP_NN || instr.type == InstructionType::JP_CC_NN) {
                uint16_t target = instr.imm16;
                uint8_t tbank = target_bank(target);
                instr.resolved_target_bank = tbank;
                if (target >= 0x4000 && target <= 0x7FFF) {
                    if (tbank > 0 && tbank != bank) {
                        if (!is_likely_valid_code(rom, tbank, target)) continue;
                    }
                    result.call_targets.insert(make_address(tbank, target));
                }
                result.label_addresses.insert(make_address(tbank, target));
                work_queue.push({make_address(tbank, target), known_a, known_b, known_c, known_d, known_e, known_h, known_l, tbank});
            } else if (instr.type == InstructionType::JR_N || instr.type == InstructionType::JR_CC_N) {
                uint16_t target = offset + instr.length + instr.offset;
                result.label_addresses.insert(make_address(bank, target));
                work_queue.push({make_address(bank, target), known_a, known_b, known_c, known_d, known_e, known_h, known_l, current_switchable_bank});
            } else if (instr.type == InstructionType::JP_HL) {
                int combined_hl = get_known_hl();
                if (combined_hl != -1) {
                    uint16_t target = (uint16_t)combined_hl;
                    uint8_t tbank = target_bank(target);
                    std::cout << "[ANALYSIS] Resolved static JP HL at " << std::hex << (int)bank << ":" << offset << " -> " << (int)tbank << ":" << target << std::dec << "\n";
                    result.call_targets.insert(make_address(tbank, target));
                    result.label_addresses.insert(make_address(tbank, target));
                    work_queue.push({make_address(tbank, target), known_a, known_b, known_c, known_d, known_e, known_h, known_l, tbank});
                } else {
                    // Backtracking Jump Table Heuristic
                    bool found_table = false;
                    // Scan back for 'LD H, imm' pattern (very common for tables)
                    for (int back = 1; back < 10; back++) {
                        if (offset < back) break;
                        uint8_t op = rom.read_banked(bank, offset - back);
                        if (op == 0x26) { // LD H, imm
                            uint8_t table_h = rom.read_banked(bank, offset - back + 1);
                            std::cout << "[ANALYSIS] Heuristic: Found potential jump table at " << std::hex << (int)table_h << "00 near " << (int)bank << ":" << offset << std::dec << "\n";
                            // Scan the page for addresses that lead to code
                            for (int i = 0; i < 256; i += 2) {
                                uint16_t entry_addr = (table_h << 8) | i;
                                uint8_t lo = rom.read(entry_addr);
                                uint8_t hi = rom.read(entry_addr + 1);
                                uint16_t target = lo | (hi << 8);
                                if (target >= 0x0100 && target < 0x8000) {
                                    uint8_t tbank = target_bank(target);
                                    if (is_likely_valid_code(rom, tbank, target)) {
                                        result.call_targets.insert(make_address(tbank, target));
                                        work_queue.push({make_address(tbank, target), -1, -1, -1, -1, -1, -1, -1, tbank});
                                        found_table = true;
                                    }
                                }
                            }
                            if (found_table) break;
                        }
                    }
                    if (!found_table) std::cout << "[ANALYSIS] Unresolved JP HL at " << std::hex << (int)bank << ":" << offset << std::dec << "\n";
                }
            }
            
            if (instr.is_conditional) {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(fall_through);
                work_queue.push({fall_through, known_a, known_b, known_c, known_d, known_e, known_h, known_l, current_switchable_bank});
            }
        } else if (instr.is_return) {
            if (instr.is_conditional) {
                uint32_t fall_through = make_address(bank, offset + instr.length);
                result.label_addresses.insert(fall_through);
                work_queue.push({fall_through, known_a, known_b, known_c, known_d, known_e, known_h, known_l, current_switchable_bank});
            }
        } else {
            work_queue.push({make_address(bank, offset + instr.length), known_a, known_b, known_c, known_d, known_e, known_h, known_l, current_switchable_bank});
        }
    } // End work_queue loop

    // Aggressive Code Scanning
    if (options.aggressive_scan && !scanning_pass) {
        scanning_pass = true; // prevent infinite loops if we find nothing new
        
        if (options.verbose) std::cout << "[ANALYSIS] Starting aggressive scan for missing code..." << std::endl;
        
        size_t found_count = 0;

        // Iterate through all known banks (and bank 0)
        std::vector<uint8_t> banks_to_scan;
        banks_to_scan.push_back(0);
        for (uint8_t b : known_banks) if (b > 0) banks_to_scan.push_back(b);

        // Track regions found by aggressive scanning to avoid overlapping detection in future passes
        // (Since operands are not marked as 'visited' by the main analysis)
        static std::set<uint32_t> aggressive_regions; 

        for (uint8_t bank : banks_to_scan) {
            uint16_t start_addr = (bank == 0) ? 0x0000 : 0x4000;
            uint16_t end_addr = (bank == 0) ? 0x3FFF : 0x7FFF;
            
            for (uint32_t addr = start_addr; addr <= end_addr; ) {
                uint32_t full_addr = make_address(bank, addr);
                
                // If already visited by ANY means, skip
                if (visited.count(full_addr) || aggressive_regions.count(full_addr)) {
                    addr++; 
                    continue;
                }
                
                // Alignment heuristic: most functions start on some boundary? No.
                // But we can skip obvious padding (0xFF or 0x00)
                uint8_t byte = rom.read_banked(bank, addr);
                if (byte == 0xFF || byte == 0x00) {
                    addr++;
                    continue;
                }

                // Check if this looks like valid code
                int code_len = is_likely_valid_code(rom, bank, addr);
                if (code_len > 0) {
                    if (options.verbose) {
                        std::cout << "[ANALYSIS] Detected potential function at " 
                                  << std::hex << (int)bank << ":" << addr << std::dec << "\n";
                    }
                    
                    // Add as a new entry point
                    uint32_t entry = make_address(bank, addr);
                    result.call_targets.insert(entry);
                    
                    // Add to queue
                    uint8_t context = (bank > 0) ? bank : 1;
                    work_queue.push({entry, -1, -1, -1, -1, -1, -1, -1, context});
                    found_count++;
                    
                    // Mark region as scanned
                    for (int i = 0; i < code_len; i++) {
                        aggressive_regions.insert(make_address(bank, addr + i));
                    }
                    
                    // Skip the block we just found to avoid overlapping detection
                    addr += code_len;
                    continue;
                } else {
                    // Not valid code, skip ahead.
                    addr++;
                }
            }
        }
        
        if (found_count > 0) {
            if (options.verbose) std::cout << "[ANALYSIS] Found " << found_count << " new entry points. Restarting analysis." << std::endl;
            scanning_pass = false; // Reset pass flag to allow further scanning after this batch is analyzed
            continue; // Go back to work_queue processing
        }
    }
    
    // If we get here, we are done
    break; 
    } // End while(true)
    
    // Build basic blocks from instruction boundaries
    std::set<uint32_t> block_starts;
    
    for (uint32_t target : result.call_targets) {
        block_starts.insert(target);
    }
    for (uint32_t target : result.label_addresses) {
        block_starts.insert(target);
    }
    
    // Create blocks
    for (uint32_t start : block_starts) {
        if (!visited.count(start)) continue;
        
        BasicBlock block;
        block.start_address = get_offset(start);
        block.bank = get_bank(start);
        block.is_reachable = true;
        
        if (result.call_targets.count(start)) {
            block.is_function_entry = true;
        }
        
        // Find instructions in this block
        uint32_t curr = start;
        while (visited.count(curr)) {
            auto it = result.addr_to_index.find(curr);
            if (it == result.addr_to_index.end()) break;
            
            block.instruction_indices.push_back(it->second);
            const Instruction& instr = result.instructions[it->second];
            
            block.end_address = get_offset(curr) + instr.length;
            
            // Track successors for control flow
            if (instr.is_jump) {
                if (instr.type == InstructionType::JP_NN || instr.type == InstructionType::JP_CC_NN) {
                    block.successors.push_back(instr.imm16);
                } else if (instr.type == InstructionType::JR_N || instr.type == InstructionType::JR_CC_N) {
                    uint16_t target = get_offset(curr) + instr.length + instr.offset;
                    block.successors.push_back(target);
                }
                // Conditional jumps also fall through
                if (instr.is_conditional) {
                    block.successors.push_back(get_offset(curr) + instr.length);
                }
            } else if (instr.is_return && instr.is_conditional) {
                // Conditional returns fall through if condition is false
                block.successors.push_back(get_offset(curr) + instr.length);
            }
            
            // Check if this ends the block
            if (instr.is_jump || instr.is_return || instr.is_call) {
                // CALLs fall through to next instruction after return
                if (instr.is_call) {
                    block.successors.push_back(get_offset(curr) + instr.length);
                }
                break;
            }
            
            curr = make_address(block.bank, get_offset(curr) + instr.length);
            
            // Check if next instruction starts a new block
            if (block_starts.count(curr)) {
                // Fall through to the new block - add as successor
                block.successors.push_back(get_offset(curr));
                break;
            }
        }
        
        result.blocks[start] = block;
    }
    
    // Create functions from call targets with better merging logic
    std::set<uint32_t> processed_targets;
    
    // Function size threshold for merging (in instructions)
    const int MIN_FUNCTION_SIZE = 3;
    
    for (uint32_t target : result.call_targets) {
        if (processed_targets.count(target)) continue;
        
        auto block_it = result.blocks.find(target);
        if (block_it == result.blocks.end()) continue;
        
        Function func;
        func.name = generate_function_name(get_bank(target), get_offset(target));
        func.entry_address = get_offset(target);
        func.bank = get_bank(target);
        func.block_addresses.push_back(get_offset(target));
        
        // Add all blocks reachable from this function (simple DFS)
        std::queue<uint32_t> func_queue;
        std::set<uint32_t> func_visited;
        func_queue.push(target);
        
        while (!func_queue.empty()) {
            uint32_t block_addr = func_queue.front();
            func_queue.pop();
            
            if (func_visited.count(block_addr)) continue;
            func_visited.insert(block_addr);
            
            auto blk = result.blocks.find(block_addr);
            if (blk == result.blocks.end()) continue;
            
            // Add this block to function if not already there
            if (block_addr != target) {
                func.block_addresses.push_back(get_offset(block_addr));
            }
            
            // Mark all reachable call targets as processed to avoid creating separate functions
            if (result.call_targets.count(block_addr) && block_addr != target) {
                processed_targets.insert(block_addr);
            }
            
            // Follow successors
            for (uint16_t succ : blk->second.successors) {
                uint32_t succ_addr = make_address(blk->second.bank, succ);
                if (!func_visited.count(succ_addr)) {
                    func_queue.push(succ_addr);
                }
            }
        }
        
        result.functions[target] = func;
        processed_targets.insert(target);
    }
    
    // Post-process: Merge very small functions into their callers
    // This reduces the number of single-instruction functions
    std::map<uint32_t, Function> merged_functions = result.functions;
    std::set<uint32_t> functions_to_remove;
    
    for (const auto& [func_addr, func] : result.functions) {
        if (functions_to_remove.count(func_addr)) continue;
        
        // Calculate total number of instructions in function
        int total_instrs = 0;
        for (uint16_t block_addr : func.block_addresses) {
            uint32_t full_blk_addr = make_address(func.bank, block_addr);
            auto blk_it = result.blocks.find(full_blk_addr);
            if (blk_it != result.blocks.end()) {
                total_instrs += blk_it->second.instruction_indices.size();
            }
        }
        
        // If function is too small and not a special entry point, consider merging
        bool is_special_entry = (func.bank == 0 && (
            func.entry_address == 0x100 || // main entry
            (func.entry_address >= 0x00 && func.entry_address <= 0x38) || // RST vectors
            (func.entry_address >= 0x40 && func.entry_address <= 0x60) // interrupt vectors
        ));
        
        if (total_instrs < MIN_FUNCTION_SIZE && !is_special_entry) {
            functions_to_remove.insert(func_addr);
        }
    }
    
    // Remove small functions
    for (uint32_t func_addr : functions_to_remove) {
        merged_functions.erase(func_addr);
        // Also remove from call_targets so they don't get re-created
        result.call_targets.erase(func_addr);
    }
    
    result.functions = merged_functions;
    
    // Update stats
    result.stats.total_instructions = result.instructions.size();
    result.stats.total_blocks = result.blocks.size();
    result.stats.total_functions = result.functions.size();
    
    return result;
}

AnalysisResult analyze_bank(const ROM& rom, uint8_t bank, const AnalyzerOptions& options) {
    (void)bank; // Unused parameter
    // For now, just analyze the whole ROM
    // TODO: Filter to specific bank
    return analyze(rom, options);
}

/* ============================================================================
 * Utility Functions
 * ========================================================================== */

std::string generate_function_name(uint8_t bank, uint16_t address) {
    std::ostringstream ss;
    
    // Check for known GameBoy entry points
    if (bank == 0) {
        switch (address) {
            case 0x0000: return "rst_00";
            case 0x0008: return "rst_08";
            case 0x0010: return "rst_10";
            case 0x0018: return "rst_18";
            case 0x0020: return "rst_20";
            case 0x0028: return "rst_28";
            case 0x0030: return "rst_30";
            case 0x0038: return "rst_38";
            case 0x0040: return "int_vblank";
            case 0x0048: return "int_lcd_stat";
            case 0x0050: return "int_timer";
            case 0x0058: return "int_serial";
            case 0x0060: return "int_joypad";
            case 0x0100: return "gb_main";  // Avoid shadowing C main()
        }
    }

    // Ghidra-derived name (from --names), if any. Reserved vectors above are
    // handled before this and never overridden.
    if (const std::string* cn = lookup_custom_name(bank, address)) {
        return *cn;
    }

    ss << "func_";
    if (bank > 0) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bank << "_";
    }
    ss << std::hex << std::setfill('0') << std::setw(4) << address;
    return ss.str();
}

std::string generate_label_name(uint8_t bank, uint16_t address) {
    std::ostringstream ss;
    ss << "loc_";
    if (bank > 0) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)bank << "_";
    }
    ss << std::hex << std::setfill('0') << std::setw(4) << address;
    return ss.str();
}

void print_analysis_summary(const AnalysisResult& result) {
    std::cout << "=== Analysis Summary ===" << std::endl;
    std::cout << "Total instructions: " << result.stats.total_instructions << std::endl;
    std::cout << "Total basic blocks: " << result.stats.total_blocks << std::endl;
    std::cout << "Total functions: " << result.stats.total_functions << std::endl;
    std::cout << "Call targets: " << result.call_targets.size() << std::endl;
    std::cout << "Label addresses: " << result.label_addresses.size() << std::endl;
    std::cout << "Bank switches detected: " << result.bank_tracker.switches().size() << std::endl;
    std::cout << "Cross-bank calls tracked: " << result.bank_tracker.calls().size() << std::endl;
    
    std::cout << "\nFunctions found:" << std::endl;
    for (const auto& [addr, func] : result.functions) {
        std::cout << "  " << func.name << " @ ";
        if (func.bank > 0) {
            std::cout << std::hex << std::setfill('0') << std::setw(2) << (int)func.bank << ":";
        }
        std::cout << std::hex << std::setfill('0') << std::setw(4) << func.entry_address << std::endl;
    }
}

bool is_likely_data(const AnalysisResult& result, uint8_t bank, uint16_t address) {
    uint32_t full_addr = AnalysisResult::make_addr(bank, address);
    return result.addr_to_index.find(full_addr) == result.addr_to_index.end();
}

} // namespace gbrecomp
