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
 * and a replacement box. Packed 24x24, MSB-first 3bpp coverage classes. ASCII advances 14px;
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
    // Union of every transition in all three mappings, derived from all 256
    // alpha samples. Store classes, not pre-thresholded monochrome glyphs.
    const gamma = Math.log(0.5) / Math.log(95.5 / 255);
    const maps = [
        Array.from({ length: 256 }, (_, a) => a >= 96 ? 3 : 0),
        Array.from({ length: 256 }, (_, a) => Math.round(3 * a / 255)),
        Array.from({ length: 256 }, (_, a) => Math.round(3 * Math.pow(a / 255, gamma))),
    ];
    const thresholds = [];
    for (let a = 1; a < 256; ++a)
        if (maps.some(m => m[a] !== m[a - 1])) thresholds.push(a);
    const classCount = thresholds.length + 1;
    if (classCount > 7) throw Error(`Too many coverage classes: ${classCount}`);
    const classes = Array.from({ length: 256 }, (_, a) => thresholds.filter(t => a >= t).length);
    const ink = maps.map(m => [0, ...thresholds].map(a => m[a]));
    for (let a = 0; a < 256; ++a) {
        for (let mode = 0; mode < 3; ++mode) {
            if (ink[mode][classes[a]] !== maps[mode][a]) throw Error('Class mapping mismatch');
            for (const inverse of [false, true]) {
                const coverage = mode === 0 ? (a >= 96 ? 255 : 0) :
                    mode === 1 ? a : 85 * Math.round(3 * Math.pow(a / 255, gamma));
                const expected = Math.round((inverse ? coverage : 255 - coverage) / 85);
                const shade = inverse ? ink[mode][classes[a]] : 3 - ink[mode][classes[a]];
                if (shade !== expected) throw Error('Foreground/background mapping mismatch');
            }
        }
        if ((ink[2][classes[a]] >= 2) !== (a >= 96)) throw Error('Mono recovery mismatch');
    }
    console.log(`Verified all 256 alpha values: gamma=${gamma}, thresholds=${thresholds}, classes=${classCount}`);
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
        const rows = await page.evaluate(async ({ base64, points, classes }) => {
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
                const packed = Array(216).fill(0);
                for (let p = 0; p < 24 * 24; ++p) {
                    const category = classes[data[p * 4 + 3]], bit = p * 3;
                    for (let b = 0; b < 3; ++b)
                        if (category & (4 >> b)) packed[(bit + b) >> 3] |= 0x80 >> ((bit + b) & 7);
                }
                return packed;
            });
        }, { base64: font.toString('base64'), points, classes });
        const unpack = (row, p) => {
            let result = 0;
            for (let b = 0; b < 3; ++b) {
                const bit = p * 3 + b;
                result = (result << 1) | ((row[bit >> 3] >> (7 - (bit & 7))) & 1);
            }
            return result;
        };
        // Optionally compare every glyph pixel against the original generated
        // 1bpp source (also automatically checked on the first migration run).
        const baseline = fs.readFileSync(process.env.UI_FONT_MONO_BASELINE ||
            path.join(__dirname, '../main/ui_font.c'), 'utf8');
        if (baseline.includes('bitmaps[][72]')) {
            const body = baseline.split('bitmaps[][72] = {')[1].split('\n};')[0];
            const old = [...body.matchAll(/\{([^}]+)\}/g)].map(m => m[1].split(',').filter(Boolean).map(Number));
            if (old.length !== rows.length) throw Error('Baseline glyph count differs');
            for (let g = 0; g < rows.length; ++g) for (let p = 0; p < 576; ++p)
                if ((ink[0][unpack(rows[g], p)] === 3) !== !!(old[g][p >> 3] & (0x80 >> (p & 7))))
                    throw Error(`Original mono pixel differs: glyph ${g}, pixel ${p}`);
            console.log(`Verified all ${rows.length * 576} UI mono pixels against original font`);
        }
        if (process.env.UI_FONT_PREVIEW) {
            await page.setViewportSize({ width: 800, height: 480 });
            await page.evaluate(({ points, rows, ink }) => {
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
                            {
                                const bit = (y * 24 + xx) * 3;
                                let category = 0;
                                for (let b = 0; b < 3; ++b)
                                    category = (category << 1) | ((row[(bit + b) >> 3] >> (7 - ((bit + b) & 7))) & 1);
                                const shade = 255 - 85 * ink[2][category];
                                ctx.fillStyle = `rgb(${shade},${shade},${shade})`;
                                ctx.fillRect(x + xx, 20 + i * 48 + y, 1, 1);
                            }
                        x += cp < 128 ? 14 : 24;
                    }
                });
            }, { points, rows, ink });
            await page.screenshot({ path: process.env.UI_FONT_PREVIEW });
        }
        const hex = n => '0x' + n.toString(16).padStart(2, '0');
        const out = `/* Generated by tools/generate_ui_font.cjs; do not hand edit.\n * Noto Sans SC bitmap derivative, SIL OFL 1.1; see tools/OFL-NotoSansSC.txt.\n * Source: notofonts/noto-cjk Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf\n * Input SHA256: ${sha}\n * ${points.length} glyphs; 24x24 MSB-first 3bpp classes; unknown Unicode -> box.\n * Alpha class boundaries: ${thresholds.join(', ')}; gamma ${gamma}.\n * All 256 alpha mappings and both foreground/background polarities verified.\n */\n#include "ui_font.h"\n#include <stddef.h>\n\nstatic const uint16_t codepoints[] = {\n` +
            points.map((p, i) => (i % 12 === 0 ? '    ' : '') + hex(p) + ',' + (i % 12 === 11 ? '\n' : ' ')).join('') +
            '\n};\nstatic const uint8_t bitmaps[][216] = {\n' + rows.map(row => '    {' + row.map(hex).join(',') + '},').join('\n') +
            '\n};\n\nconst uint8_t *ui_font_glyph(uint32_t codepoint)\n{\n    size_t lo = 0, hi = sizeof(codepoints) / sizeof(codepoints[0]);\n    while (lo < hi) {\n        size_t mid = lo + (hi - lo) / 2;\n        if (codepoints[mid] < codepoint) lo = mid + 1;\n        else hi = mid;\n    }\n    if (lo < sizeof(codepoints) / sizeof(codepoints[0]) && codepoints[lo] == codepoint)\n        return bitmaps[lo];\n    return bitmaps[sizeof(codepoints) / sizeof(codepoints[0]) - 1];\n}\n';
        const lookup = `\nconst uint8_t ui_font_ink[3][UI_FONT_CLASS_COUNT] = {\n` +
            ink.map(row => '    {' + row.join(', ') + '},').join('\n') + '\n};\n';
        fs.writeFileSync(path.join(__dirname, '../main/ui_font.c'), (out + lookup).replace(/[ \t]+$/gm, ''));
        fs.writeFileSync(path.join(__dirname, '../main/ui_font.h'), `#pragma once
#include <stdint.h>

/* Noto Sans SC / SIL OFL 1.1. One 216-byte 24x24 bitmap per glyph.
 * Each row is 9 bytes, MSB-first 3-bit coverage classes (not grayscale).
 * ui_font_ink[render_mode][class] gives quantized coverage: 0..3.
 * Mono coverage is only 0/3; normal shade=3-coverage, inverse=coverage.
 * Printable ASCII, GB2312 Han and CJK punctuation. Unknown -> visible box.
 * Returned bitmaps and lookup table have static lifetime in flash. */
#define UI_FONT_WIDTH 24
#define UI_FONT_HEIGHT 24
#define UI_FONT_ASCII_ADVANCE 14
#define UI_FONT_CLASS_COUNT ${classCount}
extern const uint8_t ui_font_ink[3][UI_FONT_CLASS_COUNT];
const uint8_t *ui_font_glyph(uint32_t codepoint);
`);
        console.log(`${points.length} glyphs, ${points.length * 218} bytes bitmap+index, SHA256 ${sha}`);
    } finally { await browser.close(); }
})().catch(err => { console.error(err); process.exitCode = 1; });
