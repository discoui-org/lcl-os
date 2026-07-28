console.log("========================================");
console.log("  LCL OS - lcl-ui Phase 2.0 JS Live Demo ");
console.log("========================================");

// 1. Initialize WindowApp Application Pipeline (800x600)
const app = new LCL.WindowApp(800, 600, "LCL-UI JS Interactive Demo App");

// 2. Build Centered Flexbox Layout Tree in User-Space App
const rootContainer = new LCL.Container();
rootContainer.setWidth(800);
rootContainer.setHeight(600);
rootContainer.setDirection("column");
rootContainer.setJustifyContent("center");
rootContainer.setAlignItems("center");
rootContainer.setGap(20);

// Inner Card Container
const cardContainer = new LCL.Container();
cardContainer.setDirection("column");
cardContainer.setAlignItems("center");
cardContainer.setPadding(24);
cardContainer.setGap(16);

// Application State & Interactive Counter Callback
let clickCounter = 0;

// Button & Text Widgets
const clickButton = new LCL.Button("Tıkla: 0");
clickButton.setWidth(160);
clickButton.setHeight(44);

const statusText = new LCL.Text("Tıklama Sayısı: 0");
statusText.setFontSize(16);

clickButton.setOnClick(() => {
    clickCounter++;
    clickButton.setLabel("Tıkla: " + clickCounter);
    statusText.setText("Tıklama Sayısı: " + clickCounter);
    console.log("[ui_demo_js] Button clicked via JS! Counter: " + clickCounter);
});

cardContainer.addChild(clickButton);
cardContainer.addChild(statusText);
rootContainer.addChild(cardContainer);

app.setRootWidget(rootContainer);

// 3. Connect to Compositor IPC & run live window event loop
if (app.connectCompositor()) {
    console.log("[ui_demo_js] JS App connected to compositor! Running live desktop UI loop...");
    app.runEventLoop();
}
