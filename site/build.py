#!/usr/bin/env python3
"""Build the Eden: Community Edition site: site/src/*.html (+ templates/layout.html, static/) -> site/_out/.

    python3 -I site/build.py            build into site/_out/
    python3 -I site/build.py --check    build into a temp dir, then gate it: every internal link and
                                        anchor-free target exists, every <img> has width/height/alt and a
                                        file, the home page fits the weight budget, no private paths or
                                        text, the publish script keeps pages.yml.  Exit 1 on any problem.

A page is `site/src/<name>.html`: header lines `key: value` (title, nav, description, toc), a line `---`,
then the body HTML. `nav` names the current tab. Pages in sub-folders (src/modding/x.html) work: `{{root}}`
is the relative prefix back to the site root. Standard library only; fonts and badges are committed outputs.
"""
import datetime, html, json, pathlib, posixpath, re, shutil, sys, tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import mdlite

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent
SRC, TPL, STATIC, OUT = HERE / "src", HERE / "templates", HERE / "static", HERE / "_out"

# One place for the host: a custom domain later is a change here (plan D1).
BASE_URL = "https://hagg3.github.io/edencom/"
GITHUB = "https://github.com/hagg3/edencom"
DISCORD = "https://discord.gg/SgEQfurMmj"

TABS = [("home", "Home", "index.html"), ("download", "Download", "download.html"),
        ("shots", "Screenshots", "screenshots.html"), ("help", "Help", "help.html"),
        ("modding", "Modding", "modding.html"), ("dev", "Developers", "developers.html"),
        ("about", "About", "about.html")]

# Pages generated from docs/ (plan D8): (doc file, site page, nav tab, section, sidebar label, description).
DOCS = [
    ("content-packs", "modding/content-packs.html", "modding", "Modding", "Blocks and packs",
     "How Eden: Community Edition content packs work: pack.json, tiles, blocks, mod packs and making your first block."),
    ("scripting", "modding/scripting.html", "modding", "Modding", "Lua scripts",
     "Lua mod scripts for Eden: Community Edition: the sandbox, the eden API, events and budgets."),
    ("console", "modding/console.html", "modding", "Modding", "The console language",
     "How the Eden: Community Edition console and command-block language works."),
    ("console-reference", "modding/console-reference.html", "modding", "Modding", "Console reference",
     "Every command-block verb, console command and setting variable in Eden: Community Edition."),
    ("architecture-overview", "developers/architecture.html", "dev", "Developers", "Architecture overview",
     "How the Eden: Community Edition engine is put together: ownership, systems, data flow and reading order."),
]
GH_BLOB = GITHUB + "/blob/main/"
GH_TREE = GITHUB + "/tree/main/"
SITE_PAGE_OF_MD = {"docs/%s.md" % d[0]: d[1] for d in DOCS}

# Budgets (plan D10): home HTML + CSS + fonts before lazy images, and the JS total.
HOME_BUDGET = 150 * 1024
JS_BUDGET = 2 * 1024
PRIVATE_TEXT = ("WORKING/", "CLAUDE.md", "/Us" + "ers/", "TESTERS/")  # split: linkcheck greps this file too


def read_page(path):
    head, sep, body = path.read_text(encoding="utf-8").partition("\n---\n")
    if not sep:
        raise SystemExit(f"{path}: no '---' line after the header")
    meta = {}
    for line in head.splitlines():
        k, _, v = line.partition(":")
        meta[k.strip()] = v.strip()
    for k in ("title", "nav", "description"):
        if k not in meta:
            raise SystemExit(f"{path}: header lacks '{k}'")
    return meta, body.strip()


def news_html():
    """Sidebar news: the `# ` headings of changelog.md with the first sentence of each intro."""
    text = (REPO / "changelog.md").read_text(encoding="utf-8")
    items = []
    for block in re.split(r"(?m)^# ", text)[1:4]:
        title, _, rest = block.partition("\n")
        intro = next((p for p in rest.strip().split("\n\n") if p and not p.startswith("#")), "")
        intro = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", intro)
        intro = re.split(r"(?<=[.!?])\s", intro.strip())[0]
        items.append(f"<li><a href=\"{GITHUB}/blob/main/changelog.md\">{html.escape(title.strip())}</a>: "
                     f"{html.escape(intro)}</li>")
    return "<ul>" + "".join(items) + "</ul>"


def tabs_html(current, root):
    out = []
    for key, label, href in TABS:
        cur = ' aria-current="page"' if key == current else ""
        out.append(f'<a class="btn" href="{root}{href}"{cur}>{label}</a>')
    return '<nav class="tabs" aria-label="Site">' + "".join(out) + "</nav>"


OG_IMAGE = "static/img/clear-noon-level.jpg"  # 960x540, the home hero
OG_SIZE = (960, 540)
OG_ALT = "A riverbank in Eden at noon: trees, a stone pool and lamps, the hotbar along the bottom"
SITE_NAME = "Eden: Community Edition"


