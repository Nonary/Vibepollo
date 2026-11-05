"""Allocate immutable numeric release versions from conventional commits.

The caller supplies published releases and the exact Git history. Stable and
prerelease releases share one numeric namespace; their channel is release
metadata, never a suffix on the version.
"""
from __future__ import annotations

import re
import subprocess


VERSION_RE = re.compile(r"^v?([0-9]+)\.([0-9]+)\.([0-9]+)$")
VERSION_TAG_RE = re.compile(r"^v?([0-9]+)\.([0-9]+)\.([0-9]+)(?:[-.]([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")
CONVENTIONAL_RE = re.compile(r"^(feat|fix)(?:\([^)]+\))?!?:\s+.+$")
MAX_COMPONENT = 255
MAX_PATCH = 654


def parse_version(value):
    """Return a numeric version tuple or None for historical suffix tags."""
    match = VERSION_RE.fullmatch(value or "")
    return tuple(map(int, match.groups())) if match else None


def version_key(tag):
    """Match the repository's existing stable/prerelease release ordering."""
    match = VERSION_TAG_RE.fullmatch(tag or "")
    if not match:
        return None
    major, minor, patch = map(int, match.groups()[:3])
    if major > MAX_COMPONENT or minor > MAX_COMPONENT or patch > MAX_PATCH:
        return None
    suffix = (match.group(4) or "").lstrip("-.").lower()
    parts = re.split(r"[-.]", suffix) if suffix else []
    suffix_num = next((int(part) for part in parts if part.isdigit()), 0)
    if not suffix or "stable" in parts:
        rank = 40
    elif "rc" in parts:
        rank = 30
    elif "beta" in parts:
        rank = 20
    elif "alpha" in parts:
        rank = 10
    else:
        rank = 5
    return major, minor, patch, rank, suffix_num, suffix


def require_newer_than(candidate, tags):
    """Fail closed when a numeric candidate is not newer than any known tag."""
    key = version_key(candidate)
    if key is None or candidate.startswith("v"):
        raise ValueError("Allocated release candidate must be a v-less numeric version")
    for tag in tags:
        existing = version_key(tag)
        if existing is not None and key <= existing:
            raise ValueError(f"Allocated version {candidate} is not newer than existing release/tag {tag}")


def verify_annotated_reservation(tag, source, channel):
    """Verify immutable Git tag metadata binds a numeric version to one channel."""
    if (parse_version(tag) is None or tag.startswith("v")
            or not re.fullmatch(r"[0-9a-f]{40}", source)
            or channel not in {"stable", "nightly"}):
        raise ValueError("Invalid annotated release reservation")
    raw = subprocess.run(["git", "cat-file", "tag", f"refs/tags/{tag}"], check=True,
                         capture_output=True, text=True).stdout
    header, separator, message = raw.partition("\n\n")
    fields = dict(line.split(" ", 1) for line in header.splitlines() if " " in line)
    expected_message = f"Vibepollo release channel: {channel}\nsource: {source}\n"
    if (not separator or fields.get("type") != "commit" or fields.get("tag") != tag
            or fields.get("object") != source or message != expected_message):
        raise ValueError("Annotated release tag does not match its exact version, channel and source")
    tagger = fields.get("tagger", "")
    if not re.search(r"\s[0-9]{9,12} [+-][0-9]{4}$", tagger):
        raise ValueError("Annotated release tag is missing a valid tagger timestamp")


def published_numeric_releases(releases):
    """Yield (version tuple, tag, release) for published numeric releases."""
    found = {}
    for page in releases:
        for release in page:
            if (release.get("draft") is not False or not release.get("published_at")):
                continue
            version = parse_version(release.get("tag_name"))
            if version is None:
                continue
            if version in found:
                raise ValueError(f"Numeric version {'.'.join(map(str, version))} is already published more than once")
            found[version] = (release["tag_name"], release)
    return [(version, *found[version]) for version in sorted(found)]


def conventional_counts(messages):
    """Count each supplied commit once; merges and non-conventional commits are caller-filtered."""
    features = fixes = 0
    for message in messages:
        lines = message.splitlines()
        if not lines:
            continue
        match = CONVENTIONAL_RE.fullmatch(lines[0].strip())
        if match:
            kind = match.group(1)
            features += kind == "feat"
            fixes += kind == "fix"
    return {"feat": features, "fix": fixes}


