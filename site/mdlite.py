"""A small Markdown subset -> HTML for the docs/ pages (standard library only).

Handles what the imported docs use: ATX headings (with GitHub-style ids), paragraphs, fenced code, pipe tables,
bullet and numbered lists (nested by indent, with continuation lines), block quotes, rules, and inline code,
bold, italics and links. `link(url)` rewrites each link target. Anything else passes through as paragraph text.
"""
import html, re


def slug(text):
    text = re.sub(r"<[^>]+>", "", text).lower()
    return re.sub(r"\s", "-", re.sub(r"[^\w\s-]", "", text)).strip("-")


def inline(text, link):
    codes = []

    def stash(m):
        codes.append("<code>%s</code>" % html.escape(m.group(1)))
        return "\0%d\0" % (len(codes) - 1)
    text = re.sub(r"`([^`]+)`", stash, text)
    text = html.escape(text, quote=False)
    text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)",
                  lambda m: '<a href="%s">%s</a>' % (html.escape(link(html.unescape(m.group(2))), quote=True), m.group(1)), text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"(?<![\w*])\*([^*\s][^*]*?)\*(?![\w*])", r"<em>\1</em>", text)
    return re.sub(r"\0(\d+)\0", lambda m: codes[int(m.group(1))], text)


def split_row(line):
    line = line.strip().strip("|")
    return [c.strip() for c in re.split(r"(?<!\\)\|", line.replace("\\|", "\\|"))]


def table(rows, link):
    head = split_row(rows[0])
    out = ["<table>", "<thead><tr>" + "".join("<th>%s</th>" % inline(c.replace("\\|", "|"), link) for c in head) + "</tr></thead><tbody>"]
    for r in rows[2:]:
        out.append("<tr>" + "".join("<td>%s</td>" % inline(c.replace("\\|", "|"), link) for c in split_row(r)) + "</tr>")
    out.append("</tbody></table>")
    return "\n".join(out)


LIST = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")


def lists(lines, i, link):
    """Parse a list starting at lines[i]; returns (html, next index)."""
    base = len(LIST.match(lines[i]).group(1))
    ordered = LIST.match(lines[i]).group(2)[0].isdigit()
    tag = "ol" if ordered else "ul"
    out, items = [], []
    while i < len(lines):
        m = LIST.match(lines[i])
        if m and len(m.group(1)) == base:
            items.append([m.group(3)])
            i += 1
        elif m and len(m.group(1)) > base and items:
            sub, i = lists(lines, i, link)
            items[-1].append(sub)
        elif lines[i].strip() and lines[i].startswith(" " * (base + 2)) and items and not m:
            items[-1].append(lines[i].strip())
            i += 1
        else:
            break
    for it in items:
        text, subs = [], []
        for part in it:
            (subs if part.startswith(("<ul", "<ol")) else text).append(part)
        out.append("<li>%s%s</li>" % (inline(" ".join(text), link), "".join(subs)))
    return "<%s>\n%s\n</%s>" % (tag, "\n".join(out), tag), i


def convert(md, link=lambda u: u, h1=False):
    """Returns (html, headings); headings is [(level, id, text)] for h2/h3. `h1` keeps the first # heading."""
    lines = md.splitlines()
    out, heads, para = [], [], []
    seen, i, first = {}, 0, True

    def flush():
        if para:
            out.append("<p>%s</p>" % inline(" ".join(s.strip() for s in para), link))
            para.clear()
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("```"):
            flush()
            lang = ln[3:].strip()
            i += 1
            code = []
            while i < len(lines) and not lines[i].startswith("```"):
                code.append(lines[i])
                i += 1
            i += 1
            note = ""
            if lang == "mermaid":
                note = '<p class="doc-src">Diagram source (Mermaid); GitHub renders it as a picture.</p>'
            out.append('<pre><code>%s</code></pre>%s' % (html.escape("\n".join(code)), note))
            continue
        m = re.match(r"^(#{1,6})\s+(.*?)\s*#*$", ln)
        if m:
            flush()
            lvl, text = len(m.group(1)), m.group(2)
            if lvl == 1 and first:
                first = False
                if h1:
                    out.append("<h1>%s</h1>" % inline(text, link))
                i += 1
                continue
            first = False
            sid = slug(inline(text, lambda u: u))
            n = seen.get(sid, 0)
            seen[sid] = n + 1
            sid = sid if not n else "%s-%d" % (sid, n)
            level = max(lvl, 2)
            if level <= 3:
                heads.append((level, sid, re.sub(r"<[^>]+>", "", inline(text, lambda u: u))))
            out.append('<h%d id="%s">%s</h%d>' % (level, sid, inline(text, link), level))
            i += 1
            continue
        if ln.lstrip().startswith("|") and i + 1 < len(lines) and re.match(r"^\s*\|[\s:|-]+\|\s*$", lines[i + 1]):
            flush()
            rows = []
            while i < len(lines) and lines[i].lstrip().startswith("|"):
                rows.append(lines[i])
                i += 1
            out.append('<div class="tbl">%s</div>' % table(rows, link))
            continue
        if LIST.match(ln) and not re.match(r"^---+$", ln):
            flush()
            h, i = lists(lines, i, link)
            out.append(h)
            continue
        if ln.startswith(">"):
            flush()
            q = []
            while i < len(lines) and lines[i].startswith(">"):
                q.append(lines[i].lstrip("> "))
                i += 1
            out.append("<blockquote><p>%s</p></blockquote>" % inline(" ".join(q), link))
            continue
        if re.match(r"^(---+|\*\*\*+)\s*$", ln):
            flush()
            out.append("<hr>")
        elif not ln.strip():
            flush()
        else:
            para.append(ln)
        i += 1
    flush()
    return "\n".join(out), heads
