/* SPDX-License-Identifier: Apache-2.0
 * 中文：/anki/ 页面的浏览器端校验。把四个部件各下载**一次**，逐个核对 size 与 SHA-256，
 *      通过后把「已校验的字节」换成 blob: 地址交回安装器，因此安装器不会再下载一遍。
 *      任何一项不一致就抛错：安装器在 `port.open()` 之前就中止，不打开串口、不擦除、不写入。
 * English: Browser-side verifier for the /anki/ page. Each of the four parts is downloaded
 *      exactly once, its size and SHA-256 are checked, and the already-verified bytes are
 *      handed back to the installer as blob: URLs, so nothing is fetched twice. Any mismatch
 *      throws: the installer aborts before `port.open()`, so the port is never opened,
 *      nothing is erased and nothing is written.
 */

// 中文：偏移与容量来自 tools/verify_flash_bundle.py 的 EXPECTED（同一套分区布局）。
// English: Offsets and capacity mirror EXPECTED in tools/verify_flash_bundle.py.
export const PARTS = Object.freeze([
  Object.freeze({ name: 'bootloader.bin', offset: 0x0, limit: 0x8000 }),
  Object.freeze({ name: 'partitions.bin', offset: 0x8000, limit: 0x8000 }),
  Object.freeze({ name: 'firmware.bin', offset: 0x10000, limit: 0x400000 }),
  Object.freeze({ name: 'ota_data_initial.bin', offset: 0xd10000, limit: 0x2000 }),
]);

export const CHIP_FAMILY = 'ESP32-S3';
export const CHIP_ID = 9;                 // ESP32-S3 image header chip id
export const PROJECT = 'Read_Pico';
export const FORK_MARKER = 'kiikoread-anki';
export const APP_DESC_MAGIC = 0xABCD5432; // esp_app_desc_t magic_word
export const APP_DESC_MIN = 176;          // 4 + 4 + 8 + 32 + 32 + 16 + 16 + 32 + 32 bytes

const VERSION_PATTERN = /^\d+\.\d+\.\d+(?:-rc\d+(?:\.\d+)?)?$/;
const SHA256_PATTERN = /^[0-9a-f]{64}$/;

/** Throw a plain Error; the installer shows `error.message` and aborts the install. */
function fail(message) {
  throw new Error(message);
}

/** SHA-256 as lowercase hex, through WebCrypto (overridable for tests). */
export async function sha256Hex(bytes, options = {}) {
  const subtle = options.subtle || globalThis.crypto?.subtle;
  if (!subtle) fail('当前浏览器不提供 WebCrypto，无法校验刷机文件，已停止安装。');
  const buffer = await subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(buffer)].map((value) => value.toString(16).padStart(2, '0')).join('');
}

/** Read the esp_app_desc_t fields the manifest has to agree with. */
export function readAppIdentity(bytes) {
  if (bytes.length < APP_DESC_MIN) fail('应用镜像过小，不是本机的刷机文件。');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes[0] !== 0xe9) fail('应用镜像缺少 ESP 镜像头，不是可烧写的固件。');
  if (view.getUint16(12, true) !== CHIP_ID) fail('应用镜像不是 ESP32-S3 版本，已停止安装。');
  if (view.getUint32(32, true) !== APP_DESC_MAGIC) fail('应用镜像缺少版本描述，已停止安装。');
  const field = (offset, size) => {
    const raw = bytes.slice(offset, offset + size);
    const end = raw.indexOf(0);
    const text = new TextDecoder().decode(end < 0 ? raw : raw.slice(0, end));
    return text;
  };
  return { version: field(48, 32), project: field(80, 32) };
}

/** Bootloader images carry the same chip identity as the app image. */
export function checkBootloader(bytes) {
  if (bytes.length < 16 || bytes[0] !== 0xe9) fail('引导程序镜像头不合法，已停止安装。');
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint16(12, true) !== CHIP_ID) fail('引导程序不是 ESP32-S3 版本，已停止安装。');
}

