"""Fail-closed daily identity and finalized artifact checks; never mutates GitHub."""
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import shutil
try:
    import numeric_release
    import nightly_notes
except ModuleNotFoundError:  # test modules import this file by path
    from scripts import numeric_release
    from scripts import nightly_notes


def identity(commit, day, base, revision=1):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected an exact source commit SHA")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", base):
        raise ValueError("Expected a numeric source version base")
    major, minor, patch = map(int, base.split("."))
    if major > 255 or minor > 255 or patch > 654 or base == "0.0.0":
        raise ValueError("Source version base exceeds MSI limits or is zero")
    if not 2000 <= day.year <= 2099:
        raise ValueError("Daily version year must fit the installer version scheme")
    if type(revision) is not int or not 1 <= revision <= 999:
        raise ValueError("Nightly revision must be between 1 and 999")
    tag = f"{base}-nightly.d{day:%Y%m%d}.r.{revision}.{commit[:12]}"
    return {"commit": commit, "tag": tag, "version": tag}


def validate_source(repository, branch, confirmation):
    if repository not in {"Nonary/vibeshine", "Nonary/Vibepollo"}:
        raise ValueError("Daily publishing is limited to the designated upstream repositories")
    allowed = {"master", "main", "canary"}
    if branch not in allowed:
        raise ValueError("Daily source must use a confirmed primary or canary branch; legacy vibe/test lanes are retired")
    if confirmation != f"{repository}:{branch}":
        raise ValueError("Set DAILY_RELEASE_SOURCE_BRANCH and explicitly confirm repository:branch in DAILY_RELEASE_SOURCE_CONFIRMED")
    subprocess.run(["git", "check-ref-format", f"refs/heads/{branch}"], check=True)


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def previous_successful_nightly(repository, releases):
    # Share the same verified published baseline used by the release notes.
    spec = importlib.util.spec_from_file_location("nightly_notes", Path(__file__).with_name("nightly_notes.py"))
    notes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(notes)
    def read_metadata(asset):
        asset_id = asset.get("id")
        if not isinstance(asset_id, int) or asset_id <= 0:
            raise ValueError("Invalid published nightly metadata asset ID")
        return json.loads(run("gh", "api", "-H", "Accept: application/octet-stream", f"repos/{repository}/releases/assets/{asset_id}"))
    # A historical tag alone is not proof that this nightly was qualified.
    candidates = [release for page in releases for release in page
                  if any(asset.get("name") == "nightly-metadata.json" for asset in release.get("assets", []))]
    return notes.previous_release(repository, candidates, read_metadata,
                                  lambda tag: run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}"))


def emit_outputs(info):
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        for key, value in info.items():
            output.write(f"{key}={value}\n")
    print(json.dumps(info))


def published_stable_anchor(releases):
    """Return the highest published stable version and its exact release tag."""
    candidates = []
    for page in releases:
        for release in page:
            if (release.get("draft") is not False or release.get("prerelease") is not False
                    or not release.get("published_at")):
                continue
            match = re.fullmatch(r"v?([0-9]+)\.([0-9]+)\.([0-9]+)(?:-stable\.([0-9]+))?",
                                 release.get("tag_name", ""))
            if match:
                version = tuple(map(int, match.groups()[:3]))
                candidates.append((version, int(match[4] or 0), release["published_at"], release["tag_name"]))
    if not candidates:
        raise ValueError("No published stable release is available to anchor the nightly version")
    version, _respin, _published, tag = max(candidates)
    return version, tag


def validate_invocation(branch):
    event = os.environ.get("GITHUB_EVENT_NAME")
    primary = os.environ.get("DAILY_PRIMARY_BRANCH")
    ref = os.environ.get("GITHUB_REF_NAME")
    if (event not in {"schedule", "workflow_dispatch"} or primary not in {"master", "main"}
            or os.environ.get("GITHUB_REF_TYPE") != "branch"
            or not re.fullmatch(r"[0-9a-f]{40}", os.environ.get("GITHUB_SHA", ""))):
        raise ValueError("Expected a branch-based nightly invocation with an exact workflow SHA")
    manual_canary = event == "workflow_dispatch" and ref == "canary" and branch == "canary"
    if ref != primary and not manual_canary:
        raise ValueError("Nightly invocation must use the default branch or explicitly confirmed manual Canary")
    return manual_canary


