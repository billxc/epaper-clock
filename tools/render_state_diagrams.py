#!/usr/bin/env python3
"""Generate editable state-machine documentation using only the standard library.

Run from any working directory: python3 tools/render_state_diagrams.py
One shared, explicit-coordinate model drives SVG and uncompressed draw.io XML.
These are descriptive abstractions of baseline 524453e, not firmware FSM enums.
"""
from dataclasses import dataclass, field
from html import escape
from pathlib import Path
import xml.etree.ElementTree as ET


OUT = Path(__file__).resolve().parent.parent / "docs" / "diagrams"
FONT = "Noto Sans CJK SC, Microsoft YaHei, PingFang SC, sans-serif"
COLORS = {
    "blue": ("#eaf2ff", "#2563a8"),
    "green": ("#e9f6ed", "#287747"),
    "red": ("#fff0ee", "#b73838"),
    "purple": ("#f3ecfa", "#8054a6"),
    "gray": ("#f0f2f5", "#647080"),
}
BASELINE = "当前实现（基线524453e），非新增固件FSM"
Point = tuple[int, int]


@dataclass
class Box:
    id: str
    x: int
    y: int
    w: int
    h: int
    lines: tuple[str, ...]
    color: str = "blue"
    kind: str = "state"
    size: int = 19


@dataclass
class Edge:
    id: str
    source: str
    target: str
    points: tuple[Point, ...]
    color: str = "blue"


@dataclass
class Page:
    id: str
    title: str
    width: int = 1600
    height: int = 1160
    boxes: list[Box] = field(default_factory=list)
    edges: list[Edge] = field(default_factory=list)

    def box(self, id, x, y, w, h, *lines, color="blue", kind="state", size=19):
        self.boxes.append(Box(id, x, y, w, h, lines, color, kind, size))

    def edge(self, source, target, points, color="blue", label=None):
        id = f"edge_{len(self.edges) + 1}"
        self.edges.append(Edge(id, source, target, tuple(points), color))
        if label:
            # Labels are independent editable white-backed text cells, NOT auto-
            # positioned edge labels. This keeps SVG and draw.io positions equal.
            x, y, w, *lines = label
            h = len(lines) * 22 + 10
            self.box(id + "_label", x - w // 2, y - h // 2, w, h,
                     *lines, kind="label", size=17)

    def decorate(self):
        self.box("title", 45, 18, 830, 40, self.title, kind="heading", size=28)
        self.box("baseline", 45, 61, 800, 28, BASELINE, kind="text", size=18)
        self.box("legend", 885, 24, 670, 58,
                 "蓝：正常  ·  绿：成功  ·  红：失败",
                 "紫：BLE  ·  灰：关机  |  实线仅表示已实现转换",
                 kind="text", size=17)
        self.box("footer", 45, self.height - 40, 1510, 26,
                 "未实机验证 · 基于已核对代码事实；状态为文档抽象，非应用 enum。各子图入口 / 返回均为调用边界。",
                 kind="text", size=16)


