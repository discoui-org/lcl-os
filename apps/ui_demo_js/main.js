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
rootContainer.setBackgroundColor(0, 0, 0, 0);

// Inner Card Container
const cardContainer = new LCL.Container();
cardContainer.setDirection("column");
cardContainer.setAlignItems("center");
cardContainer.setPadding(24);
cardContainer.setGap(16);
cardContainer.setBackgroundColor(255, 255, 255, 0);
cardContainer.setBorderColor(180, 210, 255, 0);
cardContainer.setBorderWidth(0);

// Application State & Interactive Counter Callback
let clickCounter = 0;

// Backdrop Blur Button (interactive glass surface)
const clickButton = new LCL.BackdropSurface();
clickButton.setWidth(180);
clickButton.setHeight(46);
clickButton.setDirection("row");
clickButton.setJustifyContent("center");
clickButton.setAlignItems("center");
clickButton.setPadding(10);
clickButton.setBackgroundColor(255, 255, 255, 0);
clickButton.setBorderColor(255, 255, 255, 170);
clickButton.setBorderWidth(2);
clickButton.addFilter("blur", 22);
clickButton.setOpacity(0.72);

const buttonLabel = new LCL.Text("Tıkla: 0");
buttonLabel.setFontSize(16);
buttonLabel.setTextColor(255, 255, 255, 255);
clickButton.addChild(buttonLabel);

const statusText = new LCL.Text("Tıklama Sayısı: 0");
statusText.setFontSize(16);
statusText.setTextColor(255, 255, 255, 220);

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
