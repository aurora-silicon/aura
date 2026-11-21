# SPDX-License-Identifier: MIT
"""The label the release workflow builds a ref under, run from the workflow itself.

A tag names its release. v<VERSION> is the release and is labelled VERSION. v<VERSION>.<n> is a
maintenance release that keeps VERSION, because the distributions' Stage 2 updaters parse the
Stage 1 version as <x.y.z>b<N> with an optional -g<commit>. It is labelled VERSION-g<commit>, as
a source build of that commit is. Any other tag is refused. A branch build is dev-<commit>.
"""
from pathlib import Path
import re
import subprocess

REPO = Path(__file__).resolve().parents[2]
WORKFLOW = REPO / ".github/workflows/release.yml"
SHA = "3ff77894567239362a116e7fcbb214c55dccf5a4"


def select_step():
    text = WORKFLOW.read_text()
    start = text.index("- name: Select build version")
    end = text.index("- name:", start + 1)
    step = text[start:end]
    run = step[step.index("run: |") + len("run: |"):]
    lines = [line for line in run.splitlines() if line.strip()]
    indent = min(len(line) - len(line.lstrip()) for line in lines)
    return "\n".join(line[indent:] for line in run.splitlines() if line.strip()) + "\n"


def label(tmp_path, ref_type, ref_name, version="1.0.0b14"):
    (tmp_path / "VERSION").write_text(version + "\n")
    env_file = tmp_path / "github_env"
    env_file.write_text("")
    result = subprocess.run(
        ["bash", "-e", "-c", select_step()], cwd=tmp_path, capture_output=True, text=True,
        env={"PATH": "/usr/bin:/bin", "REF_TYPE": ref_type, "REF_NAME": ref_name,
             "SOURCE_SHA": SHA, "GITHUB_ENV": str(env_file)})
    if result.returncode:
        return None
    found = re.findall(r"^AURA_VERSION_TAG=(.*)$", env_file.read_text(), re.M)
    assert len(found) == 1
    return found[0]


def test_release_tag_is_labelled_version(tmp_path):
    assert label(tmp_path, "tag", "v1.0.0b14") == "1.0.0b14"


def test_maintenance_tag_keeps_the_parsed_label(tmp_path):
    assert label(tmp_path, "tag", "v1.0.0b14.1") == "1.0.0b14-g3ff77894"
    assert label(tmp_path, "tag", "v1.0.0b14.12") == "1.0.0b14-g3ff77894"


def test_other_tags_are_refused(tmp_path):
    for name in ("v1.0.0b15", "1.0.0b14", "v1.0.0b14.", "v1.0.0b14.x", "v1.0.0b14.1.2",
                 "v1.0.0b14-1", "aura-v1.0.0b14", "v1.0.0b1"):
        assert label(tmp_path, "tag", name) is None, name


def test_branch_build_is_a_dev_label(tmp_path):
    assert label(tmp_path, "branch", "main") == "dev-3ff778945672"


def test_only_v_tags_trigger_and_a_tag_can_be_rebuilt():
    text = WORKFLOW.read_text()
    assert "aura-v" not in text
    # A manual run can rebuild an existing tag's sources under that tag's label.
    assert re.search(r"workflow_dispatch:\s*\n\s*inputs:\s*\n\s*tag:", text)
