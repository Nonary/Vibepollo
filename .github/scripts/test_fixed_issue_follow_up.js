"use strict";

const assert = require("node:assert/strict");
const { test } = require("node:test");
const fs = require("node:fs");
const path = require("node:path");

const {
  handleDailyReleaseFollowUp,
  handleFixedLabel,
  releaseFromContext,
} = require("./fixed_issue_follow_up.js");

const RELEASE_TIME = "2026-10-22T06:17:00Z";
const NOTICE_TIME = "2026-10-22T05:55:00Z";
const PROOF_TIME = "2026-10-22T06:00:00Z";
const SOURCE_COMMIT = "1".repeat(40);
const RELEASE_COMMIT = "2".repeat(40);
const UNRELATED_SOURCE_COMMIT = "3".repeat(40);
const UNRELATED_RELEASE_COMMIT = "4".repeat(40);
const DRIVER_SOURCE = "b".repeat(40);
const DRIVER_ARCHIVE_SHA256 = "a".repeat(64);
const DRIVER_LOCK_PATH = "third-party/libvirtualdisplay-release/windows-x64.json";

const matchingLock = {
  schema_version: 1,
  repository: "Nonary/libvirtualdisplay",
  tag: "v1.6.4",
  source_revision: DRIVER_SOURCE,
  archive_sha256: DRIVER_ARCHIVE_SHA256,
};

function createMockGit({ ancestor = true, lock = matchingLock } = {}) {
  const calls = [];
  const git = (command, args) => {
    calls.push({ command, args });
    assert.equal(command, "git");
    if (args[2] === "merge-base") {
      if (!ancestor) throw new Error("candidate is not an ancestor");
      return "";
    }
    if (args[2] === "show") return JSON.stringify(lock);
    throw new Error(`Unexpected mocked git command: ${args.join(" ")}`);
  };
  return { git, calls };
}

const containingFixture = {
  repositoryPath: "/mock/repository",
  candidateCommit: SOURCE_COMMIT,
  releaseCommit: RELEASE_COMMIT,
  gitProof: createMockGit(),
};
const unrelatedFixture = {
  repositoryPath: "/mock/repository",
  candidateCommit: UNRELATED_SOURCE_COMMIT,
  releaseCommit: UNRELATED_RELEASE_COMMIT,
  gitProof: createMockGit({ ancestor: false }),
};
const mismatchedDependencyFixture = {
  repositoryPath: "/mock/repository",
  candidateCommit: SOURCE_COMMIT,
  releaseCommit: RELEASE_COMMIT,
  gitProof: createMockGit({ lock: { ...matchingLock, source_revision: "c".repeat(40) } }),
};

function makeIssue(overrides = {}) {
  return {
    number: 325,
    state: "open",
    pull_request: null,
    labels: [{ name: "fixed" }],
    created_at: "2026-07-15T00:00:00Z",
    updated_at: NOTICE_TIME,
    ...overrides,
  };
}

function makeDaily(overrides = {}) {
  return {
    draft: false,
    prerelease: true,
    tag_name: "2.0.0-nightly.d20261022.222222222222",
    published_at: RELEASE_TIME,
    ...overrides,
  };
}

function makeNoticeComment() {
  return {
    body: "<!-- fixed-candidate-notice:v1:cycle-1 --> Candidate fix is awaiting confirmation.",
    user: { login: "github-actions[bot]", type: "Bot" },
    created_at: NOTICE_TIME,
    updated_at: NOTICE_TIME,
  };
}

function makeProofComment(sourceCommit, overrides = {}) {
  const proof = {
    source_commit: sourceCommit,
    dependencies: [{
      lock_path: DRIVER_LOCK_PATH,
      repository: "Nonary/libvirtualdisplay",
      source_revision: DRIVER_SOURCE,
      archive_sha256: DRIVER_ARCHIVE_SHA256,
    }],
  };
  return {
    body: `Maintainer-verified candidate metadata.\n<!-- fixed-candidate:v1\n${JSON.stringify(proof)}\n-->`,
    author_association: "OWNER",
    user: { login: "Nonary", type: "User" },
    created_at: PROOF_TIME,
    updated_at: PROOF_TIME,
    ...overrides,
  };
}

