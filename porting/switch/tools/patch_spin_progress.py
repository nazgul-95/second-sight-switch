"""Preserve the optional reviewed progress-wait hook after xboxrecomp lifting."""
from pathlib import Path

path = Path(__file__).resolve().parents[1] / "generated_boot/recomp_types.h"
text = path.read_text(encoding="utf-8")
marker = "/* SECOND_SIGHT_PROGRESS_WAIT: native bounded wait, original OFF path. */"
if marker not in text:
    decl = "void recomp_spin_yield(void);       /* runtime: yield, and the guest lock with it */"
    declarations = decl + "\n" + marker + """
#if defined(SECOND_SIGHT_HW_PROGRESS)
extern int g_recomp_spin_block;
void recomp_spin_poll(void);
void recomp_spin_block(void);
#endif"""
    old = """    if ((++turns & 15u) == 0)
        recomp_spin_wake();         /* someone is waiting: answer now */
    if ((turns & 63u) == 0)
        recomp_spin_yield();        /* lets other guest threads and ISRs run */"""
    new = """#if defined(SECOND_SIGHT_HW_PROGRESS)
    if ((++turns & 15u) == 0) {
        int block = __atomic_load_n(&g_recomp_spin_block, __ATOMIC_RELAXED);
        if (!block) recomp_spin_wake();
        else if (turns & 63u) recomp_spin_poll(); /* retain local fence/flush ticks */
        if ((turns & 63u) == 0) {
            if (block) recomp_spin_block();
            else recomp_spin_yield();
        }
    }
#else
""" + old + "\n#endif"
    if text.count(decl) != 1 or text.count(old) != 1:
        raise SystemExit("spin hint source changed; review before patching")
    text = text.replace(decl, declarations).replace(old, new)
    path.write_text(text, encoding="utf-8", newline="\n")
    print("Patched optional bounded hardware-progress wait")
else:
    print("Hardware-progress header hook already present")

