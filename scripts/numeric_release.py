"""Resolve immutable numeric release versions from Conventional Commits.

The resolver is read-only: the release workflow is responsible for creating
the annotated tag after checking the resolved outputs. Stable, prerelease and
nightly builds share one numeric version sequence; channel identity lives in
release metadata rather than in the tag suffix.

Every allocation is strictly above the highest published version. Each fix
since the last published source advances patch; any feature advances minor
(once per feature) and resets patch, ignoring the fixes alongside it.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys


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


def warn(message):
    """Surface a non-blocking anomaly as a GitHub Actions warning."""
    print(f"::warning::{message}", file=sys.stderr)


def published(releases):
    return [release for page in releases for release in page
            if release.get("draft") is False and release.get("published_at")]


def latest_release(releases):
    """Most recently published release; informational only, never a gate."""
    candidates = published(releases)
    if not candidates:
        return None
    return max(candidates, key=lambda release: (release["published_at"],
                                                version_key(release.get("tag_name", "")) or (),
                                                release.get("tag_name", "")))


def latest_stable_release(releases):
    candidates = [release for page in releases for release in page
                  if released(release) and release.get("prerelease") is False]
    if not candidates:
        raise ValueError("No published stable release is available to anchor the source")
    return max(candidates, key=lambda release: (version_key(release["tag_name"]),
                                                release["published_at"], release["tag_name"]))


def _tag_commit(tag, refs):
    """Peel a fetched tag to its commit, or None when the tag is unavailable."""
    if not tag or tag not in refs:
        return None
    try:
        return run("git", "rev-parse", f"{refs[tag]}^{{commit}}")
    except subprocess.CalledProcessError:
        return None


def published_tag_commits(releases, refs):
    """Map every published release tag to its source commit (None when missing)."""
    return {release.get("tag_name", ""): _tag_commit(release.get("tag_name", ""), refs)
            for release in published(releases)}


def published_source_commits(releases, refs):
    sources = set()
    for tag, commit in published_tag_commits(releases, refs).items():
        if commit:
            sources.add(commit)
        else:
            # Only widens the feat/fix count; the version still lands above
            # every posted release.
            warn(f"Published release {tag} has no fetched tag; its commits may be counted again")
    return sorted(sources)


def posted_version(releases, qualified_nightly=None):
    """Highest version carried by any published release: stable, prerelease or nightly.

    Legacy suffixed nightlies contribute their numeric core, so 2.0.5-nightly.*
    counts as 2.0.5. This is the only version base; git topology never lowers it.
    """
    versions = [supported_version_key(release.get("tag_name", "")) for release in published(releases)]
    if qualified_nightly:
        versions.append(supported_version_key(qualified_nightly.get("version") or ""))
    versions = [version for version in versions if version]
    if not versions:
        raise ValueError("No published release carries a supported numeric version to build on")
    return max(versions)


def tag_claims(refs):
    """Numeric versions already claimed by any tag, including unpublished reservations."""
    claims = {}
    for tag in refs:
        key = supported_version_key(tag)
        if key:
            claims.setdefault(key, []).append(tag)
    return claims


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
    if repository != "Nonary/Vibepollo":
        raise ValueError("Numeric release allocation is limited to Nonary/Vibepollo")
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
    """Allocate the next version above everything already posted.

    The base is the highest version carried by any published release, so a
    source branch that has not merged the latest stable still numbers above it.
    Git ancestry never blocks allocation: the feat/fix count excludes commits
    reachable from every published release source whether or not they are
    merged. A number already claimed by another tag (for example an unpublished
    reservation from a failed build) is skipped rather than treated as fatal.
    """
    tag_channels = tag_channels or {}
    if channel not in {"stable", "prerelease", "nightly"}:
        raise ValueError("Release channel must be stable, prerelease, or nightly")
    if not re.fullmatch(r"[0-9a-f]{40}", source_commit):
        raise ValueError("Expected an exact source commit SHA")
    validate_published_numeric_bounds(releases)

    base = posted_version(releases, qualified_nightly)
    prior = latest_release(releases)
    prior_tag = prior["tag_name"] if prior else ""
    if qualified_nightly and qualified_nightly.get("tag") == prior_tag:
        prior_commit = qualified_nightly["source_commit"]
    else:
        prior_commit = _tag_commit(prior_tag, refs) or ""
    common = {"commit": source_commit, "channel": channel, "source_branch": source_branch,
              "previous_tag": prior_tag, "previous_source_commit": prior_commit}
    commits = published_tag_commits(releases, refs)

    # Existing releases at this exact source are immutable identities. They
    # cannot be republished or reclassified as another channel.
    for release in published(releases):
        if released(release) and commits.get(release["tag_name"]) == source_commit:
            return {**common, "should_release": "false", "skip_reason":
                    f"Source already has published release {release['tag_name']} at {source_commit}"}

    try:
        stable = latest_stable_release(releases)
    except ValueError:
        stable = None
    stable_commit = commits.get(stable["tag_name"]) if stable else None
    if stable_commit:
        try:
            run("git", "merge-base", "--is-ancestor", stable_commit, source_commit)
        except subprocess.CalledProcessError:
            warn(f"{source_branch} does not contain stable {stable['tag_name']}; "
                 "numbering continues above every published version regardless")

    claims = tag_claims(refs)
    # A retry reuses this source's own reservation while it still outranks
    # every published version and no other tag claims the same number.
    own = sorted((tag for tag in refs
                  if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", tag) and tag_channels.get(tag) == channel),
                 key=version_key, reverse=True)
    for tag in own:
        if version_key(tag) > base and claims[version_key(tag)] == [tag] \
                and _tag_commit(tag, refs) == source_commit:
            return {**common, "tag": tag, "version": tag, "should_release": "true",
                    "tag_exists": "true"}

    sources = sorted({commit for commit in commits.values() if commit})
    missing = sorted(tag for tag, commit in commits.items() if not commit)
    if missing:
        warn(f"Published releases without fetched tags: {', '.join(missing)}; their commits may be counted again")
    messages = run("git", "log", "--format=%s", source_commit, "--not", *sources).splitlines()
    try:
        candidate = next_version(".".join(map(str, base)), messages)
    except ValueError as error:
        if "No feat or fix commits" not in str(error):
            raise
        return {**common, "should_release": "false", "skip_reason": str(error)}
    key = version_key(candidate)
    while key in claims:
        warn(f"Version {'.'.join(map(str, key))} is already tagged ({', '.join(sorted(claims[key]))}); "
             "taking the next patch")
        key = (key[0], key[1], key[2] + 1)
        if key[2] > 654:
            raise ValueError("Resolved version exceeds the Windows installer version range")
    candidate = ".".join(map(str, key))
    return {**common, "tag": candidate, "version": candidate, "should_release": "true",
            "tag_exists": "false"}


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
    raw = subprocess.run(
        ["git", "log", "--reverse", "-z", "--format=%H%x00%s%x00%b",
         source_commit, "--not", *previous_sources],
        check=True, capture_output=True, text=True).stdout
    fields = raw.split("\0")
    if len(fields) % 3 == 1 and fields[-1] == "":
        fields.pop()
    if len(fields) % 3:
        raise ValueError("Could not parse exact NUL-framed release notes commit data")
    commits = []
    for index in range(0, len(fields), 3):
        sha, subject, body = fields[index:index + 3]
        if not re.fullmatch(r"[0-9a-f]{40}", sha):
            raise ValueError("Release notes commit data contains an invalid full SHA")
        commits.append((sha, subject, body))
    product = "Vibepollo"
    lines = [f"# {product} {version}", "", f"Channel: {channel}.", "",
             "These notes list commits reachable from the released source that are not reachable from any previously published release source.", "",
             "## Changes", ""]
    if not commits:
        lines.append("- No new commits were found in the qualified release range.")
    for sha, subject, body in commits:
        lines.append(f"- [{sha[:12]}](https://github.com/Nonary/Vibepollo/commit/{sha}) {subject}")
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
        match = re.match(r"^Vibepollo release channel: (stable|prerelease|nightly)(?:\s|$)", subject)
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
