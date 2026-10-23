"use strict";

const { createHash } = require("node:crypto");
const { execFileSync } = require("node:child_process");

const TRUSTED_ASSOCIATIONS = new Set(["OWNER", "MEMBER", "COLLABORATOR"]);
const DAILY_TAG = /^\d+\.\d+\.\d+-(?:daily\.d\d{8}(?:\.\d+)?|nightly\.d\d{8}\.[0-9a-f]{12})$/;
const SOURCE_SHA = /^[0-9a-f]{40}$/i;
const ARCHIVE_SHA256 = /^[0-9a-f]{64}$/i;
const NOTICE_PREFIX = "<!-- fixed-candidate-notice:v1:";
const PROOF_PREFIX = "<!-- fixed-candidate:v1";

function asTime(value) {
  const parsed = Date.parse(value || "");
  return Number.isFinite(parsed) ? parsed : null;
}

function hasFixedLabel(issue) {
  return Array.isArray(issue.labels) && issue.labels.some((label) =>
    (typeof label === "string" ? label : label?.name) === "fixed",
  );
}

function isIssueCandidate(issue, publishedAt) {
  const createdAt = asTime(issue.created_at);
  const releaseTime = asTime(publishedAt);
  return issue.state === "open" &&
    !issue.pull_request &&
    hasFixedLabel(issue) &&
    createdAt !== null &&
    releaseTime !== null &&
    createdAt < releaseTime;
}

function getMarkerComment(comments, prefix) {
  return comments
    .filter((comment) => typeof comment.body === "string" &&
      comment.body.includes(prefix))
    .sort((left, right) => (asTime(left.created_at) ?? 0) - (asTime(right.created_at) ?? 0))
    .at(-1);
}

function isBotComment(comment) {
  const login = comment.user?.login || "";
  return comment.user?.type === "Bot" || login.endsWith("[bot]");
}

function parseCandidateProof(body) {
  if (typeof body !== "string") return null;
  const match = body.match(/<!-- fixed-candidate:v1\s+([\s\S]*?)\s*-->/);
  if (!match) return null;

  let proof;
  try {
    proof = JSON.parse(match[1]);
  } catch {
    return null;
  }

  if (!proof || typeof proof !== "object" || Array.isArray(proof) ||
      typeof proof.source_commit !== "string" || !SOURCE_SHA.test(proof.source_commit) ||
      !Array.isArray(proof.dependencies) || proof.dependencies.length > 32) {
    return null;
  }

  const dependencies = [];
  const seenLockPaths = new Set();
  for (const dependency of proof.dependencies) {
    const lockPath = dependency?.lock_path;
    const pathParts = typeof lockPath === "string" ? lockPath.split("/") : [];
    if (typeof lockPath !== "string" || !/^[A-Za-z0-9_./-]{1,256}$/.test(lockPath) ||
        lockPath.startsWith("/") || lockPath.includes(":") ||
        pathParts.some((part) => part === "" || part === "." || part === "..") ||
        seenLockPaths.has(lockPath) ||
        typeof dependency?.repository !== "string" ||
        !/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(dependency.repository) ||
        typeof dependency?.source_revision !== "string" ||
        !SOURCE_SHA.test(dependency.source_revision) ||
        typeof dependency?.archive_sha256 !== "string" ||
        !ARCHIVE_SHA256.test(dependency.archive_sha256)) {
      return null;
    }
    seenLockPaths.add(lockPath);
    dependencies.push({
      lock_path: lockPath,
      repository: dependency.repository,
      source_revision: dependency.source_revision.toLowerCase(),
      archive_sha256: dependency.archive_sha256.toLowerCase(),
    });
  }

  dependencies.sort((left, right) => left.lock_path.localeCompare(right.lock_path));
  return {
    source_commit: proof.source_commit.toLowerCase(),
    dependencies,
  };
}

function proofMarker(proof, noticeMarker) {
  const identity = JSON.stringify({
    notice: noticeMarker,
    source_commit: proof.source_commit,
    dependencies: proof.dependencies,
  });
  const cycle = createHash("sha256").update(identity).digest("hex").slice(0, 24);
  return `<!-- fixed-follow-up:v1:${cycle} -->`;
}

