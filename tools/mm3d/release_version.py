#!/usr/bin/env python3
"""Read the shared release version used by build scripts and CI artifact names."""
from pathlib import Path
import re


def version():
    value = (Path(__file__).resolve().parents[2] /
             'resources/mm3d/release-version.txt').read_text().strip()
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(?:beta|rc)\.[1-9][0-9]*)?', value):
        raise ValueError(f'Invalid release version: {value!r}')
    return value


if __name__ == '__main__':
    print(version())
