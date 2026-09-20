# -*- coding: utf-8 -*-
"""PDF -> Markdown za PZI risbe: besedilo po straneh, slike za strani brez besedila."""
import io, os, re, sys
import pdfplumber
import pypdfium2 as pdfium

if len(sys.argv) >= 3:
    SRC, DST = sys.argv[1], sys.argv[2]
else:
    SRC, DST = "vnos.pdf", "izhod.md"
MEDIA = "media"
os.makedirs(MEDIA, exist_ok=True)

LISTEL = re.compile(r"^(SV PROJEKT|Samo Oman s\.p\..*|Dobeno 99,.*|vojko@voprojekt\.si)\s*$")
H1 = re.compile(r"^(\d+)\.\s+([A-ZČĆŠŽĐ][A-ZČĆŠŽĐ .,'’–/-]{3,60})$")
H2 = re.compile(r"^(\d+(?:\.\d+)+)\.?\s+([A-Za-zČĆŠŽčćšž0-9].{2,60})$")
PUA = re.compile(r"[\uf000-\uf8ff]")

def clean_lines(text):
    lines = []
    for ln in (text or "").split("\n"):
        ln = PUA.sub("", ln).strip()
        if not ln or LISTEL.match(ln):
            continue
        lines.append(ln)
    return lines

def to_md(lines, page_no, is_drawing):
    out = []
    i = 0
    while i < len(lines):
        ln = lines[i]
        # "4." + naslednja vrstica v gl. črkah -> H1 (razdvojena v PDF-u)
        if re.match(r"^\d+\.?$", ln) and i + 1 < len(lines) \
           and re.match(r"^[A-ZČĆŠŽĐ][A-ZČĆŠŽĐ .,'’–/-]{3,60}$", lines[i + 1]):
            out.append("# %s" % lines[i + 1].strip())
            i += 2
            continue
        if ln.startswith("KAZALO VSEBINE"):
            out.append("# " + ln)
            i += 1
            continue
        m1 = H1.match(ln)
        m2 = H2.match(ln)
        if m1 and not re.search(r"\d", m1.group(2)):
            out.append("# %s" % m1.group(2).strip())
        elif m2:
            depth = min(m2.group(1).count(".") + 1, 3)
            out.append("#" * depth + " %s" % (m2.group(1) + " " + m2.group(2).strip()))
        else:
            out.append(ln)
        i += 1
    return out

# rasteriziraj strani brez besedila / s tabele
def render_page(pdf, idx):
    page = pdf[idx]
    img = page.render(scale=150/72).to_pil()
    p = os.path.join(MEDIA, "page_%d.png" % (idx + 1))
    img.save(p)
    return "media/page_%d.png" % (idx + 1)

pl = pdfplumber.open(SRC)
pf = pdfium.PdfDocument(SRC)
parts = []
fm = ["---", 'source: "%s"' % SRC, "strani: %d" % len(pl.pages), "---"]
parts.append("\n".join(fm))

for i, page in enumerate(pl.pages):
    n = i + 1
    text = (page.extract_text() or "").strip()
    lines = clean_lines(text)
    if not lines:
        # stran brez besedila -> slika
        p = render_page(pf, i)
        parts.append("\n## Stran %d\n\n![](%s)\n" % (n, p))
        continue
    if n <= 11:
        # tekoči dokument (kazalo + tehnično poročilo + popis)
        block = to_md(lines, n, False)
        parts.append("\n".join(block))
    else:
        # risbene strani -> vsaka stran ločena sekcija
        head = "### Stran %d" % n
        parts.append("\n%s\n\n%s\n" % (head, "\n".join(to_md(lines, n, True))))

with io.open(DST, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n\n".join(parts))
print("napisano:", DST)