def allocate_version(base, messages):
    """Bump once per feature, or once per fix when there are no features."""
    if isinstance(base, str):
        base = parse_version(base)
    if (not base or len(base) != 3 or any(type(part) is not int or part < 0 for part in base)
            or base[0] > MAX_COMPONENT or base[1] > MAX_COMPONENT or base[2] > MAX_PATCH):
        raise ValueError("Published base version exceeds the supported Windows packaging range")
    counts = conventional_counts(messages)
    major, minor, patch = base
    if counts["feat"]:
        minor += counts["feat"]
        patch = 0
    else:
        patch += counts["fix"]
    if not (0 <= major <= MAX_COMPONENT and 0 <= minor <= MAX_COMPONENT and 0 <= patch <= MAX_PATCH):
        raise ValueError("Allocated version exceeds the supported Windows packaging range")
    if not counts["feat"] and not counts["fix"]:
        raise ValueError("No eligible conventional feat/fix commits since the previous published release")
    return f"{major}.{minor}.{patch}"


def commit_messages(base_commit, target_commit):
    """Read conventional commit messages exactly once from the ancestry range."""
    if not re.fullmatch(r"[0-9a-f]{40}", base_commit or "") or not re.fullmatch(r"[0-9a-f]{40}", target_commit or ""):
        raise ValueError("Version allocation requires exact source commit SHAs")
    subprocess.run(["git", "merge-base", "--is-ancestor", base_commit, target_commit], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    commits = subprocess.run(["git", "rev-list", "--reverse", f"{base_commit}..{target_commit}"],
                             check=True, capture_output=True, text=True).stdout.splitlines()
    # Deduplicate defensively while preserving the graph's deterministic order.
    seen = set()
    unique = [commit for commit in commits if not (commit in seen or seen.add(commit))]
    return [subprocess.run(["git", "show", "-s", "--format=%B", commit], check=True,
                           capture_output=True, text=True).stdout.rstrip("\n") for commit in unique]


def commit_messages_since_sources(target_commit, source_commits):
    """Return each target-reachable commit absent from every published source history."""
    if not re.fullmatch(r"[0-9a-f]{40}", target_commit or ""):
        raise ValueError("Version allocation requires an exact target commit SHA")
    sources = list(dict.fromkeys(source_commits))
    if not sources or any(not re.fullmatch(r"[0-9a-f]{40}", source or "") for source in sources):
        raise ValueError("Version allocation requires exact published source commits")
    commits = subprocess.run(["git", "rev-list", "--reverse", target_commit, "--not", *sources],
                             check=True, capture_output=True, text=True).stdout.splitlines()
    # Git's revision walk is unique by object ID; keep a defensive deduplication
    # so any alternate source notation cannot count one commit twice.
    seen = set()
    unique = [commit for commit in commits if not (commit in seen or seen.add(commit))]
    return [subprocess.run(["git", "show", "-s", "--format=%B", commit], check=True,
                           capture_output=True, text=True).stdout.rstrip("\n") for commit in unique]


def latest_release(releases):
    """Return latest published numeric release by version, rejecting ambiguity."""
    candidates = published_numeric_releases(releases)
    if not candidates:
        raise ValueError("No published numeric release is available to anchor version allocation")
    version, tag, release = candidates[-1]
    return version, tag, release


def latest_published_base(releases):
    """Return the greatest published stable/prerelease version core."""
    candidates = {}
    for page in releases:
        for release in page:
            if release.get("draft") is not False or not release.get("published_at"):
                continue
            tag = release.get("tag_name", "")
            key = version_key(tag)
            if key is None:
                continue
            if key in candidates:
                raise ValueError(f"Ambiguous published release ordering for {tag}")
            candidates[key] = (tag, release)
    if not candidates:
        raise ValueError("No published numeric release is available to anchor version allocation")
    tag, release = candidates[max(candidates)]
    match = VERSION_TAG_RE.fullmatch(tag)
    return tuple(map(int, match.groups()[:3])), tag, release
