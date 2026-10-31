"""Phase 0（真实课件版）：PDF/PPTX → 复习资料 PDF

比 phase0.py 多四件真实课件必须的事：
  1. **样板行过滤**：每页都有的页眉/页码不滤掉，map 的一半 token 花在重复内容上；
  2. **缓存**：map 结果按 (文件哈希, 块序号, 版本) 存盘 —— 改 prompt / 改 CSS 不用重跑 145 页；
  3. **分层 reduce**：145 页的要点一次塞不进 8000 token 输出（我们真撞过截断），
     改成按页段分组各出章节，最后单独挑「全篇重点」；
  4. **覆盖校验**：章页范围必须盖住全部页，漏页 = 有内容没进复习资料。

用法：
  python phase0_doc.py <课件.pdf|pptx> <输出目录> [--chunk 5] [--groups 4] [--fresh]
"""
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from phase0 import chat, json_from_text, read_key, render, to_pdf  # noqa: E402

LIB_VERSION = "doc-v2"          # 改 prompt 就改这个，缓存自动失效


# --------------------------------------------------------------------------- #
# 抽取
# --------------------------------------------------------------------------- #
def extract_any(path: Path) -> dict:
    if path.suffix.lower() == ".pptx":
        from phase0 import extract as extract_pptx

        data = extract_pptx(path)
        data["pages"] = [
            {"page": s["page"], "title": s["title"], "bullets": s["bullets"], "notes": s["notes"]}
            for s in data["slides"]
        ]
        return data

    from extract_pdf import extract as extract_pdf

    return extract_pdf(path)


# --------------------------------------------------------------------------- #
# 样板行过滤
# --------------------------------------------------------------------------- #
def strip_boilerplate(pages: list, threshold: float = 0.5) -> tuple[list, list]:
    """把出现在 ≥ threshold 比例页面上的短行当样板行删掉。

    注意：本课件的页眉「计算机安全」只出现在 7/145 页（PDF 抽取得不稳定），
    所以这条过滤器实际没删东西 —— 留着是因为换一份课件就可能生效。
    """
    counts: dict[str, int] = {}
    for page in pages:
        for line in [page["title"], *page["bullets"]]:
            key = line.strip()
            if key:
                counts[key] = counts.get(key, 0) + 1

    total = max(1, len(pages))
    boilerplate = {line for line, n in counts.items() if n / total >= threshold and len(line) <= 40}

    cleaned = []
    for page in pages:
        bullets = [b for b in page["bullets"] if b.strip() not in boilerplate]
        bullets = [b for b in bullets if not b.strip().isdigit()]      # 页码
        title = "" if page["title"].strip() in boilerplate else page["title"]
        cleaned.append(
            {"page": page["page"], "title": title, "bullets": bullets, "notes": page.get("notes", "")}
        )

    return cleaned, sorted(boilerplate)


# --------------------------------------------------------------------------- #
# map（带缓存）
# --------------------------------------------------------------------------- #
MAP_SYSTEM = """你是课程助教，负责把课件片段整理成结构化要点。
只输出 JSON，不要解释。禁止编造：只写课件里确实出现的概念。
JSON 形状：{"segments":[{"pages":[页码],"points":[{"text":"要点，30 字以内","pages":[页码]}]}]}"""


def map_chunks(key: str, pages: list, chunk: int, cache_path: Path, fresh: bool) -> list:
    cache = {}
    if cache_path.exists() and not fresh:
        cache = json.loads(cache_path.read_text(encoding="utf-8"))

    segments = []
    for start in range(0, len(pages), chunk):
        chunk_pages = pages[start : start + chunk]
        cache_key = f"{LIB_VERSION}:{start}:{chunk}"

        if cache_key in cache:
            segments.extend(cache[cache_key])
            print(f"  map: 第 {start + 1}-{start + len(chunk_pages)} 页 → 缓存")
            continue

        text = "\n\n".join(
            f"第 {p['page']} 页\n标题：{p['title']}\n" + "\n".join(f"- {b}" for b in p["bullets"])
            + (f"\n备注：{p['notes']}" if p["notes"] else "")
            for p in chunk_pages
        )
        reply = chat(key, MAP_SYSTEM, f"课件片段：\n{text}", max_tokens=1500)
        got = json_from_text(reply)["segments"]

        cache[cache_key] = got
        cache_path.write_text(json.dumps(cache, ensure_ascii=False), encoding="utf-8")

        segments.extend(got)
        print(
            f"  map: 第 {start + 1}-{start + len(chunk_pages)} 页 → "
            f"{sum(len(s['points']) for s in got)} 条要点"
        )

    return segments


