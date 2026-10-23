#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""生成 Anki 宿主测试夹具。/ Generate Anki host-test fixtures.

在仓库根目录运行：python3 tools/gen_anki_fixtures.py
产物落在被忽略的 build/anki-fixtures/。/ Artifacts land in the ignored
build/anki-fixtures/ directory. 运行目录：仓库根。/ Run from the repo root.
"""
import json
import os
import sqlite3
import struct
import zipfile

OUT = os.path.join("build", "anki-fixtures")

V2_NOTES = """
CREATE TABLE notes (
    id integer primary key, guid text not null, mid integer not null,
    mod integer not null, usn integer not null, tags text not null,
    flds text not null, sfld text not null, csum integer not null,
    flags integer not null, data text not null);
"""
V2_CARDS = """
CREATE TABLE cards (
    id integer primary key, nid integer not null, did integer not null,
    ord integer not null, mod integer not null, usn integer not null,
    type integer not null, queue integer not null, due integer not null,
    ivl integer not null, factor integer not null, reps integer not null,
    lapses integer not null, left integer not null, odue integer not null,
    odid integer not null, flags integer not null, data text not null);
"""
V2_COL = """
CREATE TABLE col (
    id integer primary key, crt integer not null, mod integer not null,
    scm integer not null, ver integer not null, dty integer not null,
    usn integer not null, ls integer not null, conf text not null,
    models text not null, decks text not null, dconf text not null,
    tags text not null);
"""
V2_REVLOG = """
CREATE TABLE revlog (
    id integer primary key, cid integer not null, usn integer not null,
    ease integer not null, ivl integer not null, lastIvl integer not null,
    factor integer not null, time integer not null, type integer not null);
"""
V3_NOTES = V2_NOTES.replace(
    "data text not null);",
    "data text not null);", 1)
V3_CARDS = V2_CARDS
V3_TABLES = """
CREATE TABLE notetypes (
    id integer primary key, name text not null, mtime_secs integer not null,
    usn integer not null, config blob not null);
CREATE TABLE templates (
    notetype_id integer not null, ord integer not null, name text not null,
    mtime_secs integer not null, usn integer not null, config blob not null,
    primary key (notetype_id, ord));
CREATE TABLE fields (
    notetype_id integer not null, ord integer not null, name text not null,
    mtime_secs integer not null, usn integer not null, config blob not null,
    primary key (notetype_id, ord));
CREATE TABLE decks (
    id integer primary key, name text not null, mtime_secs integer not null,
    usn integer not null, common blob not null, kind integer not null);
