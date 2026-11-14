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

_numeric_spec = importlib.util.spec_from_file_location(
    "numeric_release", Path(__file__).with_name("numeric_release.py"))
numeric_release = importlib.util.module_from_spec(_numeric_spec)
_numeric_spec.loader.exec_module(numeric_release)


def identity(commit, day, base):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected an exact source commit SHA")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", base):
        raise ValueError("Expected a numeric source version base")
    major, minor, patch = map(int, base.split("."))
    if major > 255 or minor > 255 or patch > 654 or base == "0.0.0":
        raise ValueError("Source version base exceeds MSI limits or is zero")
    if not 2000 <= day.year <= 2099:
        raise ValueError("Daily version year must fit the installer version scheme")
    tag = f"{base}-nightly.d{day:%Y%m%d}.{commit[:12]}"
    return {"commit": commit, "tag": tag, "version": tag}


def validate_source(repository, branch, confirmation):
    if repository not in {"Nonary/vibeshine", "Nonary/Vibepollo"}:
        raise ValueError("Daily publishing is limited to the designated upstream repositories")
    allowed = {"master", "main", "canary"}
    if branch not in allowed:
        raise ValueError("Daily source must use a confirmed primary or canary; legacy vibe/test lanes are retired")
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


def stable_anchor(releases):
    """Best-effort published stable identity for build metadata; never blocks a nightly."""
    try:
        _version, tag = published_stable_anchor(releases)
        return tag, run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}")
    except (ValueError, subprocess.CalledProcessError):
        return "", ""


def tag_refs():
    refs = {}
    channels = {}
    for line in run("git", "for-each-ref",
                    "--format=%(refname:strip=2)%09%(objectname)%09%(contents:subject)",
                    "refs/tags").splitlines():
        tag, object_id, subject = line.split("\t", 2)
        refs[tag] = object_id
        match = re.match(r"^Vibepollo release channel: (stable|prerelease|nightly)(?:\s|$)", subject)
        if match:
            channels[tag] = match[1]
    return refs, channels


def validate_live_numeric_version(repository, tag, commit, channel="nightly"):
    """Re-resolve immediately before publication so a newer release cannot race the build.

    The reserved tag must still be the allocation for this source, which holds
    only while it outranks every published version (stable releases included).
    """
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    refs, channels = tag_refs()
    previous = previous_successful_nightly(repository, releases)
    expected = numeric_release.resolve(releases, refs, commit,
                                       os.environ["DAILY_RELEASE_SOURCE_BRANCH"], channel, channels,
                                       previous)
    if expected.get("should_release") != "true" or expected.get("tag") != tag:
        raise ValueError("Published release state changed during the build; rerun with a current numeric version")


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
    if branch != os.environ.get("DAILY_PRIMARY_BRANCH") and not (
        branch == "canary"
    ):
        raise ValueError("Daily source must equal the verified repository default primary or canary")
    manual_canary = validate_invocation(branch)
    # Fetch only the explicitly confirmed remote branch; ignore local HEAD/tags.
    run("git", "fetch", "--no-tags", "origin", f"refs/heads/{branch}:refs/remotes/origin/daily-source")
    commit = run("git", "rev-parse", "refs/remotes/origin/daily-source^{commit}")
    if manual_canary and commit != os.environ["GITHUB_SHA"]:
        raise ValueError("Canary advanced after dispatch; rerun from its intended exact workflow/source commit")
    run("git", "fetch", "--no-recurse-submodules", "origin", "refs/tags/*:refs/tags/*")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    previous = previous_successful_nightly(repository, releases)
    if previous and previous["source_commit"] == commit:
        stable_tag, stable_commit = stable_anchor(releases)
        emit_outputs({"commit": commit, "channel": "nightly", "should_build": "false",
                      "skip_reason": f"Source unchanged since {previous['tag']}",
                      "stable_anchor_tag": stable_tag, "stable_anchor_commit": stable_commit})
        return
    # GITHUB_TOKEN has no workflows:write capability. Do not select a target
    # with a different workflow tree and then discover that during publication.
    run("git", "diff", "--exit-code", os.environ["GITHUB_SHA"], commit, "--", ".github/workflows")
    stable_tag, stable_commit = stable_anchor(releases)
    if previous:
        try:
            run("git", "merge-base", "--is-ancestor", previous["source_commit"], commit)
        except subprocess.CalledProcessError:
            # The notes already describe rewound history; the version is still
            # allocated above every published release.
            numeric_release.warn(f"Previous nightly {previous['tag']} is not an ancestor of {branch}; history was rewound")
    refs, tag_channels = tag_refs()
    info = numeric_release.resolve(releases, refs, commit, branch, "nightly", tag_channels,
                                   previous)
    info.update({"stable_anchor_tag": stable_tag, "stable_anchor_commit": stable_commit})
    if info.get("should_release") != "true":
        info["should_build"] = "false"
        emit_outputs(info)
        return
    info["version"] = info["tag"]
    info["should_build"] = "true"
    emit_outputs(info)


def verify_assets(root, product, commit, tag, version):
    root = Path(root)
    expected = {f"{product}Setup.exe", "release-provenance.json"}
    actual = {p.name for p in root.iterdir()}
    if actual != expected or any(not p.is_file() or p.is_symlink() or p.stat().st_size == 0 for p in root.iterdir()):
        raise ValueError(f"Expected exactly the finalized nonempty assets {sorted(expected)}; got {sorted(actual)}")
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", tag):
        if tag != version:
            raise ValueError("Release tag/version/source identity is incoherent")
    else:
        # Preserve exact historical daily artifacts during recovery. New
        # allocations use numeric tags, while old identities remain readable.
        legacy = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)-nightly\.d([0-9]{8})\.([0-9a-f]{12})", tag)
        if not legacy:
            raise ValueError("Unsupported prerelease tag identity")
        info = identity(commit, dt.datetime.strptime(legacy[2], "%Y%m%d").date(), legacy[1])
        if info["tag"] != tag or info["version"] != version:
            raise ValueError("Legacy release tag/version/source identity is incoherent")
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
        "schema_version": 1, "tag": tag, "version": version, "source_commit": commit,
        "workflow_run_id": os.environ["GITHUB_RUN_ID"], "prerelease": True,
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
    parser.add_argument("command", choices=["resolve", "verify", "linux", "public", "validate-live"])
    parser.add_argument("--assets")
    parser.add_argument("--linux-assets")
    parser.add_argument("--destination")
    parser.add_argument("--product", choices=["Vibeshine", "Vibepollo"])
    args = parser.parse_args()
    if args.command == "resolve":
        resolve()
    elif args.command == 'verify':
        verify_assets(args.assets, args.product, os.environ["DAILY_COMMIT"], os.environ["DAILY_TAG"], os.environ["DAILY_VERSION"])
    elif args.command == 'public':
        validate_live_numeric_version(os.environ["GITHUB_REPOSITORY"], os.environ["DAILY_TAG"],
                                      os.environ["DAILY_COMMIT"], "nightly")
        stage_public_assets(args.assets, args.destination, args.product)
    elif args.command == 'validate-live':
        validate_live_numeric_version(os.environ["GITHUB_REPOSITORY"], os.environ["DAILY_TAG"],
                                      os.environ["DAILY_COMMIT"], "nightly")
    else:
        include_linux(args.assets, args.linux_assets, args.product, os.environ['DAILY_COMMIT'], os.environ['DAILY_VERSION'])