/**
 * Check that a manifest describes this fork's Pico bundle.
 *
 * 中文：页面只接受本 fork 的清单：同一个分区布局、四个部件、**默认保留数据**。
 *      `new_install_prompt_erase` 的极性反直觉：`true` 才会先弹「Erase device?」询问页，
 *      复选框默认不勾选 → 默认保留设置与阅读进度；`false` 是安装器里的「不询问、立即
 *      `_startInstall(true)`」，也就是整片 chip erase，必须拒绝。
 * English: Only this fork's bundle is accepted: same layout, four parts, data kept by
 *      default. The `new_install_prompt_erase` polarity is counter-intuitive: `true` shows
 *      the "Erase device?" step whose checkbox starts unchecked (default = keep settings and
 *      reading progress); `false` makes the installer call `_startInstall(true)` without
 *      asking, which is a full-chip erase and has to be rejected here.
 */
export function validateManifest(manifest) {
  if (!manifest || typeof manifest !== 'object') fail('刷机清单无法解析，已停止安装。');
  if (manifest.anki_fork?.fork !== FORK_MARKER) fail('这不是本 fork 的 Anki 刷机清单，已停止安装。');
  if (!VERSION_PATTERN.test(String(manifest.version))) fail('刷机清单的版本号不合法，已停止安装。');
  if (manifest.new_install_prompt_erase !== true) {
    fail('刷机清单没有开启「是否擦除设备」的询问：安装器会直接整片擦除，设置与阅读进度会丢，已停止安装。');
  }
  const builds = manifest.builds;
  if (!Array.isArray(builds) || builds.length !== 1) fail('刷机清单必须只描述一种设备，已停止安装。');
  const build = builds[0];
  if (!build || typeof build !== 'object') fail('刷机清单缺少构建信息，已停止安装。');
  if (build.chipFamily !== CHIP_FAMILY) fail(`刷机清单目标芯片是 ${build.chipFamily}，不是 ${CHIP_FAMILY}。`);
  const parts = build.parts;
  if (!Array.isArray(parts) || parts.length !== PARTS.length) {
    fail(`刷机清单必须列出 ${PARTS.length} 个部件，已停止安装。`);
  }
  const seen = new Set();
  for (const part of parts) {
    if (!part || typeof part !== 'object') fail('刷机清单有无法识别的部件条目，已停止安装。');
    const spec = PARTS.find((item) => item.offset === part.offset);
    if (!spec) fail(`刷机地址 ${part.offset} 不属于本机的分区布局，已停止安装。`);
    if (seen.has(spec.name)) fail(`刷机清单重复列出 ${spec.name}，已停止安装。`);
    seen.add(spec.name);
    if (part.path !== spec.name) fail(`部件路径 ${part.path} 与偏移 ${spec.name} 不一致，已停止安装。`);
    if (!SHA256_PATTERN.test(String(part.sha256))) fail(`部件 ${spec.name} 没有有效的 SHA-256，已停止安装。`);
    if (!Number.isSafeInteger(part.size) || part.size <= 0 || part.size > spec.limit) {
      fail(`部件 ${spec.name} 声明的大小 ${part.size} 超出该分区的容量，已停止安装。`);
    }
  }
}

/**
 * Download every part once, verify it, and return a manifest that points at the
 * verified bytes (blob: URLs) plus a `dispose()` that releases them.
 *
 * 中文：`baseURL` 是清单地址，部件按它解析；`fetchImpl`/`subtle`/`createObjectURL` 可注入
 *      以便用 Node 直接测纯逻辑（见 tools 里的验证记录）。
 * English: `baseURL` is the manifest URL the parts resolve against. `fetchImpl`, `subtle`
 *      and `createObjectURL` are injectable so the logic can be tested outside a browser.
 */