def seo_head(meta, rel):
    """Canonical, OpenGraph, Twitter card and JSON-LD for one page (rel = path under the site root)."""
    if rel == "404.html":
        return '<meta name="robots" content="noindex">'
    url = BASE_URL + ("" if rel == "index.html" else rel)
    t, d = html.escape(meta["title"], quote=True), html.escape(meta["description"], quote=True)
    img = BASE_URL + OG_IMAGE
    out = ['<link rel="canonical" href="%s">' % url,
           '<meta property="og:site_name" content="%s">' % SITE_NAME,
           '<meta property="og:type" content="website">',
           '<meta property="og:title" content="%s">' % t,
           '<meta property="og:description" content="%s">' % d,
           '<meta property="og:url" content="%s">' % url,
           '<meta property="og:image" content="%s">' % img,
           '<meta property="og:image:width" content="%d">' % OG_SIZE[0],
           '<meta property="og:image:height" content="%d">' % OG_SIZE[1],
           '<meta property="og:image:alt" content="%s">' % html.escape(OG_ALT, quote=True),
           '<meta name="twitter:card" content="summary_large_image">']
    ld = None
    if rel == "index.html":
        ld = {"@context": "https://schema.org", "@type": "VideoGame", "name": SITE_NAME,
              "alternateName": ["Eden World Builder community fork", "Eden: World Builder (community fork)"],
              "description": meta["description"], "url": url, "image": img,
              "genre": ["Sandbox", "Voxel", "Building"], "gamePlatform": ["macOS", "Windows", "Linux", "iOS"],
              "applicationCategory": "Game", "operatingSystem": "macOS, Windows, Linux, iOS",
              "license": "https://www.gnu.org/licenses/gpl-3.0.html",
              "isBasedOn": "Eden: World Builder by Ari Ronen",
              "offers": {"@type": "Offer", "price": "0", "priceCurrency": "USD"},
              "sameAs": [GITHUB, DISCORD]}
    elif rel == "developers.html":
        ld = {"@context": "https://schema.org", "@type": "SoftwareSourceCode", "name": SITE_NAME + " source",
              "description": meta["description"], "url": url, "codeRepository": GITHUB,
              "programmingLanguage": ["C++", "Objective-C++", "Lua"],
              "license": "https://www.gnu.org/licenses/gpl-3.0.html"}
    if ld:
        out.append('<script type="application/ld+json">%s</script>' % json.dumps(ld, separators=(",", ":")).replace("</", "<\\/"))
    return "\n".join(out)


def sitemap_xml(pages):
    urls = [p for p in pages if p != "404.html"]
    day = datetime.date.today().isoformat()
    body = "".join("<url><loc>%s</loc><lastmod>%s</lastmod></url>\n" % (BASE_URL + ("" if p == "index.html" else p), day)
                   for p in sorted(urls, key=lambda p: (p != "index.html", p)))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">\n'
            + body + "</urlset>\n")


def render(layout, meta, body, root, version, updated, news, rel="index.html"):
    toc = meta.get("toc", "")
    for k, v in (("version", version), ("root", root), ("discord", DISCORD), ("github", GITHUB)):
        body = body.replace("{{%s}}" % k, v)  # page text may use these
    page = {"title": html.escape(meta["title"]), "description": html.escape(meta["description"]),
            "root": root, "version": version, "tabs": tabs_html(meta["nav"], root), "body": body,
            "toc": toc, "news": news, "seo": seo_head(meta, rel), "discord": DISCORD, "github": GITHUB, "updated": updated}
    return re.sub(r"\{\{(\w+)\}\}", lambda m: str(page[m.group(1)]), layout)


def doc_link(doc_page, md_path, warn):
    """Link rewriter for one imported doc: repo path -> site page when rendered, else GitHub."""
    root = "../" * (doc_page.count("/"))

    def link(url):
        if re.match(r"^[a-z][a-z0-9+.-]*:", url) or url.startswith(("#", "/")):
            return url
        path, _, frag = url.partition("#")
        repo = posixpath.normpath(posixpath.join(posixpath.dirname(md_path), path))
        frag = "#" + frag if frag else ""
        if repo in SITE_PAGE_OF_MD:
            return root + SITE_PAGE_OF_MD[repo] + frag
        if repo.startswith("..") or not (REPO / repo).exists():
            warn.append("%s: link to a missing repo path: %s" % (md_path, url))
            return GITHUB
        return (GH_TREE if (REPO / repo).is_dir() else GH_BLOB) + repo + frag
    return link