def main_page():
    p = Page("main", "MAIN · 冷启动、用户服务与分钟循环", height=1150)
    p.box("boot", 590, 125, 420, 100, "Boot / 初始化", "先读 RTC AF，再清标志", "USB：额外 ≤10s 串口校时窗口")
    p.box("route", 590, 290, 420, 100, "启动路由", "AF=0（非RTC启动）或中键GPIO5", "或上键GPIO4 → 用户服务")
    p.box("service", 1140, 300, 380, 205, "用户服务 · 详见 BLE / LAN 图", "上键 / 无保存SSID → BLE", "否则 → STA HTTP", "BLE失败 → 结束服务、回时钟", "BLE保存成功 → 先尝试NTP", "NTP失败也继续 → STA HTTP", color="purple", size=18)
    p.box("daily", 590, 480, 420, 90, "每日自动校时检查", "条件 / 当天尝试锁定：详见 NTP 图")
    p.box("refresh", 590, 655, 420, 100, "采样 → 刷屏 → 更新缓存", "EPD_Init → ALDO3 3.3V开", "刷完 EPD_Sleep → ALDO3关", "校时成功可请求强制全刷", size=18)
    p.box("boundary", 590, 825, 420, 80, "本轮处理是否已跨整分？", "是：立即再次每日检查 / 补刷")
    p.box("off", 250, 995, 400, 100, "电池：设下分钟 RTC → PMU关机", "WiFi / HTTP / BLE 均关闭", "等待 RTC / PWR 冷启动", color="gray", size=18)
    p.box("usb", 950, 995, 400, 100, "USB：时钟等待整分", "每50ms查键；整分到达后更新", "WiFi / HTTP / BLE 均关闭", size=18)
    p.box("missing", 85, 345, 350, 140, "尚未实现（不是转换）", "常驻LAN / WiFi掉线重连", "LAN失败屏幕提示仍欠缺", "不得据此理解为自动热点", color="red", kind="note", size=18)
    p.box("clock_note", 85, 620, 350, 110, "屏幕断电仍保留画面", "USB / HTTP / BLE等待：屏幕电源关", "屏幕断电 ≠ PMU整机关机", "可与LAN常驻共存（但尚未实现）", kind="note", size=18)
    p.edge("boot", "route", [(800, 225), (800, 290)], label=(800, 258, 180, "读取启动来源"))
    p.edge("route", "daily", [(800, 390), (800, 480)], label=(800, 435, 240, "其余：RTC例行启动"))
    p.edge("route", "service", [(1010, 340), (1140, 340)], "purple", label=(1075, 319, 118, "用户触发"))
    p.edge("service", "daily", [(1330, 505), (1330, 600), (1060, 600), (1060, 525), (1010, 525)], label=(1210, 600, 220, "结束 / BLE失败"))
    p.edge("daily", "refresh", [(800, 570), (800, 655)], label=(800, 612, 240, "跳过 / 成功 / 失败"))
    p.edge("refresh", "boundary", [(800, 755), (800, 825)], label=(800, 790, 210, "检查当前分钟"))
    p.edge("boundary", "daily", [(590, 865), (490, 865), (490, 525), (590, 525)], label=(490, 775, 106, "是", "立即补刷"))
    p.edge("boundary", "off", [(720, 905), (720, 950), (450, 950), (450, 995)], "gray", label=(565, 950, 185, "否 · 电池供电"))
    p.edge("boundary", "usb", [(880, 905), (880, 950), (1150, 950), (1150, 995)], label=(1030, 950, 180, "否 · USB供电"))
    p.edge("off", "boot", [(250, 1045), (35, 1045), (35, 175), (590, 175)], "gray", label=(305, 175, 270, "RTC / PWR → 冷启动"))
    p.edge("usb", "daily", [(1150, 995), (1150, 645), (1090, 645), (1090, 555), (1010, 555)], label=(1150, 840, 230, "整分到达", "先采样按键 / 每日检查"))
    p.edge("usb", "service", [(1350, 1045), (1550, 1045), (1550, 240), (1330, 240), (1330, 300)], "purple", label=(1440, 240, 130, "检测到按键"))
    return p


