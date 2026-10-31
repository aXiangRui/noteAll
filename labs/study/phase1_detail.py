"""Phase 1：规则驱动的「详细版」复习资料

与 Phase 0 的根本区别：
  Phase 0 = map 摘要（每 5 页压成 15 条一句话）→ 信息在第一层就丢了
  Phase 1 = 先划结构，再**逐节喂原文**展开 → 保留细节

规则（SPEC）不是"示例照抄"，而是可检查的硬约束：字数下限、四段式要素、
禁用词、必须有页码依据。评测尺子是用户给的 44 页参考资料，但它**不进 prompt**。

用法：
  python phase1_detail.py <课件.pdf|pptx> <输出目录> --chapters 1-1
"""
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from phase0 import chat, json_from_text, read_key, to_pdf, HTML_TEMPLATE  # noqa: E402
from phase0_doc import extract_any, strip_boilerplate  # noqa: E402

# --------------------------------------------------------------------------- #
# 规格：这份 SPEC 就是"规则版"的全部内容，改这里等于改标准
# --------------------------------------------------------------------------- #
SPEC = """
【总规格】
产出是一份可以在考前一天通读、并且能直接抄进答卷的复习资料。
不是提纲，不是摘要 —— 每一节都要能把一个知识点讲完整。

【每一节必须包含四段，缺一段就算不合格】
1. 定义：这个概念是什么。用一句完整的话，不许用"指的是"含糊带过。
2. 机制：它**怎么工作 / 为什么这样设计**。至少 2 句因果（A 导致 B，所以 C）。
   能画流程的用箭头文字描述步骤。
3. 考法：考试里它怎么考（选择/简答/计算），以及答题时老师想看到的关键词。
   如果是推断出来的，句中要带「（推断）」，不要冒充真题结论。
4. 易错点：最容易混的地方，1–3 条，每条点明"错在哪"。

【每节还必须有一个标准表述】
"标准表述"是能直接写进答卷的一段话（60–120 字），用书面语、术语准确、不带口语。

【篇幅下限】
每节正文合计 350–550 字（不含标准表述）。少于 300 字视为没写完。

【依据规则（本版特别约定：B 方案）】
- 只写课件里**明确标记**的考点：★ / 必考 / 重点 / 例题 / 小结 页
- 课件没标注的可以写，但必须按重要性推断，并在 basis 字段标 inference
- **禁止写"真题高频""历年常考"** —— 我们手头没有真题，写出来就是编的
- 全书不生成"考情地图""真题速解模板"，等有真题再补

【可以跳过（重要）】
如果这一页没有可复习的知识点 —— 课程导论、目录页、纯故事/轶事、整页图片、
只有标题的过渡页 —— 直接输出 {"skip": true, "reason": "原因"}，**不要硬凑四段式**。
硬凑出来的「考法」和「易错点」是注水，比不写更糟。

【禁止清单（比要求更重要）】
- 禁止"本章介绍了…""概述了…""主要讨论…"这类空话开头
- 禁止把课件里的一句话拆成三句话凑字数
- 禁止写"非常重要""需要掌握"这种没有信息量的评语
- 禁止编造课件里没有的公式、数字、定理名称
- 没有依据的内容宁可不写，也不要猜测
"""

SECTION_SYSTEM = """你是这门课的助教，负责写一节**可以直接用于考前背诵**的复习内容。

""" + SPEC + """

【输出】只输出 JSON，不要任何解释文字：
{
  "title": "这一节的标题",
  "definition": "定义段",
  "mechanism": "机制段（含因果与步骤）",
  "exam": "考法段",
  "pitfalls": ["易错点1", "易错点2"],
  "standard": "标准表述（60–120 字，可直接抄进答卷）",
  "basis": "marker（课件有 ★/必考/例题/小结 等标记）或 inference（按重要性推断）",
  "pages": [引用的原页页码]
}
"""