"""


def basic_model(mid=1700000000000, name="基础"):
    return {
        str(mid): {
            "id": mid,
            "name": name,
            "type": 0,
            "flds": [
                {"name": "正面", "ord": 0, "font": "Arial", "size": 20},
                {"name": "背面", "ord": 1, "font": "Arial", "size": 20},
                {"name": "备注", "ord": 2, "font": "Arial", "size": 20},
            ],
            "tmpls": [
                {
                    "name": "卡片 1",
                    "ord": 0,
                    "qfmt": "<div class=question>{{正面}}</div>",
                    "afmt": "<div class=answer>{{FrontSide}}<hr id=answer>{{背面}}</div>",
                }
            ],
        }
    }


def write_v2(path, models, decks, notes, cards, user_version=18, crt=1704067200):
    if os.path.exists(path):
        os.unlink(path)
    db = sqlite3.connect(path)
    db.executescript(V2_COL + V2_NOTES + V2_CARDS + V2_REVLOG)
    db.execute("PRAGMA user_version=%d" % user_version)
    db.execute(
        "insert into col values (?,?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            1,
            crt,
            1704067200,
            1704067200,
            11,
            0,
            -1,
            0,
            "{}",
            json.dumps(models, ensure_ascii=False),
            json.dumps(decks, ensure_ascii=False),
            "{}",
            "{}",
        ),
    )
    db.executemany("insert into notes values (?,?,?,?,?,?,?,?,?,?,?)", notes)
    db.executemany("insert into cards values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", cards)
    db.commit()
    db.close()


def write_v3(path, notetypes, fields, templates, decks, notes, cards):
    if os.path.exists(path):
        os.unlink(path)
    db = sqlite3.connect(path)
    db.executescript(V3_TABLES + V3_NOTES + V3_CARDS)
    db.execute("PRAGMA user_version=18")
    db.executemany("insert into notetypes values (?,?,?,?,?)", notetypes)
    db.executemany("insert into fields values (?,?,?,?,?,?)", fields)
    db.executemany("insert into templates values (?,?,?,?,?,?)", templates)
    db.executemany("insert into decks values (?,?,?,?,?,?)", decks)
    db.executemany("insert into notes values (?,?,?,?,?,?,?,?,?,?,?)", notes)
    db.executemany("insert into cards values (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", cards)
    db.commit()
    db.close()


def pack_apkg(apkg_path, db_path, media=None):
    media = media or {}
    with zipfile.ZipFile(apkg_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(db_path, "collection.anki2")
        z.writestr("media", json.dumps(media))


def pack_anki21(apkg_path, db_path):
    with zipfile.ZipFile(apkg_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(db_path, "collection.anki21")


def pack_dual_apkg(apkg_path, anki21_db_path, anki2_db_path, media=None):
    """同时写入 collection.anki21 与 collection.anki2 两个条目，模拟现代
    Anki 导出。/ Pack both collection.anki21 and collection.anki2 entries
    into one ZIP, mimicking a modern Anki export."""
    media = media or {}
    with zipfile.ZipFile(apkg_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(anki21_db_path, "collection.anki21")
        z.write(anki2_db_path, "collection.anki2")
        z.writestr("media", json.dumps(media))


def note(nid, mid, flds, sfld=None):
    return (nid, "guid%d" % nid, mid, 1704067200, -1, " ", flds, sfld or flds.split("\x1f")[0], 12345, 0, "")


def card(cid, nid, did, ord_=0, type_=0, queue=0, due=1, ivl=0, factor=2500, reps=0, lapses=0):
    return (
        cid, nid, did, ord_, 1704067200, -1, type_, queue, due, ivl, factor, reps,
        lapses, 0, 0, 0, 0, "",
    )


def gen_basic_v2():
    """单卡组基础卡：新卡/学习卡/复习卡混合。/ One basic deck mixing new, learning and review cards."""
    models = basic_model()
    decks = {
        "1": {"id": 1, "name": "Default", "conf": 1},
        "1700000000001": {"id": 1700000000001, "name": "英语单词", "conf": 1},
    }
    notes = [
        note(1700000000001, 1700000000000, "apple\x1f<nbsp>;苹果</nbsp>\x1f"),
        note(1700000000002, 1700000000000, "banana\x1f香蕉\x1f"),
        note(1700000000003, 1700000000000, "cherry\x1f樱桃；<b>红色</b>水果\x1f"),
    ]
    cards = [
        card(1700000000001, 1700000000001, 1700000000001),                       # 新卡
        card(1700000000002, 1700000000002, 1700000000001, type_=1, queue=1, due=1704067200),  # 学习卡
        card(1700000000003, 1700000000003, 1700000000001, type_=2, queue=2, due=20000, ivl=10, reps=3),  # 复习卡
    ]
    db = os.path.join(OUT, "basic.anki2")
    write_v2(db, models, decks, notes, cards)
    pack_apkg(os.path.join(OUT, "basic-v2.apkg"), db)


def gen_v1_scheduler():
    """v1 调度器（schema 11）：复习 due 相对 crt。/ v1 scheduler: review due is crt-relative."""
    models = basic_model()
    decks = {"1700000000002": {"id": 1700000000002, "name": "旧版卡组", "conf": 1}}
    notes = [note(1700000000010, 1700000000000, "old\x1f旧卡\x1f")]
    cards = [card(1700000000010, 1700000000010, 1700000000002, type_=2, queue=2, due=30, ivl=30)]
    db = os.path.join(OUT, "v1sched.anki2")
    write_v2(db, models, decks, notes, cards, user_version=11, crt=1704067200)
    pack_apkg(os.path.join(OUT, "v1sched-v2.apkg"), db)


def gen_conditional():
    """条件段、FrontSide、实体与图片占位。/ Conditional sections, FrontSide, entities and image placeholders."""
    models = {
        "1700000000100": {
            "id": 1700000000100,
            "name": "带条件",
            "flds": [
                {"name": "问题", "ord": 0},
                {"name": "答案", "ord": 1},
                {"name": "例句", "ord": 2},
            ],
            "tmpls": [
                {
                    "name": "卡片 1",
                    "ord": 0,
                    "qfmt": "<div>{{问题}}{{#例句}}<br>{{例句}}{{/例句}}</div>",
                    "afmt": "<div>{{FrontSide}}<hr>{{答案}}</div>",
                }
            ],
        }
    }
    decks = {"1700000000101": {"id": 1700000000101, "name": "条件卡组", "conf": 1}}
    notes = [
        note(1700000000101, 1700000000100, "有问题 &amp; 有例句\x1f答案一\x1fThis is <b>an</b> example."),
        note(1700000000102, 1700000000100, "只有问题\x1f答案二\x1f"),
    ]
    cards = [
        card(1700000000101, 1700000000101, 1700000000101),
        card(1700000000102, 1700000000102, 1700000000101),
    ]
    db = os.path.join(OUT, "conditional.anki2")
    write_v2(db, models, decks, notes, cards)
    pack_apkg(os.path.join(OUT, "conditional-v2.apkg"), db)


def gen_cloze():
    """完形填空卡。/ Cloze cards."""
    models = {
        "1700000000200": {
            "id": 1700000000200,
            "name": "填空",
            "flds": [{"name": "文本", "ord": 0}, {"name": "背面额外", "ord": 1}],
            "tmpls": [
                {
                    "name": "克窿 1",
                    "ord": 0,
                    "qfmt": "<div>{{cloze:文本}}</div>",
                    "afmt": "<div>{{cloze:文本}}<hr>{{背面额外}}</div>",
                }
            ],
        }
    }
    decks = {"1700000000201": {"id": 1700000000201, "name": "完形卡组", "conf": 1}}
    text = "巴黎是{{c1::法国}}的首都，也是{{c2::欧洲}}的大城市。"
    notes = [note(1700000000201, 1700000000200, text + "\x1f补充说明")]
    cards = [
        card(1700000000201, 1700000000201, 1700000000201, ord_=0),  # c1 激活
        card(1700000000202, 1700000000201, 1700000000201, ord_=1),  # c2 激活
    ]
    db = os.path.join(OUT, "cloze.anki2")
    write_v2(db, models, decks, notes, cards)
    pack_apkg(os.path.join(OUT, "cloze-v2.apkg"), db)


def gen_v3():
    """anki21 v3 schema：两个普通卡组 + 一个过滤卡组 + 两个笔记类型。
    / anki21 v3 schema: two normal decks plus one filtered deck and two notetypes."""
    notetypes = [
        (1700000000300, "基础", 1704067200, -1, b"{}"),
        (1700000000900, "进阶", 1704067200, -1, b"{}"),
    ]
    fields = [
        (1700000000300, 0, "正面", 1704067200, -1, b"{}"),
        (1700000000300, 1, "背面", 1704067200, -1, b"{}"),
        (1700000000900, 0, "例句", 1704067200, -1, b"{}"),
    ]
    templates = [
        (
            1700000000300,
            0,
            "卡片 1",
            1704067200,
            -1,
            # Anki 存 JSON 文本（列亲和性为 blob，动态类型按文本存）。
            # Anki stores JSON text (blob affinity, dynamic typing keeps text).
            json.dumps(
                {
                    "qfmt": "<div>{{正面}}</div>",
                    "afmt": "<div>{{FrontSide}}<hr id=answer>{{背面}}</div>",
                }
            ),
        ),
        (
            1700000000900,
            0,
            "卡片 1",
            1704067200,
            -1,
            json.dumps({"qfmt": "<div>{{例句}}</div>", "afmt": "<div>{{例句}}</div>"}),
        ),
    ]
    decks = [
        (1700000000301, "中文卡组", 1704067200, -1, b"{}", 0),
        (1700000000302, "英文卡组", 1704067200, -1, b"{}", 0),
        (1700000000303, "过滤卡组", 1704067200, -1, b"{}", 1),  # kind=1 动态卡组应被跳过
    ]
    notes = [
        note(1700000000301, 1700000000300, "你好\x1fhello"),
        note(1700000000302, 1700000000300, "世界\x1fworld"),
        note(1700000000303, 1700000000300, "跳过\x1fskipped"),
        note(1700000000901, 1700000000900, "The paper is white\x1f纸是白的"),
    ]
    cards = [
        card(1700000000301, 1700000000301, 1700000000301),
        card(1700000000302, 1700000000302, 1700000000302),
        card(1700000000303, 1700000000303, 1700000000303),  # 过滤卡组卡片
        card(1700000000901, 1700000000901, 1700000000302),
    ]
    db = os.path.join(OUT, "v3.anki21")
    write_v3(db, notetypes, fields, templates, decks, notes, cards)
    pack_anki21(os.path.join(OUT, "multi-v3.apkg"), db)


def gen_big():
    """大集合：长模板触发溢出页，多笔记触发多页 b-树。/ A big collection: long templates force overflow pages and many notes span leaf pages."""
    long_fmt = "<div>{{正面}}</div>" + ("<span style='color:red'>padding</span>" * 400)
    models = {
        "1700000000400": {
            "id": 1700000000400,
            "name": "大卡组",
            "flds": [{"name": "正面", "ord": 0}, {"name": "背面", "ord": 1}],
            "tmpls": [{"name": "卡片 1", "ord": 0, "qfmt": long_fmt, "afmt": long_fmt}],
        }
    }
    decks = {"1700000000401": {"id": 1700000000401, "name": "大卡组", "conf": 1}}
    notes = []
    cards = []
    for i in range(120):
        nid = 1700000001000 + i
        notes.append(note(nid, 1700000000400, "词%d\x1f义%d" % (i, i)))
        cards.append(card(1700000001000 + i, nid, 1700000000401, type_=2, queue=2, due=20000 + i, ivl=5))
    db = os.path.join(OUT, "big.anki2")
    write_v2(db, models, decks, notes, cards)
    pack_apkg(os.path.join(OUT, "big-v2.apkg"), db)


def gen_bare():
    """裸 SQLite 集合（无 ZIP）。/ A bare SQLite collection (no ZIP)."""
    models = basic_model(mid=1700000000500, name="裸库")
    decks = {"1700000000501": {"id": 1700000000501, "name": "裸库卡组", "conf": 1}}
    notes = [note(1700000000501, 1700000000500, "bare\x1f裸库\x1f")]
    cards = [card(1700000000501, 1700000000501, 1700000000501)]
    db = os.path.join(OUT, "bare.anki2")
    write_v2(db, models, decks, notes, cards)
    # 裸库直接可用，无需打包。/ The bare database is used as-is.


def gen_corrupt():
    """损坏与边缘文件。/ Corrupt and edge-case files."""
    with open(os.path.join(OUT, "garbage.apkg"), "wb") as f:
        f.write(b"this is not a zip and not a sqlite database at all")
    # WAL 模式数据库：应被拒绝。/ A WAL-mode database must be rejected.
    db = os.path.join(OUT, "wal.anki2")
    if os.path.exists(db):
        os.unlink(db)
    conn = sqlite3.connect(db)
    conn.executescript(V2_COL + V2_NOTES + V2_CARDS)
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute(
        "insert into col values (1,?,?,?,?,?,?,?,?,?,?,?,?)",
        (
            1704067200, 1704067200, 1704067200, 11, 0, -1, 0, "{}",
            json.dumps(basic_model(), ensure_ascii=False),
            json.dumps({"1": {"id": 1, "name": "Default"}}, ensure_ascii=False),
            "{}", "{}",
        ),
    )
    conn.commit()
    conn.close()
    with zipfile.ZipFile(os.path.join(OUT, "wal-v2.apkg"), "w", zipfile.ZIP_DEFLATED) as z:
        z.write(db, "collection.anki2")
    # 没有集合库的 ZIP。/ A ZIP without a collection.
    with zipfile.ZipFile(os.path.join(OUT, "nocollection.apkg"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("media", "{}")


def gen_dual_v2():
    """两个笔记类型：暴露「只在扩容时清零」的槽位垃圾（真机 PSRAM 崩溃回归）。
    / Two notetypes: exposes slots zeroed only at growth time (device PSRAM
    crash regression)."""
    models = basic_model()
    models.update(basic_model(mid=1700000000099, name="词组"))
    decks = {"1700000000001": {"id": 1700000000001, "name": "双模板", "conf": 1}}
    notes = [
        note(1700000000021, 1700000000000, "sunny\x1f晴朗的\x1f"),
        note(1700000000022, 1700000000099, "take off\x1f起飞\x1f"),
    ]
    cards = [
        card(1700000000021, 1700000000021, 1700000000001),
        card(1700000000022, 1700000000022, 1700000000001),
    ]
    db = os.path.join(OUT, "dual.anki2")
    write_v2(db, models, decks, notes, cards)
    pack_apkg(os.path.join(OUT, "dual-v2.apkg"), db)


def gen_modern_dual():
    """现代 Anki（23.10+）导出的双条目 apkg：anki21 是真库，anki2 是给旧版
    Anki 的占位存根（只有 Default 卡组和一张"请升级 Anki"提示卡）。
    回归真机 Bug：导入器误选 anki2 存根导致真卡组丢失。
    / A modern Anki (23.10+) export with both entries: anki21 is the real
    collection while anki2 is a placeholder stub for old Anki (just the
    Default deck plus a "please update Anki" prompt card). Regresses the
    device bug where the importer picked the stub and lost the real deck."""
    models = basic_model(mid=1700000000600, name="无线电问答")
    # 真实导出在 Default 为空时会省略它，decks 里只有目标卡组。
    # Real exports omit an empty Default; decks holds only the target deck.
    decks = {"1700000000601": {"id": 1700000000601, "name": "无线电考试", "conf": 1}}
    notes = [
        note(1700000000601, 1700000000600, "CQ 的含义\x1f通用呼叫，寻求任意台站应答\x1f"),
        note(1700000000602, 1700000000600, "呼号前缀 BY\x1f中国\x1f"),
    ]
    cards = [
        card(1700000000601, 1700000000601, 1700000000601),
        card(1700000000602, 1700000000602, 1700000000601),
    ]
    real_db = os.path.join(OUT, "modern-real.anki21")
    write_v2(real_db, models, decks, notes, cards)

    stub_models = basic_model(mid=1700000000699, name="Basic")
    stub_decks = {"1": {"id": 1, "name": "Default", "conf": 1}}
    stub_notes = [
        note(
            1700000000699,
            1700000000699,
            "Please update to the latest Anki version, "
            "then import the .colpkg/.apkg file again.\x1f\x1f",
        )
    ]
    stub_cards = [card(1700000000699, 1700000000699, 1)]
    stub_db = os.path.join(OUT, "modern-stub.anki2")
    write_v2(stub_db, stub_models, stub_decks, stub_notes, stub_cards)

    pack_dual_apkg(os.path.join(OUT, "modern-dual.apkg"), real_db, stub_db)


def gen_choice_v2():
    """选择题卡组：复刻真实「业余无线电新版」schema（字段 I/W/J/P/Q/A/B/C/D/T/N，
    正面模板带 <li id="a">{{A}}</li>，背面模板靠一段 JavaScript 高亮正确选项——
    设备上不执行脚本，由渲染层按 T 值拼装背面并强调正确选项）。
    / Choice deck replicating the real amateur-radio schema (fields
    I/W/J/P/Q/A/B/C/D/T/N, the front template carries <li id="a">{{A}}</li> and
    the back template highlights correct options with JavaScript — never
    executed on device; the renderer assembles the back from T instead)."""
    mid = 1700000000700
    qfmt = (
        "<small>{{I}} / {{J}} / {{P}}</small>\n"
        '<p class="question"><span class="type">{{W}}</span>{{Q}}</p>\n'
        '\n<ul id="options">\n'
        '<li id="a">{{A}}</li>\n'
        '<li id="b">{{B}}</li>\n'
        '<li id="c">{{C}}</li>\n'
        '<li id="d">{{D}}</li>\n'
        '<li class="hidden">placeholder</li>\n'
        "</ul>\n"
    )
    afmt = (
        "{{FrontSide}}\n"
        "\n<script>\n"
        "(() => {\n"
        '    const keyStr = "{{T}}".toLowerCase();\n'
        '    const options = document.querySelectorAll("#options li:not(.hidden)");\n'
        "\n"
        "    options.forEach(item => {\n"
        '        item.classList.add("dim");\n'
        "        if (keyStr.includes(item.id)) {\n"
        '            item.classList.remove("dim");\n'
        "        }\n"
        "    });\n"
        "})();\n"
        "</script>\n"
    )
    models = {
        str(mid): {
            "id": mid,
            "name": "业余无线电新版",
            "type": 0,
            "flds": [
                {"name": "I", "ord": 0, "font": "Arial", "size": 20},
                {"name": "W", "ord": 1, "font": "Arial", "size": 20},
                {"name": "J", "ord": 2, "font": "Arial", "size": 20},
                {"name": "P", "ord": 3, "font": "Arial", "size": 20},
                {"name": "Q", "ord": 4, "font": "Arial", "size": 20},
                {"name": "A", "ord": 5, "font": "Arial", "size": 20},
                {"name": "B", "ord": 6, "font": "Arial", "size": 20},
                {"name": "C", "ord": 7, "font": "Arial", "size": 20},
                {"name": "D", "ord": 8, "font": "Arial", "size": 20},
                {"name": "T", "ord": 9, "font": "Arial", "size": 20},
                {"name": "N", "ord": 10, "font": "Arial", "size": 20},
            ],
            "tmpls": [
                {
                    "name": "卡片 1",
                    "ord": 0,
                    "qfmt": qfmt,
                    "afmt": afmt,
                }
            ],
        }
    }
    decks = {"1700000000701": {"id": 1700000000701, "name": "无线电考试", "conf": 1}}
    notes = [
        # 单选：T="A"，注释为空。/ Single choice: T="A", empty note.
        note(
            1700000000701,
            mid,
            "MC1-0001\x1f单选\x1fLY0001\x1f1.1.1\x1f"
            "我国依法负责对业余无线电台实施监督管理的机构是：\x1f"
            "国家无线电管理机构和省、自治区、直辖市无线电管理机构\x1f"
            "在国家或地方民政部门注册的业余无线电协会\x1f"
            "国家体育管理机构和地方体育管理机构\x1f"
            "国家和地方公安部门\x1f"
            "A\x1f",
        ),
        # 多选：T="AC"，带注释。/ Multiple choice: T="AC", with a note.
        note(
            1700000000702,
            mid,
            "MC2-0002\x1f多选\x1fLY0002\x1f1.1.1\x1f"
            "我国专门针对无线电管理的行政法规及其制定机构是：\x1f"
            "《中华人民共和国无线电管理条例》\x1f"
            "《中华人民共和国无线电管理办法》\x1f"
            "国务院和中央军委\x1f"
            "工业和信息化部\x1f"
            "AC\x1f参见《条例》第一章",
        ),
    ]
    cards = [
        card(1700000000701, 1700000000701, 1700000000701),
        card(1700000000702, 1700000000702, 1700000000701),
    ]
    db = os.path.join(OUT, "choice.anki2")
    write_v2(db, models, decks, notes, cards)
    pack_apkg(os.path.join(OUT, "choice-v2.apkg"), db)


def main():
    os.makedirs(OUT, exist_ok=True)
    gen_basic_v2()
    gen_v1_scheduler()
    gen_conditional()
    gen_cloze()
    gen_choice_v2()
    gen_v3()
    gen_big()
    gen_bare()
    gen_dual_v2()
    gen_modern_dual()
    gen_corrupt()
    print("anki fixtures written to", OUT)


if __name__ == "__main__":
    main()
