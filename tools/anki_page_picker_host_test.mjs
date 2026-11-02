/* SPDX-License-Identifier: Apache-2.0
 * 中文：/anki/ 页面「选择版本」的宿主测试。直接抽取 flash/anki/index.html 里的模块脚本，
 *      在最小的 DOM/fetch 替身上跑：下拉框由 releases.json 填充、默认选中最新版、切换后
 *      安装按钮的 manifest 与页面显示的版本/构建时间同步更新，而且校验钩子跟着当前选择走
 *      （不是写死 ./manifest.json）。
 * English: Host test for the /anki/ version picker. It lifts the module script out of
 *      flash/anki/index.html and runs it against a minimal DOM/fetch stand-in: the picker is
 *      filled from releases.json, defaults to the newest build, and switching a version moves
 *      the install button's manifest plus the version/build-time readout — and the verify
 *      hook follows the current choice instead of the hard-coded ./manifest.json.
 *
 * 用法 / Usage: node tools/anki_page_picker_host_test.mjs
 */
import assert from 'node:assert/strict';
import { mkdtempSync, readFileSync, writeFileSync, copyFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const page = readFileSync(join(root, 'flash/anki/index.html'), 'utf8');
const modules = [...page.matchAll(/<script type="module">([\s\S]*?)<\/script>/g)];
assert.equal(modules.length, 1, 'the page must carry exactly one module script');
const source = modules[0][1];

const versions = ['0.3.3-rc104.02', '0.3.3-rc104.01', '0.3.3-rc103.01'];
const releases = versions.map((version, index) => ({
  version,
  manifest: `releases/${version}/manifest.json`,
  label: index === 0 ? '当前版本' : '可回退版本',
}));
const manifests = new Map(releases.map((release, index) => [release.manifest, {
  name: 'kiikoread · Anki',
  version: release.version,
  anki_fork: { fork: 'kiikoread-anki', app_version: release.version, build_date: 'Nov  2 2026', build_time: `1${index}:00:00` },
  new_install_prompt_erase: true,
  builds: [{
    chipFamily: 'ESP32-S3',
    parts: [
      { path: 'bootloader.bin', offset: 0x0, size: 21232, sha256: 'a'.repeat(64) },
      { path: 'partitions.bin', offset: 0x8000, size: 3072, sha256: 'b'.repeat(64) },
      { path: 'firmware.bin', offset: 0x10000, size: 3632576, sha256: 'c'.repeat(64) },
      { path: 'ota_data_initial.bin', offset: 0xd10000, size: 8192, sha256: 'd'.repeat(64) },
    ],
  }],
}]));

class FakeElement {
  constructor(tag) {
    this.tag = tag;
    this.attributes = {};
    this.value = '';
    this.textContent = '';
    this.children = [];
    this.listeners = {};
  }
  setAttribute(name, value) { this.attributes[name] = value; }
  getAttribute(name) { return Object.prototype.hasOwnProperty.call(this.attributes, name) ? this.attributes[name] : null; }
  append(child) { this.children.push(child); }
  addEventListener(name, handler) { this.listeners[name] = handler; }
  dispatch(name) { return this.listeners[name] ? this.listeners[name]() : undefined; }
}

const elements = {
  'meta-version': new FakeElement('dd'),
  'meta-built': new FakeElement('dd'),
  'support-status': new FakeElement('p'),
  version: new FakeElement('select'),
};
const button = new FakeElement('esp-web-install-button');
const fetched = [];

globalThis.document = {
  getElementById: (id) => elements[id] ?? null,
  querySelector: (selector) => (selector === 'esp-web-install-button' ? button : null),
  createElement: (tag) => new FakeElement(tag),
  baseURI: 'https://cyankirin.github.io/kiikoreader-anki/anki/index.html',
};
globalThis.window = { isSecureContext: true };
// 中文：Node 22 的 navigator 只有 getter，用 defineProperty 覆盖成最小替身。
// English: Node 22 exposes navigator as a getter only; defineProperty installs the stand-in.
Object.defineProperty(globalThis, 'navigator', { value: { serial: {} }, configurable: true });
globalThis.fetch = async (url, options = {}) => {
  const href = String(url instanceof URL ? url.href : url);
  // 中文：页面既用相对路径（'./releases.json'）也用绝对清单地址，统一还原成 /anki/ 下的路径。
  // English: The page uses both a relative './releases.json' and absolute manifest URLs;
  //      normalise both back to a path under /anki/.
  const marker = '/anki/';
  const at = href.lastIndexOf(marker);
  const raw = at >= 0 ? href.slice(at + marker.length) : href.replace(/^\.\//, '');
  // 中文：部件地址带着期望的 sha256 查询串，比对路径时去掉它。
  // English: Part URLs carry the expected sha256 as a query string; drop it for path checks.
  const path = raw.split('?')[0];
  fetched.push({ href, path, cache: options.cache });
  if (path === 'releases.json') {
    return { ok: true, status: 200, json: async () => ({ schema: 1, releases }) };
  }
  if (manifests.has(path)) {
    return { ok: true, status: 200, json: async () => manifests.get(path) };
  }
  return { ok: false, status: 404, json: async () => ({}) };
};

const folder = mkdtempSync(join(tmpdir(), 'anki-page-'));
copyFileSync(join(root, 'flash/anki/integrity.js'), join(folder, 'integrity.js'));
writeFileSync(join(folder, 'page.mjs'), source, 'utf8');

const wait = async (predicate, label) => {
  for (let attempt = 0; attempt < 200; ++attempt) {
    if (predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  throw new Error(`timed out waiting for ${label}`);
};

try {
  await import(pathToFileURL(join(folder, 'page.mjs')).href);
  assert.ok(button.prepareManifest, 'the page must install the verify hook');
  await wait(() => elements.version.children.length === versions.length, 'the version picker');
  await wait(() => elements['meta-version'].textContent === versions[0], 'the default manifest metadata');

  const options = elements.version.children;
  assert.deepEqual(options.map((option) => option.value), releases.map((release) => release.manifest));
  assert.deepEqual(options.map((option) => option.textContent),
    releases.map((release) => `${release.version} · ${release.label}`));
  assert.equal(elements.version.value, releases[0].manifest, 'the newest build must be selected first');
  assert.equal(button.getAttribute('manifest'), releases[0].manifest, 'the button must follow the picker');
  assert.equal(elements['meta-built'].textContent, 'Nov  2 2026 10:00:00');

  // 切到可回退版本：按钮、版本号与构建时间一起改，校验钩子也换成那一份清单。
  // Switch to a rollback build: button, version and build time follow, and so does the verifier.
  elements.version.value = releases[1].manifest;
  elements.version.dispatch('change');
  await wait(() => elements['meta-version'].textContent === versions[1], 'the rollback metadata');
  assert.equal(button.getAttribute('manifest'), releases[1].manifest);
  assert.equal(elements['meta-built'].textContent, 'Nov  2 2026 11:00:00');

  fetched.length = 0;
  await assert.rejects(() => button.prepareManifest(), /已停止安装|下载失败/, 'missing parts must abort');
  assert.equal(fetched[0].path, releases[1].manifest,
    'the verify hook must read the selected manifest, not the hard-coded one');
  assert.ok(fetched.every((entry) => entry.cache === 'no-store'), 'every fetch bypasses the cache');

  // 摘要与部件地址都来自当前选中版本，/anki/ 下四个部件 404 就会中止。
  // The digest and part URLs come from the selected build; 404 parts must abort.
  const partBase = releases[1].manifest.replace(/manifest\.json$/, '');
  assert.ok(fetched.some((entry) => entry.path === `${partBase}firmware.bin`),
    `the verifier must fetch the parts of the selected build, saw ${fetched.map((entry) => entry.path).join(', ')}`);
  puts('PASS: version picker fills from releases.json, defaults to newest, follows the selection for');
  puts('      both the install button and the verify hook, and never caches manifests or parts');
} finally {
  rmSync(folder, { recursive: true, force: true });
}

function puts(text) { process.stdout.write(text + '\n'); }