def ble_page():
    p = Page("ble", "BLE · 配网验证、结束与持久化", height=1200)
    p.box("request", 80, 150, 400, 110, "UserRequest / 用户服务入口", "上键或无保存SSID：需要BLE", "否则直接进入LAN", size=18)
    p.box("advertise", 620, 150, 440, 120, "广播 / 等待凭据", "Security1 + 6位PoP码", "PoP失败不提交WiFi；继续等待", color="purple")
    p.box("lan", 80, 370, 360, 90, "无需BLE → LAN", "STA HTTP；详见 LAN 图")
    p.box("connect", 620, 370, 440, 110, "候选 WiFi 有限连接", "SDK 最多3次连接尝试", "失败且预算内：重置失败状态", color="purple")
    p.box("verified", 620, 590, 440, 110, "取得IP / CRED_SUCCESS", "锁存 success；后续FAIL不清零", "仅验证成功 ≠ 已保存 clock_net", color="green", size=18)
    p.box("cleanup", 620, 800, 440, 90, "结束配网会话", "停止BLE；清除SDK临时WiFi数据", color="purple", size=18)
    p.box("save", 620, 975, 440, 90, "持久化候选凭据", "写入 clock_net")
    p.box("success", 1180, 970, 350, 110, "保存成功 → run_management", "先尝试NTP（失败也继续）", "再进入 STA HTTP / LAN", color="green", size=18)
    p.box("failure", 80, 800, 360, 110, "失败结束 → 返回时钟", "未验证WiFi失败：不写clock_net", "初始化 / 清理错误：保留旧配置", "NVS保存失败：可能部分写入", color="red", size=17)
    p.box("disconnect_note", 80, 570, 360, 115, "网页断开 ≠ 取消", "已提交的配网工作仍可能保存成功", "不画“网页断开 → 失败”转换", kind="note", size=17)
    p.box("sdk_note", 1180, 390, 350, 130, "SDK内部过程为抽象状态", "CRED_SUCCESS / END 为SDK事件", "取得IP只锁存验证结果", "只有后续写 clock_net 才算保存成功", kind="note", size=17)
    p.box("deadline_note", 1180, 590, 350, 150, "活动deadline = 函数入口 + 295s", "预留5s清理；并非硬实时期限", "已锁存success时到deadline", "仍可能清理后保存新配置", color="purple", kind="note", size=17)
    p.box("persistence_note", 1180, 790, 350, 130, "持久化边界：非原子事务", "network_save_config 顺序写多个key", "NVS保存失败可能已部分写入", "不保证回滚 / 保留完整旧配置", color="red", kind="note", size=17)
    p.edge("request", "advertise", [(480, 205), (620, 205)], "purple", label=(550, 180, 134, "上键 / 无SSID"))
    p.edge("request", "lan", [(280, 260), (280, 370)], label=(280, 315, 240, "已有SSID且非上键"))
    p.edge("advertise", "connect", [(840, 270), (840, 370)], "purple", label=(840, 320, 230, "安全握手通过并提交凭据"))
    p.edge("connect", "advertise", [(620, 425), (535, 425), (535, 240), (620, 240)], "red", label=(535, 340, 132, "失败且预算内", "可重填凭据"))
    p.edge("connect", "verified", [(840, 480), (840, 590)], "green", label=(840, 535, 215, "取得IP；验证成功"))
    p.edge("verified", "cleanup", [(840, 700), (840, 800)], "purple", label=(840, 750, 430, "GetStatus触发END / SDK自动结束（默认约30s）", "或已锁存success后到活动deadline"))
    p.edge("cleanup", "save", [(840, 890), (840, 975)], label=(840, 932, 270, "清理完成后才写入"))
    p.edge("save", "success", [(1060, 1020), (1180, 1020)], "green", label=(1120, 1020, 105, "保存成功"))
    p.edge("save", "failure", [(620, 1020), (500, 1020), (500, 865), (440, 865)], "red", label=(500, 955, 140, "保存失败"))
    p.edge("cleanup", "failure", [(620, 845), (440, 845)], "red", label=(530, 818, 155, "清理错误"))
    p.edge("advertise", "failure", [(840, 150), (840, 120), (35, 120), (35, 855), (80, 855)], "red", label=(420, 120, 690, "未验证成功且到活动deadline / 初始化错误"))
    p.edge("connect", "failure", [(620, 455), (565, 455), (565, 550), (35, 550), (35, 855), (80, 855)], "red", label=(300, 550, 480, "未验证成功且到活动deadline"))
    return p


