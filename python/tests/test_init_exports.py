import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))


def test_profile_module_is_exported_from_package_namespace():
    import anofox

    assert "profile" in anofox.__all__
    assert hasattr(anofox, "profile")
