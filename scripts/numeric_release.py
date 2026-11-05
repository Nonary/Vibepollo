"""Resolve immutable numeric release versions from Conventional Commits.

The resolver is read-only: the release workflow is responsible for creating
the annotated tag after checking the resolved outputs. Stable and prerelease
builds share one numeric version sequence; channel identity lives in release
metadata rather than in the tag suffix.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess


VERSION_RE = re.compile(r"^v?([0-9]+)\.([0-9]+)\.([0-9]+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")
CONVENTIONAL_RE = re.compile(r"^(feat|fix)(?:\([^\r\n)]*\))?!?:\s+.+$")


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def version_key(tag):
    match = VERSION_RE.fullmatch(tag or "")
    return tuple(map(int, match.groups()[:3])) if match else None


def supported_version_key(tag):
    key = version_key(tag)
    if not key or key == (0, 0, 0) or key[0] > 255 or key[1] > 255 or key[2] > 654:
        return None
    return key


def numeric_release_tag(tag):
    """Accept stable/semver release tags, but ignore legacy date-based nightlies."""
    if supported_version_key(tag) is None:
        return False
    suffix = (VERSION_RE.fullmatch(tag).group(4) or "").lower()
    return not re.match(r"^(?:nightly|daily)(?:[.-]|$)", suffix)


def released(release):
    return (release.get("draft") is False and bool(release.get("published_at"))
            and numeric_release_tag(release.get("tag_name", "")))


def validate_published_numeric_bounds(releases):
    for page in releases:
        for release in page:
            if release.get("draft") is False and release.get("published_at"):
                tag = release.get("tag_name", "")
                if VERSION_RE.fullmatch(tag) and supported_version_key(tag) is None:
                    raise ValueError(f"Published product release {tag} exceeds supported Windows numeric version bounds")


def latest_release(releases):
    candidates = [release for page in releases for release in page
                  if release.get("draft") is False and release.get("published_at")]
    if not candidates:
        raise ValueError("No published release is available as a version baseline")
    latest_published_at = max(release["published_at"] for release in candidates)
    latest = [release for release in candidates
              if release["published_at"] == latest_published_at]
    if len(latest) != 1:
        raise ValueError("Latest published release source is ambiguous: releases share the same publication timestamp")
    return latest[0]


def latest_stable_release(releases):
    candidates = [release for page in releases for release in page
                  if released(release) and release.get("prerelease") is False]
    if not candidates:
        raise ValueError("No published stable release is available to anchor the source")
    return max(candidates, key=lambda release: (version_key(release["tag_name"]),
                                                release["published_at"], release["tag_name"]))


def _release_version_order(release, tag_channels):
    tag = release.get("tag_name", "")
    key = supported_version_key(tag)
    qualified_version = None
    suffix = (VERSION_RE.fullmatch(tag).group(4) or "").lower() if VERSION_RE.fullmatch(tag) else ""
    if key is None and suffix == "":
        return None
    channel = tag_channels.get(tag, "")
    if suffix:
        words = re.split(r"[.-]", suffix)
        if "stable" in words:
            rank = 40
        elif "rc" in words:
            rank = 30
        elif "beta" in words:
            rank = 20
        elif "alpha" in words:
            rank = 10
        else:
            rank = 5
        suffix_number = next((int(word) for word in words if word.isdigit()), 0)
    elif channel == "stable" or release.get("prerelease") is False:
        rank, suffix_number = 40, 0
    elif channel == "prerelease":
        rank, suffix_number = 20, 0
    else:
        rank, suffix_number = 5, 0
    return (*key, rank, suffix_number, suffix)


def latest_version_release(releases, refs, tag_channels, qualified_nightly=None):
    candidates = []
    flat = [release for page in releases for release in page
            if release.get("draft") is False and release.get("published_at")]
    for item in flat:
        order = _release_version_order(item, tag_channels)
        if order is None:
            continue
        tag = item["tag_name"]
        if qualified_nightly and qualified_nightly.get("tag") == tag:
            commit = qualified_nightly["source_commit"]
            if _commit_for_tag(tag, refs) != commit:
                raise ValueError("Qualified nightly tag does not match its metadata source")
        else:
            commit = _commit_for_tag(tag, refs)
        candidates.append((order, item["published_at"], item, commit))
    if qualified_nightly and not any(item["tag_name"] == qualified_nightly["tag"] for item in flat):
        key = supported_version_key(qualified_nightly.get("version", ""))
        if key:
            candidates.append(((*key, 5, 0, "nightly"), "", qualified_nightly, qualified_nightly["source_commit"]))
    if not candidates:
        raise ValueError("No published version has a resolvable source commit")
    _order, _published, release, commit = max(candidates, key=lambda item: (item[0], item[1]))
    return release, commit


def published_source_commits(releases, refs):
    sources = set()
    for page in releases:
        for release in page:
            if release.get("draft") is not False or not release.get("published_at"):
                continue
            tag = release.get("tag_name", "")
            sources.add(_commit_for_tag(tag, refs))
    return sorted(sources)


def published_numeric_max(releases, refs, qualified_nightly=None):
    versions = [supported_version_key(release.get("tag_name", ""))
                for page in releases for release in page
                if release.get("draft") is False and release.get("published_at")
                and supported_version_key(release.get("tag_name", ""))]
    if qualified_nightly and qualified_nightly.get("version"):
        versions.append(supported_version_key(qualified_nightly["version"]))
    for tag in refs:
        parsed = supported_version_key(tag)
        # Imported upstream date tags are far outside the installer's supported
        # range. Within that range, every numeric tag (including v-prefixed and
        # suffixed reservations) must be treated as an immutable version claim.
        if parsed:
            versions.append(parsed)
    return max(versions, default=(0, 0, 0))


def conventional_counts(messages):
    features = fixes = 0
    for message in messages:
        header = message.splitlines()[0].strip() if message.splitlines() else ""
        match = CONVENTIONAL_RE.fullmatch(header)
        if match:
            if match[1] == "feat":
                features += 1
            else:
                fixes += 1
    return features, fixes


def next_version(base, messages):
    parsed = version_key(base)
    if parsed is None:
        raise ValueError(f"Invalid published release version: {base}")
    major, minor, patch = parsed
    features, fixes = conventional_counts(messages)
    if features:
        minor += features
        patch = 0
    else:
        patch += fixes
    if features + fixes == 0:
        raise ValueError("No feat or fix commits since the previous published release")
    if major > 255 or minor > 255 or patch > 654:
        raise ValueError("Resolved version exceeds the Windows installer version range")
    return f"{major}.{minor}.{patch}"


def validate_source(repository, branch, confirmation):
    if repository != "Nonary/vibeshine":
        raise ValueError("Numeric release allocation is limited to Nonary/vibeshine")
    allowed = {"master", "main", "canary"}
    if branch not in allowed or confirmation != f"{repository}:{branch}":
        raise ValueError("Select and confirm the repository source branch explicitly")
    subprocess.run(["git", "check-ref-format", f"refs/heads/{branch}"], check=True)


def validate_channel_branch(channel, branch, primary_branch):
    if channel == "stable" and branch != primary_branch:
        raise ValueError("Stable releases must be allocated from the repository default branch")


def _commit_for_tag(tag, refs):
    commit = refs.get(tag)
    if not commit:
        raise ValueError(f"Published release tag {tag} is not available in the fetched source")
    return run("git", "rev-parse", f"{commit}^{{commit}}")


def resolve(releases, refs, source_commit, source_branch, channel, tag_channels=None,
            qualified_nightly=None):
    tag_channels = tag_channels or {}
    if channel not in {"stable", "prerelease", "nightly"}:
        raise ValueError("Release channel must be stable, prerelease, or nightly")
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("Expected an exact source commit SHA")
    validate_published_numeric_bounds(releases)

    prior = latest_release(releases)
    prior_tag = prior["tag_name"]
    legacy_nightly = re.fullmatch(r"(?:v?[0-9]+\.[0-9]+\.[0-9]+-(?:nightly|daily)\..+|daily-20[0-9]{2}-[0-9]{2}-[0-9]{2}(?:\.[0-9]+)?)", prior_tag)
    numeric_prerelease_nightly = (version_key(prior_tag) is not None and
                                  prior.get("prerelease") is True and
                                  qualified_nightly and qualified_nightly.get("tag") == prior_tag)
    if legacy_nightly or numeric_prerelease_nightly:
        if not qualified_nightly or qualified_nightly.get("tag") != prior_tag:
            raise ValueError("Latest published nightly must have validated metadata before it can anchor a numeric release")
        prior_commit = qualified_nightly["source_commit"]
        nightly_version = qualified_nightly.get("version", "")
        if not version_key(nightly_version):
            raise ValueError("Latest published nightly metadata must contain a numeric version core")
        if _commit_for_tag(prior_tag, refs) != prior_commit:
            raise ValueError("Latest published nightly tag does not match its validated source metadata")
    else:
        prior_commit = _commit_for_tag(prior_tag, refs)
    release_versions = [supported_version_key(release.get("tag_name", ""))
                        for page in releases for release in page
                        if release.get("draft") is False and release.get("published_at")
                        and supported_version_key(release.get("tag_name", ""))]
    if qualified_nightly and qualified_nightly.get("version"):
        nightly_key = supported_version_key(qualified_nightly["version"])
        if nightly_key:
            release_versions.append(nightly_key)
    if not release_versions:
        raise ValueError("No supported published numeric release is available as a version base")
    base_version = ".".join(map(str, max(release_versions)))
    stable = latest_stable_release(releases)
    stable_commit = _commit_for_tag(stable["tag_name"], refs)
    highest_version, highest_version_commit = latest_version_release(
        releases, refs, tag_channels, qualified_nightly)

    # Existing releases at this exact source are immutable identities. They
    # cannot be republished or reclassified as another channel.
    for page in releases:
        for release in page:
            if not released(release):
                continue
            commit = _commit_for_tag(release["tag_name"], refs)
            if commit == source_commit:
                return {"should_release": "false", "skip_reason":
                        f"Source already has published release {release['tag_name']} at {source_commit}",
                        "commit": source_commit, "channel": channel,
                        "source_branch": source_branch}

    try:
        run("git", "merge-base", "--is-ancestor", stable_commit, source_commit)
        run("git", "merge-base", "--is-ancestor", highest_version_commit, source_commit)
        run("git", "merge-base", "--is-ancestor", prior_commit, source_commit)
    except subprocess.CalledProcessError as error:
        raise ValueError("Release source must contain the latest stable, highest published version, and previous release source commits; reconcile the source first") from error

    prior_sources = published_source_commits(releases, refs)
    messages = run("git", "log", "--format=%s", source_commit,
                   "--not", *prior_sources).splitlines()
    try:
        candidate = next_version(base_version, messages)
    except ValueError as error:
        if "No feat or fix commits" not in str(error):
            raise
        return {"should_release": "false", "skip_reason": str(error),
                "commit": source_commit, "channel": channel,
                "source_branch": source_branch, "previous_tag": prior_tag}
    candidate_key = version_key(candidate)
    reserved_names = [name for name in (candidate, "v" + candidate) if name in refs]
    if reserved_names:
        if len(reserved_names) != 1 or reserved_names[0] != candidate:
            raise ValueError(f"Numeric release tag {candidate} has an ambiguous v-prefixed or duplicate reservation")
        tag_commit = run("git", "rev-parse", f"{refs[candidate]}^{{commit}}")
        if tag_commit != source_commit:
            raise ValueError(f"Immutable release tag {candidate} is reserved at another source commit")
        if tag_channels.get(candidate) != channel:
            raise ValueError(f"Release tag {candidate} is reserved without matching immutable {channel} channel metadata")
        other_refs = {tag: object_id for tag, object_id in refs.items()
                      if tag not in {candidate, "v" + candidate}}
        if candidate_key <= published_numeric_max(releases, other_refs, qualified_nightly):
            raise ValueError("Reserved retry is no longer newer than every other published or reserved version")
        return {"tag": candidate, "version": candidate, "commit": source_commit,
                "channel": channel, "source_branch": source_branch,
                "should_release": "true", "tag_exists": "true",
                "previous_source_commit": prior_commit,
                "previous_tag": prior_tag}
    maximum = published_numeric_max(releases, refs)
    if candidate_key <= maximum:
        raise ValueError("Resolved version is not newer than every published or reserved stable/prerelease version")

    return {"tag": candidate, "version": candidate, "commit": source_commit,
            "channel": channel, "source_branch": source_branch,
            "should_release": "true", "tag_exists": "false",
            "previous_source_commit": prior_commit,
            "previous_tag": prior_tag}


def validate_tag(releases, refs, tag_channels, tag, source_commit, source_branch,
                 primary_branch, confirmation, channel, qualified_nightly=None,
                 source_branch_commit=None):
    """Recompute a numeric release tag against live GitHub state before signing."""
    validate_source(os.environ.get("GITHUB_REPOSITORY", ""), source_branch, confirmation)
    validate_channel_branch(channel, source_branch, primary_branch)
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("The allocator gate accepts only plain numeric tags")
    if tag_channels.get(tag) != channel:
        raise ValueError("Numeric release tag lacks the exact immutable channel annotation")
    if channel != "stable":
        raise ValueError("Numeric signing currently accepts stable tags only; nightly prereleases use the daily publisher")
    if _commit_for_tag(tag, refs) != source_commit:
        raise ValueError("Numeric release tag target does not match the exact release source commit")
    if not source_branch_commit:
        raise ValueError("Exact confirmed source branch tip is required to validate a numeric tag")
    try:
        run("git", "merge-base", "--is-ancestor", source_commit, source_branch_commit)
    except subprocess.CalledProcessError as error:
        raise ValueError("Numeric release tag source is not reachable from the confirmed source branch") from error
    matching = [release for page in releases for release in page if release.get("tag_name") == tag]
    for release in matching:
        expected_prerelease = channel in {"prerelease", "nightly"}
        if release.get("prerelease") is not expected_prerelease:
            raise ValueError("Existing release channel does not match its immutable tag channel")
    filtered = [[release for release in page if release.get("tag_name") != tag]
                for page in releases]
    result = resolve(filtered, refs, source_commit, source_branch, channel,
                     tag_channels, qualified_nightly)
    if result.get("tag") != tag or result.get("should_release") != "true":
        raise ValueError("Numeric release tag is no longer the live next version; allocate a new version")
    stable = latest_stable_release(filtered)
    result["stable_anchor_tag"] = stable["tag_name"]
    result["stable_anchor_commit"] = _commit_for_tag(stable["tag_name"], refs)
    expected_anchor_tag = os.environ.get("EXPECTED_STABLE_ANCHOR_TAG")
    expected_anchor_commit = os.environ.get("EXPECTED_STABLE_ANCHOR_COMMIT")
    if expected_anchor_tag is not None and (
            result["stable_anchor_tag"] != expected_anchor_tag
            or result["stable_anchor_commit"] != expected_anchor_commit):
        raise ValueError("Stable release anchor changed since this build was prepared")
    return result


def generate_notes(version, source_commit, channel, previous_sources, output_path):
    raw = run("git", "log", "--reverse", "-z", "--format=%H%x00%s%x00%b",
              source_commit, "--not", *previous_sources)
    commits = []
    for record in raw.split("\0\0"):
        if not record:
            continue
        fields = record.split("\0", 2)
        if len(fields) == 2:
            fields.append("")
        if len(fields) != 3:
            raise ValueError("Could not parse exact NUL-framed release notes commit data")
        sha, subject, body = fields
        if not re.fullmatch(r"[0-9a-f]{40}", sha):
            raise ValueError("Release notes commit data contains an invalid full SHA")
        commits.append((sha, subject, body))
    product = "Vibeshine"
    lines = [f"# {product} {version}", "", f"Channel: {channel}.", "",
             "These notes list commits reachable from the released source that are not reachable from any previously published release source.", "",
             "## Changes", ""]
    if not commits:
        lines.append("- No new commits were found in the qualified release range.")
    for sha, subject, body in commits:
        lines.append(f"- [{sha[:12]}](https://github.com/Nonary/vibeshine/commit/{sha}) {subject}")
        if body.strip():
            lines.extend(f"  {line}" if line else "  " for line in body.rstrip().splitlines())
    Path(output_path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_refs():
    refs = {}
    tag_channels = {}
    for line in run("git", "for-each-ref",
                    "--format=%(refname:strip=2)%09%(objectname)%09%(contents:subject)",
                    "refs/tags").splitlines():
        tag, object_id, subject = line.split("\t", 2)
        refs[tag] = object_id
        match = re.match(r"^Vibeshine release channel: (stable|prerelease|nightly)(?:\s|$)", subject)
        if match:
            tag_channels[tag] = match[1]
    return refs, tag_channels


def release_pages(repository):
    return json.loads(run("gh", "api", "--paginate", "--slurp",
                          f"repos/{repository}/releases?per_page=100"))


def latest_qualified_nightly(repository, releases):
    notes_spec = importlib.util.spec_from_file_location(
        "nightly_notes", Path(__file__).with_name("nightly_notes.py"))
    notes = importlib.util.module_from_spec(notes_spec)
    notes_spec.loader.exec_module(notes)

    def read_metadata(asset):
        asset_id = asset.get("id")
        if not isinstance(asset_id, int) or asset_id <= 0:
            raise ValueError("Invalid published nightly metadata asset ID")
        return json.loads(run("gh", "api", "-H", "Accept: application/octet-stream",
                              f"repos/{repository}/releases/assets/{asset_id}"))

    return notes.previous_release(repository, [release for page in releases for release in page],
                                  read_metadata,
                                  lambda tag: run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}"))


def resolve_from_environment():
    repository = os.environ["GITHUB_REPOSITORY"]
    branch = os.environ.get("RELEASE_SOURCE_BRANCH", "")
    channel = os.environ.get("RELEASE_CHANNEL", "")
    validate_source(repository, branch, os.environ.get("RELEASE_SOURCE_CONFIRMED", ""))
    primary_branch = os.environ.get("RELEASE_PRIMARY_BRANCH", "")
    validate_channel_branch(channel, branch, primary_branch)
    run("git", "fetch", "--no-tags", "origin", f"refs/heads/{branch}:refs/remotes/origin/release-source")
    source = run("git", "rev-parse", "refs/remotes/origin/release-source^{commit}")
    run("git", "fetch", "--no-recurse-submodules", "origin", "refs/tags/*:refs/tags/*")
    releases = release_pages(repository)
    refs, tag_channels = read_refs()
    qualified_nightly = latest_qualified_nightly(repository, releases)
    result = resolve(releases, refs, source, branch, channel, tag_channels,
                     qualified_nightly)
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        for key, value in result.items():
            output.write(f"{key}={value}\n")
    print(json.dumps(result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["resolve", "validate-tag", "generate-notes"])
    args = parser.parse_args()
    if args.command == "resolve":
        resolve_from_environment()
    else:
        repository = os.environ["GITHUB_REPOSITORY"]
        releases = release_pages(repository)
        refs, channels = read_refs()
        source_branch = os.environ["RELEASE_SOURCE_BRANCH"]
        run("git", "fetch", "--no-tags", "origin",
            f"+refs/heads/{source_branch}:refs/remotes/origin/numeric-release-source")
        source_branch_commit = run("git", "rev-parse", "refs/remotes/origin/numeric-release-source^{commit}")
        result = validate_tag(releases, refs, channels, os.environ["RELEASE_TAG"],
                              os.environ["RELEASE_SOURCE_COMMIT"],
                              source_branch,
                              os.environ["RELEASE_PRIMARY_BRANCH"],
                              os.environ["RELEASE_SOURCE_CONFIRMED"],
                              os.environ["RELEASE_CHANNEL"],
                              latest_qualified_nightly(repository, releases),
                              source_branch_commit)
        if args.command == "generate-notes":
            prior_releases = [[release for release in page
                               if release.get("tag_name") != os.environ["RELEASE_TAG"]]
                              for page in releases]
            generate_notes(os.environ["RELEASE_TAG"], os.environ["RELEASE_SOURCE_COMMIT"],
                           os.environ["RELEASE_CHANNEL"],
                           published_source_commits(prior_releases, refs),
                           os.environ["RELEASE_NOTES_OUTPUT"])
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            for key, value in result.items():
                output.write(f"{key}={value}\n")
