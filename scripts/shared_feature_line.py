"""Derive one feature line from the two protected release histories.

Only numeric reservations are written, using each repository's own workflow.
The feature line itself needs no writable cross-repository state.
"""
from pathlib import Path
import re
import subprocess


BASE_LINE = (2, 18)
# Each Canary baseline is the peeled source of the recorded tag. The master
# baselines account for separately released stable history.
BASELINES = {
    "Nonary/vibeshine": {
        "refs/heads/canary": "5b8f3c0e2b33bc0ddab59a1dbee490c51e2b2cc6",
        "refs/heads/master": "9c02f262a799acd2733c203cbb7c4e59a55dd8fc",
    },
    "Nonary/Vibepollo": {
        "refs/heads/canary": "e9bf2a6cb83ca989eca45d5725a325ea8c8c50af",
        "refs/heads/master": "5e6ed8594184212d429bb093d7fd47904c9a6fa6",
    },
}
BASE_TAGS = {
    "Nonary/vibeshine": ("refs/tags/2.14.8", "7902888fc89f4bec87a3a555a3faf91fda6df1c8"),
    "Nonary/Vibepollo": ("refs/tags/2.17.6", "8116940877fc6b5dd4915ac095c3afa8baf78750"),
}

# Protected historical tags with feature identities absent from all four
# baseline tips. These lightweight refs are already their exact source SHAs.
HISTORICAL_TAGS = {
    "Nonary/vibeshine": {
        "refs/tags/0.0.2": "4c84852f82ac1c0e10ad92af6975f927eba99ceb",
        "refs/tags/1.14.0-alpha.2": "96a968b0f0c925b81a42a297f8734be3c87937af",
    },
    "Nonary/Vibepollo": {
        "refs/tags/2.0.0-beta.4": "6bb13122731ac65731917b50e57a5519a522c3e6",
    },
}

SHA = re.compile(r"[0-9a-f]{40}")
FEATURE = re.compile(r"^feat(?:\([^\r\n)]*\))?!?:\s+.+$")
CHERRY_PICK = re.compile(r"^\(cherry picked from commit ([0-9a-f]{40})\)$", re.MULTILINE)
CHANGE_ID = re.compile(r"^Change-Id: (I[0-9a-fA-F]{40})$", re.MULTILINE)
MODULE_PATH = "scripts/shared_feature_line.py"


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True, text=True).stdout


def public_url(repository):
    return f"https://github.com/{repository}.git"


def read_refs(repository, supported_version_key):
    raw = git("-c", "credential.helper=", "-c", "protocol.version=2",
              "ls-remote", "--refs", public_url(repository),
              *BASELINES[repository], "refs/tags/*")
    refs = {}
    for line in raw.splitlines():
        oid, ref = line.split("\t", 1)
        if ref not in BASELINES[repository] and not supported_version_key(ref.removeprefix("refs/tags/")):
            continue
        if not SHA.fullmatch(oid) or ref in refs:
            raise ValueError(f"Invalid Git ref in {repository}: {ref}")
        refs[ref] = oid
    baseline_tag, baseline_oid = BASE_TAGS[repository]
    required_tags = {baseline_tag: baseline_oid, **HISTORICAL_TAGS[repository]}
    if (not BASELINES[repository].keys() <= refs.keys()
            or any(refs.get(ref) != oid for ref, oid in required_tags.items())):
        raise ValueError(f"Missing protected source or bootstrap tag history in {repository}")
    return refs


def feature_ids(roots, excluded=()):
    raw = git("log", "--no-notes", "--no-show-signature", "--grep=^feat", "-z",
              "--format=%H%x00%s%x00%b", *roots, "--not", *excluded)
    fields = raw.split("\0")
    if fields.pop() != "" or len(fields) % 3:
        raise ValueError("Invalid commit history for shared feature allocation")
    identities = set()
    for index in range(0, len(fields), 3):
        sha, subject, body = fields[index:index + 3]
        if not FEATURE.fullmatch(subject.strip()):
            continue
        # A port preserves the original message, including any existing
        # Change-Id and origin trailers. git cherry-pick -x appends its link.
        ids = {match.lower() for match in CHANGE_ID.findall(body)}
        if len(ids) > 1:
            raise ValueError(f"Feature commit {sha} has conflicting Change-Id trailers")
        origin = CHERRY_PICK.search(body)
        identities.add(ids.pop() if ids else origin[1] if origin else sha)
    return identities


