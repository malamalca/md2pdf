# -*- coding: utf-8 -*-
"""Pretvornik .docx -> Markdown, ki ohrani naslove, sezname, tabele, slike in poudarke."""
import io, os, re, sys, zipfile
import xml.etree.ElementTree as ET

W = "{http://schemas.openxmlformats.org/wordprocessingml/2006/main}"
R = "{http://schemas.openxmlformats.org/officeDocument/2006/relationships}"
A = "{http://schemas.openxmlformats.org/drawingml/2006/main}"
V = "{urn:schemas-microsoft-com:vml}"
PKG_REL = "{http://schemas.openxmlformats.org/package/2006/relationships}"

HEADING_MAP = {
    "Naslov1": 1, "Naslov2": 2, "Naslov3": 3, "Naslov4": 4,
    "Heading1": 1, "Heading2": 2, "Heading3": 3, "Heading4": 4,
    "Naslov": 1, "Title": 1,
}


def norm_style(s):
    return re.sub(r"[\s\-_]", "", s or "")


def esc(t):
    # minimalno beženje, da se markdown ne razsuje
    return t.replace("|", "\\|")


class Conv:
    def __init__(self, path, media_dir, media_prefix):
        self.z = zipfile.ZipFile(path)
        self.media_dir = media_dir
        self.media_prefix = media_prefix
        self.rels = self._rels()
        self.styles = self._styles()
        self.numbering = self._numbering()
        self.saved = {}
        os.makedirs(media_dir, exist_ok=True)

    def _rels(self):
        rels = {}
        try:
            root = ET.fromstring(self.z.read("word/_rels/document.xml.rels"))
        except KeyError:
            return rels
        for rel in root:
            rels[rel.get("Id")] = rel.get("Target")
        return rels

    def _styles(self):
        """styleId -> ime sloga (za prepoznavanje naslovov tudi po imenu)."""
        out = {}
        try:
            root = ET.fromstring(self.z.read("word/styles.xml"))
        except KeyError:
            return out
        for st in root.iter(W + "style"):
            sid = st.get(W + "styleId")
            nm = st.find(W + "name")
            out[sid] = nm.get(W + "val") if nm is not None else sid
        return out

    def _numbering(self):
        """numId -> {ilvl: 'bullet'|'decimal'}"""
        out = {}
        try:
            root = ET.fromstring(self.z.read("word/numbering.xml"))
        except KeyError:
            return out
        abstract = {}
        for ab in root.iter(W + "abstractNum"):
            aid = ab.get(W + "abstractNumId")
            lvls = {}
            for lvl in ab.iter(W + "lvl"):
                ilvl = lvl.get(W + "ilvl")
                fmt = lvl.find(W + "numFmt")
                lvls[ilvl] = fmt.get(W + "val") if fmt is not None else "bullet"
            abstract[aid] = lvls
        for num in root.iter(W + "num"):
            nid = num.get(W + "numId")
            ab = num.find(W + "abstractNumId")
            if ab is not None:
                out[nid] = abstract.get(ab.get(W + "val"), {})
        return out

    # ---------- slike ----------
    def save_image(self, rid):
        target = self.rels.get(rid)
        if not target:
            return None
        if rid in self.saved:
            return self.saved[rid]
        name = os.path.basename(target)
        arc = "word/" + target.replace("\\", "/") if not target.startswith("word/") else target
        arc = arc.replace("word/../", "")
        try:
            data = self.z.read(arc)
        except KeyError:
            try:
                data = self.z.read("word/" + os.path.basename(target))
            except KeyError:
                return None
        out = os.path.join(self.media_dir, name)
        with open(out, "wb") as f:
            f.write(data)
        ref = self.media_prefix + "/" + name
        self.saved[rid] = ref
        return ref

    def images_in(self, el):
        refs = []
        for blip in el.iter(A + "blip"):
            rid = blip.get(R + "embed") or blip.get(R + "link")
            ref = self.save_image(rid) if rid else None
            if ref:
                refs.append(ref)
        for imd in el.iter(V + "imagedata"):
            rid = imd.get(R + "id")
            ref = self.save_image(rid) if rid else None
            if ref:
                refs.append(ref)
        return refs

    # ---------- besedilo ----------
    def run_text(self, run):
        parts = []
        for ch in run:
            if ch.tag == W + "t":
                parts.append(ch.text or "")
            elif ch.tag == W + "tab":
                parts.append("\t")
            elif ch.tag in (W + "br", W + "cr"):
                parts.append("\n")
            elif ch.tag == W + "noBreakHyphen":
                parts.append("-")
        return "".join(parts)

    def run_fmt(self, run):
        rpr = run.find(W + "rPr")
        if rpr is None:
            return (False, False)
        b = rpr.find(W + "b")
        i = rpr.find(W + "i")
        return (b is not None and b.get(W + "val") not in ("0", "false"),
                i is not None and i.get(W + "val") not in ("0", "false"))

    def emit(self, chunks):
        """chunks: [(besedilo, bold, italic)] -> markdown z zlitimi sosednjimi poudarki"""
        merged = []
        for txt, b, i in chunks:
            if not txt:
                continue
            if merged and merged[-1][1] == b and merged[-1][2] == i:
                merged[-1][0] += txt
            else:
                merged.append([txt, b, i])
        out = []
        for txt, b, i in merged:
            if not txt.strip() or not (b or i):
                out.append(txt)
                continue
            lead = len(txt) - len(txt.lstrip())
            trail = len(txt) - len(txt.rstrip())
            core = txt.strip()
            mark = "***" if (b and i) else ("**" if b else "*")
            out.append(txt[:lead] + mark + core + mark + (txt[len(txt) - trail:] if trail else ""))
        return "".join(out)

    def para_text(self, p, in_table=False):
        chunks = []
        for child in p:
            if child.tag == W + "r":
                b, i = self.run_fmt(child)
                chunks.append((self.run_text(child), b, i))
            elif child.tag == W + "hyperlink":
                inner = self.emit([(self.run_text(r),) + self.run_fmt(r)
                                   for r in child.findall(W + "r")])
                rid = child.get(R + "id")
                tgt = self.rels.get(rid) if rid else None
                chunks.append(("[%s](%s)" % (inner.strip(), tgt) if tgt and inner.strip() else inner,
                               False, False))
        txt = self.emit(chunks)
        imgs = self.images_in(p)
        for ref in imgs:
            txt += ("" if not txt.strip() else " ") + "![](%s)" % ref
        if in_table:
            txt = txt.replace("\n", "<br>").replace("\t", " ")
        return txt

    def fmt_run(self, run):
        t = self.run_text(run)
        if not t.strip():
            return t
        rpr = run.find(W + "rPr")
        bold = italic = False
        if rpr is not None:
            b = rpr.find(W + "b")
            i = rpr.find(W + "i")
            bold = b is not None and b.get(W + "val") not in ("0", "false")
            italic = i is not None and i.get(W + "val") not in ("0", "false")
        lead = len(t) - len(t.lstrip())
        trail = len(t) - len(t.rstrip())
        core = t.strip()
        if bold and italic:
            core = "***%s***" % core
        elif bold:
            core = "**%s**" % core
        elif italic:
            core = "*%s*" % core
        return t[:lead] + core + t[len(t) - trail:] if trail else t[:lead] + core

    def para_style(self, p):
        ppr = p.find(W + "pPr")
        if ppr is None:
            return None, None, None
        sid = None
        ps = ppr.find(W + "pStyle")
        if ps is not None:
            sid = ps.get(W + "val")
        numid = ilvl = None
        npr = ppr.find(W + "numPr")
        if npr is not None:
            n = npr.find(W + "numId")
            l = npr.find(W + "ilvl")
            numid = n.get(W + "val") if n is not None else None
            ilvl = l.get(W + "val") if l is not None else "0"
        return sid, numid, ilvl

    def heading_level(self, sid):
        if not sid:
            return 0
        key = norm_style(sid)
        if key in HEADING_MAP:
            return HEADING_MAP[key]
        name = norm_style(self.styles.get(sid, ""))
        return HEADING_MAP.get(name, 0)

    # ---------- tabele ----------
    def table_md(self, tbl):
        rows = []
        for tr in tbl.findall(W + "tr"):
            cells = []
            for tc in tr.findall(W + "tc"):
                txt = " ".join(
                    self.para_text(p, in_table=True).strip()
                    for p in tc.findall(W + "p")
                ).strip()
                txt = re.sub(r"\s+", " ", txt)
                cells.append(esc(txt))
                # vodoravno združene celice ponovimo, da ostane mreža pravokotna
                tcpr = tc.find(W + "tcPr")
                if tcpr is not None:
                    gs = tcpr.find(W + "gridSpan")
                    if gs is not None:
                        for _ in range(int(gs.get(W + "val", "1")) - 1):
                            cells.append("")
            rows.append(cells)
        if not rows:
            return ""
        width = max(len(r) for r in rows)
        rows = [r + [""] * (width - len(r)) for r in rows]
        head = rows[0]
        if not any(c.strip() for c in head):
            head = ["" for _ in range(width)]
        out = ["| " + " | ".join(head) + " |",
               "|" + "|".join([" --- "] * width) + "|"]
        for r in rows[1:]:
            out.append("| " + " | ".join(r) + " |")
        return "\n".join(out)

    # ---------- glavni sprehod ----------
    def convert(self):
        root = ET.fromstring(self.z.read("word/document.xml"))
        body = root.find(W + "body")
        lines = []
        prev_blank = True
        for el in body:
            if el.tag == W + "p":
                sid, numid, ilvl = self.para_style(el)
                txt = self.para_text(el)
                lvl = self.heading_level(sid)
                if lvl:
                    if lines and lines[-1] != "":
                        lines.append("")
                    lines.append("#" * min(lvl, 6) + " " + txt.strip())
                    lines.append("")
                    prev_blank = True
                    continue
                if numid:
                    fmt = self.numbering.get(numid, {}).get(ilvl or "0", "bullet")
                    marker = "1." if fmt not in ("bullet",) else "-"
                    indent = "  " * int(ilvl or 0)
                    lines.append("%s%s %s" % (indent, marker, txt.strip()))
                    prev_blank = False
                    continue
                if not txt.strip():
                    if not prev_blank:
                        lines.append("")
                        prev_blank = True
                    continue
                lines.append(txt.rstrip())
                prev_blank = False
            elif el.tag == W + "tbl":
                if lines and lines[-1] != "":
                    lines.append("")
                lines.append(self.table_md(el))
                lines.append("")
                prev_blank = True
        return "\n".join(lines).rstrip() + "\n"


