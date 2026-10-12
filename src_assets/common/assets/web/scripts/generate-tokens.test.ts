import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtemp, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import test, { type TestContext } from "node:test";
import { fileURLToPath } from "node:url";

const webRoot = fileURLToPath(new URL("../", import.meta.url));
const outputs = ["styles/tokens.css", "generated/tokens.ts"] as const;
const fixtureFiles = [
  "scripts/generate-tokens.mjs",
  "design/tokens.json",
  ...outputs,
];

async function fixture(context: TestContext) {
  const directory = await mkdtemp(join(tmpdir(), "vibeshine tokens # % Ω-"));
  assert.equal(dirname(resolve(directory)), resolve(tmpdir()));
  context.after(async () => {
    assert.equal(dirname(resolve(directory)), resolve(tmpdir()));
    await rm(directory, { recursive: true, force: true });
  });
  const expected = new Map<string, string>();
  for (const path of fixtureFiles) {
    const destination = join(directory, path);
    await mkdir(dirname(destination), { recursive: true });
    const contents = (await readFile(join(webRoot, path), "utf8")).replace(
      /\r\n/g,
      "\n",
    );
    await writeFile(destination, contents, "utf8");
    expected.set(path, contents);
  }
  return { directory, expected };
}

async function snapshot(directory: string) {
  const result = new Map<string, Buffer | null>();
  for (const path of fixtureFiles) {
    result.set(path, await readFile(join(directory, path)).catch(() => null));
  }
  return result;
}

function run(directory: string, ...arguments_: string[]) {
  // Run from an unrelated cwd; the generator's URL-relative paths must win.
  const result = spawnSync(
    process.execPath,
    [join(directory, fixtureFiles[0]!), ...arguments_],
    {
      cwd: tmpdir(),
      encoding: "utf8",
    },
  );
  assert.equal(result.error, undefined);
  return result;
}

for (const mode of ["LF", "CRLF", "mixed"] as const) {
  test(`token check accepts ${mode} checkout text without rewriting it`, async (context) => {
    const { directory, expected } = await fixture(context);
    for (const path of outputs) {
      let count = 0;
      const content = expected.get(path)!;
      await writeFile(
        join(directory, path),
        mode === "LF"
          ? content
          : content.replace(/\n/g, () =>
              mode === "CRLF" || count++ % 2 === 0 ? "\r\n" : "\n",
            ),
        "utf8",
      );
    }
    const before = await snapshot(directory);
    const result = run(directory, "--check");
    assert.equal(result.status, 0, result.stderr);
    assert.equal(result.stdout, "");
    assert.equal(result.stderr, "");
    assert.deepEqual(await snapshot(directory), before);
  });
}

for (const path of outputs) {
  for (const drift of [
    "content",
    "missing",
    "bare CR",
    "BOM",
    "trailing whitespace",
    "missing final newline",
  ] as const) {
    test(`token check rejects ${drift} in ${path} without writing`, async (context) => {
      const { directory, expected } = await fixture(context);
      const destination = join(directory, path);
      const original = expected.get(path)!;
      if (drift === "missing") {
        await rm(destination);
      } else {
        const contents =
          drift === "content"
            ? `${original}/* stale generated content */\n`
            : drift === "bare CR"
              ? `${original}\r`
              : drift === "BOM"
                ? `\uFEFF${original}`
                : drift === "trailing whitespace"
                  ? `${original} `
                  : original.slice(0, -1);
        await writeFile(destination, contents, "utf8");
      }
      const before = await snapshot(directory);
      const result = run(directory, "--check");
      assert.equal(result.status, 1, result.stderr);
      assert.match(
        result.stderr,
        new RegExp(`${path.replace(".", "\\.")} is out of date`),
      );
      const other = outputs.find((output) => output !== path)!;
      assert.equal(result.stderr.includes(`${other} is out of date`), false);
      assert.deepEqual(await snapshot(directory), before);
    });
  }
}

test("token print stays deterministic with stale and missing targets and never writes", async (context) => {
  const { directory, expected } = await fixture(context);
  await writeFile(join(directory, outputs[0]), "stale CSS\r\n", "utf8");
  await rm(join(directory, outputs[1]));
  const before = await snapshot(directory);
  for (const [target, path] of [
    ["css", outputs[0]],
    ["typescript", outputs[1]],
  ] as const) {
    const first = run(directory, `--print=${target}`);
    const second = run(directory, "--check", `--print=${target}`);
    assert.equal(first.status, 0, first.stderr);
    assert.equal(second.status, 0, second.stderr);
    assert.equal(first.stdout, expected.get(path));
    assert.equal(second.stdout, first.stdout);
    assert.equal(first.stdout.includes("\r"), false);
    assert.equal(first.stderr, "");
    assert.equal(second.stderr, "");
  }
  assert.deepEqual(await snapshot(directory), before);
});

test("ordinary token generation still writes exact LF output deterministically", async (context) => {
  const { directory, expected } = await fixture(context);
  for (const path of outputs) {
    await writeFile(
      join(directory, path),
      "stale generated output\r\n",
      "utf8",
    );
  }
  const first = run(directory);
  assert.equal(first.status, 0, first.stderr);
  for (const path of outputs) {
    assert.equal(
      await readFile(join(directory, path), "utf8"),
      expected.get(path),
    );
  }
  const before = await snapshot(directory);
  const second = run(directory);
  const check = run(directory, "--check");
  assert.equal(second.status, 0, second.stderr);
  assert.equal(check.status, 0, check.stderr);
  assert.deepEqual(await snapshot(directory), before);
});
