import { createHash } from "node:crypto";
import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { createInterface } from "node:readline";
import { afterAll, beforeAll, expect, test } from "vitest";
import * as hegel from "@hegeldev/hegel";
import * as gs from "@hegeldev/hegel/generators";

const propertySettings = {
  testCases: 2_000,
  database: hegel.Database.unset,
};

class PythonBridge {
  private process: ChildProcessWithoutNullStreams;
  private pending: Array<{
    resolve: (value: unknown) => void;
    reject: (error: Error) => void;
  }> = [];

  constructor() {
    this.process = spawn(process.env.PYTHON ?? "python3", ["tests/hegel/bridge.py"], {
      stdio: ["pipe", "pipe", "pipe"],
    });
    createInterface({ input: this.process.stdout }).on("line", (line) => {
      const request = this.pending.shift();
      if (!request) return;
      const response = JSON.parse(line) as { result?: unknown; error?: string };
      if (response.error) request.reject(new Error(response.error));
      else request.resolve(response.result);
    });
    this.process.on("exit", (code) => {
      const error = new Error(`Python property bridge exited with code ${code}`);
      for (const request of this.pending.splice(0)) request.reject(error);
    });
  }

  call<T>(operation: string, ...args: unknown[]): Promise<T> {
    return new Promise((resolve, reject) => {
      this.pending.push({ resolve: resolve as (value: unknown) => void, reject });
      this.process.stdin.write(`${JSON.stringify({ operation, arguments: args })}\n`);
    });
  }

  close(): void {
    this.process.stdin.end();
  }
}

let python: PythonBridge;

beforeAll(() => {
  python = new PythonBridge();
});

afterAll(() => {
  python.close();
});

const text = gs.text({ maxSize: 200 });
const pathSegment = gs
  .text({ minSize: 1, maxSize: 40 })
  .map((value) => encodeURIComponent(value));
const hostLabel = gs.text({
  minSize: 1,
  maxSize: 20,
  alphabet: "abcdefghijklmnopqrstuvwxyz0123456789",
});

test("xml_id always returns a nonempty XML-safe identifier", () =>
  hegel.testAsync(async (tc) => {
    const input = tc.draw(text);
    const result = await python.call<string>("xml_id", input);
    expect(result).toMatch(/^[A-Za-z_][A-Za-z0-9_.-]*$/);
  }, propertySettings));

test("route_name always returns a portable nonempty filename component", () =>
  hegel.testAsync(async (tc) => {
    const route = tc.draw(text);
    const result = await python.call<string>("route_name", route);
    expect(result).toMatch(/^[A-Za-z0-9._-]+$/);
    expect(result).not.toBe(".");
    expect(result).not.toBe("..");
  }, propertySettings));

test("page_key is the stable 16-character SHA-256 prefix", () =>
  hegel.testAsync(async (tc) => {
    const url = tc.draw(text);
    const result = await python.call<string>("page_key", url);
    const expected = createHash("sha256").update(url).digest("hex").slice(0, 16);
    expect(result).toBe(expected);
  }, propertySettings));

test("canonical_url removes query, fragment, duplicate slashes, and trailing slash", () =>
  hegel.testAsync(async (tc) => {
    const host = `${tc.draw(hostLabel)}.example`;
    const first = tc.draw(pathSegment);
    const second = tc.draw(pathSegment);
    const query = encodeURIComponent(tc.draw(text));
    const fragment = encodeURIComponent(tc.draw(text));
    const input = `https://${host}//${first}///${second}//?q=${query}#${fragment}`;
    const result = await python.call<string>("canonical_url", input);
    expect(result).toBe(`https://${host}/${first}/${second}`);
  }, propertySettings));

test("in_scope accepts the base path and its descendants", () =>
  hegel.testAsync(async (tc) => {
    const host = `${tc.draw(hostLabel)}.example`;
    const basePath = tc.draw(pathSegment);
    const childPath = tc.draw(pathSegment);
    const base = `https://${host}/${basePath}`;
    const result = await python.call<boolean>("in_scope", `${base}/${childPath}`, base);
    expect(result).toBe(true);
  }, propertySettings));

test("in_scope rejects paths that only share the base path prefix", () =>
  hegel.testAsync(async (tc) => {
    const host = `${tc.draw(hostLabel)}.example`;
    const basePath = tc.draw(pathSegment);
    const suffix = tc.draw(pathSegment);
    const base = `https://${host}/${basePath}`;
    const result = await python.call<boolean>("in_scope", `${base}${suffix}`, base);
    expect(result).toBe(false);
  }, propertySettings));

test("chapter_name is a portable XHTML filename", () =>
  hegel.testAsync(async (tc) => {
    const route = tc.draw(text);
    const url = tc.draw(gs.urls());
    const result = await python.call<string>("chapter_name", route, url);
    expect(result).toMatch(/^[A-Za-z0-9._-]+\.xhtml$/);
  }, propertySettings));

test("chapter_name includes the stable URL digest", () =>
  hegel.testAsync(async (tc) => {
    const route = tc.draw(text);
    const url = tc.draw(gs.urls());
    const result = await python.call<string>("chapter_name", route, url);
    const digest = createHash("sha256").update(url).digest("hex").slice(0, 8);
    expect(result.endsWith(`-${digest}.xhtml`)).toBe(true);
  }, propertySettings));

test("default_workspace remains inside the docs2epub workspace root", () =>
  hegel.testAsync(async (tc) => {
    const host = `${tc.draw(hostLabel)}.example`;
    const first = tc.draw(pathSegment);
    const second = tc.draw(pathSegment);
    const result = await python.call<string>(
      "default_workspace",
      `https://${host}/${first}/${second}`,
    );
    expect(result).toBe(`.docs2epub/${host}-${first}-${second}`);
  }, propertySettings));
