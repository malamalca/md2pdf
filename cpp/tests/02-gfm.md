# GitHub-flavoured markdown

The parser runs with `MD_DIALECT_GITHUB`, so these should all render.

## Tables

| Fix | Symptom before | Symptom after |
|-----|----------------|---------------|
| `cdpCall` recv | md2pdf spins at 100% CPU forever | returns the reply |
| handshake read | 30 s stall every run | instant |
| `STARTF_USESTDHANDLES` | stderr log always empty | Chrome errors captured |
| profile cleanup | `%TEMP%` fills with dirs | removed after exit |

## Alignment

| Left | Centre | Right |
|:-----|:------:|------:|
| a    |   b    |     c |
| long cell value | x | 1234.56 |

## Strikethrough and task lists

~~This text is struck out.~~

- [x] fix the busy-wait in `cdpCall`
- [x] reassemble fragmented WebSocket frames
- [ ] port the config file reader

## Autolink

Plain URL: https://github.com/mity/md4c

## Nested structures

1. Outer item

   > quote inside a list item

2. Another outer item

   ```text
   fenced code inside a list item
   ```
