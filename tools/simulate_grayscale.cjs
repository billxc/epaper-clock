#!/usr/bin/env node
/* Offline display study only: does not change firmware or its font tables.
 * Usage: NODE_PATH="$(npm root -g)" node tools/simulate_grayscale.cjs FONT.ttf OUTPUT_DIR
 * Input Noto Sans SC and license: tools/OFL-NotoSansSC.txt.
 * Mirrors the clock fixture/layout and native font rasterizers, then quantizes
 * coverage (not a blurred monochrome screenshot), using four ideal gray levels.
 */
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { chromium } = require('@playwright/test');
const root = path.resolve(__dirname, '..');

(async () => {
    const font = fs.readFileSync(process.argv[2]);
    if (crypto.createHash('sha256').update(font).digest('hex') !==
        'd68bafcb48a2707749396aa12bbbd833cb70401f3a9a689fd2902c7e0d295964') throw Error('Unexpected Noto font');
    const out = path.resolve(process.argv[3]);
    fs.mkdirSync(out, { recursive: true });
    const source = fs.readFileSync(path.join(root, 'main/clock_font.c'), 'utf8');
    const metrics = {};
    for (const name of ['time', 'value']) {
        const body = source.match(new RegExp(`static const clock_glyph_t ${name}_glyphs\\[\\] = \\{([\\s\\S]+?)\\n\\};`))[1];
        metrics[name] = {};
        for (const row of body.matchAll(/\{([^}]+)\}/g)) {
            const [cp, width, height, stride, advance, bx, by] = row[1].split(',').slice(0, 7).map(Number);
            metrics[name][cp] = { width, height, stride, advance, bx, by };
        }
    }
    const advance = Number(fs.readFileSync(path.join(root, 'main/ui_font.h'), 'utf8')
        .match(/UI_FONT_ASCII_ADVANCE\s+(\d+)/)[1]);
    const browser = await chromium.launch({ headless: true, chromiumSandbox: true });
    try {
        const page = await browser.newPage();
        const result = await page.evaluate(async ({ font, baseline, metrics, advance }) => {
            const face = new FontFace('Clock Noto', `url(data:font/ttf;base64,${font})`, { weight: '100 900' });
            const uiFace = new FontFace('UI Noto', `url(data:font/ttf;base64,${font})`);
            await Promise.all([face.load(), uiFace.load()]);
            document.fonts.add(face); document.fonts.add(uiFace);
            const gray = new Uint8Array(800 * 480).fill(255);
            const bw = new Uint8Array(800 * 480).fill(255);
            function put(x, y, alpha, threshold) {
                if (!alpha) return;
                const i = y * 800 + x;
                gray[i] = Math.min(gray[i], 255 - alpha);
                if (alpha >= threshold) bw[i] = 0;
            }
            function canvas(w, h) {
                const c = document.createElement('canvas'); c.width = w; c.height = h; return c;
            }
            function uiWidth(text) { return [...text].reduce((n, c) => n + (c.codePointAt(0) < 127 ? advance : 24), 0); }
            function ui(x, y, text) {
                const c = canvas(24, 24), ctx = c.getContext('2d', { willReadFrequently: true });
                ctx.font = '500 24px "UI Noto"'; ctx.textBaseline = 'alphabetic'; ctx.fillStyle = '#000';
                for (const ch of text) {
                    ctx.clearRect(0, 0, 24, 24); ctx.save();
                    const a = ch.codePointAt(0) < 127 ? advance : 24;
                    if (a === advance) ctx.scale(Math.min(1, 13 / ctx.measureText(ch).width), 1);
                    ctx.fillText(ch, 0, 22); ctx.restore();
                    const rgba = ctx.getImageData(0, 0, 24, 24).data;
                    for (let r = 0; r < 24; ++r) for (let col = 0; col < a; ++col)
                        put(x + col, y + r, rgba[(r * 24 + col) * 4 + 3], 96);
                    x += a;
                }
            }
            function width(kind, text) { return [...text].reduce((n, c) => n + metrics[kind][c.codePointAt(0)].advance, 0); }
            function numbers(x, baselineY, kind, text) {
                const size = kind === 'time' ? 200 : 40, origin = size * 3;
                const c = canvas(size * 6, size * 6), ctx = c.getContext('2d', { willReadFrequently: true });
                ctx.font = `400 ${size}px "Clock Noto"`; ctx.textBaseline = 'alphabetic'; ctx.fillStyle = '#000';
                for (const ch of text) {
                    ctx.clearRect(0, 0, c.width, c.height); ctx.fillText(ch, origin, origin);
                    const data = ctx.getImageData(0, 0, c.width, c.height).data;
                    let l = c.width, r = -1, t = c.height, b = -1;
                    for (let y = 0; y < c.height; ++y) for (let xx = 0; xx < c.width; ++xx)
                        if (data[(y * c.width + xx) * 4 + 3] >= 128) {
                            l = Math.min(l, xx); r = Math.max(r, xx); t = Math.min(t, y); b = Math.max(b, y);
                        }
                    const m = metrics[kind][ch.codePointAt(0)];
                    if (r - l + 1 !== m.width || b - t + 1 !== m.height || t - origin !== m.by)
                        throw Error('Native raster metrics no longer match C font');
                    // Include the one-pixel antialias fringe beyond the 1-bit ink bbox.
                    for (let y = t - 2; y <= b + 2; ++y) for (let xx = l - 2; xx <= r + 2; ++xx)
                        put(x + m.bx + xx - l, baselineY + y - origin, data[(y * c.width + xx) * 4 + 3], 128);
                    x += m.advance;
                }
            }
            ui(64, 48, '2026年10月20日');
            ui(736 - uiWidth('电量 85%'), 48, '电量 85%');
            numbers((800 - width('time', '23:59')) / 2, 280, 'time', '23:59');
            ui(232 - uiWidth('温度') / 2, 344, '温度'); ui(568 - uiWidth('湿度') / 2, 344, '湿度');
            numbers(232 - Math.floor(width('value', '23.6℃') / 2), 408, 'value', '23.6℃');
            numbers(568 - Math.floor(width('value', '58.2%') / 2), 408, 'value', '58.2%');
            const reference = await createImageBitmap(await (await fetch(`data:image/png;base64,${baseline}`)).blob());
            const refCanvas = canvas(800, 480), refCtx = refCanvas.getContext('2d');
            refCtx.drawImage(reference, 0, 0);
            const ref = refCtx.getImageData(0, 0, 800, 480).data;
            let mismatches = 0;
            const regions = [0, 0, 0, 0];
            for (let i = 0; i < bw.length; ++i) if (ref[i * 4] !== bw[i]) {
                ++mismatches;
                const y = Math.floor(i / 800);
                ++regions[y < 100 ? 0 : y < 300 ? 1 : y < 370 ? 2 : 3];
            }
            if (mismatches) throw Error(`Reference C framebuffer mismatch: ${mismatches} pixels; header/time/labels/values=${regions}`);
            function image(pixels) {
                const c = canvas(800, 480), ctx = c.getContext('2d'), rgba = ctx.createImageData(800, 480);
                for (let i = 0; i < pixels.length; ++i) {
                    rgba.data[i * 4] = rgba.data[i * 4 + 1] = rgba.data[i * 4 + 2] = pixels[i]; rgba.data[i * 4 + 3] = 255;
                }
                ctx.putImageData(rgba, 0, 0); return c;
            }
            const versions = [bw, Uint8Array.from(gray, a => Math.round(a / 85) * 85)];
            const frames = versions.map(image);
            const labels = ['黑白：当前固件', '4 级灰：理想模拟'];
            const comparison = canvas(680, 750), cx = comparison.getContext('2d');
            cx.fillStyle = '#fff'; cx.fillRect(0, 0, comparison.width, comparison.height);
            cx.fillStyle = '#000'; cx.font = '400 20px "Clock Noto"';
            cx.fillText('相同像素布局，最近邻放大；不是实屏照片', 24, 30);
            for (let i = 0; i < 2; ++i) {
                const x = 24 + i * 328;
                cx.font = '400 19px "Clock Noto"'; cx.fillText(labels[i], x, 66);
                cx.imageSmoothingEnabled = false;
                cx.drawImage(frames[i], 136, 128, 100, 160, x, 82, 300, 480);
                cx.font = '400 18px "Clock Noto"'; cx.fillText('温湿度读数边缘（2倍）', x, 606);
                cx.drawImage(frames[i], 156, 371, 150, 48, x, 624, 300, 96);
            }
            return { images: frames.map(c => c.toDataURL().split(',')[1]), comparison: comparison.toDataURL().split(',')[1],
                metrics: versions.map((a, i) => ({ levels: [2, 4][i], unique: [...new Set(a)].sort((a,b)=>a-b),
                    gray_pixels: a.reduce((n, v) => n + (v !== 0 && v !== 255), 0),
                    changed_pixels: a.reduce((n,v,j)=>n+(v!==bw[j]),0) })), mismatches };
        }, { font: font.toString('base64'), baseline: fs.readFileSync(path.join(root, 'docs/screenshots/clock.png')).toString('base64'), metrics, advance });
        ['mono.png', 'gray4.png'].forEach((name, i) => fs.writeFileSync(path.join(out, name), Buffer.from(result.images[i], 'base64')));
        fs.writeFileSync(path.join(out, 'edge-comparison.png'), Buffer.from(result.comparison, 'base64'));
        fs.writeFileSync(path.join(out, 'metrics.json'), JSON.stringify({ reference_matches_c: result.mismatches === 0, images: result.metrics }, null, 2));
        console.log(JSON.stringify({ output: out, reference_matches_c: result.mismatches === 0, images: result.metrics }, null, 2));
    } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
