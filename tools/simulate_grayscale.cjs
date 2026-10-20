#!/usr/bin/env node
/* Offline display study only: does not change firmware or its font tables.
 * Usage: NODE_PATH="$(npm root -g)" node tools/simulate_grayscale.cjs FONT.ttf OUTPUT_DIR
 * Input Noto Sans SC and license: tools/OFL-NotoSansSC.txt.
 * Mirrors clock/menu/management fixtures and native font rasterizers, then quantizes
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
        const result = await page.evaluate(async ({ font, baseline, menuBaseline, metrics, advance }) => {
            const face = new FontFace('Clock Noto', `url(data:font/ttf;base64,${font})`, { weight: '100 900' });
            const uiFace = new FontFace('UI Noto', `url(data:font/ttf;base64,${font})`);
            await Promise.all([face.load(), uiFace.load()]);
            document.fonts.add(face); document.fonts.add(uiFace);
            const gray = new Uint8Array(800 * 480).fill(255);
            const bw = new Uint8Array(800 * 480).fill(255);
            function put(x, y, alpha, threshold, inverse = false) {
                if (!alpha) return;
                const i = y * 800 + x, color = inverse ? 255 : 0;
                gray[i] = Math.round((gray[i] * (255 - alpha) + color * alpha) / 255);
                if (alpha >= threshold) bw[i] = color;
            }
            function rectangle(x, y, w, h, color = 0) {
                for (let r = y; r < y + h; ++r) for (let col = x; col < x + w; ++col)
                    gray[r * 800 + col] = bw[r * 800 + col] = color;
            }
            function canvas(w, h) {
                const c = document.createElement('canvas'); c.width = w; c.height = h; return c;
            }
            function uiWidth(text) { return [...text].reduce((n, c) => n + (c.codePointAt(0) < 127 ? advance : 24), 0); }
            function ui(x, y, text, inverse = false) {
                const c = canvas(24, 24), ctx = c.getContext('2d', { willReadFrequently: true });
                ctx.font = '500 24px "UI Noto"'; ctx.textBaseline = 'alphabetic'; ctx.fillStyle = '#000';
                for (const ch of text) {
                    ctx.clearRect(0, 0, 24, 24); ctx.save();
                    const a = ch.codePointAt(0) < 127 ? advance : 24;
                    if (x + a > 768) break;
                    if (a === advance) ctx.scale(Math.min(1, 13 / ctx.measureText(ch).width), 1);
                    ctx.fillText(ch, 0, 22); ctx.restore();
                    const rgba = ctx.getImageData(0, 0, 24, 24).data;
                    for (let r = 0; r < 24; ++r) for (let col = 0; col < a; ++col)
                        put(x + col, y + r, rgba[(r * 24 + col) * 4 + 3], 96, inverse);
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
            const versions = [bw.slice(), Uint8Array.from(gray, a => Math.round(a / 85) * 85)];
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
            function screen(title, lines, selected = -1) {
                gray.fill(255); bw.fill(255);
                ui(32, 24, title); rectangle(32, 64, 736, 2);
                lines.forEach((line, i) => {
                    const y = 88 + i * 40;
                    if (i === selected) rectangle(32, y - 5, 736, 34);
                    ui(48, y, line, i === selected);
                });
                ui(32, 440, selected >= 0 ? '上／下移动  中键选择' : '长按中键打开选单');
                return [image(bw), image(Uint8Array.from(gray, a => Math.round(a / 85) * 85))];
            }
            const pages = [
                { name: 'menu', title: '模式选单', frames: screen('模式选单',
                    ['时钟模式', 'LAN 管理模式', '蓝牙管理模式', '本机自动调试', '立刻自动对时', '初始化／配网'], 0) },
                { name: 'provision', title: '初始化／配网', frames: screen('初始化／配网',
                    ['蓝牙名称：PROV_EPAPER_A1B2C3', '校验码：012345', '手机打开 GitHub Pages 页面',
                     '选择连接设备，输入校验码', '可设置 Wi-Fi 或手动设置时间', '等待暂停后，短按中键重新开启']) },
                { name: 'lan', title: 'LAN 管理模式', frames: screen('LAN 管理模式',
                    ['地址：http://192.0.2.10', '用户名：admin', '密码：demo-only-1234567',
                     '使用已保存的 Wi-Fi 连接局域网', '连续五分钟无操作后返回时钟']) },
            ];
            const menuRef = await createImageBitmap(await (await fetch(`data:image/png;base64,${menuBaseline}`)).blob());
            const menuCanvas = canvas(800, 480), menuCtx = menuCanvas.getContext('2d');
            menuCtx.drawImage(menuRef, 0, 0);
            const menuData = menuCtx.getImageData(0, 0, 800, 480).data;
            const simulatedMenu = pages[0].frames[0].getContext('2d').getImageData(0, 0, 800, 480).data;
            let menuMismatches = 0;
            for (let i = 0; i < menuData.length; i += 4) if (menuData[i] !== simulatedMenu[i]) ++menuMismatches;
            if (menuMismatches) throw Error(`Menu C framebuffer mismatch: ${menuMismatches}`);
            const textComparison = canvas(864, 688), tc = textComparison.getContext('2d');
            tc.fillStyle = '#fff'; tc.fillRect(0, 0, 864, 688);
            tc.fillStyle = '#000'; tc.font = '400 22px "Clock Noto"';
            tc.fillText('相同 24px 字库、字重与布局，仅比较灰度', 24, 30);
            tc.font = '400 20px "Clock Noto"';
            tc.fillText('黑白', 24, 66); tc.fillText('四级灰（理想模拟）', 440, 66);
            const crops = [
                { page: 0, x: 40, y: 164, title: '菜单普通中文' },
                { page: 0, x: 40, y: 83, title: '黑底选中项：白字' },
                { page: 2, x: 218, y: 84, title: 'LAN 地址：数字与标点' },
                { page: 1, x: 138, y: 124, title: '配网校验码：012345（示例）' },
            ];
            crops.forEach((crop, row) => {
                const y = 112 + row * 144;
                tc.font = '400 18px "Clock Noto"'; tc.fillText(crop.title, 24, y - 12);
                tc.imageSmoothingEnabled = false;
                for (let i = 0; i < 2; ++i)
                    tc.drawImage(pages[crop.page].frames[i], crop.x, crop.y, 190, 36,
                        24 + i * 416, y, 380, 72);
            });
            tc.font = '400 18px "Clock Noto"';
            tc.fillText('最近邻放大 2 倍；非实屏图，地址和校验码均为虚构示例', 24, 666);
            return { images: frames.map(c => c.toDataURL().split(',')[1]), comparison: comparison.toDataURL().split(',')[1],
                pages: pages.map(p => ({ name: p.name, images: p.frames.map(c => c.toDataURL().split(',')[1]) })),
                textComparison: textComparison.toDataURL().split(',')[1], menuMismatches,
                metrics: versions.map((a, i) => ({ levels: [2, 4][i], unique: [...new Set(a)].sort((a,b)=>a-b),
                    gray_pixels: a.reduce((n, v) => n + (v !== 0 && v !== 255), 0),
                    changed_pixels: a.reduce((n,v,j)=>n+(v!==versions[0][j]),0) })), mismatches };
        }, { font: font.toString('base64'), baseline: fs.readFileSync(path.join(root, 'docs/screenshots/clock.png')).toString('base64'),
            menuBaseline: fs.readFileSync(path.join(root, 'docs/screenshots/menu.png')).toString('base64'), metrics, advance });
        ['mono.png', 'gray4.png'].forEach((name, i) => fs.writeFileSync(path.join(out, name), Buffer.from(result.images[i], 'base64')));
        fs.writeFileSync(path.join(out, 'edge-comparison.png'), Buffer.from(result.comparison, 'base64'));
        for (const p of result.pages) p.images.forEach((data, i) =>
            fs.writeFileSync(path.join(out, `${p.name}-${i ? 'gray4' : 'mono'}.png`), Buffer.from(data, 'base64')));
        fs.writeFileSync(path.join(out, 'ui-text-comparison.png'), Buffer.from(result.textComparison, 'base64'));
        const report = { reference_matches_c: result.mismatches === 0, menu_matches_c: result.menuMismatches === 0, images: result.metrics };
        fs.writeFileSync(path.join(out, 'metrics.json'), JSON.stringify(report, null, 2));
        console.log(JSON.stringify({ output: out, ...report }, null, 2));
    } finally { await browser.close(); }
})().catch(e => { console.error(e); process.exitCode = 1; });
