# Edge cases

## Literal template placeholder

The next line contains the literal text {{CONTENT}} — before the fix, substituting
it back into the template restarted the search at offset 0 and looped forever,
growing the document until the process died.

Also {{HEADER}}, {{FOOTER}} and {{MARGIN_TOP}} as plain text.

## Quotes and escapes that go through jsonEscape

Straight quotes: "double" and 'single'.
Backslashes: C:\Users\Miha Nahtigal\AppData — and a lone trailing one: \
Control-ish text: tab→	←tab, and a literal \n sequence.

## SVG fragment references

<svg width="120" height="30" xmlns="http://www.w3.org/2000/svg">
  <defs>
    <linearGradient id="g1"><stop offset="0%" stop-color="#0366d6"/><stop offset="100%" stop-color="#28a745"/></linearGradient>
  </defs>
  <rect width="120" height="30" fill="url(#g1)"/>
</svg>

The `url(#g1)` above must be left alone — it is an in-document reference, not a
file path, and must not be reported as missing media.

## Missing media

![](does-not-exist.png)

That should print one warning and still produce a PDF.

## Very long unbroken token

aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa

## Empty table cells

| a |  | c |
|---|---|---|
|   | b |   |
