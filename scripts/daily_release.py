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


def identity(commit, day, base, revision=None):
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("Expected an exact source commit SHA")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", base):
        raise ValueError("Expected a numeric source version base")
    major, minor, patch = map(int, base.split("."))
    if major > 255 or minor > 255 or patch > 654 or base == "0.0.0":
        raise ValueError("Source version base exceeds MSI limits or is zero")
    if not 2000 <= day.year <= 2099:
        raise ValueError("Daily version year must fit the installer version scheme")
    if revision is not None and (not isinstance(revision, int) or not 1 <= revision <= 999):
        raise ValueError("Nightly revision must be in 1..999")
    suffix = "" if revision is None else f".{revision}"
    return {
        "commit": commit,
        "tag": f"daily-{day.isoformat()}{suffix}",
        "version": f"{base}-daily.d{day:%Y%m%d}{suffix}+{commit[:12]}",
    }


def validate_source(repository, branch, confirmation):
    if repository not in {"Nonary/vibeshine", "Nonary/Vibepollo"}:
        raise ValueError("Daily publishing is limited to the designated upstream repositories")
    if branch not in {"master", "main"}:
        raise ValueError("Daily source must use the verified master/main primary; legacy vibe/test lanes are retired")
    if confirmation != f"{repository}:{branch}":
        raise ValueError("Set DAILY_RELEASE_SOURCE_BRANCH and explicitly confirm repository:branch in DAILY_RELEASE_SOURCE_CONFIRMED")
    subprocess.run(["git", "check-ref-format", f"refs/heads/{branch}"], check=True)


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()


def validate_migration(repository, target_commit, upstream_commit, confirmation):
    if repository != "Nonary/Vibepollo":
        return
    if not re.fullmatch(r"[0-9a-f]{40}", target_commit) or not re.fullmatch(r"[0-9a-f]{40}", upstream_commit):
        raise ValueError("Migration confirmation requires exact source and target SHAs")
    expected = f"Nonary/vibeshine:{upstream_commit}:Nonary/Vibepollo:{target_commit}"
    if confirmation != expected:
        raise ValueError("Vibepollo daily is blocked until the exact upstream-to-primary migration is reviewed and confirmed")


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


def resolve():
    repository = os.environ["GITHUB_REPOSITORY"]
    branch = os.environ.get("DAILY_RELEASE_SOURCE_BRANCH", "")
    validate_source(repository, branch, os.environ.get("DAILY_RELEASE_SOURCE_CONFIRMED", ""))
    if branch != os.environ.get("DAILY_PRIMARY_BRANCH"):
        raise ValueError("Daily source must equal the verified repository default primary")
    # Fetch only the explicitly confirmed remote branch; ignore local HEAD/tags.
    run("git", "fetch", "--no-tags", "origin", f"refs/heads/{branch}:refs/remotes/origin/daily-source")
    commit = run("git", "rev-parse", "refs/remotes/origin/daily-source^{commit}")
    run("git", "fetch", "--no-recurse-submodules", "origin", "refs/tags/*:refs/tags/*")
    releases = json.loads(run("gh", "api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    previous = previous_successful_nightly(repository, releases)
    if previous and previous["source_commit"] == commit:
        # Each repository owns its baseline. Upstream advances cannot turn an
        # unchanged target into a build or an assertion that migration happened.
        emit_outputs({"commit": commit, "should_build": "false", "skip_reason": f"Source unchanged since {previous['tag']}"})
        return
    if repository == "Nonary/Vibepollo":
        upstream = run("git", "ls-remote", "https://github.com/Nonary/vibeshine.git", "refs/heads/master").split()
        if len(upstream) != 2 or upstream[1] != "refs/heads/master":
            raise ValueError("Could not resolve the verified Vibeshine primary for migration")
        validate_migration(repository, commit, upstream[0], os.environ.get("DAILY_RELEASE_MIGRATION_CONFIRMED", ""))
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
    prefix = f"daily-{day.isoformat()}"
    reserved = run("git", "ls-remote", "--tags", "origin", f"refs/tags/{prefix}*")
    names = [line.split()[-1].removeprefix("refs/tags/") for line in reserved.splitlines()]
    names += [r["tag_name"] for page in releases for r in page]
    revisions = [1 if name == prefix else int(name[len(prefix)+1:]) for name in names
                 if re.fullmatch(re.escape(prefix) + r"(?:\.[1-9][0-9]{0,2})?", name)]
    info = identity(commit, day, base, max(revisions, default=0) + 1)
    info["should_build"] = "true"
    emit_outputs(info)


def verify_assets(root, product, commit, tag, version):
    root = Path(root)
    expected = {f"{product}Setup.exe", "release-provenance.json"}
    actual = {p.name for p in root.iterdir()}
    if actual != expected or any(not p.is_file() or p.is_symlink() or p.stat().st_size == 0 for p in root.iterdir()):
        raise ValueError(f"Expected exactly the finalized nonempty assets {sorted(expected)}; got {sorted(actual)}")
    info = identity(commit, dt.date.fromisoformat(tag.removeprefix("daily-").split(".")[0]), version.split("-")[0],
                    int(tag.split(".")[1]) if "." in tag else None)
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["resolve", "verify", "linux"])
    parser.add_argument("--assets")
    parser.add_argument("--linux-assets")
    parser.add_argument("--product", choices=["Vibeshine", "Vibepollo"])
    args = parser.parse_args()
    if args.command == "resolve":
        resolve()
    elif args.command == 'verify':
        verify_assets(args.assets, args.product, os.environ["DAILY_COMMIT"], os.environ["DAILY_TAG"], os.environ["DAILY_VERSION"])
    else:
        include_linux(args.assets, args.linux_assets, args.product, os.environ['DAILY_COMMIT'], os.environ['DAILY_VERSION'])