def lan_page():
    p = Page("lan", "LAN · 临时 STA HTTP 管理窗口", height=1160)
    p.box("entry", 100, 150, 400, 110, "用户服务 / BLE成功后的入口", "不是常驻LAN；不自启动热点")
    p.box("sta", 600, 150, 430, 110, "STA 连接 ≤15s", "或复用已经连接的STA")
    p.box("failure", 1150, 150, 350, 110, "连接失败", "记录日志；关闭WiFi", "屏幕失败提示仍欠缺", color="red", size=18)
    p.box("http", 600, 420, 430, 140, "HTTP 启动 / 活动（临时密码）", "GET /api/status：不延长idle", "GET / 和 POST：延长idle", "提供状态 / 手动NTP / exit", size=18)
    p.box("manual", 1160, 420, 340, 140, "HTTP 手动校时", "内部STA最多15s + NTP≤10s", "成功 / 失败均回HTTP", size=18)
    p.box("offline", 600, 740, 430, 105, "WiFi断线 → 网络不可达", "HTTP任务仍在；没有自动重连", "仍等待原idle / 总时限清理", color="red", size=18)
    p.box("cleanup", 100, 740, 360, 105, "结束管理窗口", "停止HTTP + 关闭WiFi", color="gray")
    p.box("clock", 100, 990, 360, 90, "返回时钟调用方", "MAIN：每日检查 → 刷屏")
    p.box("limits", 80, 355, 360, 120, "窗口限额", "idle 5min 或总时长15min", "POST exit 可提前结束", kind="note", size=18)
    p.box("offline_note", 1100, 910, 400, 100, "未实现：常驻LAN / 掉线自动重连", "不可达不是退出；任务等待限额到期", color="red", kind="note", size=17)
    p.edge("entry", "sta", [(500, 205), (600, 205)], label=(550, 180, 90, "进入"))
    p.edge("sta", "failure", [(1030, 205), (1150, 205)], "red", label=(1090, 205, 100, "失败"))
    p.edge("sta", "http", [(815, 260), (815, 420)], "green", label=(815, 340, 330, "得到IP / 复用连接", "每次生成临时HTTP密码"))
    p.edge("failure", "clock", [(1500, 205), (1550, 205), (1550, 1045), (460, 1045)], "red", label=(1120, 1045, 300, "连接失败后直接返回时钟"))
    p.edge("http", "manual", [(1030, 455), (1090, 455), (1090, 375), (1330, 375), (1330, 420)], label=(1280, 375, 250, "POST 手动NTP"))
    p.edge("manual", "http", [(1160, 525), (1030, 525)], label=(1095, 525, 122, "成功 / 失败"))
    p.edge("http", "http", [(600, 455), (520, 455), (520, 610), (650, 610), (650, 560)], label=(650, 610, 250, "GET请求：按节点规则"))
    p.edge("http", "offline", [(815, 560), (815, 740)], "red", label=(815, 650, 190, "WiFi断线"))
    p.edge("manual", "offline", [(1330, 560), (1330, 792), (1030, 792)], "red", label=(1330, 650, 270, "若掉线：网络不可达"))
    p.edge("http", "cleanup", [(600, 505), (485, 505), (485, 690), (280, 690), (280, 740)], label=(280, 665, 350, "exit / idle 5min / 总15min", "或HTTP启动 / 注册失败"))
    p.edge("offline", "cleanup", [(600, 792), (460, 792)], label=(530, 792, 128, "idle / 上限"))
    p.edge("cleanup", "clock", [(280, 845), (280, 990)], label=(280, 918, 220, "无线服务已关闭"))
    return p