function createMockGithub(issues, commentsByIssue = new Map()) {
  const calls = {
    listIssues: [],
    listComments: [],
    createdComments: [],
    stateUpdates: [],
  };
  const listForRepo = async (parameters) => parameters;
  const listComments = async (parameters) => parameters;
  const github = {
    rest: {
      issues: {
        listForRepo,
        listComments,
        createComment: async (parameters) => {
          calls.createdComments.push(parameters);
          const existing = commentsByIssue.get(parameters.issue_number) || [];
          const now = "2026-10-22T06:18:00Z";
          existing.push({
            body: parameters.body,
            author_association: "NONE",
            user: { login: "github-actions[bot]", type: "Bot" },
            created_at: now,
            updated_at: now,
          });
          commentsByIssue.set(parameters.issue_number, existing);
          return { data: {} };
        },
        update: async (parameters) => {
          calls.stateUpdates.push(parameters);
          return { data: {} };
        },
      },
    },
    paginate: async (endpoint, parameters) => {
      if (endpoint === listForRepo) {
        calls.listIssues.push(parameters);
        return issues;
      }
      if (endpoint === listComments) {
        calls.listComments.push(parameters);
        return commentsByIssue.get(parameters.issue_number) || [];
      }
      throw new Error("Unexpected mocked GitHub endpoint");
    },
  };
  return { github, calls, commentsByIssue };
}

function createMockReleaseFetch(publishedRelease, { status = 200 } = {}) {
  const calls = [];
  const fetchImpl = async (url, options) => {
    calls.push({ url, options });
    return {
      ok: status >= 200 && status < 300,
      status,
      json: async () => publishedRelease,
    };
  };
  return { fetchImpl, calls };
}

function candidateComments(sourceCommit, proofOverrides = {}) {
  return [makeNoticeComment(), makeProofComment(sourceCommit, proofOverrides)];
}

async function runDaily({
  issue = makeIssue(),
  comments = candidateComments(containingFixture.candidateCommit),
  release = makeDaily(),
  publishedRelease = release,
  fetchImpl = createMockReleaseFetch(publishedRelease).fetchImpl,
  fixture = containingFixture,
} = {}) {
  const commentsByIssue = new Map([[issue.number, comments]]);
  const mock = createMockGithub([issue], commentsByIssue);
  const result = await handleDailyReleaseFollowUp({
    github: mock.github,
    owner: "Nonary",
    repo: "Vibepollo",
    release,
    releaseCommit: fixture.releaseCommit,
    repositoryPath: fixture.repositoryPath,
    git: fixture.gitProof.git,
    fetchImpl,
  });
  return { ...mock, result, issue, fixture };
}

function assertNoIssueStateUpdates(mock) {
  assert.deepEqual(mock.calls.stateUpdates, []);
}

test("the fixed label notice identifies a candidate and retries do not duplicate it", async () => {
  const issue = makeIssue({ updated_at: "2026-10-22T05:40:00Z" });
  const mock = createMockGithub([issue], new Map([[issue.number, []]]));
  const context = {
    eventName: "issues",
    repo: { owner: "Nonary", repo: "Vibepollo" },
    payload: {
      action: "labeled",
      label: { name: "fixed" },
      issue,
    },
  };

  const first = await handleFixedLabel({ github: mock.github, context });
  const retry = await handleFixedLabel({ github: mock.github, context });

  assert.equal(first.commented, true);
  assert.equal(retry.reason, "notice-already-exists");
  assert.equal(mock.calls.createdComments.length, 1);
  const body = mock.calls.createdComments[0].body;
  assert.match(body, /candidate/i);
  assert.match(body, /awaiting your confirmation/i);
  assert.match(body, /does not confirm the issue is resolved/i);
  assert.doesNotMatch(body, /will be fixed in the next release/i);
  assertNoIssueStateUpdates(mock);
});