STRUCTURE_SYSTEM = """你是课程助教。下面给出课件的逐页首行（含页码）。
请把它划分成「章 → 节」两级结构。

规则：
- 章按内容主题划分（不要把每一页当一章）
- 节是章内的知识块，粒度参考：一个节大约覆盖 2–8 页
- pages 用 [起始页, 结束页]，**必须连续覆盖全部页码，不许留空档**
- 标题用课件里出现过的说法，不要自己发明

只输出 JSON：
{"chapters":[{"title":"...","pages":[起,止],
  "sections":[{"id":"1.1","title":"...","pages":[起,止]}]}]}"""

CHAPTER_EXTRA_SYSTEM = """你是课程助教。下面给你**一章已经写好的各节内容**。
请为这一章补三样东西，只输出 JSON：

{
  "compareTable": {
    "caption": "表名（例如「三种调度算法对比」）",
    "headers": ["列1","列2","列3"],
    "rows": [["...","...","..."]]
  },
  "calcTemplate": {
    "title": "计算题速解模板（没有可计算内容就设为 null）",
    "steps": ["第1步：...", "第2步：..."]
  },
  "chapterPitfalls": ["本章最容易混的 1–3 条"]
}

要求：对比表至少 3 行；速解模板必须给出**可操作的步骤**（含中间量怎么求），
不要写"理解概念"这种废话。没有可计算内容时 calcTemplate 设为 null。
"""

BOOK_EXTRA_SYSTEM = """你是课程助教。下面是一份复习资料的全部各节「标准表述」与「易错点」。
请补全书的两个收尾板块，只输出 JSON：

{
  "pitfallSummary": [{"point":"易混点","wrong":"错误理解","right":"正确理解"}],
  "memorizeList": ["考前必须背下来的 N 条，每条一句话"]
}

要求：pitfallSummary 挑最容易被考到且最容易混的 6–10 条；
memorizeList 8–12 条，必须是"背了就能拿分"的内容（定义、公式、判据、步骤），
不要放"理解调度思想"这类无法背诵的话。禁止新增上面没有的内容。
"""


# --------------------------------------------------------------------------- #
def header_lines(pages: list, min_count: int = 3, max_len: int = 14) -> set:
    """页眉页脚候选：在 ≥3 页上重复出现的短行（课程名、章节名）。

    这份课件的 PDF 抽取把页码当成了每页第一行（144/145 页），
    所以"标题"基本不可用，只能靠这个集合把「计算机安全」这类页眉排除掉。
    """
    counts: dict[str, int] = {}
    for page in pages:
        for line in page["bullets"]:
            line = line.strip()
            if line and len(line) <= max_len:
                counts[line] = counts.get(line, 0) + 1
    return {line for line, n in counts.items() if n >= min_count}


def heading_of(page: dict, headers: set) -> str:
    """这一页的"标题提示"：第一个不是纯数字、也不是页眉的短行。"""
    for line in [page["title"], *page["bullets"]]:
        line = line.strip()
        if not line or line.isdigit():
            continue
        if line in headers:
            continue
        return line[:40]
    return ""


def detect_structure(key: str, pages: list) -> dict:
    headers = header_lines(pages)
    outline = "\n".join(
        f"p{p['page']} {heading_of(p, headers) or '（无标题）'}" for p in pages
    )
    reply = chat(key, STRUCTURE_SYSTEM, f"课件逐页首行：\n{outline}", max_tokens=4000)
    structure = json_from_text(reply)
    return validate_structure(structure, pages, headers)