def ntp_page():
    p = Page("ntp", "NTP · 每日自动校时与当天尝试锁定", height=1160)
    p.box("entry", 100, 150, 340, 110, "每分钟时钟流程进入", "MAIN：每日检查调用点")
    p.box("gate", 590, 150, 440, 145, "自动校时条件：必须全部满足", "有SSID && RTC有效", "hour ≥ 配置小时 && 当天未尝试", "默认8点；不满足则跳过无线", size=18)
    p.box("skip", 100, 420, 340, 120, "不联网，直接返回", "条件不满足：skip", "或 last_attempt_day 保存失败", color="gray", size=17)
    p.box("record", 590, 405, 440, 100, "先写 NVS：last_attempt_day", "保存成功才允许联网")
    p.box("sta", 590, 615, 440, 90, "STA连接 ≤15s", "失败不重试当天自动校时")
    p.box("ntp", 590, 810, 440, 90, "NTP请求 ≤10s", "失败不重试当天自动校时")
    p.box("success", 1130, 800, 370, 120, "成功：写RTC", "尝试保存 last_sync / 校正后日期", "NVS失败仅告警；仍请求强制全刷", color="green", size=18)
    p.box("cleanup", 1130, 1020, 370, 80, "关闭WiFi", "自动校时结束", color="gray")
    p.box("return", 100, 1010, 340, 90, "返回时钟调用方", "继续采样 / 刷屏")
    p.box("invalid_note", 1140, 150, 380, 145, "待设计缺口（不是已实现分支）", "RTC无效会一直skip自动校时", "本图不画自动修复 / 强制联网", color="red", kind="note", size=18)
    p.box("attempt_note", 100, 650, 340, 170, "当天尝试锁定", "先记录尝试日，再尝试联网", "STA / NTP失败：当天不自动再试", "手动NTP不走本图每日门槛", kind="note", size=17)
    p.edge("entry", "gate", [(440, 205), (590, 205)], label=(515, 180, 140, "每日检查"))
    p.edge("gate", "skip", [(590, 245), (490, 245), (490, 360), (270, 360), (270, 420)], label=(330, 360, 270, "任一条件不满足"))
    p.edge("gate", "record", [(810, 295), (810, 405)], label=(810, 350, 210, "条件全部满足"))
    p.edge("record", "skip", [(590, 455), (515, 455), (515, 490), (440, 490)], "red", label=(515, 490, 138, "NVS保存失败"))
    p.edge("record", "sta", [(810, 505), (810, 615)], label=(810, 560, 220, "NVS保存成功"))
    p.edge("sta", "ntp", [(810, 705), (810, 810)], "green", label=(810, 757, 180, "连接成功"))
    p.edge("sta", "cleanup", [(1030, 660), (1550, 660), (1550, 1060), (1500, 1060)], "red", label=(1310, 660, 300, "连接失败 / 超时"))
    p.edge("ntp", "cleanup", [(1030, 870), (1080, 870), (1080, 1060), (1130, 1060)], "red", label=(1080, 965, 150, "NTP失败", "/ 超时"))
    p.edge("ntp", "success", [(1030, 835), (1130, 835)], "green", label=(1080, 810, 92, "成功"))
    p.edge("success", "cleanup", [(1315, 920), (1315, 1020)], "green", label=(1315, 970, 250, "RTC已更新", "元数据写失败仅告警"))
    p.edge("cleanup", "return", [(1130, 1060), (440, 1060)], label=(790, 1060, 300, "成功或失败均关闭WiFi后返回"))
    p.edge("skip", "return", [(270, 540), (65, 540), (65, 1055), (100, 1055)], "gray", label=(250, 900, 280, "不联网；保持时钟流程"))
    return p


def text_width(text, size):
    """Conservative advance estimate for the Latin/CJK labels used here."""
    return sum(1 if ord(c) > 255 else .56 for c in text) * size


def overlaps(a, b):
    return a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h


def validate_layout(p):
    ids = [b.id for b in p.boxes] + [e.id for e in p.edges]
    assert len(ids) == len(set(ids)), f"{p.id}: duplicate ID"
    states = {b.id: b for b in p.boxes if b.kind == "state"}
    assert len(states) <= 10, f"{p.id}: too many states"
    for b in p.boxes:
        assert 0 <= b.x < b.x + b.w <= p.width, (p.id, b.id, "x bounds")
        assert 0 <= b.y < b.y + b.h <= p.height, (p.id, b.id, "y bounds")
        assert max(text_width(t, b.size) for t in b.lines) <= b.w - 8, (p.id, b.id, "text width")
        assert len(b.lines) * (b.size + 6) <= b.h + 2, (p.id, b.id, "text height")
    for i, a in enumerate(p.boxes):
        for b in p.boxes[i + 1:]:
            assert not overlaps(a, b), (p.id, a.id, b.id, "box overlap")
    for e in p.edges:
        assert e.source in states and e.target in states, (p.id, e.id, "reference")
        for id, pt in ((e.source, e.points[0]), (e.target, e.points[-1])):
            b = states[id]
            x, y = pt
            assert (x in (b.x, b.x + b.w) and b.y <= y <= b.y + b.h) or (y in (b.y, b.y + b.h) and b.x <= x <= b.x + b.w), (p.id, e.id, "anchor")
        for (x1, y1), (x2, y2) in zip(e.points, e.points[1:]):
            assert x1 == x2 or y1 == y2, (p.id, e.id, "nonorthogonal")
            for b in p.boxes:
                if b.id == e.id + "_label":
                    continue  # An edge's own white-backed label masks its line.
                # Routes may touch a perimeter, but must not enter a node, note,
                # or another edge's label. Check decorations too, not just states.
                cross = (b.y < y1 < b.y + b.h and min(x1, x2) < b.x + b.w and max(x1, x2) > b.x) if y1 == y2 else (b.x < x1 < b.x + b.w and min(y1, y2) < b.y + b.h and max(y1, y2) > b.y)
                assert not cross, (p.id, e.id, b.id, "route crosses unrelated box")


