# Basic formatting

A paragraph with **bold**, *italic*, ***both***, `inline code`, and a
[link to example](https://example.com).

## Lists

- first item
- second item
  - nested item
  - another nested
- third item

1. numbered one
2. numbered two
3. numbered three

## Blockquote

> Chrome renders this through CDP's `Page.printToPDF`.
> The second line of the same quote.

## Code block

```cpp
static bool wsRecvN(SOCKET sock, unsigned char* buf, size_t n, uint64_t deadline) {
    size_t off = 0;
    while (off < n) {
        int r = recv(sock, (char*)buf + off, (int)(n - off), 0);
        if (r == 0) return false;
        off += r;
    }
    return true;
}
```

---

### Heading level 3

Final paragraph after a horizontal rule.
