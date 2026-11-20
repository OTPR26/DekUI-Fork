"""Developer-only build of an existing AOT source set against current headers.

Uses a private object cache and never writes into the supplied source directory.
Does not regenerate title code, update a release, or replace a working module.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

TOOLS = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from triaevum_release.whole_aot_object_cache import NativeToolchain, build_generated_cpp_archive
from triaevum_release import whole_aot_object_cache as cache


def main():
    p = argparse.ArgumentParser(__doc__)
    for name in ("generated", "output", "compiler", "archiver", "support", "include"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--jobs", type=int, default=2)
    p.add_argument("--object-cache", type=Path)
    p.add_argument("--max-new-objects", type=int)
    p.add_argument("--timeout-seconds", type=int, default=600)
    pgo = p.add_mutually_exclusive_group()
    pgo.add_argument("--pgo-generate", action="store_true",
                     help="instrument the module; LLVM_PROFILE_FILE names the .profraw output")
    pgo.add_argument("--pgo-generate-ir", action="store_true",
                     help="like --pgo-generate with IR-level counters; its profile enables CS-PGO")
    pgo.add_argument("--pgo-use", type=Path, help="merged .profdata from real workloads")
    # Context-sensitive PGO runs in the ThinLTO backend after inlining, so it
    # changes only the link: objects compiled with --pgo-use are reused.
    cspgo = p.add_mutually_exclusive_group()
    cspgo.add_argument("--cspgo-generate", action="store_true",
                       help="with --pgo-use: add post-inline counters at link time")
    cspgo.add_argument("--cspgo-use", type=Path,
                       help="with --pgo-use: merged profile including context-sensitive counters")
    args = p.parse_args()
    if (args.cspgo_generate or args.cspgo_use) and not args.pgo_use:
        p.error("context-sensitive PGO requires --pgo-use")
    repo = TOOLS.parent
    args.output.mkdir(parents=True, exist_ok=True)
    cache_root = args.object_cache or args.output / "objects"
    profile_mode = ("generate" if args.pgo_generate else "generate-ir" if args.pgo_generate_ir
                    else "use" if args.pgo_use else "none")
    toolchain = NativeToolchain(compiler=args.compiler, archiver=args.archiver,
                                profile_instrumentation=profile_mode,
                                profile_data=args.pgo_use.resolve() if args.pgo_use else None)
    profile_flags = cache._profile_arguments(toolchain)
    if args.max_new_objects is not None:
        # Preflight uses the same identities as the builder. Refuse accidental
        # full rebuilds when measuring a small emitter-only experiment.
        sysroot = cache.compiler_sysroot(toolchain.compiler, toolchain.sysroot)
        tool_id, checked_toolchain = cache._toolchain_identity(toolchain, sysroot)
        dep_id, _ = cache._dependency_identity(cache.default_dependency_files(
            repo, args.generated, args.include))
        arguments = cache._compile_arguments(checked_toolchain, args.generated,
                                             repo, args.include, sysroot)
        _, sources = cache._load_generated_sources(args.generated)
        missing = []
        for source in sources:
            key = cache.source_object_key(source.sha256, dep_id, tool_id, arguments)
            path = cache_root / "objects" / key[:2] / f"{key}.obj"
            if not path.is_file() or not path.with_suffix(".json").is_file():
                missing.append(source.name)
        print(json.dumps({"new_object_count": len(missing),
                          "first_new_objects": missing[:10]}), flush=True)
        if len(missing) > args.max_new_objects:
            raise RuntimeError(f"Refusing {len(missing)} new objects; limit {args.max_new_objects}")
    archive = build_generated_cpp_archive(
        generated_directory=args.generated, repo_root=repo,
        nlohmann_include=args.include, cache_root=cache_root,
        toolchain=toolchain,
        jobs=args.jobs)
    print(json.dumps(archive), flush=True)
    (args.output / "archive.json").write_text(json.dumps(archive, indent=2))
    runtime = repo / "tools/oot3d/native_game_runtime"
    a32 = repo / "tools/oot3d/native_a32_runtime"
    obj = args.output / "wrapper.obj"
    dll = args.output / "triaevum_title_aot.dll"
    compile_command = [str(args.compiler), "--target=x86_64-pc-windows-msvc",
        "/nologo", "/TP", "/EHsc", "/O2", "/Ob2", "/DNDEBUG", "/std:c++20",
        "/MT", "/bigobj", "/fp:strict", "/w", "/Zc:preprocessor", "/Brepro",
        "/DNOMINMAX", "/DOOT3D_NATIVE_GENERATED_WHOLE_AOT=1", *profile_flags,
        *(f"/I{x}" for x in (args.generated, runtime, a32, a32 / "upstream", args.include)),
        "/c", f"/Fo{obj}", str(runtime / "triaevum_title_whole_aot_plugin.cpp")]
    link_command = [str(args.compiler), "--target=x86_64-pc-windows-msvc", "/nologo",
        "/LD", "/MT", f"/Fe{dll}", str(obj), archive["archive"], str(args.support),
        # The driver adds the profile runtime only when generating counters.
        *(("-fprofile-instr-generate",) if args.pgo_generate or args.cspgo_generate else ()),
        *(("-fprofile-generate",) if args.pgo_generate_ir else ()),
        "-fuse-ld=lld", "/link", "/NOIMPLIB", "/OPT:REF", "/OPT:ICF",
        "/INCREMENTAL:NO", "/Brepro", f"/threads:{args.jobs}",
        f"/OPT:LLDLTOJOBS={args.jobs}", f"/lldltocache:{args.output / 'thinlto-cache'}",
        f"/MAP:{args.output / 'title.map'}",
        *(("/lto-cs-profile-generate",) if args.cspgo_generate else ()),
        *((f"/lto-cs-profile-file:{args.cspgo_use.resolve()}",) if args.cspgo_use else ())]
    steps = [("wrapper", compile_command)]
    if args.cspgo_generate:
        # Link-time-only instrumentation leaves the runtime's default bias
        # variables undefined on COFF; zero means no counter relocation.
        bias = args.output / "cspgo_profile_bias.c"
        bias.write_text("\n".join((
            "#include <stdint.h>",
            "intptr_t __llvm_profile_counter_bias = 0;",
            "intptr_t __llvm_profile_bitmap_bias = 0;", "")), encoding="ascii")
        bias_obj = args.output / "cspgo_profile_bias.obj"
        steps.append(("bias", [str(args.compiler), "--target=x86_64-pc-windows-msvc", "/nologo",
                               "/MT", "/c", f"/Fo{bias_obj}", str(bias)]))
        link_command.insert(link_command.index("-fuse-ld=lld"), str(bias_obj))
    steps.append(("link", link_command))
    for label, command in steps:
        print(f"Starting {label}", flush=True)
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            stdout, stderr = process.communicate(timeout=args.timeout_seconds)
        except subprocess.TimeoutExpired:
            # clang-cl owns an lld child. Killing only the driver leaves a
            # memory-heavy linker running after the experiment has failed.
            subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                           capture_output=True, check=False)
            stdout, stderr = process.communicate()
            (args.output / f"{label}.log").write_text(stdout + stderr)
            raise RuntimeError(f"{label} exceeded {args.timeout_seconds} seconds")
        (args.output / f"{label}.log").write_text(stdout + stderr)
        if process.returncode:
            raise subprocess.CalledProcessError(process.returncode, command)
    print(f"Built {dll}: {dll.stat().st_size} bytes", flush=True)


if __name__ == "__main__":
    main()
