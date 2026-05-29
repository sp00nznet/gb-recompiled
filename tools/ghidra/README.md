# Ghidra-assisted function naming

The recompiler names functions `func_<bank>_<addr>` by default. If you analyse
the same ROM in [Ghidra](https://ghidra-sre.org/) — naming functions yourself,
or just letting auto-analysis label what it can — this tool lifts those names
into the generated C, so `func_03_4d23` becomes e.g. `AdvanceLogoState`
everywhere it's defined, called, and dispatched.

## Pipeline

```
   ROM ──▶ run_ghidra.py ──▶ functions.json   ──▶ build_names.py ──▶ names.sym
            (analyzeHeadless     symbols.json
             + ExportGbJson)     decompiled.json
                                                                         │
                          gbrecomp <rom> -o out/ --names names.sym ◀─────┘
```

1. **`run_ghidra.py`** drives Ghidra `analyzeHeadless`: imports the ROM with the
   GhidraBoy loader, runs auto-analysis, then runs `ExportGbJson.java` as a
   post-script to dump three JSON files.
2. **`ExportGbJson.java`** (a Ghidra GhidraScript — no Python-in-Ghidra setup
   needed) writes:
   - `functions.json` — every ROM-resident function: `{bank, addr, name, auto, source}`
   - `symbols.json` — meaningful labels (Ghidra placeholders filtered out)
   - `decompiled.json` — decompiled C per function (for human reference; slow,
     so capped by `--max-decomp`)
3. **`build_names.py`** merges functions + symbols into `names.sym`, a
   `bank:addr  c_identifier` map (the recompiler's existing address convention).
   Ghidra placeholder names (`FUN_`, `SUB_`, `LAB_`, …) are dropped, the rest
   are sanitised to valid C identifiers and de-duplicated.
4. **`gbrecomp --names names.sym`** uses those names. Reserved GB vectors
   (`rst_*`, `int_*`, `gb_main`) keep their canonical names; everything else
   that has a Ghidra name gets it.

## Bank mapping

GhidraBoy loads ROM bank 0 at `0x0000-0x3FFF` (block `rom`/`rom0`) and each
higher bank as an overlay block `rom<N>` based at `0x4000`. `ExportGbJson`
reads the block name to recover the bank, producing exactly the `(bank, addr)`
pairs the recompiler keys functions by.

## Setup

You need a Ghidra install with the **GhidraBoy** (SM83) extension:

```bash
# Build GhidraBoy against your Ghidra (JDK 21 + the bundled gradlew):
git clone https://github.com/Gekkio/GhidraBoy.git
cd GhidraBoy
GHIDRA_INSTALL_DIR=/path/to/ghidra ./gradlew
# Install: unzip build/distributions/*GhidraBoy.zip into
#   <ghidra>/Ghidra/Extensions/
```

> GhidraBoy targets Ghidra 11.4.x. On Ghidra 12.x the loader needs small API
> fixups (the `Loader.ImporterSettings` refactor + `HashUtilities` removal);
> a patched copy is what these scripts were validated against.

## Usage

```bash
export GHIDRA_INSTALL_DIR=/path/to/ghidra

# 1. Export (cap decompilation while iterating; use 0 for all — slow):
python tools/ghidra/run_ghidra.py roms/game.gbc -o ghidra_out --max-decomp 0

# 2. Merge into a names file:
python tools/ghidra/build_names.py ghidra_out -o names.sym

# 3. Recompile using the names:
./build/bin/gbrecomp roms/game.gbc -o out/ --names names.sym
```

Re-export and re-run any time you rename more functions in Ghidra — the
generated code's names track your analysis.