def validate_structure(structure: dict, pages: list, headers: set) -> dict:
    """校验模型给的结构；不可用就退化成确定性分块。

    要防的两种情况（都真的发生过）：
      * 章节页码范围乱序 / 重叠 / 留空档；
      * 一页一节（标题是页码时最容易发生）。
    """
    total = len(pages)
    chapters = structure.get("chapters") or []

    ok = bool(chapters)
    cursor = 1
    for chapter in chapters:
        chapter_pages = chapter.get("pages") or [0, 0]
        if int(chapter_pages[0]) != cursor:
            ok = False
            break
        sections = chapter.get("sections") or []
        section_cursor = int(chapter_pages[0])
        for section in sections:
            section_pages = section.get("pages") or [0, 0]
            if int(section_pages[0]) != section_cursor:
                ok = False
                break
            section_cursor = int(section_pages[-1]) + 1
        if not ok or section_cursor != int(chapter_pages[-1]) + 1:
            ok = False
            break
        cursor = int(chapter_pages[-1]) + 1

    if ok and cursor == total + 1 and len(chapters) >= 1:
        # 章边界可用就留，但**节一律重切成 1 页一节**：
        # 一节 2 页时输出被摊薄（实测 228 字/页），一页一节能到 450+ 字/页，
        # 这才是"超级详细"该有的密度。
        return rebuild_sections(chapters, pages, headers, section_size=1)

    print("  结构不可用（乱序/空档/一页一节）→ 退化：章范围尽量保留，节按 2 页重切")
    return rebuild_sections(chapters if ok else [], pages, headers, section_size=1)


def rebuild_sections(chapters: list, pages: list, headers: set, section_size: int = 2) -> dict:
    """保留（或退化出）章范围，但**节一律按固定页数重切**。

    为什么不让模型定节边界：它的页范围经常乱序/重叠（这份课件尤其难，
    144/145 页的第一行是页码），而节的粒度错误会直接毁掉每节的详细度。
    """
    total = len(pages)

    # 章范围可用就留，否则按 12 页切章
    usable = []
    cursor = 1
    for chapter in chapters:
        chapter_pages = chapter.get("pages") or [0, 0]
        if int(chapter_pages[0]) != cursor or int(chapter_pages[-1]) < cursor:
            usable = []
            break
        usable.append((chapter.get("title", ""), cursor, int(chapter_pages[-1])))
        cursor = int(chapter_pages[-1]) + 1
    if cursor != total + 1:
        usable = []

    if not usable:
        usable = []
        lo = 1
        while lo <= total:
            hi = min(total, lo + 12 - 1)
            usable.append((heading_of(pages[lo - 1], headers) or f"第 {lo}–{hi} 页", lo, hi))
            lo = hi + 1

    built = []
    for index, (title, lo, hi) in enumerate(usable, start=1):
        sections = []
        section_lo = lo
        while section_lo <= hi:
            section_hi = min(hi, section_lo + section_size - 1)
            heading = heading_of(pages[section_lo - 1], headers) or f"第 {section_lo}–{section_hi} 页"
            sections.append(
                {
                    "id": f"{index}.{len(sections) + 1}",
                    "title": heading,
                    "pages": [section_lo, section_hi],
                }
            )
            section_lo = section_hi + 1
        built.append({"title": title, "pages": [lo, hi], "sections": sections})

    return {"chapters": built}


def fallback_structure_unused(pages: list, headers: set, chunk: int = 4) -> dict:
    chapters = []
    index = 0
    lo = 1
    total = len(pages)
    while lo <= total:
        hi = min(total, lo + chunk * 3 - 1)
        sections = []
        section_lo = lo
        while section_lo <= hi:
            section_hi = min(hi, section_lo + chunk - 1)
            first = pages[section_lo - 1]
            title = heading_of(first, headers) or f"第 {section_lo}–{section_hi} 页"
            sections.append(
                {"id": f"{index + 1}.{len(sections) + 1}", "title": title,
                 "pages": [section_lo, section_hi]}
            )
            section_lo = section_hi + 1

        first_page = pages[lo - 1]
        chapter_title = heading_of(first_page, headers) or f"第 {lo}–{hi} 页"
        chapters.append({"title": f"{chapter_title}", "pages": [lo, hi], "sections": sections})
        index += 1
        lo = hi + 1

    return {"chapters": chapters}


