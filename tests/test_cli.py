import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

executable = str(Path(sys.argv[1]).resolve())
env = os.environ.copy()
env["OPENAI_API_KEY"] = ""
with tempfile.TemporaryDirectory(prefix="cpp-agent-cli-") as directory:
    trace = Path(directory) / "offline.json"
    run = subprocess.run([executable, "--offline", "--trace", str(trace)], cwd=directory,
                         env=env, capture_output=True, text=True, timeout=10)
    assert run.returncode == 0, run.stdout + run.stderr
    steps = json.loads(trace.read_text())["steps"]
    results = [result for step in steps for result in step.get("tool_results", [])]
    assert len(results) == 1 and results[0]["success"]
    run = subprocess.run([executable], cwd=directory, env=env, capture_output=True,
                         text=True, timeout=10)
    assert run.returncode == 1, run.stdout + run.stderr
    assert "No API key configured" in run.stderr
print("Offline CLI and missing-credential exit status passed from an unrelated working directory")
