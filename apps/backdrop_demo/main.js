// Run from the repository root:
//   build_host/lcl-js apps/backdrop_demo/main.js

const app = new LCL.WindowApp(1080, 700, "Backdrop playground");
app.setAppId("org.lcl.backdrop-demo");
app.setEdgeToEdge(true);
app.setWindowCornerStyle(22, 2);

const root = new LCL.BackdropSurface();
root.setWidth(1080);
root.setHeight(700);
root.setInteractive(false);
root.setEffectBounds("outer-surface");
root.setBackgroundColor(10, 14, 23, 96);

function place(widget, x, y, width, height) {
    widget.setPositionType("absolute");
    widget.setPosition("left", x);
    widget.setPosition("top", y);
    widget.setWidth(width);
    widget.setHeight(height);
    return widget;
}

function text(value, size = 14, color = [224, 230, 242, 255]) {
    const label = new LCL.Text(value);
    label.setFontSize(size);
    label.setTextColor(color);
    return label;
}

function card(color, x, y, width, height, radius = 24) {
    const view = place(new LCL.Container(), x, y, width, height);
    view.setBackgroundColor(color);
    view.setBorderRadius(radius, 2);
    return view;
}

// Painted first: these cards make the in-app backdrop visibly sample sibling content.
root.addChild(card([44, 86, 224, 230], 338, 72, 276, 194, 34));
root.addChild(card([220, 61, 152, 220], 688, 118, 260, 226, 36));
root.addChild(card([31, 185, 159, 210], 470, 430, 330, 174, 34));
root.addChild(card([238, 166, 61, 205], 820, 465, 176, 132, 28));

const heading = place(text("Drag either glass box", 28), 338, 24, 400, 38);
root.addChild(heading);

function makeBox(name, color, x, y) {
    const box = place(new LCL.Container(), x, y, 220, 140);
    box.setBackgroundColor(color);
    box.setBorderColor(255, 255, 255, 155);
    box.setBorderWidth(1);
    box.setBorderRadius(28, 2);
    box.addChild(place(text(name, 18), 20, 20, 180, 28));
    box.addChild(place(text("Backdrop target", 13, [225, 234, 255, 190]),
                       20, 56, 180, 22));
    box.addChild(place(text("Drag me", 13, [225, 234, 255, 170]),
                       20, 94, 180, 22));
    root.addChild(box);
    return { node: box, x, y, width: 220, height: 140,
             effect: "blur", source: "backdrop", values: [18, 2, 6] };
}

const boxA = makeBox("Box A", [238, 248, 255, 74], 412, 180);
const boxB = makeBox("Box B", [255, 244, 252, 72], 652, 344);
const windowTarget = { effect: "blur", values: [16, 2, 6] };

const panel = card([20, 25, 38, 236], 20, 20, 286, 660, 18);
panel.setBorderColor(105, 126, 167, 130);
panel.setBorderWidth(1);
root.addChild(panel);

panel.addChild(place(text("Backdrop demo", 22), 18, 18, 240, 30));
panel.addChild(place(text("Target", 13, [160, 177, 209, 255]), 18, 60, 220, 22));

let selected = "boxA";
const stateFor = () => selected === "boxA" ? boxA :
    selected === "boxB" ? boxB : windowTarget;

const targetLabel = place(text("Editing: Box A", 14, [170, 203, 255, 255]),
                          18, 162, 240, 22);
const effectLabel = place(text("Effect: Blur", 14, [170, 203, 255, 255]),
                          18, 248, 240, 22);
const sourceLabel = place(text("Source: Backdrop", 13, [160, 177, 209, 255]),
                          18, 276, 240, 20);
panel.addChild(targetLabel);
panel.addChild(effectLabel);
panel.addChild(sourceLabel);

function smallButton(label, x, y, width, onClick) {
    const button = place(new LCL.Button(label), x, y, width, 34);
    button.setBorderRadius(10, 2);
    button.setOnClick(onClick);
    panel.addChild(button);
    return button;
}

smallButton("Box A", 18, 88, 76, () => selectTarget("boxA"));
smallButton("Box B", 102, 88, 76, () => selectTarget("boxB"));
smallButton("Window", 186, 88, 82, () => selectTarget("window"));

smallButton("Blur", 18, 192, 76, () => selectEffect("blur"));
smallButton("Glass", 102, 192, 76, () => selectEffect("glass"));
smallButton("Bright", 186, 192, 82, () => selectEffect("brightness"));

smallButton("Backdrop", 18, 302, 120, () => selectSource("backdrop"));
smallButton("Surface", 146, 302, 122, () => selectSource("surface-backdrop"));

