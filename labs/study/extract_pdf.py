"""从 PDF 课件抽文本（Phase 0 的 PDF 版）。

为什么单独一个文件：真实课件常常是 PDF（老师直接导出的讲义），
而 python-pptx 只认 pptx。产品里这两个都要有（C++ 用 miniz+XML 抽 pptx、
用 PDF 文字层抽 PDF）。

判定：如果 PDF 抽不出文字（纯图片页），要明确说出来 —— 那说明需要 OCR。
"""
import json
import sys
from pathlib import Path

from pypdf import PdfReader


def clean(text: str) -> str:
    """PDF 里偶尔有落单的代理项（UTF-16 残片），json 序列化会直接崩。

    这些字符本来就没法显示，丢掉比替换成 ? 更干净。
    """
    return "".join(ch for ch in text if not 0xD800 <= ord(ch) <= 0xDFFF)


def extract(pdf_path: Path) -> dict:
    reader = PdfReader(str(pdf_path))
    pages = []

    for index, page in enumerate(reader.pages, start=1):
        try:
            text = clean(page.extract_text() or "")
        except Exception as exc:                     # 单页坏了不该让整篇失败
            text = ""
            print(f"  第 {index} 页抽取失败：{exc}", file=sys.stderr)

        lines = [line.strip() for line in text.splitlines() if line.strip()]
        pages.append({"page": index, "title": lines[0] if lines else "", "bullets": lines[1:], "notes": ""})

    empty = [p["page"] for p in pages if not p["title"] and not p["bullets"]]
    return {"source": pdf_path.name, "pages": pages, "empty_pages": empty}


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit("用法：python extract_pdf.py <课件.pdf> <输出.json>")

    result = extract(Path(sys.argv[1]))
    Path(sys.argv[2]).write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"共 {len(result['pages'])} 页")
    print(f"抽不出文字的页：{len(result['empty_pages'])} 页 {result['empty_pages'][:20]}")
    for page in result["pages"][:3]:
        print(f"\n第 {page['page']} 页：{page['title']}")
        for bullet in page["bullets"][:5]:
            print(f"   - {bullet}")


if __name__ == "__main__":
    main()