def resolve():
    repository = os.environ["GITHUB_REPOSITORY"]
    branch = os.environ.get("DAILY_RELEASE_SOURCE_BRANCH", "")
    validate_source(repository, branch, os.environ.get("DAILY_RELEASE_SOURCE_CONFIRMED", ""))
    if branch != os.environ.get("DAILY_PRIMARY_BRANCH") and branch != "canary":
        raise ValueError("Daily source must equal the verified repository default primary or canary")
    manual_canary = validate_invocation(branch)
    # Fetch only the explicitly confirmed remote branch; ignore local HEAD/tags.
    run("git", "fetch", "--no-tags", "origin", f"refs/heads/{branch}:refs/remotes/origin/daily-source")
    commit = run("git", "rev-parse", "refs/remotes/origin/daily-source^{commit}")
    if manual_canary and commit != os.environ["GITHUB_SHA"]:
        raise ValueError("Canary advanced after dispatch; rerun from its intended exact workflow/source commit")
    run("git", "fetch", "--no-recurse-submodules", "origin", "refs/tags/*:refs/tags/*")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    # Numeric stable and prerelease releases share one immutable version stream.
    base_version, base_version_tag, base_version_source = published_release_baseline(repository, releases)
    latest_tag, _latest_release = latest_published_release(releases)
    base_source = run("git", "rev-parse", f"refs/tags/{latest_tag}^{{commit}}")
    if not re.fullmatch(r"[0-9a-f]{40}", base_source):
        raise ValueError("Latest published numeric release has no exact source commit")
    if base_source == commit:
        emit_outputs({"commit": commit, "should_build": "false", "skip_reason": f"Source already published as {latest_tag}"})
        return
    # Each repository builds its own changed source, independently of upstream migration.
    # GITHUB_TOKEN has no workflows:write capability. Do not select a target
    # with a different workflow tree and then discover that during publication.
    run("git", "diff", "--exit-code", os.environ["GITHUB_SHA"], commit, "--", ".github/workflows")
    # The exact source of every newer published release must be reachable from
    # this source. This retains PR #711's ancestry gate and prevents counting a
    # branch that cannot be compared with the last published source.
    _stable_version, stable_tag = published_stable_anchor(releases)
    stable_source = run("git", "rev-parse", f"refs/tags/{stable_tag}^{{commit}}")
    for anchor_name, anchor_commit in (("highest-version published release", base_version_source),
                                       ("latest published release", base_source),
                                       ("latest stable release", stable_source)):
        subprocess.run(["git", "merge-base", "--is-ancestor", anchor_commit, commit], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    previous = previous_successful_nightly(repository, releases)
    if previous:
        subprocess.run(["git", "merge-base", "--is-ancestor", previous["source_commit"], commit], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    duplicate = published_release_source(commit, releases)
    if duplicate:
        emit_outputs({"commit": commit, "should_build": "false", "skip_reason": f"Source already published as {duplicate}"})
        return
    messages = numeric_release.commit_messages_since_sources(commit, published_source_commits(repository, releases))
    try:
        version = numeric_release.allocate_version(base_version, messages)
    except ValueError as error:
        if str(error).startswith("No eligible conventional feat/fix"):
            emit_outputs({"commit": commit, "should_build": "false", "skip_reason": str(error)})
            return
        raise
    known_tags = [existing for existing in run("git", "tag", "--list").splitlines() if existing != version]
    known_tags.extend(release.get("tag_name", "") for page in releases for release in page
                       if release.get("tag_name") != version)
    numeric_release.require_newer_than(version, known_tags)
    info = reserve_numeric_identity(commit, version, releases, "nightly")
    info["stable_anchor_tag"] = stable_tag
    info["stable_anchor_commit"] = stable_source
    info["base_release_tag"] = latest_tag
    info["base_release_commit"] = base_source
    info["should_build"] = "true"
    emit_outputs(info)


def latest_published_release(releases):
    """Return the exact source tag for the most recently published release."""
    candidates = []
    for page in releases:
        for release in page:
            if release.get("draft") is not False or not release.get("published_at"):
                continue
            tag = release.get("tag_name", "")
            if subprocess.run(["git", "check-ref-format", f"refs/tags/{tag}"], check=False,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
                raise ValueError("Published release has an invalid source tag")
            candidates.append((release["published_at"], int(release.get("id", 0)), tag))
    if not candidates:
        raise ValueError("No published release is available to anchor source history")
    latest_published = max(candidate[0] for candidate in candidates)
    newest = [candidate for candidate in candidates if candidate[0] == latest_published]
    if len(newest) != 1:
        raise ValueError("Multiple published releases share the latest publication timestamp")
    _published, _id, tag = newest[0]
    return tag, next(release for page in releases for release in page if release.get("tag_name") == tag)


def published_release_baseline(repository, releases):
    """Pair the greatest published version with its exact source commit."""
    candidates = []
    for page in releases:
        for release in page:
            if release.get("draft") is not False or not release.get("published_at"):
                continue
            tag = release.get("tag_name", "")
            version = tag
            key = numeric_release.version_key(version)
            metadata = None
            if (release.get("prerelease") is True
                    and nightly_notes.vibepollo_nightly_tag(tag)):
                assets = [asset for asset in release.get("assets", [])
                          if asset.get("name") == "nightly-metadata.json"]
                numeric_tag = numeric_release.parse_version(tag) is not None
                if len(assets) > 1:
                    raise ValueError("Published nightly metadata assets are ambiguous")
                if numeric_tag and (len(assets) != 1 or not isinstance(assets[0].get("id"), int)):
                    raise ValueError("Published numeric nightly metadata asset is missing")
                if assets:
                    if not isinstance(assets[0].get("id"), int):
                        raise ValueError("Published nightly metadata asset ID is invalid")
                    metadata = json.loads(run("gh", "api", "-H", "Accept: application/octet-stream",
                                              f"repos/{repository}/releases/assets/{assets[0]['id']}"))
                    if (metadata.get("schema_version") != 1 or metadata.get("channel") != "nightly"
                            or metadata.get("tag") != tag):
                        raise ValueError("Published legacy prerelease metadata identity mismatch")
                    metadata_version = metadata.get("version", "")
                    metadata_key = numeric_release.version_key(metadata_version)
                    if metadata_key is None or (key is not None and key != metadata_key):
                        raise ValueError("Published prerelease metadata version differs from its tag")
                    version = metadata_version
                    key = metadata_key
            if key is None:
                continue
            if subprocess.run(["git", "check-ref-format", f"refs/tags/{tag}"], check=False,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
                raise ValueError("Published release has an invalid source tag")
            source = run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}")
            if metadata:
                if metadata.get("source_commit") != source:
                    raise ValueError("Published legacy prerelease tag differs from exact metadata source")
                expected_sequence = nightly_notes.sequence(repository, tag, metadata.get("version"), source)
                if expected_sequence is None:
                    if "nightly_sequence" in metadata:
                        raise ValueError("Numeric Vibepollo nightly must not carry a dated sequence")
                elif metadata.get("nightly_sequence") != expected_sequence:
                    raise ValueError("Published legacy prerelease nightly sequence mismatch")
                version = metadata["version"]
                key = numeric_release.version_key(version)
                if key is None:
                    raise ValueError("Published legacy prerelease has no numeric version core")
            if key is None:
                continue
            candidates.append((key, release["published_at"], int(release.get("id", 0)),
                               tuple(map(int, key[:3])), tag, source))
    if not candidates:
        raise ValueError("No published versioned release is available to anchor allocation")
    key, _published, _id, core, tag, source = max(candidates)
    if sum(candidate[0] == key for candidate in candidates) != 1:
        raise ValueError("Multiple published releases share the same version ordering")
    return core, tag, source


def published_release_source(commit, releases):
    """Find a previously published release at this exact immutable source."""
    for page in releases:
        for release in page:
            if release.get("draft") is not False or not release.get("published_at"):
                continue
            tag = release.get("tag_name", "")
            if subprocess.run(["git", "check-ref-format", f"refs/tags/{tag}"], check=False,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
                raise ValueError("Published release has an invalid source tag")
            source = run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}")
            if source == commit:
                return tag
    return None


def published_source_commits(repository, releases):
    """Resolve every published stable/qualified prerelease tag to its exact source."""
    sources = []
    for page in releases:
        for release in page:
            if release.get("draft") is not False or not release.get("published_at"):
                continue
            tag = release.get("tag_name", "")
            if subprocess.run(["git", "check-ref-format", f"refs/tags/{tag}"], check=False,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode != 0:
                raise ValueError("Published release has an invalid source tag")
            source = run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}")
            if not re.fullmatch(r"[0-9a-f]{40}", source):
                raise ValueError("Published release tag has no exact source commit")
            if release.get("prerelease") is True:
                metadata_assets = [asset for asset in release.get("assets", [])
                                   if asset.get("name") == "nightly-metadata.json"]
                if metadata_assets:
                    if len(metadata_assets) != 1 or not isinstance(metadata_assets[0].get("id"), int):
                        raise ValueError("Published prerelease metadata asset is ambiguous")
                    metadata = json.loads(run("gh", "api", "-H", "Accept: application/octet-stream",
                                              f"repos/{repository}/releases/assets/{metadata_assets[0]['id']}"))
                    if (metadata.get("schema_version") != 1 or metadata.get("channel") != "nightly"
                            or metadata.get("tag") != tag or metadata.get("source_commit") != source):
                        raise ValueError("Published prerelease metadata does not match its immutable tag")
                    expected_sequence = nightly_notes.sequence(repository, tag, metadata.get("version"), source)
                    if expected_sequence is None:
                        if "nightly_sequence" in metadata:
                            raise ValueError("Numeric Vibepollo nightly must not carry a dated sequence")
                    elif metadata.get("nightly_sequence") != expected_sequence:
                        raise ValueError("Published nightly metadata sequence mismatch")
            sources.append(source)
    return list(dict.fromkeys(sources))


def reserve_numeric_identity(commit, version, releases, channel):
    if (not re.fullmatch(r"[0-9a-f]{40}", commit) or numeric_release.parse_version(version) is None
            or channel not in {"stable", "nightly"}):
        raise ValueError("Invalid numeric release identity")
    matching_releases = [release for page in releases for release in page
                         if release.get("tag_name") in {version, f"v{version}"}]
    if matching_releases:
        expected_prerelease = channel == "nightly"
        if (len(matching_releases) != 1 or matching_releases[0].get("tag_name") != version
                or matching_releases[0].get("draft") is not True
                or matching_releases[0].get("prerelease") is not expected_prerelease):
            raise ValueError("Numeric release version is already published or reserved for another identity")
    lines = run("git", "ls-remote", "--tags", "origin", f"refs/tags/{version}", f"refs/tags/v{version}").splitlines()
    refs = {}
    for line in lines:
        target, ref = line.split()
        if ref not in {f"refs/tags/{version}", f"refs/tags/{version}^{{}}", f"refs/tags/v{version}", f"refs/tags/v{version}^{{}}"} or not re.fullmatch(r"[0-9a-f]{40}", target):
            raise ValueError("Invalid reserved numeric release tag")
        refs[ref] = target
    if f"refs/tags/v{version}" in refs:
        raise ValueError("A v-prefixed tag already reserves this numeric release version")
    if matching_releases and not refs:
        raise ValueError("Numeric release draft has no immutable annotated tag reservation")
    if refs:
        tag_object = refs.get(f"refs/tags/{version}")
        peeled = refs.get(f"refs/tags/{version}^{{}}")
        if not tag_object or peeled != commit:
            raise ValueError("Numeric release tag is already reserved at another source commit")
        run("git", "fetch", "--force", "--no-tags", "origin", f"refs/tags/{version}:refs/tags/{version}")
        numeric_release.verify_annotated_reservation(version, commit, channel)
    return {"commit": commit, "tag": version, "version": version, "channel": channel}


def verify_remote_numeric_reservation(repository, tag, commit, channel):
    """Re-read and validate the live GitHub annotated tag before publication."""
    ref = json.loads(run("gh", "api", f"repos/{repository}/git/ref/tags/{tag}"))
    if ref.get("ref") != f"refs/tags/{tag}" or ref.get("object", {}).get("type") != "tag":
        raise ValueError("Remote release identity is not the exact annotated numeric tag")
    object_sha = ref["object"].get("sha", "")
    if not re.fullmatch(r"[0-9a-f]{40}", object_sha):
        raise ValueError("Remote annotated release tag object SHA is invalid")
    tag_object = json.loads(run("gh", "api", f"repos/{repository}/git/tags/{object_sha}"))
    expected_message = f"Vibepollo release channel: {channel}\nsource: {commit}"
    if (tag_object.get("tag") != tag or tag_object.get("object", {}).get("type") != "commit"
            or tag_object.get("object", {}).get("sha") != commit
            or tag_object.get("message") != expected_message):
        raise ValueError("Remote annotated release tag changed version, channel or exact source")


def host_nightly_order(tag):
    match = re.fullmatch(r"(?:[0-9]+\.[0-9]+\.[0-9]+-nightly\.d([0-9]{8})\.(?:r\.([1-9][0-9]{0,2})\.)?[0-9a-f]{12}|"
                         r"(?:[0-9]+\.[0-9]+\.[0-9]+-daily\.d([0-9]{8})|daily-([0-9]{4})-([0-9]{2})-([0-9]{2}))(?:\.([1-9][0-9]{0,2}))?)", tag)
    if not match:
        return None
    date = match[1] or match[3] or ''.join(match.groups()[3:6])
    dt.datetime.strptime(date, "%Y%m%d")  # Never manufacture calendar ordinals.
    return (int(date), int(bool(match[1])), int(match[2] or match[7] or 0))


def published_source(commit, day, releases, tag=None):
    # Published immutable tag targets, including older accepted sources, prevent
    # repeat builds. Qualification of the latest source is checked separately.
    for page in releases:
        for release in page:
            if (release.get("draft") is not False or release.get("prerelease") is not True
                    or not release.get("published_at")):
                continue
            published_tag = release.get("tag_name", "")
            order = host_nightly_order(published_tag)
            if order is None:
                continue
            source = run("git", "rev-parse", f"refs/tags/{published_tag}^{{commit}}")
            if source == commit:
                return published_tag
            if order[0] > int(f"{day:%Y%m%d}"):
                raise ValueError("Published nightly date is newer than the current UTC date")
            if tag is not None:
                def core(value):
                    match = re.match(r"([0-9]+)\.([0-9]+)\.([0-9]+)-", value)
                    return tuple(map(int, match.groups())) if match else None
                old_core, new_core = core(published_tag), core(tag)
                if old_core is not None and new_core is not None and old_core != new_core:
                    if old_core > new_core:
                        raise ValueError("A newer numeric nightly base is already published")
                elif order >= host_nightly_order(tag):
                    raise ValueError("A newer or equal nightly identity is already published")
    return None


def guard_publication(repository, commit, tag, expected_stable_tag=None, expected_stable_commit=None,
                      expected_draft_release_id=None, expected_notes_path=None):
    # Recheck live publication state even on a partial rerun of an old build.
    # Do not retag or relabel artifacts when source or release state advances.
    version = numeric_release.parse_version(tag)
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or version is None or tag.startswith("v"):
        raise ValueError("Invalid exact publication identity")
    if run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}") != commit:
        raise ValueError("Reserved release tag no longer matches this build")
    numeric_release.verify_annotated_reservation(tag, commit, "nightly")
    verify_remote_numeric_reservation(repository, tag, commit, "nightly")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    base_version, base_version_tag, base_version_source = published_release_baseline(repository, releases)
    base_tag, _latest_release = latest_published_release(releases)
    base_source = run("git", "rev-parse", f"refs/tags/{base_tag}^{{commit}}")
    subprocess.run(["git", "merge-base", "--is-ancestor", base_source, commit], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    subprocess.run(["git", "merge-base", "--is-ancestor", base_version_source, commit], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    expected = numeric_release.allocate_version(
        base_version, numeric_release.commit_messages_since_sources(commit, published_source_commits(repository, releases)))
    if tag != expected:
        raise ValueError("Published release baseline changed during the build; rerun with a current numeric version")
    matching_release = [r for page in releases for r in page if r.get("tag_name") == tag]
    if expected_draft_release_id is None:
        if matching_release:
            raise ValueError("Release already exists; recovery must preserve its original assets")
    elif (len(matching_release) != 1 or str(matching_release[0].get("id")) != str(expected_draft_release_id)
          or matching_release[0].get("draft") is not True
          or matching_release[0].get("prerelease") is not True):
        raise ValueError("Nightly draft release identity changed before publication")
    elif expected_notes_path:
        live_release = json.loads(run("gh", "api", f"repos/{repository}/releases/{expected_draft_release_id}"))
        if Path(expected_notes_path).read_text(encoding="utf-8").rstrip("\n") != live_release.get("body", "").rstrip("\n"):
            raise ValueError("Nightly draft notes no longer match the exact source/channel candidate")
    if published_release_source(commit, releases):
        raise ValueError("Source already published; refusing duplicate release")
    _stable_version, stable_tag = published_stable_anchor(releases)
    stable_source = run("git", "rev-parse", f"refs/tags/{stable_tag}^{{commit}}")
    if ((expected_stable_tag is not None and stable_tag != expected_stable_tag)
            or (expected_stable_commit is not None and stable_source != expected_stable_commit)):
        raise ValueError("Stable anchor changed during the nightly build; rerun with a current source")
    subprocess.run(["git", "merge-base", "--is-ancestor", stable_source, commit], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    previous = previous_successful_nightly(repository, releases)
    if previous:
        subprocess.run(["git", "merge-base", "--is-ancestor", previous["source_commit"], commit], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    known_tags = [existing for existing in run("git", "tag", "--list").splitlines() if existing != tag]
    known_tags.extend(release.get("tag_name", "") for page in releases for release in page
                       if release.get("tag_name") != tag)
    numeric_release.require_newer_than(tag, known_tags)
    emit_outputs({"stable_anchor_tag": stable_tag, "stable_anchor_commit": stable_source})


def validate_stable_candidate(repository, tag, commit, expected_draft_release_id=None,
                              allow_existing_draft=True):
    """Validate a stable CI tag against the same shared allocator as nightly."""
    if (numeric_release.parse_version(tag) is None or tag.startswith("v")
            or not re.fullmatch(r"[0-9a-f]{40}", commit)):
        raise ValueError("New stable releases require a v-less numeric tag and exact source commit")
    if run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}") != commit:
        raise ValueError("Stable tag must point to the exact source commit")
    numeric_release.verify_annotated_reservation(tag, commit, "stable")
    verify_remote_numeric_reservation(repository, tag, commit, "stable")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    matching = [release for page in releases for release in page
                if release.get("tag_name") in {tag, f"v{tag}"}]
    if matching:
        # Existing releases are rejected for ordinary signing. The only
        # exception is a newly created draft that this exact run will expose.
        if (not allow_existing_draft or len(matching) != 1
                or matching[0].get("tag_name") != tag
                or (expected_draft_release_id is not None
                    and str(matching[0].get("id")) != str(expected_draft_release_id))
                or matching[0].get("draft") is not True
                or matching[0].get("prerelease") is not False):
            raise ValueError("Stable release version is already published or reserved")
    if published_release_source(commit, releases):
        raise ValueError("Source already published; refusing another stable release")
    base_version, base_version_tag, base_version_source = published_release_baseline(repository, releases)
    previous_tag, _latest_release = latest_published_release(releases)
    previous_source = run("git", "rev-parse", f"refs/tags/{previous_tag}^{{commit}}")
    stable_version, stable_tag = published_stable_anchor(releases)
    stable_source = run("git", "rev-parse", f"refs/tags/{stable_tag}^{{commit}}")
    previous_nightly = previous_successful_nightly(repository, releases)
    anchors = [("previous published release", previous_source),
               ("highest-version release", base_version_source),
               ("previous stable release", stable_source)]
    if previous_nightly:
        anchors.append(("previous qualified prerelease", previous_nightly["source_commit"]))
    for label, anchor in anchors:
        result = subprocess.run(["git", "merge-base", "--is-ancestor", anchor, commit], check=False,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode != 0:
            raise ValueError(f"Stable source does not descend from the {label}; reconcile history before release")
    expected = numeric_release.allocate_version(
        base_version, numeric_release.commit_messages_since_sources(commit, published_source_commits(repository, releases)))
    if tag != expected:
        raise ValueError(f"Stable tag {tag} does not match the shared allocated version {expected}")
    known_tags = [existing for existing in run("git", "tag", "--list").splitlines() if existing != tag]
    known_tags.extend(release.get("tag_name", "") for page in releases for release in page
                       if release.get("tag_name") != tag)
    numeric_release.require_newer_than(tag, known_tags)
    emit_outputs(stable_anchor_tag=stable_tag, stable_anchor_commit=stable_source)


def validate_stable_publication(repository, tag, commit, draft_release_id,
                                expected_stable_tag, expected_stable_commit, expected_notes_path,
                                expected_title):
    """Revalidate one exact uploaded draft immediately before exposing it."""
    if not draft_release_id.isdigit():
        raise ValueError("Stable publication requires an exact draft release ID")
    validate_stable_candidate(repository, tag, commit, expected_draft_release_id=int(draft_release_id))
    live_release = json.loads(run("gh", "api", f"repos/{repository}/releases/{draft_release_id}"))
    if (live_release.get("tag_name") != tag or live_release.get("name") != expected_title
            or live_release.get("draft") is not True
            or live_release.get("prerelease") is not False
            or Path(expected_notes_path).read_text(encoding="utf-8").rstrip("\n") != live_release.get("body", "").rstrip("\n")):
        raise ValueError("Stable draft notes or identity changed before publication")
    if (os.environ.get("STABLE_ANCHOR_TAG") != expected_stable_tag
            or os.environ.get("STABLE_ANCHOR_COMMIT") != expected_stable_commit):
        raise ValueError("Stable publication anchor input is incomplete")
    live_anchor = published_stable_anchor(json.loads(run(
        "gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100")))
    live_commit = run("git", "rev-parse", f"refs/tags/{live_anchor[1]}^{{commit}}")
    if live_anchor[1] != expected_stable_tag or live_commit != expected_stable_commit:
        raise ValueError("Stable anchor changed while signed assets were being prepared")


def validate_release_draft(repository, release_id, tag, title, prerelease, notes_path,
                           artifact_dir, require_complete):
    """Allow recovery only for the exact draft and immutable assets we built."""
    if not release_id.isdigit() or not isinstance(prerelease, bool):
        raise ValueError("Release draft recovery requires an exact release ID and channel")
    release = json.loads(run("gh", "api", f"repos/{repository}/releases/{release_id}"))
    expected_body = Path(notes_path).read_text(encoding="utf-8").rstrip("\n")
    if (str(release.get("id")) != release_id or release.get("tag_name") != tag
            or release.get("name") != title or release.get("draft") is not True
            or release.get("prerelease") is not prerelease
            or release.get("body", "").rstrip("\n") != expected_body):
        raise ValueError("Existing release draft tag, title, channel or notes do not match this build")
    assets = json.loads(run("gh", "api", f"repos/{repository}/releases/{release_id}/assets"))
    expected = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                for path in Path(artifact_dir).iterdir()
                if path.is_file() and path.name != "release-provenance.json"}
    names = [asset.get("name") for asset in assets]
    if len(names) != len(set(names)) or any(name not in expected for name in names):
        raise ValueError("Existing draft has duplicate or unexpected release assets")
    for asset in assets:
        if asset.get("digest") != f"sha256:{expected[asset['name']]}" or not asset.get("digest"):
            raise ValueError(f"Existing draft asset differs from this exact build: {asset['name']}")
    if require_complete and set(names) != set(expected):
        raise ValueError("Release draft is missing one or more exact candidate assets")


def resolve_stable_anchor():
    """Resolve the exact current stable source for validation-only nightlies."""
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    commit = os.environ.get("GITHUB_SHA", "")
    if repository != "Nonary/Vibepollo" or not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Stable anchor lookup requires an exact Vibepollo source SHA")
    run("git", "fetch", "--no-recurse-submodules", "origin", "refs/tags/*:refs/tags/*")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    _version, tag = published_stable_anchor(releases)
    source = run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}")
    subprocess.run(["git", "merge-base", "--is-ancestor", source, commit], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    version = os.environ.get("RELEASE_VERSION", "")
    numeric = re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is not None
    emit_outputs({"stable_anchor_tag": tag, "stable_anchor_commit": source,
                  "daily_validation_only": str(numeric).lower()})


def generate_stable_notes(repository, tag, commit, output_path):
    """Generate notes for an allocated numeric release from its exact source delta."""
    validate_stable_candidate(repository, tag, commit)
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    previous_tag, _release = latest_published_release(releases)
    previous_source = run("git", "rev-parse", f"refs/tags/{previous_tag}^{{commit}}")
    messages = numeric_release.commit_messages_since_sources(commit, published_source_commits(repository, releases))
    counted = []
    for message in messages:
        subject = message.splitlines()[0].strip() if message.splitlines() else ""
        match = numeric_release.CONVENTIONAL_RE.fullmatch(subject)
        if match:
            counted.append((match.group(1), subject))
    features = [subject for kind, subject in counted if kind == "feat"]
    fixes = [subject for kind, subject in counted if kind == "fix"]
    notes = [f"# Vibepollo {tag}", "", f"Changes since the previous published source `{previous_tag}`.", ""]
    if features:
        notes.extend(["## Features", "", *[f"- {item}" for item in features], ""])
    if fixes:
        notes.extend(["## Fixes", "", *[f"- {item}" for item in fixes], ""])
    Path(output_path).write_text("\n".join(notes).rstrip() + "\n", encoding="utf-8")


def allocate_stable():
    """Resolve a stable release directly from the exact default-branch source."""
    repository = os.environ.get("GITHUB_REPOSITORY", "")
    commit = os.environ.get("GITHUB_SHA", "")
    if (repository != "Nonary/Vibepollo" or os.environ.get("GITHUB_EVENT_NAME") != "workflow_dispatch"
            or os.environ.get("GITHUB_REF_TYPE") != "branch" or os.environ.get("GITHUB_REF_NAME") != "master"
            or not re.fullmatch(r"[0-9a-f]{40}", commit)):
        raise ValueError("Stable allocation is limited to a manual dispatch from exact Vibepollo master")
    run("git", "fetch", "--no-tags", "origin", "refs/heads/master:refs/remotes/origin/stable-source")
    if run("git", "rev-parse", "refs/remotes/origin/stable-source^{commit}") != commit:
        raise ValueError("Master advanced after stable allocation dispatch; rerun from its current exact commit")
    run("git", "fetch", "--no-recurse-submodules", "origin", "refs/tags/*:refs/tags/*")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    base_version, base_version_tag, base_version_source = published_release_baseline(repository, releases)
    base_tag, _latest_release = latest_published_release(releases)
    base_source = run("git", "rev-parse", f"refs/tags/{base_tag}^{{commit}}")
    if published_release_source(commit, releases):
        emit_outputs({"should_release": "false", "skip_reason": "Source already has a published release"})
        return
    _stable_version, stable_tag = published_stable_anchor(releases)
    stable_source = run("git", "rev-parse", f"refs/tags/{stable_tag}^{{commit}}")
    previous_nightly = previous_successful_nightly(repository, releases)
    anchors = [("previous published release", base_source),
               ("highest-version release", base_version_source),
               ("previous stable release", stable_source)]
    if previous_nightly:
        anchors.append(("previous qualified prerelease", previous_nightly["source_commit"]))
    for label, anchor in anchors:
        result = subprocess.run(["git", "merge-base", "--is-ancestor", anchor, commit], check=False,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if result.returncode != 0:
            raise ValueError(f"Stable source does not descend from the {label}; reconcile history before release")
    try:
        version = numeric_release.allocate_version(
            base_version, numeric_release.commit_messages_since_sources(commit, published_source_commits(repository, releases)))
    except ValueError as error:
        if str(error).startswith("No eligible conventional feat/fix"):
            emit_outputs({"should_release": "false", "skip_reason": str(error)})
            return
        raise
    info = reserve_numeric_identity(commit, version, releases, "stable")
    known_tags = [existing for existing in run("git", "tag", "--list").splitlines() if existing != version]
    known_tags.extend(release.get("tag_name", "") for page in releases for release in page
                       if release.get("tag_name") != version)
    numeric_release.require_newer_than(version, known_tags)
    info.update(should_release="true", stable_anchor_tag=stable_tag,
                stable_anchor_commit=stable_source, base_release_tag=base_tag,
                base_release_commit=base_source)
    emit_outputs(info)


def reserve_identity(commit, day, base, releases):
    prefix = f"{base}-nightly.d{day:%Y%m%d}."
    reserved = run("git", "ls-remote", "--tags", "origin", f"refs/tags/{prefix}*")
    occupied = {}
    for line in reserved.splitlines():
        target, ref = line.split()
        if ref.endswith("^{}"):
            raise ValueError("Nightly reservations must be lightweight exact-commit tags")
        tag = ref.removeprefix("refs/tags/")
        suffix = tag.removeprefix(prefix)
        match = re.fullmatch(r"(?:r\.([1-9][0-9]{0,2})\.)?([0-9a-f]{12})", suffix)
        if not tag.startswith(prefix) or not match:
            raise ValueError("Invalid reserved nightly identity")
        revision = int(match[1] or 0)
        if revision and revision in occupied:
            raise ValueError("Nightly revision is reserved by multiple source identities")
        if match[2] == commit[:12] and target != commit:
            raise ValueError("Immutable nightly tag is reserved at another source commit")
        if not re.fullmatch(r"[0-9a-f]{40}", target) or match[2] != target[:12]:
            raise ValueError("Reserved nightly suffix does not match its exact source commit")
        # Old failed builds can leave several hash-only reservations on one
        # date. Those are identities without an allocated numeric revision.
        if revision:
            occupied[revision] = (tag, target)
    latest = max(occupied, default=0)
    # A failed build can reuse its exact latest lightweight reservation. Drafts
    # retain their assets and require explicit recovery, never create/upload over.
    if latest and occupied[latest][1] == commit:
        info = identity(commit, day, base, latest)
    else:
        info = identity(commit, day, base, latest + 1)
    if any(release.get("tag_name") == info["tag"] for page in releases for release in page):
        raise ValueError("Nightly release already exists; inspect or recover it without replacement")
    return info


def verify_assets(root, product, commit, tag, version):
    root = Path(root)
    expected = {f"{product}Setup.exe", "release-provenance.json"}
    actual = {p.name for p in root.iterdir()}
    if actual != expected or any(not p.is_file() or p.is_symlink() or p.stat().st_size == 0 for p in root.iterdir()):
        raise ValueError(f"Expected exactly the finalized nonempty assets {sorted(expected)}; got {sorted(actual)}")
    # Keep artifact-integrity recovery available for already published dated
    # nightlies. New allocations and publication gates are numeric-only, but a
    # retained build must still be verifiable after the identity transition.
    legacy_daily = tag == version and host_nightly_order(tag) is not None
    numeric_daily = numeric_release.parse_version(tag) is not None and not tag.startswith("v") and tag == version
    if not (numeric_daily or legacy_daily):
        raise ValueError("Release tag/version/source identity is incoherent")
    provenance = json.loads((root / "release-provenance.json").read_text(encoding="utf-8-sig"))
    if provenance.get("schema_version") != 1 or provenance.get("release_commit") != commit or provenance.get("release_tag") != tag:
        raise ValueError("Finalized artifact provenance does not match this source/tag")
    if str(provenance.get("source_build_run_id")) != os.environ["GITHUB_RUN_ID"]:
        raise ValueError("Artifacts must come from this successful daily build")
    if set(provenance.get("assets", {})) != {f"{product}Setup.exe"}:
        raise ValueError("Unexpected finalized artifact provenance assets")
    embedded = provenance.get("embedded_msi", {})
    if (embedded.get("verified") is not True or
            not re.fullmatch(r"[0-9a-f]{64}", embedded.get("sha256", ""))):
        raise ValueError("Finalized setup must contain the exact qualified internal MSI")
    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in root.iterdir()}
    if hashes[f"{product}Setup.exe"] != provenance["assets"][f"{product}Setup.exe"]:
        raise ValueError("Finalized installer checksum mismatch")
    manifest = {
        "schema_version": 1, "channel": "nightly", "tag": tag, "version": version,
        "source_commit": commit, "workflow_run_id": os.environ["GITHUB_RUN_ID"], "prerelease": True,
        "signpath": False, "assets": hashes,
    }
    (root / "daily-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    hashes["daily-manifest.json"] = hashlib.sha256((root / "daily-manifest.json").read_bytes()).hexdigest()
    (root / "SHA256SUMS").write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(hashes.items())), encoding="utf-8")


def include_linux(root, linux_root, product, commit, version):
    root, linux_root = Path(root), Path(linux_root)
    packages = list(linux_root.glob(product.lower() + '-*.pkg.tar.zst'))
    if len(packages) != 1 or any(not p.is_file() or p.is_symlink() or p.stat().st_size == 0 for p in packages):
        raise ValueError('Expected one nonempty installable Linux package')
    provenance = json.loads((linux_root / 'linux-provenance.json').read_text(encoding='utf-8'))
    if (provenance.get('source_commit') != commit or provenance.get('version') != version
            or str(provenance.get('workflow_run_id')) != os.environ['GITHUB_RUN_ID']):
        raise ValueError('Linux provenance must match the qualified daily source/version/run')
    package = packages[0]
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    if provenance.get('assets') != {package.name: digest}:
        raise ValueError('Linux package checksum mismatch')
    shutil.copyfile(package, root / package.name)
    # Consolidate Linux evidence into the existing manifest instead of exposing
    # an extra internal build recipe or another provenance download.
    manifest_path = root / 'daily-manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    manifest['assets'][package.name] = digest
    manifest['linux'] = provenance
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')


def stage_public_assets(root, destination, product):
    root, destination = Path(root), Path(destination)
    packages = list(root.glob(product.lower() + '-*.pkg.tar.zst'))
    if len(packages) != 1:
        raise ValueError('Expected exactly one qualified Linux package')
    public = {product + 'Setup.exe', packages[0].name, 'nightly-metadata.json', 'SHA256SUMS'}
    evidence = {'daily-manifest.json', 'release-provenance.json'}
    if {p.name for p in root.iterdir()} != public | evidence:
        raise ValueError('Unexpected public or qualification assets')
    if any(not p.is_file() or p.is_symlink() or p.stat().st_size == 0 for p in root.iterdir()):
        raise ValueError('Unsafe or incomplete public asset input')
    # Qualification stays in the source directory for the CI artifact.
    destination.mkdir()
    for name in sorted(public):
        shutil.copyfile(root / name, destination / name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["resolve", "verify", "linux", "public", "publication", "publication-draft", "validate-release-draft", "validate-stable", "validate-stable-publication", "allocate-stable", "stable-anchor", "stable-notes"])
    parser.add_argument("--assets")
    parser.add_argument("--linux-assets")
    parser.add_argument("--destination")
    parser.add_argument("--product", choices=["Vibeshine", "Vibepollo"])
    parser.add_argument("--output")
    args = parser.parse_args()
    if args.command == "resolve":
        resolve()
    elif args.command == "publication":
        guard_publication(os.environ["GITHUB_REPOSITORY"], os.environ["DAILY_COMMIT"], os.environ["DAILY_TAG"],
                          os.environ.get("STABLE_ANCHOR_TAG"), os.environ.get("STABLE_ANCHOR_COMMIT"))
    elif args.command == "publication-draft":
        guard_publication(os.environ["GITHUB_REPOSITORY"], os.environ["DAILY_COMMIT"], os.environ["DAILY_TAG"],
                          os.environ.get("STABLE_ANCHOR_TAG"), os.environ.get("STABLE_ANCHOR_COMMIT"),
                          os.environ.get("RELEASE_ID"), os.environ.get("EXPECTED_RELEASE_NOTES"))
    elif args.command == "validate-stable":
        validate_stable_candidate(os.environ["GITHUB_REPOSITORY"], os.environ["RELEASE_TAG"],
                                  os.environ["RELEASE_COMMIT"])
    elif args.command == "validate-stable-publication":
        validate_stable_publication(os.environ["GITHUB_REPOSITORY"], os.environ["RELEASE_TAG"],
                                    os.environ["RELEASE_COMMIT"], os.environ["RELEASE_ID"],
                                    os.environ["STABLE_ANCHOR_TAG"], os.environ["STABLE_ANCHOR_COMMIT"],
                                    os.environ["NOTES_FILE"], os.environ["RELEASE_TITLE"])
    elif args.command == "validate-release-draft":
        validate_release_draft(os.environ["GITHUB_REPOSITORY"], os.environ["RELEASE_ID"],
                               os.environ["RELEASE_TAG"], os.environ["RELEASE_TITLE"],
                               os.environ["RELEASE_PRERELEASE"] == "true", os.environ["NOTES_FILE"],
                               args.assets, os.environ.get("REQUIRE_COMPLETE_ASSETS") == "true")
    elif args.command == "allocate-stable":
        allocate_stable()
    elif args.command == "stable-anchor":
        resolve_stable_anchor()
    elif args.command == "stable-notes":
        generate_stable_notes(os.environ["GITHUB_REPOSITORY"], os.environ["RELEASE_TAG"],
                              os.environ["RELEASE_COMMIT"], args.output)
    elif args.command == 'verify':
        verify_assets(args.assets, args.product, os.environ["DAILY_COMMIT"], os.environ["DAILY_TAG"], os.environ["DAILY_VERSION"])
    elif args.command == 'public':
        stage_public_assets(args.assets, args.destination, args.product)
    else:
        include_linux(args.assets, args.linux_assets, args.product, os.environ['DAILY_COMMIT'], os.environ['DAILY_VERSION'])