def render_svg(p):
    # No foreignObject or external resources: portable, selectable native SVG text.
    root = ET.Element("svg", {"xmlns": "http://www.w3.org/2000/svg", "width": str(p.width), "height": str(p.height), "viewBox": f"0 0 {p.width} {p.height}", "role": "img", "aria-labelledby": "svg-title svg-desc"})
    ET.SubElement(root, "title", id="svg-title").text = p.title
    ET.SubElement(root, "desc", id="svg-desc").text = BASELINE + "。未实机验证。实线仅为当前转换；未实现行为仅列于注记。"
    defs = ET.SubElement(root, "defs")
    for name, (_, stroke) in COLORS.items():
        marker = ET.SubElement(defs, "marker", id="arrow-" + name, viewBox="0 0 10 10", refX="9", refY="5", markerWidth="9", markerHeight="9", orient="auto", markerUnits="userSpaceOnUse")
        ET.SubElement(marker, "path", d="M 0 0 L 10 5 L 0 10 z", fill=stroke)
    ET.SubElement(root, "rect", width=str(p.width), height=str(p.height), fill="#ffffff")
    ET.SubElement(root, "path", d=f"M 45 100 H {p.width - 45}", stroke="#dbe2ea", fill="none")
    for e in p.edges:
        ET.SubElement(root, "polyline", {"id": e.id, "points": " ".join(f"{x},{y}" for x, y in e.points), "fill": "none", "stroke": COLORS[e.color][1], "stroke-width": "2.2", "stroke-linejoin": "round", "marker-end": f"url(#arrow-{e.color})"})
    for b in p.boxes:
        g = ET.SubElement(root, "g", id=b.id)
        fill, stroke = COLORS[b.color]
        if b.kind in ("state", "note", "label"):
            ET.SubElement(g, "rect", x=str(b.x), y=str(b.y), width=str(b.w), height=str(b.h), rx="10" if b.kind != "label" else "3", fill=fill if b.kind != "label" else "#ffffff", stroke=stroke if b.kind != "label" else "none", **{"stroke-width": "1.8"})
        line_height = b.size + 6 if b.kind != "label" else 22
        top = b.y + (b.h - (len(b.lines) - 1) * line_height) / 2
        for i, line in enumerate(b.lines):
            attrs = {"x": str(b.x + b.w / 2), "y": str(top + i * line_height), "text-anchor": "middle", "dominant-baseline": "central", "font-family": FONT, "font-size": str(b.size), "fill": "#172b44", "font-weight": "600" if (i == 0 and b.kind in ("state", "note", "heading")) else "400"}
            if b.kind in ("heading", "text"):
                attrs.update({"x": str(b.x), "text-anchor": "start"})
            ET.SubElement(g, "text", attrs).text = line
    ET.indent(root)
    return ET.tostring(root, encoding="unicode", xml_declaration=True)


def drawio_box(root, b):
    fill, stroke = COLORS[b.color]
    lines = [escape(t) for t in b.lines]
    if b.kind in ("state", "note", "heading"):
        lines[0] = "<b>" + lines[0] + "</b>"
    style = f"html=1;whiteSpace=wrap;fontFamily={FONT};fontSize={b.size};fontColor=#172b44;align=center;verticalAlign=middle;spacing=0;spacingTop=0;spacingBottom=0;overflow=hidden;"
    if b.kind in ("state", "note"):
        style += f"rounded=1;arcSize=10;fillColor={fill};strokeColor={stroke};strokeWidth=1.8;"
    elif b.kind == "label":
        style += "text;fillColor=#ffffff;strokeColor=none;"
    else:
        style += "text;fillColor=none;strokeColor=none;align=left;"
    cell = ET.SubElement(root, "mxCell", id=b.id, value="<br>".join(lines), style=style, vertex="1", parent="1")
    ET.SubElement(cell, "mxGeometry", x=str(b.x), y=str(b.y), width=str(b.w), height=str(b.h), **{"as": "geometry"})