def doc_pages(layout, version, updated, news, warn):
    """Render each docs/ file in DOCS; returns {page: html}."""
    out = {}
    for doc, page, nav, section, label, desc in DOCS:
        md_path = "docs/%s.md" % doc
        text = (REPO / md_path).read_text(encoding="utf-8")
        title = re.match(r"#\s+(.*)", text).group(1)
        title = re.sub(r"`", "", title)
        root = "../" * page.count("/")
        body, heads = mdlite.convert(text, doc_link(page, md_path, warn))
        crumbs = '<p class="crumbs"><a href="%s%s.html">%s</a> › %s</p>' % (
            root, "modding" if nav == "modding" else "developers", section, html.escape(label))
        src = ('<p class="doc-src">Generated from <a href="%s%s">%s</a> · <a href="%sedit/main/%s">edit on GitHub</a></p>'
               % (GH_BLOB, md_path, md_path, GITHUB + "/", md_path))
        sib = "".join('<li class="on">%s</li>' % html.escape(d[4]) if d[1] == page else
                      '<li><a href="%s%s">%s</a></li>' % (root, d[1], html.escape(d[4]))
                      for d in DOCS if d[2] == nav)
        here = "".join('<li><a href="#%s">%s</a></li>' % (i, html.escape(t)) for lvl, i, t in heads if lvl == 2)
        toc = ('<section class="box toc"><h2 class="box__t">%s</h2><ol>%s</ol></section>' % (section, sib)
               + ('<section class="box toc"><h2 class="box__t">On this page</h2><ol>%s</ol></section>' % here if here else ""))
        meta = {"title": "%s - Eden: Community Edition" % re.sub(r"\s*\(.*?\)", "", title), "nav": nav, "description": desc, "toc": toc}
        full = "%s<h1>%s</h1>\n%s\n%s" % (crumbs, html.escape(title), body, src)
        out[page] = render(layout, meta, full, root, version, updated, news, page)
    return out


def build(out):
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    shutil.copytree(STATIC, out / "static")
    layout = (TPL / "layout.html").read_text(encoding="utf-8")
    version = (REPO / "VERSION.txt").read_text().strip()
    updated = datetime.date.today().strftime("%-d %b %Y")
    news = news_html()
    pages = []
    for path in sorted(SRC.rglob("*.html")):
        rel = path.relative_to(SRC)
        meta, body = read_page(path)
        # 404.html is served from any depth, so it links from the site's absolute base path
        root = "/" + BASE_URL.split("/", 3)[3] if rel.name == "404.html" else "../" * (len(rel.parts) - 1)
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(render(layout, meta, body, root, version, updated, news, rel.as_posix()), encoding="utf-8")
        pages.append(rel.as_posix())
    warn = []
    for page, text in doc_pages(layout, version, updated, news, warn).items():
        (out / page).parent.mkdir(parents=True, exist_ok=True)
        (out / page).write_text(text, encoding="utf-8")
        pages.append(page)
    (out / "sitemap.xml").write_text(sitemap_xml(pages), encoding="utf-8")
    (out / "robots.txt").write_text("User-agent: *\nAllow: /\n\nSitemap: %ssitemap.xml\n" % BASE_URL, encoding="utf-8")
    for w in warn:
        print("site:", w)
    if warn and "--check" in sys.argv:
        sys.exit(1)
    return pages


