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
    previous = previous_successful_nightly(repository, releases)
    if previous and previous["source_commit"] == commit:
        # Each repository owns its baseline. Upstream advances cannot turn an
        # unchanged target into a build or an assertion that migration happened.
        emit_outputs({"commit": commit, "should_build": "false", "skip_reason": f"Source unchanged since {previous['tag']}"})
        return
    # Each repository builds its own changed source, independently of upstream migration.
    # GITHUB_TOKEN has no workflows:write capability. Do not select a target
    # with a different workflow tree and then discover that during publication.
    run("git", "diff", "--exit-code", os.environ["GITHUB_SHA"], commit, "--", ".github/workflows")
    tags = run("git", "tag", "--merged", commit, "--sort=-version:refname", "--list", "[0-9]*.[0-9]*.[0-9]*").splitlines()
    stable_tags = [tag for tag in tags if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-stable\.[0-9]+)?", tag)]
    if not stable_tags:
        raise ValueError("Source has no merged stable release anchor")
    stable = max((tuple(map(int, re.match(r"[0-9]+\.[0-9]+\.[0-9]+", tag)[0].split("."))) for tag in stable_tags))
    major, minor, patch = stable
    # A nightly is development AFTER the released patch and BEFORE its successor.
    # Do not change SemVer precedence to place an old nightly above its final stable.
    base = f"{major}.{minor}.{patch + 1}"
    day = dt.datetime.now(dt.timezone.utc).date()
    duplicate = published_source(commit, day, releases)
    if duplicate:
        emit_outputs({"commit": commit, "should_build": "false", "skip_reason": f"Source already published as {duplicate}"})
        return
    if previous:
        # A changed source must advance the qualified source, never rewind or
        # silently publish an unrelated history as an update.
        run("git", "merge-base", "--is-ancestor", previous["source_commit"], commit)
    info = reserve_identity(commit, day, base, releases)
    info["should_build"] = "true"
    emit_outputs(info)


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


def guard_publication(repository, commit, tag):
    # Recheck live publication state even on a partial rerun of an old build.
    # Do not retag or relabel artifacts when source or release state advances.
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or host_nightly_order(tag) is None:
        raise ValueError("Invalid exact publication identity")
    if run("git", "rev-parse", f"refs/tags/{tag}^{{commit}}") != commit:
        raise ValueError("Reserved release tag no longer matches this build")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    if any(r.get("tag_name") == tag for page in releases for r in page):
        raise ValueError("Release already exists; recovery must preserve its original assets")
    if published_source(commit, dt.datetime.now(dt.timezone.utc).date(), releases, tag):
        raise ValueError("Source already published; refusing duplicate release")
    previous = previous_successful_nightly(repository, releases)
    if previous:
        run("git", "merge-base", "--is-ancestor", previous["source_commit"], commit)


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
    match = re.fullmatch(r"([0-9]+\.[0-9]+\.[0-9]+)-nightly\.d([0-9]{8})\.r\.([1-9][0-9]{0,2})\.([0-9a-f]{12})", tag)
    if not match:
        raise ValueError("New releases require a commit-bound nightly tag")
    info = identity(commit, dt.datetime.strptime(match[2], "%Y%m%d").date(), match[1], int(match[3]))
    if info["tag"] != tag or info["version"] != version:
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
    parser.add_argument("command", choices=["resolve", "verify", "linux", "public", "publication"])
    parser.add_argument("--assets")
    parser.add_argument("--linux-assets")
    parser.add_argument("--destination")
    parser.add_argument("--product", choices=["Vibeshine", "Vibepollo"])
    args = parser.parse_args()
    if args.command == "resolve":
        resolve()
    elif args.command == "publication":
        guard_publication(os.environ["GITHUB_REPOSITORY"], os.environ["DAILY_COMMIT"], os.environ["DAILY_TAG"])
    elif args.command == 'verify':
        verify_assets(args.assets, args.product, os.environ["DAILY_COMMIT"], os.environ["DAILY_TAG"], os.environ["DAILY_VERSION"])
    elif args.command == 'public':
        stage_public_assets(args.assets, args.destination, args.product)
    else:
        include_linux(args.assets, args.linux_assets, args.product, os.environ['DAILY_COMMIT'], os.environ['DAILY_VERSION'])