def render_drawio(pages):
    doc = ET.Element("mxfile", host="app.diagrams.net", version="24.7.17", compressed="false")
    for p in pages:
        diagram = ET.SubElement(doc, "diagram", id=p.id, name=p.title)
        model = ET.SubElement(diagram, "mxGraphModel", dx=str(p.width), dy=str(p.height), grid="1", gridSize="10", guides="1", tooltips="1", connect="1", arrows="1", fold="1", page="1", pageScale="1", pageWidth=str(p.width), pageHeight=str(p.height), math="0", shadow="0")
        root = ET.SubElement(model, "root")
        ET.SubElement(root, "mxCell", id="0")
        ET.SubElement(root, "mxCell", id="1", parent="0")
        # Edges first so white label cells and nodes remain above every route.
        states = {b.id: b for b in p.boxes if b.kind == "state"}
        for e in p.edges:
            src, dst = states[e.source], states[e.target]
            x1, y1 = e.points[0]
            x2, y2 = e.points[-1]
            style = f"html=1;edgeStyle=none;rounded=0;endArrow=block;endFill=1;endSize=9;strokeWidth=2.2;strokeColor={COLORS[e.color][1]};exitX={(x1 - src.x) / src.w};exitY={(y1 - src.y) / src.h};exitPerimeter=0;entryX={(x2 - dst.x) / dst.w};entryY={(y2 - dst.y) / dst.h};entryPerimeter=0;"
            cell = ET.SubElement(root, "mxCell", id=e.id, value="", style=style, edge="1", parent="1", source=e.source, target=e.target)
            geo = ET.SubElement(cell, "mxGeometry", relative="1", **{"as": "geometry"})
            ET.SubElement(geo, "mxPoint", x=str(x1), y=str(y1), **{"as": "sourcePoint"})
            ET.SubElement(geo, "mxPoint", x=str(x2), y=str(y2), **{"as": "targetPoint"})
            if len(e.points) > 2:
                points = ET.SubElement(geo, "Array", **{"as": "points"})
                for x, y in e.points[1:-1]:
                    ET.SubElement(points, "mxPoint", x=str(x), y=str(y))
        for b in p.boxes:
            drawio_box(root, b)
    ET.indent(doc)
    return ET.tostring(doc, encoding="unicode", xml_declaration=True)


def validate_xml(path):
    doc = ET.parse(path).getroot()
    if doc.tag == "mxfile":
        assert len(doc.findall("diagram")) == 4
        for diagram in doc.findall("diagram"):
            cells = diagram.findall("mxGraphModel/root/mxCell")
            ids = {c.attrib["id"] for c in cells}
            assert len(ids) == len(cells), "duplicate mxCell ID"
            vertices = {c.attrib["id"] for c in cells if c.get("vertex") == "1"}
            for cell in cells:
                assert cell.get("parent", "0") in ids
                if cell.get("edge") == "1":
                    assert cell.get("source") in vertices
                    assert cell.get("target") in vertices
                if "<" in cell.get("value", ""):
                    assert "html=1;" in cell.get("style", "")
    else:
        assert doc.tag == "{http://www.w3.org/2000/svg}svg"
        ids = [e.attrib["id"] for e in doc.iter() if "id" in e.attrib]
        assert len(ids) == len(set(ids))


def main():
    pages = [main_page(), ble_page(), lan_page(), ntp_page()]
    for p in pages:
        p.decorate()
        validate_layout(p)
    OUT.mkdir(parents=True, exist_ok=True)
    for p in pages:
        path = OUT / (p.id + ".svg")
        path.write_text(render_svg(p) + "\n", encoding="utf-8")
        validate_xml(path)
        print(f"OK docs/diagrams/{path.name}: {p.width}×{p.height}, "
              f"{sum(b.kind == 'state' for b in p.boxes)} states, {len(p.edges)} transitions")
    path = OUT / "state-machines.drawio"
    path.write_text(render_drawio(pages) + "\n", encoding="utf-8")
    validate_xml(path)
    print("OK docs/diagrams/state-machines.drawio: 4 editable pages; XML IDs, parent/source/target, html=1 validated")
    print("Layout: absolute orthogonal routes; independent white-backed labels; no overlapping boxes or routes through unrelated boxes.")
    print("Notes: labels remain independently editable (not attached to edges); Chinese fonts depend on the viewer. No hardware validation.")


if __name__ == "__main__":
    main()
