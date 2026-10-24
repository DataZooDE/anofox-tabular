"""
The NER model download from Hugging Face can be turned off (#110).

Each case runs the extension in a subprocess with HOME pointing at an empty
directory: the NER model manager is a process-wide singleton, and the model
directory is derived from HOME.
"""

import json
import os
import subprocess
import sys
import textwrap

import pytest

SCRIPT = textwrap.dedent(
    """
    import json, sys
    import duckdb
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{sys.argv[1]}'")
    for statement in json.loads(sys.argv[2]):
        con.execute(statement)
    names = [r[0] for r in con.execute(
        "SELECT pii_mask('John works at Microsoft, mail john@corp.com', 'redact')").fetchall()]
    status = con.execute("SELECT model_status, status_message FROM anofox_ner_status()").fetchone()
    print(json.dumps({"masked": names[0], "status": status[0], "message": status[1]}))
    """
)


def run_in_empty_home(ext_path, tmp_path, statements):
    env = dict(os.environ, HOME=str(tmp_path), DATAZOO_DISABLE_TELEMETRY="1")
    out = subprocess.run(
        [sys.executable, "-I", "-c", SCRIPT, ext_path, json.dumps(statements)],
        env=env, capture_output=True, text=True, timeout=300, check=True,
    )
    return json.loads(out.stdout.strip().splitlines()[-1])


@pytest.fixture
def openvino_ext(ext_path):
    if ext_path is None:
        pytest.skip("Extension binary not found. Set ANOFOX_EXT_PATH or run 'make release'.")
    import duckdb
    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{ext_path}'")
    row = con.execute("SELECT onnx_available FROM anofox_ner_status()").fetchone()
    if row is None or not row[0]:
        pytest.skip("extension built without OpenVINO")
    return ext_path


def test_auto_download_off_never_downloads(openvino_ext, tmp_path):
    result = run_in_empty_home(openvino_ext, tmp_path, ["SET anofox_ner_auto_download = false"])

    assert not (tmp_path / ".duckdb").exists(), "nothing may be written below HOME"
    assert result["status"] == "NOT_DOWNLOADED"
    assert "anofox_ner_auto_download" in result["message"]
    # the pattern recognizers still work
    assert "[EMAIL]" in result["masked"]


def test_auto_download_can_be_turned_back_on(openvino_ext, tmp_path):
    # The model is downloaded once turned back on (network required)
    if os.environ.get("ANOFOX_TEST_NETWORK") != "1":
        pytest.skip("set ANOFOX_TEST_NETWORK=1 to run the Hugging Face download")
    result = run_in_empty_home(openvino_ext, tmp_path, [
        "SET anofox_ner_auto_download = false",
        "SELECT pii_detect('John works at Microsoft')",
        "SET anofox_ner_auto_download = true",
    ])

    assert result["status"] == "LOADED"
    assert "[ORGANIZATION]" in result["masked"]
