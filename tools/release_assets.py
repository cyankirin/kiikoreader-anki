#!/usr/bin/env python3
"""Fetch immutable official Release assets with bounded sizes and SHA-256 checks.
中文：正式二进制来自 Release；站点镜像逐一验证，不写回源码目录。
English: Release binaries are verified into the site staging directory, never back into source.
"""
from pathlib import Path
from urllib.request import Request, urlopen
import hashlib
import json
import random
import re
import shutil
import time

REPO = 'https://github.com/wegooo-cell/read-pico-reader/releases/download/'

def hydrate(source: Path, output: Path) -> None:
    catalog = source / 'release-assets.json'
    if not catalog.is_file():
        return
    release = json.loads(catalog.read_text())
    assert re.fullmatch(r'v\d+\.\d+\.\d+-rc\d+(?:\.\d+)?', release['tag'])
    assert release['tag'] == 'v' + json.loads((source / 'manifest.json').read_text())['version']
    _hydrate_files(source, output, release)
    # 中文：旧固定网址继续从原 Release 取字节，不能被当前版本别名覆盖。
    # English: Keep immutable older URLs backed by their original Release, not current aliases.
    seen = set(release['files'])
    for historical in release.get('historical', []):
        assert re.fullmatch(r'v\d+\.\d+\.\d+-rc\d+(?:\.\d+)?', historical['tag'])
        expected = 'Pico-update-' + historical['tag'][1:] + '.bin'
        assert set(historical['files']) == {expected} and expected not in seen
        seen.add(expected)
        _hydrate_files(source, output, historical)

def _hydrate_files(source: Path, output: Path, release: dict, maximum: int = 0x400000) -> None:
    fetched = {}
    for name, info in release['files'].items():
        assert re.fullmatch(r'[A-Za-z0-9_.-]+\.bin', name)
        asset = info['asset']
        assert re.fullmatch(r'[A-Za-z0-9_.-]+\.bin', asset)
        assert re.fullmatch(r'[a-f0-9]{64}', info['sha256']) and 0 < info['size'] <= maximum
        target = output / name
        local = source / name
        if local.is_file() and local.stat().st_size == info['size'] and hashlib.sha256(local.read_bytes()).hexdigest() == info['sha256']:
            shutil.copy2(local, target)
            fetched[asset] = target
            continue
        if asset in fetched:
            cached = fetched[asset]
            assert cached.stat().st_size == info['size'] and hashlib.sha256(cached.read_bytes()).hexdigest() == info['sha256'], 'Release alias checksum mismatch'
            shutil.copy2(cached, target)
            continue
        url = REPO + release['tag'] + '/' + asset
        # 中文：GitHub 偶尔对 Release 直链返回 504，部署因此中断过多次；下载失败按指数退避
        #       重试（5/10/20/40 秒 + 抖动），但校验失败属于真实不一致，必须立刻失败。
        # / GitHub intermittently answers Release downloads with 504, which used to abort
        #   the deploy. Retry transport failures with exponential backoff (5/10/20/40 s plus
        #   jitter), but let a checksum/length mismatch fail immediately — that is real.
        data = None
        for attempt in range(5):
            try:
                with urlopen(Request(url, headers={'User-Agent': 'kiikoread-publisher'}), timeout=60) as response:
                    data = response.read(info['size'] + 1)
                break
            except Exception as exc:
                if attempt == 4:
                    raise
                delay = min(40, 5 * (2 ** attempt)) + random.uniform(0, 2)
                print(f'  retry {name} in {delay:.1f}s after {type(exc).__name__}: {exc}', flush=True)
                time.sleep(delay)
        assert data is not None
        assert len(data) == info['size'] and hashlib.sha256(data).hexdigest() == info['sha256'], 'Release asset checksum mismatch'
        target.write_bytes(data)
        fetched[asset] = target
