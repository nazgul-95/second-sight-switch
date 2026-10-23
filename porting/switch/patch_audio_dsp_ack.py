"""Add the existing runtime DSP mailbox acknowledgement to one known XBE call."""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: patch_audio_dsp_ack.py GENERATED_DIR")
    generated = Path(sys.argv[1])
    candidates = []
    for path in generated.glob("*.c"):
        text = path.read_text(encoding="utf-8")
        if "void sub_002B0B10(void)" in text:
            candidates.append((path, text))
    if len(candidates) != 1:
        raise SystemExit(
            f"expected exactly one generated sub_002B0B10, found {len(candidates)}"
        )

    path, text = candidates[0]
    declaration = "extern int xbox_ApuDspAckWord(uint32_t va);"
    store = "    MMIO_WR32(ebx, (eax));\n\nloc_002B0C77:"
    patched_store = (
        "    MMIO_WR32(ebx, (eax));\n"
        "    /* Second Sight's DSSTDFX upload waits for the MCPX DSP to clear\n"
        "     * this doorbell. The Switch runtime has no DSP MMIO path, so use\n"
        "     * its existing address-specific bring-up acknowledgement. */\n"
        "    (void)xbox_ApuDspAckWord(ebx);\n\n"
        "loc_002B0C77:"
    )
    if "xbox_ApuDspAckWord(ebx)" in text:
        if declaration not in text:
            raise SystemExit("DSP acknowledgement call exists without declaration")
        print(f"DSP acknowledgement already present in {path}")
        return 0
    if text.count(store) != 1:
        raise SystemExit(
            f"expected one DSP doorbell store in {path}, found {text.count(store)}"
        )
    if "#include \"recomp_funcs.h\"\n" not in text:
        raise SystemExit(f"unexpected generated C preamble in {path}")
    text = text.replace(
        "#include \"recomp_funcs.h\"\n",
        "#include \"recomp_funcs.h\"\n\n" + declaration + "\n",
        1,
    )
    text = text.replace(store, patched_store, 1)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"Added address-specific DSP acknowledgement in {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