def page_text(pages: list, lo: int, hi: int, limit: int = 9000) -> str:
    """把某段页码的原文拼起来（喂给模型的就是这个，不是摘要）。"""
    chunks = []
    for page in pages:
        number = int(page["page"])
        if not lo <= number <= hi:
            continue
        lines = [f"第 {number} 页"]
        if page["title"]:
            lines.append(f"标题：{page['title']}")
        lines.extend(page["bullets"])
        if page.get("notes"):
            lines.append(f"备注：{page['notes']}")
        chunks.append("\n".join(lines))

    text = "\n\n".join(chunks)
    return text[:limit]


def write_section(key: str, section: dict, pages: list) -> dict:
    lo, hi = section["pages"][0], section["pages"][-1]
    text = page_text(pages, int(lo), int(hi))
    user = f"这一节：{section['title']}（第 {lo}–{hi} 页）\n\n课件原文：\n{text}"
    reply = chat(key, SECTION_SYSTEM, user, max_tokens=2000)
    detail = json_from_text(reply)
    if detail.get("skip"):
        return None
    detail.setdefault("title", section["title"])
    detail.setdefault("pages", list(section["pages"]))
    return detail


def write_chapter_extra(key: str, chapter: dict, details: list) -> dict:
    payload = json.dumps(
        [{"title": d["title"], "definition": d.get("definition", ""),
          "mechanism": d.get("mechanism", ""), "exam": d.get("exam", ""),
          "pitfalls": d.get("pitfalls", [])} for d in details],
        ensure_ascii=False,
    )
    reply = chat(key, CHAPTER_EXTRA_SYSTEM, f"本章：{chapter['title']}\n各节：{payload}", max_tokens=2500)
    return json_from_text(reply)


def write_book_extra(key: str, details: list) -> dict:
    payload = json.dumps(
        [{"title": d["title"], "standard": d.get("standard", ""), "pitfalls": d.get("pitfalls", [])}
         for d in details],
        ensure_ascii=False,
    )
    reply = chat(key, BOOK_EXTRA_SYSTEM, payload, max_tokens=2500)
    return json_from_text(reply)


# --------------------------------------------------------------------------- #
# 渲染
# --------------------------------------------------------------------------- #
DETAIL_TEMPLATE = """<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>{title} 复习资料</title>
<style>
  @page {{ size: A4; margin: 16mm 15mm 18mm; }}
  body {{ font-family: "Source Han Sans SC", "Microsoft YaHei", sans-serif;
         font-size: 10.5pt; line-height: 1.75; color: #1b1e23; }}
  h1 {{ font-size: 21pt; margin: 0 0 6pt; }}
  .sub {{ color: #6b7280; font-size: 9pt; margin-bottom: 16pt; }}
  h2 {{ font-size: 15pt; margin: 22pt 0 8pt; padding: 4pt 8pt;
        background: #eef3fb; border-left: 4pt solid #2f6fd0; color: #1f3a63; }}
  h3 {{ font-size: 11.5pt; margin: 14pt 0 5pt; color: #2f6fd0; }}
  h4 {{ font-size: 10.5pt; margin: 8pt 0 2pt; color: #445; }}
  p {{ margin: 3pt 0; }}
  ul {{ margin: 3pt 0 3pt 16pt; padding: 0; }}
  li {{ margin: 2pt 0; }}
  .page {{ color: #9aa3ad; font-size: 8pt; margin-left: 3pt; }}
  .std {{ background: #f2fbf5; border-left: 3pt solid #2f9e6e;
          padding: 7pt 10pt; margin: 7pt 0; font-size: 10pt; }}
  .std b {{ color: #1f7a52; }}
  .pit {{ background: #fff7f0; border-left: 3pt solid #d97542;
          padding: 6pt 10pt; margin: 6pt 0; }}
  .pit b {{ color: #b4521f; }}
  table {{ border-collapse: collapse; width: 100%; margin: 8pt 0; font-size: 9.5pt; }}
  th, td {{ border: 1pt solid #c8d2e0; padding: 4pt 6pt; vertical-align: top; }}
  th {{ background: #eef3fb; }}
  .calc {{ background: #f6f2fd; border-left: 3pt solid #7a5bd0; padding: 7pt 10pt; margin: 8pt 0; }}
  .memorize {{ background: #eef4ff; border: 1pt solid #b8cdf0; padding: 9pt 12pt; margin: 12pt 0; }}
  .chapter {{ page-break-inside: auto; }}
  h2, h3 {{ page-break-after: avoid; }}
</style></head><body>
<h1>{title}</h1>
<div class="sub">{source} · 共 {pages} 页 · 由课件原文逐节展开，页码可回查</div>
{body}
</body></html>
"""


