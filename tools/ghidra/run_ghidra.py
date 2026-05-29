#!/usr/bin/env python3
"""Run Ghidra headless analysis on a Game Boy ROM and export
functions.json / symbols.json / decompiled.json.

Requires Ghidra with the GhidraBoy (SM83) extension installed. Point at the
Ghidra install with --ghidra or the GHIDRA_INSTALL_DIR environment variable.

Example:
    python run_ghidra.py roms/oracle-of-ages.gbc -o ghidra_out/ages
    python build_names.py ghidra_out/ages -o ages/names.sym
    gbrecomp roms/oracle-of-ages.gbc -o ages/ --names ages/names.sym
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", help="path to the .gb/.gbc ROM")
    ap.add_argument("-o", "--out", default="ghidra_out",
                    help="output directory for the JSON files (default: ghidra_out)")
    ap.add_argument("--ghidra", default=os.environ.get("GHIDRA_INSTALL_DIR"),
                    help="Ghidra install dir (default: $GHIDRA_INSTALL_DIR)")
    ap.add_argument("--loader", default="GameBoyLoader",
                    help="Ghidra loader class (default: GameBoyLoader, from GhidraBoy)")
    ap.add_argument("--max-decomp", type=int, default=0,
                    help="max functions to decompile, 0 = all (decompiling everything is slow)")
    ap.add_argument("--keep-project", action="store_true",
                    help="keep the temporary Ghidra project instead of deleting it")
    args = ap.parse_args()

    if not args.ghidra:
        sys.exit("error: Ghidra install not found. Pass --ghidra or set GHIDRA_INSTALL_DIR.")
    if not os.path.isfile(args.rom):
        sys.exit("error: ROM not found: %s" % args.rom)

    analyze = os.path.join(args.ghidra, "support",
                           "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")
    if not os.path.isfile(analyze):
        sys.exit("error: analyzeHeadless not found at %s" % analyze)

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    proj_dir = tempfile.mkdtemp(prefix="gbghidra_")

    cmd = [
        analyze, proj_dir, "gbproj",
        "-import", os.path.abspath(args.rom),
        "-loader", args.loader,
        "-scriptPath", HERE,
        "-postScript", "ExportGbJson.java", out, str(args.max_decomp),
        "-overwrite",
    ]
    if not args.keep_project:
        cmd.append("-deleteProject")

    print("[run_ghidra] " + " ".join('"%s"' % c if " " in c else c for c in cmd))
    try:
        rc = subprocess.call(cmd)
    finally:
        if not args.keep_project:
            shutil.rmtree(proj_dir, ignore_errors=True)

    produced = [f for f in ("functions.json", "symbols.json", "decompiled.json")
                if os.path.isfile(os.path.join(out, f))]
    print("[run_ghidra] analyzeHeadless exit=%d; produced: %s" % (rc, ", ".join(produced) or "(none)"))
    if len(produced) < 2:
        sys.exit("error: export incomplete — check that GhidraBoy is installed and the ROM is a valid GB image.")


if __name__ == "__main__":
    main()
