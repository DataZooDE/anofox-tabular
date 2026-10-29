"""
Tests for the package metadata in pyproject.toml — no extension binary required.
"""

from pathlib import Path

import pytest

tomllib = pytest.importorskip("tomllib")

PYTHON_DIR = Path(__file__).parent.parent
PROJECT = tomllib.loads((PYTHON_DIR / "pyproject.toml").read_text())["project"]


def test_license_matches_the_extension():
    # The package is distributed under the same Business Source License as the extension.
    assert PROJECT["license"] == "BUSL-1.1"


def test_license_file_is_shipped_and_matches_the_repository():
    assert PROJECT["license-files"] == ["LICENSE"]
    shipped = (PYTHON_DIR / "LICENSE").read_text()
    assert shipped == (PYTHON_DIR.parent / "LICENSE").read_text()


def test_python_classifiers_match_requires_python():
    minimum = tuple(int(p) for p in PROJECT["requires-python"].removeprefix(">=").split("."))
    for classifier in PROJECT["classifiers"]:
        version = classifier.removeprefix("Programming Language :: Python :: ")
        if version != classifier and "." in version:
            assert tuple(int(p) for p in version.split(".")) >= minimum, classifier


def test_wheel_ships_py_typed(tmp_path):
    # PEP 561: the marker tells type checkers to use the package's inline annotations.
    import shutil
    import subprocess
    import zipfile

    uv = shutil.which("uv")
    if uv is None:
        pytest.skip("uv is not installed")
    subprocess.run(
        [uv, "build", "--wheel", "--out-dir", str(tmp_path), "--quiet", str(PYTHON_DIR)],
        check=True,
        timeout=300,
    )
    (wheel,) = tmp_path.glob("*.whl")
    names = zipfile.ZipFile(wheel).namelist()
    assert "anofox/py.typed" in names
    assert not any("__pycache__" in name for name in names)
