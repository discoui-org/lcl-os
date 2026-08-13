console.log("========================================");
console.log("  LCL OS - lcl-ui Phase 2.0 JS Live Demo ");
console.log("========================================");

// 1. Initialize WindowApp Application Pipeline (800x600)
const app = new LCL.WindowApp(800, 600, "LCL-UI JS Interactive Demo App");
app.setAppId("org.lcl.uidemo-js");

// 2. Build Centered Flexbox Layout Tree in User-Space App
const rootContainer = new LCL.BackdropSurface();
rootContainer.setInteractive(false);
rootContainer.addFilter("blur", 50);
rootContainer.addFilter("glass", 30, 3, 12);
rootContainer.setWidth(800);
rootContainer.setHeight(600);
rootContainer.setDirection("column");
rootContainer.setJustifyContent("center");
rootContainer.setAlignItems("center");
rootContainer.setGap(20);
rootContainer.setBackgroundColor(17, 19, 23, 190);

// Inner Card Container
const cardContainer = new LCL.Container();
cardContainer.setDirection("column");
cardContainer.setAlignItems("center");
cardContainer.setPadding(24);
cardContainer.setGap(16);
cardContainer.setBackgroundColor(26, 30, 36, 255);
cardContainer.setBorderColor(10, 12, 16, 120);
cardContainer.setBorderWidth(1);
cardContainer.setBorderRadius(0);

// Application State & Interactive Counter Callback
let clickCounter = 0;

// Interactive demo button
const clickButton = new LCL.Container();
clickButton.setWidth(180);
clickButton.setHeight(46);
clickButton.setDirection("row");
clickButton.setJustifyContent("center");
clickButton.setAlignItems("center");
clickButton.setPadding(10);
clickButton.setBackgroundColor(36, 42, 52, 255);
clickButton.setBorderColor(86, 95, 112, 255);
clickButton.setBorderWidth(1);
clickButton.setBorderRadius(14);
clickButton.setOpacity(1.0);
clickButton.setInteractionStyle("normal", {scale: 1, opacity: 1});
clickButton.setInteractionStyle("hover", {
    scale: 1.015,
    motion: {type: "spring", duration: 180, bounce: 0}
});
clickButton.setInteractionStyle("pressed", {
    scale: 0.965,
    opacity: 0.92,
    motion: {type: "spring", duration: 100, bounce: 0}
});

const buttonLabel = new LCL.Text("Tıkla: 0");
buttonLabel.setFontSize(16);
buttonLabel.setTextColor(236, 239, 244, 255);
clickButton.addChild(buttonLabel);

const statusText = new LCL.Text("Tıklama Sayısı: 0");
statusText.setFontSize(16);
statusText.setTextColor(196, 202, 211, 255);

clickButton.setOnClick(() => {
    clickCounter++;
    buttonLabel.setText("Tıkla: " + clickCounter);
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