def feature_line(tips):
    baselines = [commit for branches in BASELINES.values() for commit in branches.values()]
    baselines += [commit for tags in HISTORICAL_TAGS.values() for commit in tags.values()]
    roots = [commit for branches in tips.values() for commit in branches.values()]
    # Restrict the candidate walk by reachability. Subtract the identity set
    # separately so a late cherry-pick of a pre-cutover feature adds nothing.
    new = feature_ids(roots, baselines) - feature_ids(baselines)
    major, minor = BASE_LINE
    return major, minor + len(new)


def load_tips(repository, refs):
    tips = {ref: refs[ref] for ref in BASELINES[repository]}
    required = set(tips.values()) | set(HISTORICAL_TAGS[repository].values())
    missing = [oid for oid in required if subprocess.run(
        ["git", "cat-file", "-e", f"{oid}^{{commit}}"], capture_output=True).returncode]
    if missing:
        git("-c", "credential.helper=", "fetch", "--no-tags", "--no-write-fetch-head",
            "--no-recurse-submodules", public_url(repository), *sorted(set(missing)))
    protocol = git("hash-object", str(Path(__file__))).strip()
    for ref, commit in tips.items():
        try:
            git("merge-base", "--is-ancestor", BASELINES[repository][ref], commit)
        except subprocess.CalledProcessError as error:
            raise ValueError(f"Missing bootstrap ancestry at {repository}:{ref}") from error
        # Check the deployed protocol as data. Never execute peer code.
        try:
            deployed = git("rev-parse", "--verify", f"{commit}:{MODULE_PATH}").strip()
        except subprocess.CalledProcessError as error:
            raise ValueError(f"Shared feature allocation is missing at {repository}:{ref}") from error
        if deployed != protocol:
            raise ValueError(f"Shared feature allocation is not in sync at {repository}:{ref}")
    return tips


def current_line(repository, branch, source_commit, local_tags, supported_version_key, *, require_tip):
    """Validate a coherent snapshot and return its common (major, minor).

    Branches are append-only, and supported numeric reservations are immutable.
    Scanning in reverse order the second time brackets an instant when both
    repositories had these inputs. If either changes while reading, fail closed.
    """
    names = tuple(BASELINES)
    if repository not in BASELINES or f"refs/heads/{branch}" not in BASELINES[repository]:
        raise ValueError("Shared feature allocation requires a protected release branch")
    if not SHA.fullmatch(source_commit):
        raise ValueError("Expected an exact source SHA for shared allocation")
    if git("rev-parse", "--is-shallow-repository").strip() != "false":
        raise ValueError("Shared feature allocation requires complete Git history")
    snapshot = {name: read_refs(name, supported_version_key) for name in names}
    remote_tags = {ref.removeprefix("refs/tags/"): oid
                   for ref, oid in snapshot[repository].items() if ref.startswith("refs/tags/")}
    if remote_tags != {tag: oid for tag, oid in local_tags.items() if supported_version_key(tag)}:
        raise ValueError("Local numeric tag state changed while reading release history; retry")
    tips = {name: load_tips(name, snapshot[name]) for name in names}
    tip = tips[repository][f"refs/heads/{branch}"]
    if require_tip and source_commit != tip:
        raise ValueError("Release source advanced before shared allocation; retry from the new tip")
    git("merge-base", "--is-ancestor", source_commit, tip)
    line = feature_line(tips)
    if line[1] > 255:
        raise ValueError("Shared feature line exceeds the Windows installer version range")
    watermarks = [supported_version_key(ref.removeprefix("refs/tags/"))[:2]
                  for refs in snapshot.values() for ref in refs if ref.startswith("refs/tags/")]
    if any(watermark > line for watermark in watermarks):
        raise ValueError("Shared feature history is behind an existing numeric reservation; stop and reconcile")
    for name in reversed(names):
        if read_refs(name, supported_version_key) != snapshot[name]:
            raise ValueError(f"Shared feature inputs changed while reading {name}; retry")
    return line
