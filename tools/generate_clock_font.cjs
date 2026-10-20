#!/usr/bin/env node
/* Offline native-size Canvas rasterizer; no installation or network required.
 * Usage: NODE_PATH="$(npm root -g)" node tools/generate_clock_font.cjs FONT.ttf
 * Optional: CHROMIUM_EXECUTABLE=/path/to/chromium
 * Source: https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf
 * License: SIL OFL 1.1, retained in tools/OFL-NotoSansSC.txt.
 * Bitmap derivatives retain that license. Do not commit the source TTF.
 * Required source SHA256: d68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964
 * Native raster sizes: TIME 400 weight / 200px; VALUE 400 weight / 40px.
 * No scaling, resampling, bitmap fonts, gray dithering, or synthetic glyphs.
 */
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { chromium } = require('@playwright/test');

(async () => {
    if (process.argv.length !== 3) throw new Error('Pass the local NotoSansSC-VF.ttf path');
    const font = fs.readFileSync(process.argv[2]);
    const sha = crypto.createHash('sha256').update(font).digest('hex');
    const expected = 'd68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964';
    if (sha !== expected) throw new Error(`Unexpected source SHA256: ${sha}`);
    // Both gray modes use this same linear 2bpp coverage. Its high bit is
    // exactly the original alpha>=128 mask; verify all discrete alpha values.
    const coverage = Array.from({ length: 256 }, (_, a) => Math.round(3 * a / 255));
    const gamma = Math.log(0.5) / Math.log(127.5 / 255);
    for (let a = 0; a < 256; ++a) {
        if ((coverage[a] >= 2) !== (a >= 128) ||
            coverage[a] !== Math.round(3 * Math.pow(a / 255, gamma)))
            throw Error('Clock alpha mapping mismatch');
        for (const inverse of [false, true])
            if ((inverse ? coverage[a] : 3 - coverage[a]) !==
                Math.round((inverse ? a : 255 - a) / 85)) throw Error('Clock blend mismatch');
    }
    console.log('Verified all 256 clock alpha values: boundaries=43,128,213; gamma=1');
    const specs = [
        { name: 'TIME', size: 200, chars: '0123456789:-' },
        { name: 'VALUE', size: 40, chars: '0123456789.-%\u2103' },
    ];
    const browser = await chromium.launch({ headless: true, chromiumSandbox: true,
        ...(process.env.CHROMIUM_EXECUTABLE ? { executablePath: process.env.CHROMIUM_EXECUTABLE } : {}) });
    let fonts;
    try {
        const page = await browser.newPage();
        await page.goto('about:blank');
        fonts = await page.evaluate(async ({ base64, specs, coverage }) => {
            const face = new FontFace('Clock Noto', `url(data:font/ttf;base64,${base64})`, { weight: '100 900' });
            await face.load();
            document.fonts.add(face);
            return specs.map(spec => {
                const canvas = document.createElement('canvas');
                // Large margins on ALL sides, including below the baseline.
                canvas.width = canvas.height = spec.size * 6;
                const origin = spec.size * 3, baseline = spec.size * 3;
                const ctx = canvas.getContext('2d', { willReadFrequently: true });
                ctx.font = `400 ${spec.size}px "Clock Noto"`;
                ctx.textBaseline = 'alphabetic';
                ctx.textAlign = 'left';
                ctx.fillStyle = '#000';
                const glyphs = [...spec.chars].map(ch => {
                    ctx.clearRect(0, 0, canvas.width, canvas.height);
                    const measuredAdvance = ctx.measureText(ch).width;
                    ctx.fillText(ch, origin, baseline);
                    const rgba = ctx.getImageData(0, 0, canvas.width, canvas.height).data;
                    let left = canvas.width, top = canvas.height, right = -1, bottom = -1;
                    for (let y = 0; y < canvas.height; ++y) {
                        for (let x = 0; x < canvas.width; ++x) {
                            const alpha = rgba[(y * canvas.width + x) * 4 + 3];
                            if (alpha && (x === 0 || y === 0 || x === canvas.width - 1 || y === canvas.height - 1))
                                throw new Error(`${spec.name} ${ch}: raster touches canvas edge`);
                            if (alpha < 128) continue;
                            left = Math.min(left, x); right = Math.max(right, x);
                            top = Math.min(top, y); bottom = Math.max(bottom, y);
                        }
                    }
                    if (right < left) throw new Error(`${spec.name} ${ch}: empty glyph`);
                    const width = right - left + 1, height = bottom - top + 1;
                    // Keep original mono bbox/bearings/advance. Store the same
                    // two-pixel fringe used by docs/grayscale outside that bbox.
                    const stride = Math.ceil((width + 4) / 4);
                    const advance = spec.name === 'TIME' ? (ch === ':' ? 64 : 120) : Math.ceil(measuredAdvance) + 2;
                    // TIME centers actual ink, not the font's asymmetric side bearings.
                    // VALUE retains native side bearings with one pixel padding per side.
                    const bearing_x = spec.name === 'TIME' ? Math.floor((advance - width) / 2) : left - origin + 1;
                    const bearing_y = top - baseline;
                    if (spec.name === 'TIME' && (bearing_x < 0 || bearing_x + width > advance))
                        throw new Error(`${spec.name} ${ch}: ink exceeds advance`);
                    const bitmap = Array(stride * (height + 4)).fill(0);
                    for (let y = 0; y < height + 4; ++y) for (let x = 0; x < width + 4; ++x) {
                        const a = rgba[((top + y - 2) * canvas.width + left + x - 2) * 4 + 3];
                        bitmap[y * stride + (x >> 2)] |= coverage[a] << (6 - 2 * (x & 3));
                    }
                    // No visible quantized alpha may be lost outside the fringe.
                    for (let y = 0; y < canvas.height; ++y) for (let x = 0; x < canvas.width; ++x)
                        if ((x < left - 2 || x > right + 2 || y < top - 2 || y > bottom + 2) &&
                            coverage[rgba[(y * canvas.width + x) * 4 + 3]])
                            throw Error('Coverage outside stored fringe');
                    return { codepoint: ch.codePointAt(0), width, height, stride, advance,
                        bearing_x, bearing_y, measuredAdvance, bitmap };
                }).sort((a, b) => a.codepoint - b.codepoint);
                return { ...spec, glyphs };
            });
        }, { base64: font.toString('base64'), specs, coverage });
    } finally {
        await browser.close();
    }

    const baseline = fs.readFileSync(process.env.CLOCK_FONT_MONO_BASELINE ||
        path.join(__dirname, '../main/clock_font.c'), 'utf8');
    const oldMono = baseline.includes('cropped 1-bit ink');
    // Validate packing and original tight mono bboxes independently of Canvas.
    for (const f of fonts) for (const g of f.glyphs) {
        const level = (x, y) => (g.bitmap[y * g.stride + (x >> 2)] >> (6 - 2 * (x & 3))) & 3;
        const ink = (x, y) => level(x + 2, y + 2) >= 2;
        if (g.bitmap.length !== g.stride * (g.height + 4) ||
            !Array.from({ length: g.width }, (_, x) => ink(x, 0)).some(Boolean) ||
            !Array.from({ length: g.width }, (_, x) => ink(x, g.height - 1)).some(Boolean) ||
            !Array.from({ length: g.height }, (_, y) => ink(0, y)).some(Boolean) ||
            !Array.from({ length: g.height }, (_, y) => ink(g.width - 1, y)).some(Boolean))
            throw new Error(`${f.name} U+${g.codepoint.toString(16)}: invalid tight bbox`);
        for (let y = 0; y < g.height + 4; ++y) for (let x = 0; x < g.stride * 4; ++x) {
            if (x >= g.width + 4 && level(x, y)) throw Error('Nonzero stride padding');
            if ((x < 2 || y < 2 || x >= g.width + 2 || y >= g.height + 2) && level(x, y) >= 2)
                throw Error('Mono ink outside original bbox');
        }
        if (oldMono) {
            const name = `${f.name.toLowerCase()}_${g.codepoint.toString(16)}`;
            const match = baseline.match(new RegExp(`static const uint8_t ${name}\\[\\] = \\{([\\s\\S]*?)\\};`));
            if (!match) throw Error(`Missing original glyph ${name}`);
            const bytes = match[1].split(',').map(s => s.trim()).filter(Boolean).map(Number);
            const metrics = baseline.match(new RegExp(`\\{0x${g.codepoint.toString(16).padStart(2, '0')}, ([^}]+), ${name}\\}`));
            const old = metrics[1].split(',').map(Number);
            if ([g.width, g.height, Math.ceil(g.width / 8), g.advance, g.bearing_x, g.bearing_y].some((v, i) => v !== old[i]))
                throw Error(`Original metrics differ: ${name}`);
            for (let y = 0; y < g.height; ++y) for (let x = 0; x < g.width; ++x)
                if (ink(x, y) !== !!(bytes[y * old[2] + (x >> 3)] & (0x80 >> (x & 7))))
                    throw Error(`Original mono pixel differs: ${name} ${x},${y}`);
        }
        if (g.codepoint >= 48 && g.codepoint <= 57 &&
            (g.height < (f.name === 'TIME' ? 130 : 25) || g.height > (f.name === 'TIME' ? 170 : 40)))
            throw new Error('Unexpected native digit height');
    }
    const sourceNotice = ` * Noto Sans SC bitmap derivative, SIL OFL 1.1; see tools/OFL-NotoSansSC.txt.
 * Source: notofonts/noto-cjk Sans/Variable/TTF/Subset/NotoSansSC-VF.ttf
 * Input SHA256: ${sha}
 * Native Canvas rasterization: weight 400, TIME 200px, VALUE 40px.
 * MSB-first linear 2bpp coverage; alpha boundaries 43,128,213, gamma=1.
 * Original alpha>=128 bbox/bearings retained; bitmap includes 2px fringe.
 * All 256 alpha values verified; no scaling/resampling; zero stride padding.
`;
    const header = `/* Generated by tools/generate_clock_font.cjs; do not hand edit.
${sourceNotice} */
#ifndef CLOCK_FONT_H
#define CLOCK_FONT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_FONT_PADDING 2

typedef enum { CLOCK_FONT_TIME, CLOCK_FONT_VALUE } clock_font_t;

typedef struct {
    uint16_t codepoint, width, height, stride, advance;
    int16_t bearing_x, bearing_y;
    const uint8_t *bitmap;
} clock_glyph_t;

/* Draw at (pen_x + bearing_x, baseline + bearing_y), then advance pen_x.
 * Width/height are ORIGINAL tight mono ink bounds; stride is bytes per row.
 * Bitmap is (width+4)x(height+4), padded with zero coverage to byte alignment.
 * Draw bitmap at (pen_x+bearing_x-2, baseline+bearing_y-2).
 * Packed 2bpp MSB-first coverage 0..3; mono uses coverage>=2, gray uses all.
 * Linear/ink-match have identical coverage for this font (gamma=1).
 * TIME: digits and '-' advance 120px; ':' advances 64px (HH:MM = 544px).
 * TIME ink is centered within advance; VALUE uses ceil(native advance)+2px.
 * Native baseline is preserved across glyphs. No runtime scaling is needed.
 * Unsupported codepoints or invalid font identifiers return NULL.
 * Returned glyphs/bitmaps have static lifetime.
 */
const clock_glyph_t *clock_font_glyph(clock_font_t font, uint32_t codepoint);

#ifdef __cplusplus
}
#endif
#endif
`;
    const hex = n => '0x' + n.toString(16).padStart(2, '0');
    let source = `/* Generated by tools/generate_clock_font.cjs; do not hand edit.
${sourceNotice} */
#include "clock_font.h"
#include <stddef.h>

`;
    for (const f of fonts) {
        const name = f.name.toLowerCase();
        const bytes = f.glyphs.reduce((sum, g) => sum + g.bitmap.length, 0);
        const top = Math.min(...f.glyphs.map(g => g.bearing_y));
        const bottom = Math.max(...f.glyphs.map(g => g.bearing_y + g.height));
        source += `/* ${f.name}: ${f.size}px, ${f.glyphs.length} glyphs, ${bytes} bitmap bytes;
 * baseline-relative ink y = [${top}, ${bottom}) across this character set. */\n`;
        for (const g of f.glyphs) {
            source += `static const uint8_t ${name}_${g.codepoint.toString(16)}[] = {\n`;
            for (let i = 0; i < g.bitmap.length; i += 16)
                source += '    ' + g.bitmap.slice(i, i + 16).map(hex).join(', ') + ',\n';
            source += '};\n';
        }
        source += `\nstatic const clock_glyph_t ${name}_glyphs[] = {\n`;
        for (const g of f.glyphs)
            source += `    {${hex(g.codepoint)}, ${g.width}, ${g.height}, ${g.stride}, ${g.advance}, ${g.bearing_x}, ${g.bearing_y}, ${name}_${g.codepoint.toString(16)}},\n`;
        source += '};\n\n';
        console.log(`${f.name}: ${f.size}px / weight 400, ${bytes} bitmap bytes, ink y=[${top}, ${bottom})`);
        console.table(f.glyphs.map(({ bitmap, measuredAdvance, ...g }) => ({ char: String.fromCodePoint(g.codepoint), ...g })));
    }
    source += `const clock_glyph_t *clock_font_glyph(clock_font_t font, uint32_t codepoint)
{
    const clock_glyph_t *glyphs;
    size_t count;
    switch (font) {
    case CLOCK_FONT_TIME:
        glyphs = time_glyphs;
        count = sizeof(time_glyphs) / sizeof(time_glyphs[0]);
        break;
    case CLOCK_FONT_VALUE:
        glyphs = value_glyphs;
        count = sizeof(value_glyphs) / sizeof(value_glyphs[0]);
        break;
    default:
        return NULL;
    }
    for (size_t i = 0; i < count; ++i)
        if (glyphs[i].codepoint == codepoint) return &glyphs[i];
    return NULL;
}
`;
    fs.writeFileSync(path.join(__dirname, '../main/clock_font.h'), header);
    fs.writeFileSync(path.join(__dirname, '../main/clock_font.c'), source);
    if (oldMono) console.log('Verified all original clock mono pixels and all glyph metrics');
    console.log(`Total bitmap bytes: ${fonts.flatMap(f => f.glyphs).reduce((sum, g) => sum + g.bitmap.length, 0)}`);
})().catch(err => { console.error(err); process.exitCode = 1; });
