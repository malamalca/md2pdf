<?php
declare(strict_types=1);

namespace App\Core;

use HeadlessChromium\BrowserFactory;

/**
 * Headless Chrome PDF engine via chrome-php/chrome library.
 */
class PdfEngine
{
    private const PROFILE_PREFIX = 'md2pdf-chrome-';

    /** @var array<string, mixed> */
    private array $options = [];

    /** @var string */
    private string $error = '';

    /** @var string[] Missing media paths */
    public static array $missingMedia = [];

    /** @param array<string, mixed> $options */
    public function __construct(array $options)
    {
        $defaults = [
            'binary' => 'chrome',
            'timeout' => 120,
            'page-size' => 'A4',
            'orientation' => 'portrait',
            // Margins in mm for chrome-php
            'margin-top' => 19,
            'margin-right' => 15,
            'margin-bottom' => 25,
            'margin-left' => 15,
        ];

        $this->options = array_replace($defaults, $options);
    }

    public function __destruct() {}

    /** @return string */
    public function getError(): string
    {
        return $this->error;
    }

    /**
     * Resolve relative media paths to file:/// URLs (Windows-compatible).
     */
    public static function resolveMediaPaths(string $html, string $sourceDir): string
    {
        $sourceDir = rtrim($sourceDir, '/\\') . DS;

        $html = preg_replace_callback(
            '/(<img[^>]*src=["\'])([^"\']+)(["\'])/i',
            static fn ($m) => self::resolveImageTag($m, $sourceDir),
            $html
        );

        $html = preg_replace_callback(
            '#(url\(["\']?)([^"\')]+)(["\']?\))#i',
            static fn ($m) => $m[1] . self::resolvePath($m[2], $sourceDir) . $m[3],
            $html
        );

        return $html;
    }

    /** @return string */
    private static function resolveImageTag(array $m, string $sourceDir): string
    {
        $tagStart = $m[1];
        $quote = $m[3];

        // Support markdown-style width/alignment: ![](path "50% center") or ![](path|50%|center)
        $rawSrc = trim($m[2]);
        $parsedSrc = $rawSrc;
        $width = null;
        $align = null;

        // Parse pipe syntax: path|50%|center or path|float:left|50% etc.
        if (str_contains($rawSrc, '|')) {
            $parts = array_map('trim', explode('|', $rawSrc));
            $parsedSrc = array_shift($parts);
            foreach ($parts as $part) {
                $lp = strtolower($part);
                if (preg_match('/^\d+(?:\.\d+)?%(?:\/\d+)?$/', $part)) {
                    $width = $part;
                } elseif (in_array($lp, ['left', 'center', 'right', 'float:left', 'float:right'])) {
                    $align = $lp;
                }
            }
        }

        // Resolve the actual image path (without pipe params)
        $src = self::resolvePath($parsedSrc, $sourceDir);

        // Build style attribute
        $style = '';
        if ($width) {
            $style .= "width:{$width};";
        }
        if ($align) {
            if (str_starts_with($align, 'float:')) {
                $dir = substr($align, 6);
                $style .= "float:{$dir};margin-{$dir}:0;margin-" . ($dir === 'left' ? 'right' : 'left') . ":12px;";
            } elseif ($align === 'center') {
                $style .= "display:block;margin-left:auto;margin-right:auto;";
            } elseif ($align === 'right') {
                $style .= "display:block;margin-left:auto;margin-right:0;";
            } else {
                $style .= "display:block;margin-left:0;margin-right:auto;";
            }
        }

        if ($style) {
            // Insert style attribute after src quote, before rest of tag
            return $tagStart . $src . $quote . ' style="' . $style . '"';
        }

        return $tagStart . $src . $quote;
    }

    /** @return string */
    private static function resolvePath(string $path, string $sourceDir): string
    {
        if (str_starts_with($path, 'data:') || str_starts_with($path, 'http://') ||
            str_starts_with($path, 'https://') || str_starts_with($path, '//') ||
            str_starts_with($path, 'file:///')) {
            return $path;
        }
        if (strlen($path) >= 3 && ctype_alpha($path[0]) && $path[1] === ':' && ($path[2] === '/' || $path[2] === '\\')) {
            // Convert to proper file:/// URL
            return 'file:///' . str_replace('\\', '/', $path);
        }

        $resolved = realpath($sourceDir . $path);
        if ($resolved === false) {
            self::$missingMedia[] = $path;
            return $path;
        }

        return 'file:///' . str_replace('\\', '/', $resolved);
    }