def esc(text: str) -> str:
    return (text or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def refs(pages_list) -> str:
    return "".join(f'<span class="page">p{esc(str(p))}</span>' for p in (pages_list or []))


def render_detail(extracted: dict, material: dict, out_html: Path) -> None:
    parts = []

    for chapter in material["chapters"]:
        parts.append('<div class="chapter">')
        pages = chapter.get("pages") or []
        span = f"第 {pages[0]}–{pages[-1]} 页" if len(pages) >= 2 else ""
        parts.append(f'<h2>{esc(chapter["title"])} <span class="page">{span}</span></h2>')

        for section in chapter.get("sections", []):
            parts.append(f'<h3>{esc(section["title"])} {refs(section.get("pages"))}</h3>')

            if section.get("definition"):
                parts.append(f'<h4>定义</h4><p>{esc(section["definition"])}</p>')
            if section.get("mechanism"):
                parts.append(f'<h4>机制</h4><p>{esc(section["mechanism"])}</p>')
            if section.get("exam"):
                parts.append(f'<h4>考法</h4><p>{esc(section["exam"])}</p>')
            if section.get("pitfalls"):
                items = "".join(f"<li>{esc(p)}</li>" for p in section["pitfalls"])
                parts.append(f'<div class="pit"><b>易错点</b><ul>{items}</ul></div>')
            if section.get("standard"):
                basis = section.get("basis", "")
                badge = (
                    '<span class="page">依据：课件标记</span>'
                    if basis == "marker"
                    else '<span class="page">依据：推断</span>'
                )
                parts.append(
                    f'<div class="std"><b>考试标准表述</b> {badge}<br>'
                    f'{esc(section["standard"])}</div>'
                )

        extra = chapter.get("extra") or {}

        table = extra.get("compareTable") or {}
        if table.get("rows"):
            head = "".join(f"<th>{esc(h)}</th>" for h in table.get("headers", []))
            body = "".join(
                "<tr>" + "".join(f"<td>{esc(c)}</td>" for c in row) + "</tr>"
                for row in table["rows"]
            )
            parts.append(f'<h4>{esc(table.get("caption", "对比表"))}</h4>')
            parts.append(f"<table><tr>{head}</tr>{body}</table>")

        calc = extra.get("calcTemplate") or {}
        if calc.get("steps"):
            steps = "".join(f"<li>{esc(s)}</li>" for s in calc["steps"])
            parts.append(f'<div class="calc"><b>{esc(calc.get("title", "速解模板"))}</b>'
                         f"<ol>{steps}</ol></div>")

        if extra.get("chapterPitfalls"):
            items = "".join(f"<li>{esc(p)}</li>" for p in extra["chapterPitfalls"])
            parts.append(f'<div class="pit"><b>本章易混</b><ul>{items}</ul></div>')

        parts.append("</div>")

    extra = material.get("bookExtra") or {}

    if extra.get("pitfallSummary"):
        rows = "".join(
            f'<tr><td>{esc(item.get("point", ""))}</td>'
            f'<td>{esc(item.get("wrong", ""))}</td><td>{esc(item.get("right", ""))}</td></tr>'
            for item in extra["pitfallSummary"]
        )
        parts.append("<h2>全书高频易混点</h2>")
        parts.append(f"<table><tr><th>易混点</th><th>错误理解</th><th>正确理解</th></tr>{rows}</table>")

    if extra.get("memorizeList"):
        items = "".join(f"<li>{esc(m)}</li>" for m in extra["memorizeList"])
        parts.append('<div class="memorize"><h3>考前背诵清单</h3>'
                     f"<ul>{items}</ul></div>")

    html = DETAIL_TEMPLATE.format(
        title=Path(extracted["source"]).stem,
        source=extracted["source"],
        pages=len(extracted["pages"]),
        body="\n".join(parts),
    )
    out_html.write_text(html, encoding="utf-8")


# --------------------------------------------------------------------------- #
def parse_range(text: str, total: int) -> list:
    """--chapters 1-1 这种，按章序号筛选（结构识别之后再过滤）。"""
    if "-" not in text:
        index = int(text)
        return [index]
    lo, hi = text.split("-", 1)
    return list(range(int(lo), int(hi) + 1))


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit("用法：python phase1_detail.py <课件.pdf|pptx> <输出目录> [--chapters 1-1]")

    source = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    only = None
    for i, arg in enumerate(sys.argv[3:], start=3):
        if arg == "--chapters" and len(sys.argv) > i + 1:
            only = sys.argv[i + 1]

    print("[1/5] 抽取")
    extracted = extract_any(source)
    pages, boilerplate = strip_boilerplate(extracted["pages"])
    print(f"  {len(pages)} 页（样板行 {len(boilerplate)} 条）")

    key = read_key()

    print("[2/5] 结构识别（章 → 节）")
    structure = detect_structure(key, pages)
    chapters = structure.get("chapters", [])
    print(f"  {len(chapters)} 章 / {sum(len(c.get('sections', [])) for c in chapters)} 节")

    if only:
        wanted = parse_range(only, len(chapters))
        chapters = [c for i, c in enumerate(chapters, start=1) if i in wanted]
        print(f"  只生成第 {only} 章 → {len(chapters)} 章")

    print("[3/5] 逐节展开（喂原文，不是摘要）")
    for chapter in chapters:
        details = []
        for section in chapter.get("sections", []):
            detail = write_section(key, section, pages)
            if detail is None:
                print(f"  {section.get('id', '')} {section['title'][:20]} → 跳过（无可复习内容）")
                continue
            details.append(detail)
            words = sum(len(detail.get(k, "")) for k in ("definition", "mechanism", "exam"))
            print(f"  {section.get('id', '')} {section['title'][:20]} → {words} 字"
                  f" / 易错 {len(detail.get('pitfalls', []))} 条")
        chapter["sections"] = details

        print(f"  章补充：{chapter['title'][:20]}")
        chapter["extra"] = write_chapter_extra(key, chapter, details)

    print("[4/5] 全书收尾板块")
    all_details = [d for c in chapters for d in c.get("sections", [])]
    book_extra = write_book_extra(key, all_details)

    material = {"chapters": chapters, "bookExtra": book_extra}
    (out_dir / "material-detail.json").write_text(
        json.dumps(material, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    print("[5/5] 渲染 + PDF")
    html_path = out_dir / "detail.html"
    render_detail({**extracted, "pages": pages}, material, html_path)
    to_pdf(html_path, out_dir / "detail.pdf")

    total_words = sum(
        len(d.get(k, "")) for d in all_details for k in ("definition", "mechanism", "exam")
    )
    print(f"\n完成：{out_dir / 'detail.pdf'}")
    print(f"正文合计 {total_words} 字 / {len(all_details)} 节"
          f" / 平均 {total_words // max(1, len(all_details))} 字每节")


if __name__ == "__main__":
    main()
