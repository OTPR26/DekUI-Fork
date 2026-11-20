"""Single application entry: prepare a title on first run, otherwise play it."""

from collections.abc import Sequence
from pathlib import Path
import sys

from . import forge_gui


def _split_launch_arguments(arguments: Sequence[str]) -> tuple[list[str], list[str]]:
    """Separate options from launched-file arguments.

    Desktop integrations may pass the file they opened (for example a ``%U``
    expansion of the AppImage path) even though this entry takes no files.
    Rejecting those arguments left a normally opened AppImage doing nothing.
    """
    options: list[str] = []
    launched: list[str] = []
    for argument in arguments:
        if argument.startswith("-") or not Path(argument).exists():
            options.append(argument)
        else:
            launched.append(argument)
    return options, launched


def main(argv: Sequence[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    options, launched = _split_launch_arguments(arguments)
    if launched:
        print("triaevum: ignoring opened file arguments: " + " ".join(launched),
              file=sys.stderr)
    if options not in ([], ["--forge"]):
        print("Usage: triaevum [--forge]", file=sys.stderr)
        return 2
    if options == ["--forge"] or forge_gui.load_active_title() is None:
        return forge_gui.main()
    try:
        return forge_gui.launch_runtime().wait()
    except (OSError, ValueError, forge_gui.forge.ForgeError) as exc:
        # Show the failure and keep import/repair accessible after an update.
        return forge_gui.main(startup_error=str(exc))
