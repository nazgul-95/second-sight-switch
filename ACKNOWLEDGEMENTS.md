# Acknowledgements, prior art, and tools

This file separates projects that informed the work from tools used to build or analyze it. Studying a project does not mean its code was copied. Any code reuse must be recorded separately with the original file, revision, license, and local changes.

## Runtime and recompilation foundation

- [xboxrecomp @ `eb0aa1d5fcb1`](https://github.com/sp00nznet/xboxrecomp/tree/eb0aa1d5fcb1) — the Xbox static recompilation toolchain. Its upstream repository carries an MIT license. The project also selectively reviews newer translator changes rather than replacing the complete game lift.
- [mksm-switch @ `14741aa08b61`](https://github.com/gerisson1996/mksm-switch/tree/14741aa08b61) — the Switch-enabled runtime/toolkit used as the current porting base. This repository does not vendor that checkout. The inspected upstream tree has no root license file; the nested `xboxrecomp/` directory has its own MIT license. Do not assume that license covers other files in the repository.

## Switch-port references

- [FlatOut 2 Switch port @ `a89f73d64ab5`](https://github.com/antoxa2584x/flatout2-sw/tree/a89f73d64ab5) — renderer and performance implementation studied as a donor. Selected mechanisms were adapted for Second Sight, including GPU unpacking of packed vertex data, render-target feedback snapshots, and a lower-copy texture upload path. These were adapted individually; the donor runtime and title-specific code were not wholesale transplanted. Its MIT notice is retained in [`licenses/third-party-MIT.txt`](licenses/third-party-MIT.txt). Hardware results are tracked separately from code-review/build results.
- [NFS Underground 2 Switch port](https://github.com/antoxa2584x/nfsu2-sw) — related renderer/runtime reference examined during the port comparison.
- [TimeSplitters: Future Perfect recompilation](https://github.com/NeilSeligmann/tsfp-recomp) — a related Free Radical title project reviewed for recompilation and runtime patterns.
- [Split/Second recompilation](https://github.com/phobos665/split2-recomp) — another recompilation project reviewed for comparison. Its title-specific addresses and game logic are not used as Second Sight replacements.

## Binary-analysis references

- [Ghidra XBE loader](https://github.com/XboxDev/ghidra-xbe) — Xbox executable import support for Ghidra.
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) — Xbox symbol-name reference.
- [XboxXBERecomp](https://github.com/TGP482/XboxXBERecomp) — another XBE recompilation project reviewed for comparison.
- [GhidraMCP](https://github.com/LaurieWired/GhidraMCP) — MCP interface used with Ghidra during analysis.

## Tools used in this project workflow

- **Ghidra** with the XBE loader — disassembly and decompilation of the user's Xbox executable.
- **GhidraMCP** and **REA MCP** — assisted inspection and evidence collection. REA's direct XBE load was not successful in the recorded session, so XBE-specific conclusions were checked with Ghidra and the original executable.
- **xboxrecomp** — guest-code translation and generation.
- **devkitPro / devkitA64 / libnx**, **CMake**, **Ninja**, and **MSYS2** — Switch homebrew build toolchain.
- **GitHub MCP** — upstream repository and revision inspection, and repository publishing.

## Attribution and licensing practice

The project-authored code and documentation are licensed under MIT (see [`LICENSE`](LICENSE)). Third-party code retains its own terms; the separate notice for adapted renderer/runtime code is included under `licenses/`. No third-party runtime checkout is vendored. The Xbox executable, extracted game resources, generated guest implementation, NROs, logs, and reverse-engineering databases are excluded. The supplied header artwork has separate permission for public inclusion and is not covered by the project MIT license.

