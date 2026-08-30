// Deterministic retained-UI workload for LCL_TRACE_FRAMES=1 measurements.
// It deliberately exercises two different costs without calling renderer APIs:
// retained transform/opacity updates, followed by bounded content/layout damage.

const app = new LCL.WindowApp(1080, 700, "UI Performance Demo");
app.setAppId("org.lcl.ui-perf-demo");
app.setEdgeToEdge(true);
app.setWindowCornerStyle(22, 2);

const root = new LCL.Container();
root.setWidth(1080);
root.setHeight(700);
root.setBackgroundColor(10, 14, 24, 255);

function place(widget, x, y, width, height) {
    widget.setPositionType("absolute");
    widget.setPosition("left", x);
    widget.setPosition("top", y);
    widget.setWidth(width);
    widget.setHeight(height);
    return widget;
}

function label(value, size = 14, color = [225, 232, 245, 255]) {
    const text = new LCL.Text(value);
    text.setFontSize(size);
    text.setTextColor(color);
    return text;
}

function card(x, y, width, height, color, radius = 18) {
    const view = place(new LCL.Container(), x, y, width, height);
    view.setBackgroundColor(color);
    view.setBorderRadius(radius, 2);
    return view;
}

const sidebar = card(20, 20, 244, 660, [22, 30, 46, 255], 20);
sidebar.setBorderColor(96, 116, 152, 120);
sidebar.setBorderWidth(1);
sidebar.addChild(place(label("Frame pipeline", 23), 18, 22, 200, 34));
sidebar.addChild(place(label("Deterministic stress scene", 13,
                             [158, 178, 212, 255]), 18, 62, 210, 22));
const phaseText = place(label("Phase: retained motion", 15,
                              [134, 212, 255, 255]), 18, 122, 210, 24);
const sampleText = place(label("Frame: 0", 14, [182, 197, 222, 255]),
                         18, 152, 210, 22);
sidebar.addChild(phaseText);
sidebar.addChild(sampleText);
for (let index = 0; index < 12; index++) {
    const row = card(18, 204 + index * 32, 208, 24,
                     index % 2 ? [28, 39, 60, 255] : [31, 44, 67, 255], 8);
    row.addChild(place(label(`Subsystem ${String(index + 1).padStart(2, "0")}`,
                             12, [178, 195, 224, 255]), 10, 4, 130, 18));
    const indicator = card(174, 7, 22 + (index % 4) * 6, 10,
                           [66 + index * 7, 177, 185, 255], 5);
    row.addChild(indicator);
    sidebar.addChild(row);
}
root.addChild(sidebar);

const title = place(label("Mixed retained interface", 30), 292, 26, 490, 42);
const subtitle = place(label("120 frames retained motion, then 120 frames bounded repaint",
                             14, [154, 176, 210, 255]), 292, 72, 640, 24);
root.addChild(title);
root.addChild(subtitle);

const chart = card(292, 116, 748, 174, [20, 28, 43, 255], 20);
chart.setBorderColor(95, 118, 156, 105);
chart.setBorderWidth(1);
chart.addChild(place(label("Composition workload", 16), 20, 16, 260, 24));
for (let index = 0; index < 26; index++) {
    const height = 24 + ((index * 37) % 92);
    const bar = card(20 + index * 27, 144 - height, 16, height,
                     [54 + (index % 4) * 25, 112 + (index % 5) * 15,
                      216, 220], 8);
    chart.addChild(bar);
}
root.addChild(chart);

const tileColors = [
    [43, 104, 220, 255], [180, 66, 191, 255], [32, 156, 147, 255],
    [207, 133, 53, 255], [68, 126, 205, 255], [158, 87, 177, 255],
];
const tiles = [];
for (let index = 0; index < 30; index++) {
    const column = index % 6;
    const row = Math.floor(index / 6);
    const tile = card(292 + column * 124, 318 + row * 70, 108, 56,
                      tileColors[index % tileColors.length], 16);
    tile.setBorderColor(235, 244, 255, 80);
    tile.setBorderWidth(1);
    tile.addChild(place(label(`Node ${index + 1}`, 12), 12, 10, 80, 18));
    tile.addChild(place(label(index % 3 === 0 ? "active" : "retained", 11,
                              [232, 240, 255, 210]), 12, 30, 82, 16));
    root.addChild(tile);
    tiles.push(tile);
}

const meter = card(292, 676 - 26, 748, 10, [30, 42, 64, 255], 5);
const meterFill = card(0, 0, 1, 10, [83, 202, 167, 255], 5);
meter.addChild(meterFill);
root.addChild(meter);

let frame = 0;
let displayedPhase = -1;
let lastMutationMs = -Infinity;
app.setOnFrame(() => {
    // WindowApp polls input while a frame is in flight. Keep this benchmark's
    // mutation cadence at most 144 Hz so its phase length reflects submitted
    // frames instead of the 2 ms idle poll cadence.
    const nowMs = Date.now();
    if (nowMs - lastMutationMs < 6) return;
    lastMutationMs = nowMs;
    const phase = Math.floor(frame / 120) % 2;
    const cycleFrame = frame % 120;

    // Phase 0 stays on the retained-presentation path: no text/content changes.
    // Phase 1 changes small, disjoint card bounds to exercise region damage.
    for (let index = 0; index < tiles.length; index++) {
        const wave = Math.sin((frame + index * 11) * 0.085);
        if (phase === 0) {
            tiles[index].setTranslation(wave * (5 + index % 5),
                                        Math.cos((frame + index * 7) * 0.065) * 3);
            tiles[index].setOpacity(0.82 + (wave + 1) * 0.09);
            tiles[index].setRotation(wave * 0.025);
        } else if ((index + cycleFrame) % 3 === 0) {
            tiles[index].setWidth(102 + ((cycleFrame + index * 3) % 12));
            tiles[index].setHeight(52 + ((cycleFrame + index) % 8));
        }
    }

    if (phase !== displayedPhase) {
        displayedPhase = phase;
        phaseText.setText(phase === 0 ? "Phase: retained motion" :
                                      "Phase: bounded repaint");
    }
    if (frame % 30 === 0) {
        sampleText.setText(`Frame: ${frame}`);
    }
    if (phase === 1) {
        meterFill.setWidth(1 + Math.floor((cycleFrame / 119) * 746));
    }
    frame++;
});

app.setRootWidget(root);
if (!app.connectCompositor()) {
    throw new Error("Could not connect to the LCL compositor");
}
app.runEventLoop();
