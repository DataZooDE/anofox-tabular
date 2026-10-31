import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


def test_profile_module_is_exported_from_package_namespace():
    import anofox

    assert "profile" in anofox.__all__
    assert hasattr(anofox, "profile")


PUBLIC_MODULES = ["anomaly", "diff", "money", "pii", "profile", "quality", "validate", "vat"]


def _pyproject_version():
    import re

    text = (Path(__file__).parent.parent / "pyproject.toml").read_text(encoding="utf-8")
    return re.search(r'^version\s*=\s*"([^"]+)"', text, re.MULTILINE).group(1)


def test_version_matches_pyproject():
    import anofox

    assert isinstance(anofox.__version__, str)
    assert anofox.__version__ == _pyproject_version()


def test_version_falls_back_when_package_metadata_is_missing(monkeypatch):
    import importlib
    import importlib.metadata

    import anofox

    def not_installed(name):
        raise importlib.metadata.PackageNotFoundError(name)

    monkeypatch.setattr(importlib.metadata, "version", not_installed)
    try:
        reloaded = importlib.reload(anofox)
        assert reloaded.__version__ == "0.0.0+unknown"
    finally:
        monkeypatch.undo()
        importlib.reload(anofox)


def test_all_lists_the_documented_public_api():
    import anofox

    expected = {"__version__", "connect", "AnofoxConnection", "UnsupportedDuckDBVersionError", *PUBLIC_MODULES}
    assert set(anofox.__all__) == expected
    assert len(anofox.__all__) == len(set(anofox.__all__))


def test_star_import_exports_modules_and_connect():
    namespace = {}
    exec("from anofox import *", namespace)

    for name in ["connect", "AnofoxConnection", "UnsupportedDuckDBVersionError", "__version__", *PUBLIC_MODULES]:
        assert name in namespace, name
    # Private helpers stay private
    assert "_loader" not in namespace
    assert "os" not in namespace
    # Modules are usable without a separate import
    assert callable(namespace["quality"].volume)
    assert hasattr(namespace["validate"], "EmailRule")