export async function verifyBundle(manifest, baseURL, options = {}) {
  validateManifest(manifest);
  const fetchImpl = options.fetchImpl || (typeof fetch === 'function' ? fetch.bind(globalThis) : null);
  if (!fetchImpl) fail('当前环境无法下载刷机文件，已停止安装。');
  const createObjectURL = options.createObjectURL || ((blob) => URL.createObjectURL(blob));
  const revokeObjectURL = options.revokeObjectURL || ((url) => URL.revokeObjectURL(url));
  const subtle = options.subtle || globalThis.crypto?.subtle;
  const snapshot = JSON.parse(JSON.stringify(manifest));
  const urls = [];
  let disposed = false;
  const dispose = () => {
    if (disposed) return;
    disposed = true;
    while (urls.length) {
      try {
        revokeObjectURL(urls.pop());
      } catch (error) {
        // 中文：释放失败不影响「已中止」的结论。 / Revoking is best-effort only.
      }
    }
  };
  try {
    const verified = [];
    const downloads = snapshot.builds[0].parts.map(async (part) => {
      const spec = PARTS.find((item) => item.offset === part.offset);
      const target = new URL(part.path, baseURL);
      // 中文：二道防线。`validateManifest()` 已经把 `part.path` 钉成裸文件名、`baseURL`
      //      也来自本页的 http(s) 地址，所以今天这条分支不会被触发；留着是为了万一以后
      //      放宽路径校验时，仍不会去取 `blob:`/`data:`/跨源地址。
      // English: Second line of defence. `validateManifest()` already pins `part.path` to a
      //      bare file name (and `baseURL` comes from this page's http(s) origin), so this
      //      branch cannot fire today; it stays so a future relaxation still cannot fetch
      //      from blob:, data: or a foreign origin.
      if (!['http:', 'https:', 'file:'].includes(target.protocol)) {
        fail(`部件 ${spec.name} 的来源不可用，已停止安装。`);
      }
      // 中文：把期望的哈希带进查询串，避免 CDN/浏览器把旧字节当成新文件。
      // English: Carry the expected hash in the query string so no cache can serve old bytes.
      target.searchParams.set('sha256', part.sha256);
      const response = await fetchImpl(target.href, { cache: 'no-store' });
      if (!response || !response.ok) {
        fail(`部件 ${spec.name} 下载失败（HTTP ${response ? response.status : '未知'}），尚未写入设备。`);
      }
      const data = new Uint8Array(await response.arrayBuffer());
      if (data.length !== part.size) {
        fail(`部件 ${spec.name} 的大小是 ${data.length} 字节，清单声明 ${part.size} 字节，已停止安装。`);
      }
      const digest = await sha256Hex(data, { subtle });
      if (digest !== part.sha256) {
        fail(`部件 ${spec.name} 的 SHA-256 与清单不一致，已停止安装，设备未被写入。`);
      }
      if (spec.name === 'firmware.bin') {
        const identity = readAppIdentity(data);
        if (identity.project !== PROJECT) {
          fail(`应用镜像是 ${identity.project}，不是 ${PROJECT}，已停止安装。`);
        }
        if (identity.version !== manifest.version) {
          fail(`应用镜像内部版本 ${identity.version} 与清单版本 ${manifest.version} 不一致，已停止安装。`);
        }
      }
      if (spec.name === 'bootloader.bin') checkBootloader(data);
      const url = createObjectURL(new Blob([data], { type: 'application/octet-stream' }));
      urls.push(url);
      // 中文：安装器随后从 blob: 取字节——同一份字节，不再走一次网络。
      // English: The installer then reads these blob: URLs, so no byte is fetched twice.
      part.path = url;
      verified.push({
        name: spec.name,
        offset: part.offset,
        size: data.length,
        sha256: digest,
      });
      return spec.name;
    });
    const results = await Promise.allSettled(downloads);
    const failure = results.find((result) => result.status === 'rejected');
    if (failure) throw failure.reason;
    verified.sort((left, right) => left.offset - right.offset);
    const url = createObjectURL(new Blob([JSON.stringify(snapshot)], { type: 'application/json' }));
    urls.push(url);
    return { url, dispose, parts: verified, manifest: snapshot };
  } catch (error) {
    dispose();
    throw error;
  }
}

/**
 * Fetch the manifest and verify the whole bundle. Shape matches the
 * `prepareManifest` hook of `vendor/web/install-button.js`.
 *
 * 中文：返回值 `{url, dispose}` 与安装器约定的钩子一致：`url` 指向已校验的清单快照。
 * English: Returns `{url, dispose}` exactly as the install-button hook expects.
 */
export async function prepareBundle(manifestURL, options = {}) {
  const fetchImpl = options.fetchImpl || (typeof fetch === 'function' ? fetch.bind(globalThis) : null);
  if (!fetchImpl) fail('当前环境无法下载刷机清单，已停止安装。');
  const response = await fetchImpl(manifestURL, { cache: 'no-store' });
  if (!response || !response.ok) {
    fail(`刷机清单下载失败（HTTP ${response ? response.status : '未知'}），已停止安装。`);
  }
  const manifest = await response.json();
  return verifyBundle(manifest, manifestURL, options);
}