function verifyCandidateInRelease(proof, releaseCommit, repositoryPath, git = execFileSync) {
  if (!SOURCE_SHA.test(releaseCommit || "")) return false;
  try {
    git("git", ["-C", repositoryPath, "merge-base", "--is-ancestor", proof.source_commit, releaseCommit], {
      stdio: "ignore",
    });
  } catch {
    return false;
  }

  for (const dependency of proof.dependencies) {
    let lock;
    try {
      const contents = git("git", ["-C", repositoryPath, "show", `${releaseCommit}:${dependency.lock_path}`], {
        encoding: "utf8",
        stdio: ["ignore", "pipe", "ignore"],
      });
      lock = JSON.parse(contents);
    } catch {
      return false;
    }
    if (lock?.schema_version !== 1 ||
        lock?.repository !== dependency.repository ||
        typeof lock?.source_revision !== "string" ||
        lock.source_revision.toLowerCase() !== dependency.source_revision ||
        typeof lock?.archive_sha256 !== "string" ||
        lock.archive_sha256.toLowerCase() !== dependency.archive_sha256) {
      return false;
    }
  }
  return true;
}

async function listIssueComments(github, owner, repo, issueNumber) {
  return github.paginate(github.rest.issues.listComments, {
    owner,
    repo,
    issue_number: issueNumber,
    per_page: 100,
  });
}

async function handleFixedLabel({ github, context }) {
  const issue = context.payload?.issue;
  if (context.eventName !== "issues" || context.payload?.action !== "labeled" ||
      context.payload?.label?.name !== "fixed" || issue?.state !== "open" ||
      issue?.pull_request) {
    return { commented: false, reason: "not-an-open-fixed-issue" };
  }

  const { owner, repo } = context.repo;
  const comments = await listIssueComments(github, owner, repo, issue.number);
  const cycle = createHash("sha256")
    .update(`${issue.number}:${issue.updated_at || issue.created_at || ""}`)
    .digest("hex")
    .slice(0, 24);
  const marker = `${NOTICE_PREFIX}${cycle} -->`;
  if (comments.some((comment) => isBotComment(comment) && comment.body?.includes(marker))) {
    return { commented: false, reason: "notice-already-exists" };
  }

  await github.rest.issues.createComment({
    owner,
    repo,
    issue_number: issue.number,
    body: `${marker}\nThe maintainer has marked this as a candidate because a change appears to address the report. That does not confirm the issue is resolved, and the candidate is still awaiting your confirmation. We will ask you to test a specific daily build only after this change and any required packaged dependencies are verified in it. No next-release inclusion is promised; the issue stays open pending your report.`,
  });
  return { commented: true, reason: "candidate-notice-created" };
}

function isPublishedDaily(release) {
  return !!release && release.draft === false && release.prerelease === true &&
    DAILY_TAG.test(release.tag_name || "") && asTime(release.published_at) !== null;
}

async function getPublishedDailyRelease({ owner, repo, tagName, fetchImpl = globalThis.fetch }) {
  if (!DAILY_TAG.test(tagName || "") || typeof fetchImpl !== "function") return null;
  const url = `https://api.github.com/repos/${encodeURIComponent(owner)}/${encodeURIComponent(repo)}` +
    `/releases/tags/${encodeURIComponent(tagName)}`;
  try {
    const response = await fetchImpl(url, {
      headers: {
        Accept: "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
      },
    });
    if (!response.ok) return null;
    const release = await response.json();
    return release?.tag_name === tagName && isPublishedDaily(release) ? release : null;
  } catch {
    return null;
  }
}

function releaseFromContext(context) {
  if (context.eventName === "release") return context.payload?.release || null;
  if (context.eventName === "repository_dispatch" &&
      context.payload?.action === "daily-release-published") {
    return context.payload?.client_payload || null;
  }
  return null;
}

