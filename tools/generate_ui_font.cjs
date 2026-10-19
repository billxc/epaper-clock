#!/usr/bin/env node
/* Offline rasterizer: no npm/tool installation required.
 * Usage: NODE_PATH="$(npm root -g)" node tools/generate_ui_font.cjs /tmp/NotoSansSC-VF.ttf
 * Optional: CHROMIUM_EXECUTABLE=/path/to/chromium UI_FONT_PREVIEW=/tmp/font.png
 * Download font and its OFL (do NOT commit the TTF):
 * https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf
 * https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/LICENSE
 * The exact input SHA256 is embedded in generated main/ui_font.c. Generation
 * uses only Node built-ins and already-installed @playwright/test/Chromium.
 * Glyph coverage: printable ASCII, GB2312's 6763 Han characters, CJK punctuation
 * and a replacement box. Packed 24x24, MSB first, 1=ink. ASCII advances 14px;
 * CJK advances 24px. Generated bitmaps retain Noto's SIL OFL 1.1 license.
 */
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { chromium } = require('@playwright/test');

(async () => {
    if (!process.argv[2]) throw new Error('Pass the local NotoSansSC-VF.ttf path');
    const font = fs.readFileSync(process.argv[2]);
    const sha = crypto.createHash('sha256').update(font).digest('hex');
    const expected = 'd68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964';
    if (sha !== expected) throw new Error(`Unexpected font SHA256 ${sha}; expected the documented Noto source ${expected}`);
    const chars = new Set(Array.from({ length: 95 }, (_, i) => i + 32));
    const decoder = new TextDecoder('gb18030', { fatal: true });
    for (let hi = 0xb0; hi <= 0xf7; ++hi) for (let lo = 0xa1; lo <= 0xfe; ++lo) {
        const cp = decoder.decode(Uint8Array.of(hi, lo)).codePointAt(0);
        if (cp >= 0x4e00 && cp <= 0x9fff) chars.add(cp);
    }
    for (let cp = 0x3000; cp <= 0x303f; ++cp) chars.add(cp);
    for (const c of '！？，．：；（）／－％［］＋＝') chars.add(c.codePointAt(0));
    chars.add(0xfffd);
    const points = [...chars].sort((a, b) => a - b);
    const browser = await chromium.launch({ headless: true, chromiumSandbox: true,
        ...(process.env.CHROMIUM_EXECUTABLE ? { executablePath: process.env.CHROMIUM_EXECUTABLE } : {}) });
    try {
        const page = await browser.newPage();
        await page.goto('about:blank');
        const rows = await page.evaluate(async ({ base64, points }) => {
            const face = new FontFace('UI Noto', `url(data:font/ttf;base64,${base64})`);
            await face.load();
            document.fonts.add(face);
            const canvas = document.createElement('canvas');
            canvas.width = canvas.height = 24;
            const ctx = canvas.getContext('2d', { willReadFrequently: true });
            return points.map(cp => {
                ctx.clearRect(0, 0, 24, 24);
                ctx.fillStyle = '#000';
                ctx.font = '500 24px "UI Noto"';
                ctx.textBaseline = 'alphabetic';
                if (cp === 0xfffd) {
                    ctx.fillRect(2, 2, 20, 1); ctx.fillRect(2, 21, 20, 1);
                    ctx.fillRect(2, 2, 1, 20); ctx.fillRect(21, 2, 1, 20);
                    ctx.fillRect(11, 7, 2, 7); ctx.fillRect(11, 17, 2, 2);
                } else {
                    ctx.save();
                    if (cp < 128) ctx.scale(Math.min(1, 13 / ctx.measureText(String.fromCodePoint(cp)).width), 1);
                    ctx.fillText(String.fromCodePoint(cp), 0, 22);
                    ctx.restore();
                }
                const data = ctx.getImageData(0, 0, 24, 24).data;
                const packed = [];
                for (let y = 0; y < 24; ++y) for (let byte = 0; byte < 3; ++byte) {
                    let bits = 0;
                    for (let b = 0; b < 8; ++b)
                        if (data[(y * 24 + byte * 8 + b) * 4 + 3] >= 96) bits |= 0x80 >> b;
                    packed.push(bits);
                }
                return packed;
            });
        }, { base64: font.toString('base64'), points });
        if (process.env.UI_FONT_PREVIEW) {
            await page.setViewportSize({ width: 800, height: 480 });
            await page.evaluate(({ points, rows }) => {
                const c = document.createElement('canvas'); c.width = 800; c.height = 480;
                document.body.style.margin = '0'; document.body.append(c);
                const ctx = c.getContext('2d'); ctx.fillStyle = 'white'; ctx.fillRect(0, 0, 800, 480);
                ctx.fillStyle = 'black';
                ['模式选单', '时钟模式', 'LAN 管理模式', '蓝牙管理模式', '本机自动调试',
                 '立刻自动对时', '初始化／配网', '取消  确认', 'SSID: MyWiFi  IP: 192.168.1.100'].forEach((line, i) => {
                    let x = 32;
                    for (const ch of line) {
                        const cp = ch.codePointAt(0), row = rows[points.indexOf(cp)];
                        for (let y = 0; y < 24; ++y) for (let xx = 0; xx < 24; ++xx)
                            if (row[y * 3 + (xx >> 3)] & (0x80 >> (xx & 7))) ctx.fillRect(x + xx, 20 + i * 48 + y, 1, 1);
                        x += cp < 128 ? 14 : 24;
                    }
                });
            }, { points, rows });
            await page.screenshot({ path: process.env.UI_FONT_PREVIEW });
        }
        const hex = n => '0x' + n.toString(16).padStart(2, '0');
        const out = `/* Generated by tools/generate_ui_font.cjs; do not hand edit.\n * Noto Sans SC bitmap derivative, SIL OFL 1.1; see tools/OFL-NotoSansSC.txt.\n * Source: notofonts/noto-cjk Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf\n * Input SHA256: ${sha}\n * ${points.length} glyphs; 24x24 MSB-first; 1=ink; unknown Unicode -> box.\n */\n#include "ui_font.h"\n#include <stddef.h>\n\nstatic const uint16_t codepoints[] = {\n` +
            points.map((p, i) => (i % 12 === 0 ? '    ' : '') + hex(p) + ',' + (i % 12 === 11 ? '\n' : ' ')).join('') +
            '\n};\nstatic const uint8_t bitmaps[][72] = {\n' + rows.map(row => '    {' + row.map(hex).join(',') + '},').join('\n') +
            '\n};\n\nconst uint8_t *ui_font_glyph(uint32_t codepoint)\n{\n    size_t lo = 0, hi = sizeof(codepoints) / sizeof(codepoints[0]);\n    while (lo < hi) {\n        size_t mid = lo + (hi - lo) / 2;\n        if (codepoints[mid] < codepoint) lo = mid + 1;\n        else hi = mid;\n    }\n    if (lo < sizeof(codepoints) / sizeof(codepoints[0]) && codepoints[lo] == codepoint)\n        return bitmaps[lo];\n    return bitmaps[sizeof(codepoints) / sizeof(codepoints[0]) - 1];\n}\n';
        fs.writeFileSync(path.join(__dirname, '../main/ui_font.c'), out.replace(/[ \t]+$/gm, ''));
        console.log(`${points.length} glyphs, ${points.length * 74} bytes bitmap+index, SHA256 ${sha}`);
    } finally { await browser.close(); }
})().catch(err => { console.error(err); process.exitCode = 1; });