    /**
     * Build HTML document from template.
     */
    public function buildDocument(string $content, array $replacements = [], ?string $sourceDir = null): string
    {
        $template = file_get_contents(TEMPLATES . 'pdf.html');

        $defaults = [
            '{{PAGE_SIZE}}' => (string)$this->options['page-size'],
            '{{ORIENTATION}}' => strtolower((string)$this->options['orientation']),
            '{{MARGIN_TOP}}' => (float)$this->options['margin-top'],
            '{{MARGIN_RIGHT}}' => (float)$this->options['margin-right'],
            '{{MARGIN_BOTTOM}}' => (float)$this->options['margin-bottom'],
            '{{MARGIN_LEFT}}' => (float)$this->options['margin-left'],
            '{{HEADER_HEIGHT}}' => 0,
            '{{FOOTER_HEIGHT}}' => 0,
            '{{HEADER}}' => '',
            '{{FOOTER}}' => '',
            '{{CONTENT}}' => $content,
        ];

        $document = strtr($template, array_replace($defaults, $replacements));

        if ($sourceDir !== null) {
            $document = self::resolveMediaPaths($document, $sourceDir);
        }

        return $document;
    }

    /**
     * Convert HTML to PDF via chrome-php/chrome.
     */
    public function convert(string $html, string $filename, string $headerTemplate = '', string $footerTemplate = ''): bool
    {
        $this->cleanupProfiles();

        $userDataDir = TMP . self::PROFILE_PREFIX . uniqid('', true);
        $sourceFile = null;

        if (file_exists($filename)) {
            unlink($filename);
        }

        try {
            // Build Chrome options
            $factory = new BrowserFactory((string)$this->options['binary']);
            $browser = $factory->createBrowser([
                'noSandbox' => true,
                'customFlags' => [
                    '--disable-extensions',
                    '--hide-scrollbars',
                    '--no-first-run',
                    '--no-default-browser-check',
                    '--allow-file-access-from-files',
                ],
                'userDataDir' => $userDataDir,
                'startupTimeout' => (int)($this->options['timeout'] ?? 120),
            ]);

            // Write HTML to temp file (data URI blocks image loading)
            $sourceFile = TMP . uniqid('md2pdf-cdp-', true) . '.html';
            file_put_contents($sourceFile, $html);

            // Create page and navigate via file:// URL
            $page = $browser->createPage();
            $fileUrl = 'file:///' . str_replace('\\', '/', $sourceFile);
            $page->navigate($fileUrl)->waitForNavigation();

            // Build PDF options (flat CDP names)
            $pdfOptions = [
                'printBackground' => true,
                'displayHeaderFooter' => ($headerTemplate !== '' || $footerTemplate !== ''),
                'preferCSSPageSize' => false,
                'paperWidth' => $this->toInches((string)$this->options['page-size'], false),
                'paperHeight' => $this->toInches((string)$this->options['page-size'], true),
                'marginTop' => $this->mmToInch((float)$this->options['margin-top']),
                'marginRight' => $this->mmToInch((float)$this->options['margin-right']),
                'marginBottom' => $this->mmToInch((float)$this->options['margin-bottom']),
                'marginLeft' => $this->mmToInch((float)$this->options['margin-left']),
            ];

            if (strtolower((string)$this->options['orientation']) === 'landscape') {
                $pdfOptions['landscape'] = true;
            }

            // When displayHeaderFooter is on, Chrome falls back to its built-in
            // template (source URL, page numbers) for whichever side is left
            // unset, so always send both and blank out the unused one.
            if ($pdfOptions['displayHeaderFooter']) {
                $pdfOptions['headerTemplate'] = $headerTemplate !== '' ? $headerTemplate : '<span></span>';
                $pdfOptions['footerTemplate'] = $footerTemplate !== '' ? $footerTemplate : '<span></span>';
            }

            // Generate PDF
            $page->pdf($pdfOptions)->saveToFile($filename);

            $browser->close();
            @unlink($sourceFile);
        } catch (\Throwable $e) {
            $this->error = $e->getMessage();
            if ($sourceFile !== null) @unlink($sourceFile);
            return false;
        } finally {
            $this->removeDirectory($userDataDir);
        }

        return true;
    }

    /** @return float */
    private function toInches(string $pageSize, bool $isHeight): float
    {
        $sizes = [
            'A4' => [8.27, 11.69],
            'A3' => [11.69, 16.54],
            'A5' => [5.83, 8.27],
            'Letter' => [8.5, 11.0],
            'Legal' => [8.5, 14.0],
        ];
        $upper = strtoupper($pageSize);
        if (isset($sizes[$upper])) {
            return $isHeight ? $sizes[$upper][1] : $sizes[$upper][0];
        }
        return $isHeight ? 11.69 : 8.27;
    }

    /** @return float */
    private function mmToInch(float $mm): float
    {
        return round($mm / 25.4, 3);
    }

    private function cleanupProfiles(): void
    {
        $folders = glob(TMP . self::PROFILE_PREFIX . '*', GLOB_ONLYDIR);
        if ($folders === false) return;
        foreach ($folders as $f) {
            if (filemtime($f) < time() - 3600) {
                $this->removeDirectory($f);
            }
        }
    }

    private function removeDirectory(string $path): void
    {
        if (!is_dir($path)) return;
        $entries = scandir($path);
        if ($entries === false) return;
        foreach ($entries as $e) {
            if ($e === '.' || $e === '..') continue;
            $full = $path . DS . $e;
            if (is_dir($full)) {
                $this->removeDirectory($full);
            } else {
                @unlink($full);
            }
        }
        @rmdir($path);
    }
}