def core_props(z):
    try:
        root = ET.fromstring(z.read("docProps/core.xml"))
    except KeyError:
        return {}
    ns = {
        "dc": "http://purl.org/dc/elements/1.1/",
        "cp": "http://schemas.openxmlformats.org/package/2006/metadata/core-properties",
        "dcterms": "http://purl.org/dc/terms/",
    }
    out = {}
    for key, path in [("title", "dc:title"), ("subject", "dc:subject"),
                      ("creator", "dc:creator"), ("lastModifiedBy", "cp:lastModifiedBy"),
                      ("created", "dcterms:created"), ("modified", "dcterms:modified")]:
        el = root.find(path, ns)
        if el is not None and (el.text or "").strip():
            out[key] = el.text.strip()
    return out


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    media_dir = os.path.join(os.path.dirname(dst), "media")
    conv = Conv(src, media_dir, "media")
    md = conv.convert()
    props = core_props(conv.z)
    fm = ["---"]
    for k, v in props.items():
        fm.append("%s: \"%s\"" % (k, v.replace('"', "'")))
    fm.append("source: \"%s\"" % os.path.basename(src))
    fm.append("---")
    with io.open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(fm) + "\n\n" + md)
    print("napisano:", dst, len(md), "znakov;", len(conv.saved), "slik")