const parameterNames = [
    place(text("Radius", 13), 18, 354, 110, 20),
    place(text("Parameter 1", 13), 18, 442, 110, 20),
    place(text("Parameter 2", 13), 18, 530, 110, 20),
];
const parameterValues = [
    place(text("18", 13, [170, 203, 255, 255]), 210, 354, 58, 20),
    place(text("2", 13, [170, 203, 255, 255]), 210, 442, 58, 20),
    place(text("6", 13, [170, 203, 255, 255]), 210, 530, 58, 20),
];
const sliders = [
    place(new LCL.Slider(18, 0, 64, 1), 18, 380, 250, 28),
    place(new LCL.Slider(2, 0, 12, 0.1), 18, 468, 250, 28),
    place(new LCL.Slider(6, 0, 24, 0.1), 18, 556, 250, 28),
];
for (const label of parameterNames) panel.addChild(label);
for (const label of parameterValues) panel.addChild(label);
for (const slider of sliders) panel.addChild(slider);

function rounded(value) {
    return Math.round(value * 10) / 10;
}

function applyTarget() {
    const state = stateFor();
    if (selected === "window") {
        root.clearFilters();
        root.addFilter(state.effect, state.values[0], state.values[1], state.values[2]);
        return;
    }
    state.node.setEffect(state.source, state.effect,
                         state.values[0], state.values[1], state.values[2]);
}

function refreshControls() {
    const state = stateFor();
    const isGlass = state.effect === "glass";
    effectLabel.setText(`Effect: ${state.effect}`);
    sourceLabel.setText(selected === "window"
        ? "Source: Surface backdrop" : `Source: ${state.source}`);
    const names = isGlass ? ["Thickness", "Refraction", "Dispersion"] :
        state.effect === "blur" ? ["Radius", "Unused", "Unused"] :
        ["Amount", "Unused", "Unused"];
    const ranges = isGlass ? [[0, 64, 1], [0, 12, 0.1], [0, 24, 0.1]] :
        state.effect === "blur" ? [[0, 64, 1], [0, 1, 1], [0, 1, 1]] :
        [[-1, 1, 0.05], [0, 1, 1], [0, 1, 1]];
    for (let index = 0; index < sliders.length; index++) {
        parameterNames[index].setText(names[index]);
        parameterValues[index].setText(String(rounded(state.values[index])));
        sliders[index].setRange(ranges[index][0], ranges[index][1]);
        sliders[index].setStep(ranges[index][2]);
        sliders[index].setValue(state.values[index]);
        sliders[index].setEnabled(index === 0 || isGlass);
    }
}

function selectTarget(target) {
    selected = target;
    targetLabel.setText(`Editing: ${target === "boxA" ? "Box A" :
        target === "boxB" ? "Box B" : "Window"}`);
    refreshControls();
}

function selectEffect(effect) {
    stateFor().effect = effect;
    applyTarget();
    refreshControls();
}

function selectSource(source) {
    if (selected === "window") return;
    stateFor().source = source;
    applyTarget();
    refreshControls();
}

for (let index = 0; index < sliders.length; index++) {
    sliders[index].setOnChange((value) => {
        const state = stateFor();
        state.values[index] = value;
        parameterValues[index].setText(String(rounded(value)));
        applyTarget();
    });
}

applyTarget();
selected = "boxB"; applyTarget();
selected = "window"; applyTarget();
selected = "boxA"; refreshControls();

let drag = null;
function hitBox(box, x, y) {
    return x >= box.x && x <= box.x + box.width &&
        y >= box.y && y <= box.y + box.height;
}

app.setOnRawPointerEvent((type, x, y) => {
    if (type === "down") {
        const box = hitBox(boxB, x, y) ? boxB : hitBox(boxA, x, y) ? boxA : null;
        if (!box) return false;
        drag = { box, offsetX: x - box.x, offsetY: y - box.y };
        return true;
    }
    if (!drag) return false;
    if (type === "move") {
        drag.box.x = Math.max(320, Math.min(840, x - drag.offsetX));
        drag.box.y = Math.max(60, Math.min(520, y - drag.offsetY));
        drag.box.node.setPosition("left", drag.box.x);
        drag.box.node.setPosition("top", drag.box.y);
        return true;
    }
    if (type === "up" || type === "cancel") {
        drag = null;
        return true;
    }
    return true;
});

app.setRootWidget(root);
if (!app.connectCompositor()) {
    throw new Error("Could not connect to the LCL compositor");
}
app.runEventLoop();