# --------------------------------------------------------------------------- #
# reduce（分层）
# --------------------------------------------------------------------------- #
GROUP_SYSTEM = """你是课程助教，负责把「一段页范围」内的要点整理成章节。
只输出 JSON，不要解释。

规则：
1. 这一组只出 chapters，不要出 examSummary
2. 按主题合并成 1–3 章，pages 用 [起始页, 结束页]，**必须覆盖给定页范围，不许留空档**
3. 知识点密度：大约每 2–3 页 1 条；保留具体概念/公式/算法步骤，
   不要写「本章介绍了…」这种空话
4. 每条 point 带 pages；每条 examFocus 带 pages 和 basis，basis 只能是：
   "老师强调"（备注/口头补充）、"PPT 标记"（★ 必考 重点 例题 等字样）、"高频出现"
5. 找不到依据就不写 examFocus —— 编造的「考核重点」比没有更糟

JSON 形状：{"chapters":[{"title":"...","pages":[起,止],"outline":["..."],
  "points":[{"text":"...","pages":[...]}],
  "examFocus":[{"text":"...","pages":[...],"basis":"..."}]}]}"""

SUMMARY_SYSTEM = """你是课程助教。下面是一份复习资料的各章标题与其考核重点。
请挑出全篇最关键的 5–8 条，作为「全篇重点」。
只输出 JSON：{"examSummary":["...","..."]}
每条必须能在给定的考核重点里找到对应，**不许新增内容**。"""


