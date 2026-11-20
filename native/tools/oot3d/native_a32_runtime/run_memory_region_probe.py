"""Build differential tests, optionally including the two measured audio kernels."""
import argparse
from dataclasses import replace
import json
from pathlib import Path
import subprocess
from test_whole_aot_memory_regions import copy_fixture
from whole_aot_cpp import _load_functions, _render_region_function, _symbol, SELECTION_FORMAT


def main():
    p = argparse.ArgumentParser(__doc__)
    for name in ("output", "compiler", "support", "include"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--program", type=Path)
    p.add_argument("--code", type=Path)
    args = p.parse_args()
    here = Path(__file__).resolve().parent
    args.output.mkdir(parents=True, exist_ok=True)
    functions = [copy_fixture()]
    if args.program:
        loaded, _ = _load_functions(json.loads(args.program.read_text()),
            {"format": SELECTION_FORMAT, "functions": [{"entry": entry} for entry in (0x4A022C, 0x4A0338)]},
            args.code.read_bytes())
        functions.extend(replace(f, name="Audio") for f in loaded)
    source = []
    for name, selected in (("Baseline", frozenset()), ("Candidate", frozenset(f.entry for f in functions))):
        source.append(f"namespace {name} {{")
        for function in functions:
            source.extend(_render_region_function(function, {}, frozenset(), frozenset(), selected))
        source.append("}")
    source.append("static const std::array<TestFunction, %d> kFunctions{{" % len(functions))
    for f in functions:
        symbol = _symbol(f.name, f.entry)
        source.append(f"    {{0x{f.entry:X}U, Baseline::Execute_{symbol}, Candidate::Execute_{symbol}}},")
    source.append("}};")
    (args.output / "memory_region_fixture.inc").write_text('\n'.join(source))
    obj, exe = args.output / "probe.obj", args.output / "probe.exe"
    subprocess.run([str(args.compiler), "/nologo", "/MT", "/c", "/O2", "/std:c++20", "/EHsc", "/DNOMINMAX",
                    *(f"/I{v}" for v in (args.output, here, here / "upstream", here.parent / "native_game_runtime", args.include)),
                    str(here / "memory_region_probe.cpp"), f"/Fo{obj}"], check=True, timeout=120)
    subprocess.run([str(args.compiler), "/nologo", "/MT", str(obj), str(args.support), f"/Fe{exe}",
                    "-fuse-ld=lld", "/link", "/NOIMPLIB"], check=True, timeout=120)
    subprocess.run([str(exe)], check=True, timeout=180)


if __name__ == '__main__':
    main()
