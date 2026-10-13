"""Format/lint maintained sources using the repository environment."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
OWNED = ("main/", "components/boards/", "components/board_utils/",
         "components/dac/", "components/dac_es8311/", "components/dac_tas57xx/",
         "components/dac_tas58xx/", "components/display/",
         "components/spiffs_storage/", "components/weather_epd/")


def owned(path):
    return (path.startswith(OWNED)
            and not path.startswith("components/weather_epd/arduino_port/")
            and Path(path).suffix in (".c", ".h"))


def find_tool(name, variable):
    override = os.environ.get(variable)
    if override:
        return shutil.which(override) or override
    # Bypass launchers retaining paths from before the venv was relocated.
    if name == "clang-format":
        try:
            from clang_format import get_executable
            return get_executable(name)
        except ImportError:
            pass
    found = shutil.which(name)
    if found:
        return found
    raise RuntimeError(f"{name} unavailable; set {variable} to its executable")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("format", "lint"))
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--fix", action="store_true")
    parser.add_argument("--env", default="esp32s3-weather")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("files", nargs="*")
    args = parser.parse_intermixed_args()
    os.chdir(ROOT)
    if args.files:
        files = [Path(f).resolve().relative_to(ROOT).as_posix() for f in args.files]
        if any(not owned(f) for f in files):
            raise RuntimeError("Refusing non-maintained source paths")
    else:
        files = sorted({p.relative_to(ROOT).as_posix() for d in OWNED
                        for p in (ROOT / d).rglob("*")
                        if p.is_file() and owned(p.relative_to(ROOT).as_posix())})
    if not files:
        raise RuntimeError("No sources selected")
    if args.mode == "format":
        tool = find_tool("clang-format", "CLANG_FORMAT")
        version = subprocess.check_output([tool, "--version"], text=True)
        expected = os.environ.get("EXPECTED_CLANG_FORMAT_VERSION", "22.1.4")
        if f"version {expected}" not in version:
            raise RuntimeError(f"clang-format {expected} required: {version.strip()}")
        flags = ["--dry-run", "--Werror"] if args.check else ["-i"]
        failed = False
        for offset in range(0, len(files), 30):
            failed |= subprocess.run([tool, *flags, *files[offset:offset+30]]).returncode != 0
        return int(failed)
    build = (args.build_dir or ROOT / ".pio" / "build" / args.env).resolve()
    database = build / "compile_commands.json"
    if not database.is_file():
        raise RuntimeError(f"Missing {database}; build using .venv Python "
                           f"-m platformio run -e {args.env}")
    compiled = set()
    for entry in json.loads(database.read_text(encoding="utf-8")):
        path = Path(entry["file"])
        if not path.is_absolute():
            path = Path(entry["directory"]) / path
        compiled.add(path.resolve())
    sources = [f for f in files if Path(f).suffix == ".c"
               and (ROOT / f).resolve() in compiled]
    if not sources:
        raise RuntimeError("No selected C files in target compile database")
    tool = find_tool("clang-tidy", "CLANG_TIDY")
    failed = False
    for source in sources:
        print(f"Checking {source}", flush=True)
        command = [tool, "-p", str(build), "--warnings-as-errors=*", source]
        if args.fix:
            command.append("--fix")
        # Parsing errors and tool failures must fail the check.
        failed |= subprocess.run(command).returncode != 0
    return int(failed)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