test("draft releases are ignored", async () => {
  const result = await runDaily({ release: makeDaily({ draft: true }) });
  assert.equal(result.result.comments_created, 0);
  assert.equal(result.calls.listIssues.length, 0);
  assertNoIssueStateUpdates(result);
});

test("a dispatch cannot claim a daily is published when GitHub has no release", async () => {
  const api = createMockReleaseFetch(null, { status: 404 });
  const result = await runDaily({ fetchImpl: api.fetchImpl });
  assert.equal(result.result.comments_created, 0);
  assert.equal(result.calls.listIssues.length, 0);
  assert.equal(api.calls.length, 1);
  assertNoIssueStateUpdates(result);
});

test("dispatch publication fields cannot override a draft or publication timestamp", async () => {
  const draftApi = createMockReleaseFetch(makeDaily({ draft: true }));
  const draftResult = await runDaily({
    release: makeDaily({ draft: false, prerelease: true, published_at: RELEASE_TIME }),
    fetchImpl: draftApi.fetchImpl,
  });
  assert.equal(draftResult.result.comments_created, 0);
  assertNoIssueStateUpdates(draftResult);

  const actualRelease = makeDaily({ published_at: "2026-10-22T06:05:00Z" });
  const claimedRelease = makeDaily({ published_at: RELEASE_TIME });
  const lateProof = candidateComments(containingFixture.candidateCommit, {
    created_at: "2026-10-22T06:10:00Z",
    updated_at: "2026-10-22T06:10:00Z",
  });
  const timeResult = await runDaily({
    release: claimedRelease,
    publishedRelease: actualRelease,
    comments: lateProof,
  });
  assert.equal(timeResult.result.comments_created, 0);
  assert.equal(timeResult.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(timeResult);
});

test("legacy daily tags remain eligible for verified follow-up", async () => {
  const result = await runDaily({ release: makeDaily({ tag_name: "2.0.0-daily.d20261022" }) });
  assert.equal(result.result.comments_created, 1);
  assertNoIssueStateUpdates(result);
});

test("unrelated prereleases are ignored", async () => {
  const result = await runDaily({ release: makeDaily({ tag_name: "2.0.0-beta.1" }) });
  assert.equal(result.result.comments_created, 0);
  assert.equal(result.calls.listIssues.length, 0);
  assertNoIssueStateUpdates(result);
});

test("unlabeled issues are ignored", async () => {
  const result = await runDaily({ issue: makeIssue({ labels: [{ name: "bug" }] }) });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
});

test("pull requests are ignored", async () => {
  const result = await runDaily({ issue: makeIssue({ pull_request: { url: "https://example.invalid/pr/1" } }) });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
});

test("issues created after the daily are ignored", async () => {
  const result = await runDaily({
    issue: makeIssue({ created_at: "2026-10-22T06:18:00Z" }),
  });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
});

test("proof comments created after publication are ignored", async () => {
  const result = await runDaily({
    comments: candidateComments(containingFixture.candidateCommit, {
      created_at: "2026-10-22T06:18:00Z",
      updated_at: "2026-10-22T06:18:00Z",
    }),
  });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
});

test("missing proof leaves the issue untouched", async () => {
  const result = await runDaily({ comments: [makeNoticeComment()] });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
});

test("proof from an untrusted commenter leaves the issue untouched", async () => {
  const result = await runDaily({
    comments: candidateComments(containingFixture.candidateCommit, {
      author_association: "NONE",
    }),
  });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
});

test("malformed proof field types fail closed", async () => {
  const invalidProof = {
    source_commit: SOURCE_COMMIT,
    dependencies: [{
      lock_path: 7,
      repository: "Nonary/libvirtualdisplay",
      source_revision: DRIVER_SOURCE,
      archive_sha256: DRIVER_ARCHIVE_SHA256,
    }],
  };
  const comments = [
    makeNoticeComment(),
    makeProofComment(SOURCE_COMMIT, {
      body: `<!-- fixed-candidate:v1\n${JSON.stringify(invalidProof)}\n-->`,
    }),
  ];
  const result = await runDaily({ comments });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
});

test("a non-ancestor candidate is not presented as included", async () => {
  const result = await runDaily({
    comments: candidateComments(unrelatedFixture.candidateCommit),
    fixture: unrelatedFixture,
  });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
  assert.ok(unrelatedFixture.gitProof.calls.some(({ args }) =>
    args[2] === "merge-base" && args[4] === UNRELATED_SOURCE_COMMIT &&
    args[5] === UNRELATED_RELEASE_COMMIT,
  ));
});

test("a dependency lock mismatch is not presented as included", async () => {
  const result = await runDaily({ fixture: mismatchedDependencyFixture });
  assert.equal(result.calls.createdComments.length, 0);
  assertNoIssueStateUpdates(result);
  assert.ok(mismatchedDependencyFixture.gitProof.calls.some(({ args }) =>
    args[2] === "show" && args[3] === `${RELEASE_COMMIT}:${DRIVER_LOCK_PATH}`,
  ));
});

test("an eligible containing daily asks for confirmation and leaves the issue open", async () => {
  const result = await runDaily();
  assert.equal(result.result.comments_created, 1);
  assert.equal(result.calls.createdComments.length, 1);
  const body = result.calls.createdComments[0].body;
  assert.match(body, /2\.0\.0-nightly\.d20261022\.222222222222/);
  assert.match(body, /Could you test this specific build and confirm/i);
  assert.match(body, /issue remains open until you confirm/i);
  assert.equal(result.issue.state, "open");
  assertNoIssueStateUpdates(result);
});

test("release events, dispatch retries, and later dailies deduplicate one candidate cycle", async () => {
  const release = makeDaily();
  const fromRelease = releaseFromContext({
    eventName: "release",
    payload: { release },
  });
  const fromDispatch = releaseFromContext({
    eventName: "repository_dispatch",
    payload: {
      action: "daily-release-published",
      client_payload: { ...release, source_commit: containingFixture.releaseCommit },
    },
  });
  assert.deepEqual(fromRelease, release);
  assert.equal(fromDispatch.tag_name, release.tag_name);

  const commentsByIssue = new Map([[
    325,
    candidateComments(containingFixture.candidateCommit),
  ]]);
  const mock = createMockGithub([makeIssue()], commentsByIssue);
  for (const daily of [
    fromRelease,
    { ...fromDispatch },
    makeDaily({ tag_name: "2.0.0-daily.d20261023", published_at: "2026-10-23T06:17:00Z" }),
  ]) {
    await handleDailyReleaseFollowUp({
      github: mock.github,
      owner: "Nonary",
      repo: "Vibepollo",
      release: daily,
      releaseCommit: containingFixture.releaseCommit,
      repositoryPath: containingFixture.repositoryPath,
      git: containingFixture.gitProof.git,
      fetchImpl: createMockReleaseFetch(daily).fetchImpl,
    });
  }

  assert.equal(mock.calls.createdComments.length, 1);
  assertNoIssueStateUpdates(mock);
});


test("workflow eligibility accepts legacy daily and commit-bound nightly tags", () => {
  const workflow = fs.readFileSync(path.join(__dirname, "../workflows/fixed-issue-follow-up.yml"), "utf8");
  const literal = workflow.match(/const dailyTag = (\/[^\n]+\/);/);
  assert.ok(literal, "workflow must retain its release tag gate");
  const gate = new Function(`return ${literal[1]}`)();
  assert.ok(gate.test("2.0.2-daily.d20261020.4"));
  assert.ok(gate.test("2.0.2-nightly.d20261022.aaaaaaaaaaaa"));
  assert.equal(gate.test("2.0.2-nightly.d20261022.invalid"), false);
});