function isMaintainerProof(comment) {
  return TRUSTED_ASSOCIATIONS.has(comment.author_association) &&
    typeof comment.body === "string" && comment.body.includes(PROOF_PREFIX);
}

function isBotNotice(comment) {
  return isBotComment(comment) &&
    typeof comment.body === "string" && comment.body.includes(NOTICE_PREFIX);
}

async function handleDailyReleaseFollowUp({
  github,
  owner,
  repo,
  release,
  releaseCommit,
  repositoryPath,
  git = execFileSync,
  fetchImpl = globalThis.fetch,
}) {
  if (!DAILY_TAG.test(release?.tag_name || "")) {
    return { comments_created: 0, reason: "release-not-a-published-daily" };
  }

  // Release and repository_dispatch payloads identify the tag but are not
  // publication evidence. Read the public GitHub Release by tag and use its
  // draft, prerelease, and published_at fields as the sole authority.
  const publishedRelease = await getPublishedDailyRelease({
    owner,
    repo,
    tagName: release.tag_name,
    fetchImpl,
  });
  if (!publishedRelease) {
    return { comments_created: 0, reason: "release-not-a-published-daily" };
  }

  const issues = await github.paginate(github.rest.issues.listForRepo, {
    owner,
    repo,
    state: "open",
    labels: "fixed",
    per_page: 100,
  });
  const publishedAt = asTime(publishedRelease.published_at);
  let created = 0;

  for (const issue of issues) {
    if (!isIssueCandidate(issue, publishedRelease.published_at)) continue;

    const comments = await listIssueComments(github, owner, repo, issue.number);
    const latestNotice = getMarkerComment(comments.filter(isBotNotice), NOTICE_PREFIX);
    if (!latestNotice) continue;
    const noticeCreatedAt = asTime(latestNotice.created_at);
    if (noticeCreatedAt === null || noticeCreatedAt >= publishedAt) continue;
    const noticeMarker = latestNotice.body.match(/<!-- fixed-candidate-notice:v1:[^ ]+ -->/)?.[0];
    if (!noticeMarker) continue;

    // The newest maintainer proof after this label cycle owns the decision. A
    // newer malformed, late, or incomplete proof does not fall back to stale data.
    const latestProof = getMarkerComment(
      comments.filter(isMaintainerProof).filter((comment) => {
        const createdAt = asTime(comment.created_at);
        return createdAt !== null && createdAt > noticeCreatedAt;
      }),
      PROOF_PREFIX,
    );
    if (!latestProof) continue;
    const proofCreatedAt = asTime(latestProof.created_at);
    const proofUpdatedAt = asTime(latestProof.updated_at || latestProof.created_at);
    if (proofCreatedAt === null || proofUpdatedAt === null ||
        proofCreatedAt >= publishedAt || proofUpdatedAt >= publishedAt) {
      continue;
    }

    const proof = parseCandidateProof(latestProof.body);
    if (!proof || !verifyCandidateInRelease(proof, releaseCommit, repositoryPath, git)) continue;
    const marker = proofMarker(proof, noticeMarker);
    if (comments.some((comment) => isBotComment(comment) && comment.body?.includes(marker))) continue;

    const releaseUrl = `https://github.com/${owner}/${repo}/releases/tag/${encodeURIComponent(publishedRelease.tag_name)}`;
    await github.rest.issues.createComment({
      owner,
      repo,
      issue_number: issue.number,
      body: `${marker}\nA daily build containing this candidate change and its declared dependency locks is available: [${publishedRelease.tag_name}](${releaseUrl}). Could you test this specific build and confirm whether it resolves the reported issue? If it does not, please share what still fails. The issue remains open until you confirm.`,
    });
    created += 1;
  }

  return { comments_created: created, reason: created ? "follow-up-created" : "no-eligible-candidate" };
}

module.exports = {
  handleFixedLabel,
  handleDailyReleaseFollowUp,
  isPublishedDaily,
  getPublishedDailyRelease,
  releaseFromContext,
  parseCandidateProof,
  verifyCandidateInRelease,
};
