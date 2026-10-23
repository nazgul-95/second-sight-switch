"""Transfer five upstream join conditions while preserving working C/ABI glue.

Default is analysis-only: stage changed chunks and exact edge plans. --apply
integrates those changes. Upstream computes every edge expression from the
original XBE. Only join locals/assignments/consumers and their observers change.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT.parents[1] / "external_versions/xboxrecomp"
FUNCTIONS = (0x6A430, 0x6AED0, 0x6BA30, 0x6C5E0, 0x175F20, 0x11C8A0)
LABEL = re.compile(r"^loc_([0-9A-F]{8}): ;\s*$", re.M)


def blocks(body):
    marks = list(LABEL.finditer(body))
    return {m.group(1): (m.end(), marks[i+1].start() if i+1<len(marks) else len(body))
            for i,m in enumerate(marks)}


def transfer(old, new):
    declarations = re.findall(r"^    int (_jf_[0-9A-F]{8}) = 0;[^\n]*", new, re.M)
    if len(declarations) != 1:
        raise ValueError("Expected exactly one upstream edge-flag local")
    variable = declarations[0]
    consumer = re.search(r"^    if \("+variable+r" /\*[^\n]+", new, re.M).group(0)
    prior = consumer.replace(variable, "_flags", 1)
    if variable in old:
        if consumer not in old:
            raise ValueError("Existing edge-flag patch differs from upstream")
        return old, dict(variable=variable, already_applied=True)
    if old.count(prior) != 1:
        raise ValueError("Expected original unresolved consumer exactly once")
    old_blocks, new_blocks = blocks(old), blocks(new)
    edits, edges = [], []
    for label, (begin, end) in new_blocks.items():
        block = new[begin:end]
        assignment = re.search(r"^    "+variable+r" = [^\n]+", block, re.M)
        if not assignment:
            continue
        a,b = old_blocks[label]
        current = old[a:b]
        remaining = block[assignment.end():].strip()
        if remaining:
            if not re.fullmatch(r"goto loc_[0-9A-F]{8};", remaining) or current.count(remaining)!=1:
                raise ValueError(f"Unsupported edge terminator in {label}: {remaining}")
            point = current.index(remaining)
            line = current.rfind("\n", 0, point)+1
            replacement = current[:line]+assignment.group(0)+"\n"+current[line:]
        else:
            replacement = current.rstrip()+"\n"+assignment.group(0)+"\n\n"
        edits.append((a,b,replacement))
        edges.append(dict(predecessor=label, assignment=assignment.group(0).strip(),
                          terminator=remaining or "fallthrough"))
    if len(edges)!=2:
        raise ValueError("Expected the two proven original incoming edges")
    for a,b,replacement in sorted(edits, reverse=True):
        old = old[:a]+replacement+old[b:]
    old = old.replace("    int _flags = 0;", "    int "+variable+" = 0; /* upstream edge join */\n    int _flags = 0;", 1)
    old = old.replace(prior, consumer, 1)
    old = old.replace("recomp_unresolved_condition_observe(uint32_t,uint32_t)",
                      "recomp_flag_join_observe(uint32_t,uint32_t,uint32_t)")
    old = re.sub(r"recomp_unresolved_condition_observe\((0x[0-9A-F]+u,0x[0-9A-F]+u)\)",
                 lambda m: "recomp_flag_join_observe("+m.group(1)+","+variable+")", old)
    return old, dict(variable=variable, consumer=consumer.strip(), edges=edges)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--output", type=Path, default=ROOT/"analysis_boot/upstream_flag_join_transfer")
    args=parser.parse_args()
    sys.path.insert(0,str(UPSTREAM))
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    xbe=ROOT/"game_files/game_ntsc.xbe"; image=xbe.read_bytes()
    digest=hashlib.sha256(image).hexdigest()
    if digest!="226ebc4834fbd2f8f7c7e657fb4b1c8b8a7f53105d9189e6615bca1f4983c83e":
        raise ValueError("Unsupported XBE")
    config.configure_from_xbe(str(xbe))
    entries=json.loads((ROOT/"analysis_boot/functions_repaired.json").read_text())
    db={int(f["start"],16):dict(f,end=int(f["end"],16)) for f in entries}
    translator=FunctionTranslator(image,db)
    sources={p:p.read_text(encoding="utf-8-sig") for p in (ROOT/"generated_boot").glob("recomp_*.c")}
    changed, plans={},[]
    for va in FUNCTIONS:
        name=f"sub_{va:08X}"; marker=f"void {name}(void)"
        matches=[p for p,s in sources.items() if marker in s]
        if len(matches)!=1: raise ValueError(f"Ambiguous function {name}")
        p=matches[0]; source=changed.get(p,sources[p]); start=source.index(marker)
        end=source.find("\n/**",start+1); end=len(source) if end<0 else end
        upstream=translator.translate_function(va,db[va])
        body,plan=transfer(source[start:end],upstream)
        changed[p]=source[:start]+body+source[end:]
        plans.append(dict(function=name,file=p.name,**plan))
    args.output.mkdir(parents=True,exist_ok=True)
    for p,source in changed.items():
        (args.output/p.name).write_text(source,encoding="utf-8")
        if args.apply and source!=sources[p]: p.write_text(source,encoding="utf-8")
    receipt=dict(xbe_sha256=digest,upstream=str(UPSTREAM),applied=args.apply,
                 translator_sha256=hashlib.sha256((UPSTREAM/"tools/recomp/translator.py").read_bytes()).hexdigest(),
                 plans=plans)
    (args.output/"edge-plans.json").write_text(json.dumps(receipt,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(receipt,indent=2))


if __name__=="__main__": main()