def partition(total_pages: int, groups: int) -> list:
    size = max(1, total_pages // max(1, groups))
    bounds, lo = [], 1
    while lo <= total_pages:
        hi = min(total_pages, lo + size - 1)
        bounds.append((lo, hi))
        lo = hi + 1
    return bounds


def reduce_hierarchical(key: str, segments: list, total_pages: int, groups: int = 4) -> dict:
    """分层 reduce。

    为什么不一次看完 145 页的所有要点：我们真撞过 ——
    输出 16245 字符被 max_tokens 截断，JSON 从中间断开。
    分组各自成章，最后用各章的 examFocus（很小）挑「全篇重点」。
    """
    chapters = []
    for lo, hi in partition(total_pages, groups):
        group_segments = [
            s for s in segments if any(lo <= int(p) <= hi for p in s.get("pages", []))
        ]
        if not group_segments:
            print(f"  组 {lo}-{hi}：没有要点，跳过")
            continue

        system = GROUP_SYSTEM + f"\n\n本组页范围：第 {lo} 页到第 {hi} 页（必须全覆盖）。"
        reply = chat(
            key, system, "要点：" + json.dumps({"segments": group_segments}, ensure_ascii=False),
            max_tokens=8000,
        )
        try:
            got = json_from_text(reply)["chapters"]
        except (json.JSONDecodeError, KeyError) as exc:
            raise SystemExit(f"组 {lo}-{hi} 的输出不可用（{exc}）\n  结尾：…{reply[-160:]}")

        chapters.extend(got)
        print(f"  组 {lo}-{hi}：{len(got)} 章，{sum(len(c.get('points', [])) for c in got)} 条知识点")

    digest = [
        {"title": c.get("title", ""), "examFocus": [f.get("text", "") for f in c.get("examFocus", [])]}
        for c in chapters
    ]
    reply = chat(key, SUMMARY_SYSTEM, json.dumps(digest, ensure_ascii=False), max_tokens=1500)
    try:
        summary = json_from_text(reply).get("examSummary", [])
    except json.JSONDecodeError:
        summary = []

    return {"chapters": chapters, "examSummary": summary}


def repair_coverage(material: dict, total_pages: int) -> list:
    """把漏掉的页并进前一个章（确定性修补，不额外调模型）。

    模型的章页范围偶尔会留 1–2 页空档（本课件的第 91 页）。
    「漏页」意味着有内容根本没进复习资料 —— 这种事不该靠模型自觉。
    """
    chapters = material.get("chapters", [])
    if not chapters:
        return []

    for chapter in chapters:
        pages = chapter.get("pages") or []
        if not pages:
            chapter["pages"] = [1, 1]

    chapters.sort(key=lambda c: int((c.get("pages") or [0])[0]))

    fixed = []
    cursor = 1
    for chapter in chapters:
        pages = chapter["pages"]
        start = max(cursor, int(pages[0]))
        end = max(start, int(pages[-1]) if len(pages) > 1 else start)
        if start > cursor:
            fixed.append([cursor, start - 1])          # 记录修补的区间（给人看）
        chapter["pages"] = [start, end]
        cursor = end + 1

    if cursor <= total_pages:
        chapters[-1]["pages"][-1] = total_pages        # 尾部补齐

    return fixed


def coverage(material: dict, total_pages: int) -> tuple[int, list]:
    """章页范围没盖住的页 —— 漏页意味着有内容根本没进复习资料。"""
    covered = set()
    for chapter in material.get("chapters", []):
        pages = chapter.get("pages") or []
        if len(pages) >= 2:
            covered.update(range(int(pages[0]), int(pages[-1]) + 1))
        elif len(pages) == 1:
            covered.add(int(pages[0]))

    missing = [page for page in range(1, total_pages + 1) if page not in covered]
    return len(covered), missing


# --------------------------------------------------------------------------- #
def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(
            "用法：python phase0_doc.py <课件.pdf|pptx> <输出目录> [--chunk 5] [--groups 4] [--fresh]"
        )

    source = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    chunk, groups, fresh = 5, 4, False
    for i, arg in enumerate(sys.argv[3:], start=3):
        if arg == "--chunk" and len(sys.argv) > i + 1:
            chunk = int(sys.argv[i + 1])
        if arg == "--groups" and len(sys.argv) > i + 1:
            groups = int(sys.argv[i + 1])
        if arg == "--fresh":
            fresh = True

    print("[1/4] 抽取")
    extracted = extract_any(source)
    pages = extracted["pages"]
    print(f"  {len(pages)} 页")

    pages, boilerplate = strip_boilerplate(pages)
    (out_dir / "extracted.json").write_text(
        json.dumps({**extracted, "pages": pages, "boilerplate": boilerplate},
                   ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(f"  滤掉样板行 {len(boilerplate)} 条：{boilerplate[:5]}")

    digest = hashlib.sha1(source.read_bytes()).hexdigest()[:12]
    cache_path = out_dir / f"map-cache-{digest}.json"

    print(f"[2/4] map（每 {chunk} 页一次，有缓存）")
    key = read_key()
    segments = map_chunks(key, pages, chunk, cache_path, fresh)

    print(f"[3/4] reduce（分 {groups} 组，各自成章）")
    material = reduce_hierarchical(key, segments, len(pages), groups)
    (out_dir / "material.json").write_text(
        json.dumps(material, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    fixed = repair_coverage(material, len(pages))
    if fixed:
        print(f"  修补了 {len(fixed)} 处页空档：{fixed[:5]}")

    count, missing = coverage(material, len(pages))
    print(
        f"  覆盖 {count}/{len(pages)} 页"
        + (f"，漏 {len(missing)} 页：{missing[:15]}" if missing else "，无漏页")
    )
    points = sum(len(c.get("points", [])) for c in material.get("chapters", []))
    focus = sum(len(c.get("examFocus", [])) for c in material.get("chapters", []))
    print(f"  {len(material.get('chapters', []))} 章 / {points} 条知识点 / {focus} 条考核重点")

    print("[4/4] 渲染 + PDF")
    html_path = out_dir / "review.html"
    render(
        {
            **extracted,
            "slides": [
                {"page": p["page"], "title": p["title"], "bullets": p["bullets"], "notes": p["notes"]}
                for p in pages
            ],
        },
        {k: v for k, v in material.items() if k != "chapters"} | {"chapters": material["chapters"]},
        html_path,
    )
    to_pdf(html_path, out_dir / "review.pdf")

    print(f"\n完成：{out_dir / 'review.pdf'}")


if __name__ == "__main__":
    main()