# ------------------------------------------------------------------ --check
def check(out, pages):
    bad = []
    attr = re.compile(r'(?:href|src)="([^"]+)"')
    for rel in pages:
        text = (out / rel).read_text(encoding="utf-8")
        for t in PRIVATE_TEXT:
            if t in text:
                bad.append(f"{rel}: private text {t!r}")
        for m in attr.finditer(text):
            url = m.group(1).split("#")[0].split("?")[0]
            frag = m.group(1).partition("#")[2]
            if frag and not re.match(r"^[a-z][a-z0-9+.-]*:", m.group(1)) and not url.startswith("//"):
                tgt = (out / rel) if not url else ((out / rel).parent / url if not url.startswith("/") else None)
                if tgt is not None and tgt.suffix == ".html" and tgt.exists() and \
                        'id="%s"' % frag not in tgt.read_text(encoding="utf-8"):
                    bad.append(f"{rel}: broken anchor {m.group(1)}")
            if not url or re.match(r"^[a-z][a-z0-9+.-]*:", url) or url.startswith("//"):
                if url.startswith("http://"):
                    bad.append(f"{rel}: insecure link {url}")
                continue
            if url.startswith("/"):  # absolute path: only the 404 page, rooted at the base path
                base = "/" + BASE_URL.split("/", 3)[3]
                if not url.startswith(base):
                    bad.append(f"{rel}: absolute link outside the base path: {url}")
                    continue
                target = out / url[len(base):]
            else:
                target = (out / rel).parent / url
            if not target.resolve().exists():
                bad.append(f"{rel}: broken link {m.group(1)}")
        for img in re.findall(r"<img\b[^>]*>", text):
            for a in ("width=", "height=", "alt="):
                if a not in img:
                    bad.append(f"{rel}: <img> without {a[:-1]}: {img[:70]}")
    # SEO: unique titles, sane descriptions, canonical, sitemap, robots, OG image
    titles = {}
    for rel in pages:
        text = (out / rel).read_text(encoding="utf-8")
        t = re.search(r"<title>(.*?)</title>", text, re.S).group(1)
        d = re.search(r'<meta name="description" content="(.*?)">', text, re.S).group(1)
        if t in titles:
            bad.append(f"{rel}: title duplicates {titles[t]}")
        titles[t] = rel
        if len(t) > 70:
            bad.append(f"{rel}: title over 70 characters ({len(t)})")
        if not 50 <= len(d) <= 175:
            bad.append(f"{rel}: description length {len(d)} outside 50-175")
        if rel != "404.html" and '<link rel="canonical" href="%s">' % (BASE_URL + ("" if rel == "index.html" else rel)) not in text:
            bad.append(f"{rel}: missing or wrong canonical")
        if rel != "404.html" and 'property="og:image"' not in text:
            bad.append(f"{rel}: no og:image")
        if rel == "404.html" and "noindex" not in text:
            bad.append("404.html: not noindex")
    sm = (out / "sitemap.xml").read_text(encoding="utf-8")
    for rel in pages:
        if rel != "404.html" and (BASE_URL + ("" if rel == "index.html" else rel)) not in sm:
            bad.append(f"sitemap.xml lacks {rel}")
    if "Sitemap: " + BASE_URL + "sitemap.xml" not in (out / "robots.txt").read_text(encoding="utf-8"):
        bad.append("robots.txt does not name the sitemap")
    if not (out / OG_IMAGE).exists():
        bad.append(f"og image {OG_IMAGE} missing")
    for ld in re.findall(r'<script type="application/ld\+json">(.*?)</script>', "".join((out / r).read_text(encoding="utf-8") for r in pages), re.S):
        try:
            json.loads(ld.replace("<\\/", "</"))
        except ValueError as e:
            bad.append(f"invalid JSON-LD: {e}")
    # budget: home HTML + CSS + fonts it uses
    css = (out / "static/css/eden-site.css").read_bytes()
    fonts = sum(p.stat().st_size for p in (out / "static/fonts").glob("*.woff2"))
    home = (out / "index.html").stat().st_size + len(css) + fonts
    print(f"site: home page weight {home / 1024:.1f} KB of {HOME_BUDGET // 1024} KB (HTML + CSS + fonts)")
    if home > HOME_BUDGET:
        bad.append(f"home page over budget: {home} > {HOME_BUDGET}")
    js = sum(p.stat().st_size for p in out.rglob("*.js"))
    if js > JS_BUDGET:
        bad.append(f"JS {js} bytes over the {JS_BUDGET} budget")
    for p in (out / "static").rglob("*"):
        if p.is_file() and p.suffix in (".woff", ".ttf", ".otf"):
            bad.append(f"{p.name}: fonts ship as WOFF2 only")
    # no third-party requests anywhere (plan section 4)
    for rel in pages:
        text = (out / rel).read_text(encoding="utf-8")
        for m in re.finditer(r'<(?:script|link|img|iframe)\b[^>]*(?:src|href)="(https?://[^"]+)"', text):
            if "rel=\"stylesheet\"" in m.group(0) or m.group(0).startswith(("<script", "<img", "<iframe")):
                bad.append(f"{rel}: third-party resource {m.group(1)}")
    # the publish script must not delete pages.yml and must sync site/
    # (the script is private and not synced to edencom, so CI there skips this check)
    pubf = REPO / "publish-to-edencom.sh"
    pub = pubf.read_text(encoding="utf-8") if pubf.exists() else None
    if pub is None:
        pass
    elif re.search(r"git rm[^\n]*pages\.yml", pub):
        bad.append("publish-to-edencom.sh still removes pages.yml")
    if pub is not None and '"$SRC/site/"' not in pub:
        bad.append("publish-to-edencom.sh does not sync site/")
    if not ((HERE / "pages.yml").exists() or (REPO / ".github" / "workflows" / "pages.yml").exists()):
        bad.append("site/pages.yml (or .github/workflows/pages.yml on edencom) is missing")
    for b in bad:
        print("site:", b)
    print(f"site: {len(pages)} pages, {len(bad)} problem(s)")
    return not bad


def main():
    if "--check" in sys.argv:
        with tempfile.TemporaryDirectory(prefix="eden-site-") as tmp:
            out = pathlib.Path(tmp) / "out"
            sys.exit(0 if check(out, build(out)) else 1)
    pages = build(OUT)
    print(f"site: built {len(pages)} pages into {OUT.relative_to(REPO)}")


if __name__ == "__main__":
    main()
