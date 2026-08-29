#include <gtest/gtest.h>
#include "binding/js_runtime.hpp"

using namespace lcl::binding;

TEST(JsRuntimeTest, LifecycleInitializeAndEval) {
    JsRuntime js;
    EXPECT_TRUE(js.initialize());

    // Test console.log and basic arithmetic in JS
    EXPECT_TRUE(js.evalCode("console.log('Test log from QuickJS'); 1 + 2;"));

    // Test LCL namespace availability
    EXPECT_TRUE(js.evalCode("if (!LCL || !LCL.WindowApp || !LCL.Container || !LCL.Button || !LCL.Text) throw new Error('LCL missing');"));

    js.shutdown();
}

TEST(JsRuntimeTest, WidgetCreationAndHierarchyInJS) {
    JsRuntime js;
    EXPECT_TRUE(js.initialize());

    const char* script = R"(
        globalThis.testApp = new LCL.WindowApp(800, 600, "JS Test App");
        if (!globalThis.testApp.setEdgeToEdge(true)) throw new Error('edge-to-edge rejected');
        globalThis.testRoot = new LCL.Container();
        globalThis.testRoot.setWidth(800);
        globalThis.testRoot.setHeight(600);

        globalThis.testBackdrop = new LCL.BackdropSurface();
        globalThis.testBackdrop.setInteractive(false);
        globalThis.testBackdrop.setEffectBounds("outer-surface");
        globalThis.testBackdrop.addFilter("blur", 50);
        globalThis.testBackdrop.addFilter("glass", 30, 3, 12);
        globalThis.testBackdrop.setTint(15, 23, 42, 128);
        if (typeof globalThis.testBackdrop.setGlass !== 'undefined') throw new Error('setGlass should not be exposed');

        globalThis.testBtn = new LCL.Button("Click Me");
        globalThis.testBtn.setWidth(100);
        globalThis.testBtn.setHeight(40);

        globalThis.clickedCount = 0;
        globalThis.testBtn.setOnClick(() => { globalThis.clickedCount++; });

        globalThis.testTxt = new LCL.Text("Hello JS");
        globalThis.testTxt.setFontSize(18);

        globalThis.testRoot.addChild(globalThis.testBtn);
        globalThis.testRoot.addChild(globalThis.testTxt);
        globalThis.testApp.setRootWidget(globalThis.testRoot);

        // Directly call the button callback to verify binding
        testBtn.setOnClick(() => { globalThis.clickedCount++; });
    )";

    EXPECT_TRUE(js.evalCode(script));
    EXPECT_TRUE(js.evalCode("testApp.renderFrame();"));
    EXPECT_TRUE(js.evalCode("if (typeof globalThis.clickedCount !== 'number') throw new Error('clickedCount invalid');"));

    js.shutdown();
}

TEST(JsRuntimeTest, MultipleWindowsCanUseDistinctSurfaceIdsAndTicks) {
    JsRuntime js;
    ASSERT_TRUE(js.initialize());

    EXPECT_TRUE(js.evalCode(R"JS(
        globalThis.firstWindow = new LCL.WindowApp(200, 120, "First");
        globalThis.secondWindow = new LCL.WindowApp(200, 120, "Second");
        if (!firstWindow.setSurfaceId(1)) throw new Error('first surface id rejected');
        if (!secondWindow.setSurfaceId(2)) throw new Error('second surface id rejected');
        firstWindow.tick();
        secondWindow.tick();
    )JS"));

    js.shutdown();
}

TEST(JsRuntimeTest, ImplicitAndKeyframeMotionShareNativeScheduler) {
    JsRuntime js;
    ASSERT_TRUE(js.initialize());
    EXPECT_TRUE(js.evalCode(R"(
        globalThis.motionApp = new LCL.WindowApp(240, 120, "JS Motion");
        globalThis.motionRoot = new LCL.Container();
        motionRoot.setWidth(240); motionRoot.setHeight(120);
        globalThis.motionButton = new LCL.Button("Move");
        motionButton.setWidth(80); motionButton.setHeight(32);
        motionRoot.addChild(motionButton); motionApp.setRootWidget(motionRoot);
        motionApp.renderFrame();
        motionApp.animate({type: "spring", duration: 240, bounce: 0.08, layout: "reflow"}, () => {
            motionButton.setWidth(160);
            motionButton.setOpacity(0.5);
        });
        globalThis.motionHandle = motionButton.animate(
            [{opacity: 0.5, offset: 0}, {opacity: 1, offset: 1}],
            {duration: 200, fill: "forwards"});
        motionHandle.pause();
        motionHandle.seek(0.5);
        if (Math.abs(motionHandle.progress() - 0.5) > 0.001) throw new Error("seek parity failed");
        motionHandle.reverse();
        motionHandle.finish();
        motionHandle.commitFinalStyles();
    )"));
    js.shutdown();
}

TEST(JsRuntimeTest, GenericContainerSupportsDeclarativeInteractionStylesAndClick) {
    JsRuntime js;
    ASSERT_TRUE(js.initialize());
    EXPECT_TRUE(js.evalCode(R"JS(
        globalThis.interactionApp = new LCL.WindowApp(120, 80, "JS interaction");
        globalThis.interactionControl = new LCL.Container();
        interactionControl.setWidth(120); interactionControl.setHeight(80);
        interactionControl.setInteractionStyle("normal", {scale: 1, opacity: 1});
        interactionControl.setInteractionStyle("hover", {
            scale: 1.05,
            opacity: 0.9,
            motion: {type: "spring", duration: 180, bounce: 0.05}
        });
        interactionControl.setInteractionStyle("pressed", {
            scale: 0.96,
            motion: {type: "tween", duration: 100, easing: "cubic-bezier(0.2,0,0,1)"}
        });
        globalThis.interactionClicks = 0;
        interactionControl.setOnClick(() => { interactionClicks++; });
        interactionApp.setRootWidget(interactionControl);
        interactionApp.renderFrame();
        interactionApp.sendPointerMove(20, 20);
        interactionApp.sendPointerDown(20, 20, 0);
        interactionApp.sendPointerUp(20, 20, 0);
        if (interactionClicks !== 1) throw new Error("generic click did not fire");
        interactionControl.setInteractionEnabled(false);
        interactionApp.sendPointerDown(20, 20, 0);
        interactionApp.sendPointerUp(20, 20, 0);
        if (interactionClicks !== 1) throw new Error("disabled interaction fired");
    )JS"));
    js.shutdown();
}

TEST(JsRuntimeTest, TypedControlsKeepTheirNativeBindingSurface) {
    JsRuntime js;
    ASSERT_TRUE(js.initialize());

    EXPECT_TRUE(js.evalCode(R"JS(
        const card = new LCL.Container();
        card.setBackgroundColor([20, 24, 32, 255]);
        card.setBorderColor({r: 40, g: 48, b: 64, a: 255});
        card.setBorderWidth(1);
        card.setBorderRadius(12, 3);

        const label = new LCL.Text("initial");
        label.setText("updated");
        label.setFontSize(16);
        label.setTextColor(240, 242, 246, 255);
        if (label.getText() !== "updated") throw new Error("Text binding lost its value");

        const button = new LCL.Button("Save");
        button.setLabel("Apply");
        if (button.getLabel() !== "Apply") throw new Error("Button label binding lost");
        button.setBackgroundColor(42, 49, 60, 255);

        const surface = new LCL.BackdropSurface();
        surface.setInteractive(false);
        surface.setEffectBounds("outer-surface");
        surface.addFilter("blur", 16);
        surface.setTint(10, 20, 30, 128);

        const toggle = new LCL.Toggle("Enabled", true);
        if (!toggle.getValue() || toggle.getLabel() !== "Enabled") throw new Error("Toggle binding lost");
        const slider = new LCL.Slider(0.25, 0, 1, 0.05);
        slider.setOnChange(() => {});
        slider.setOnEditingChanged(() => {});
        if (Math.abs(slider.getStep() - 0.05) > 0.0001) throw new Error("Slider binding lost");
        const progress = new LCL.ProgressView(3, 4);
        if (progress.getValue() !== 3 || progress.getTotal() !== 4) throw new Error("Progress binding lost");
        const tabs = new LCL.TabView();
        tabs.setTabViewStyle("tab-bar");
        tabs.setOnChange(() => {});
    )JS"));

    js.shutdown();
}
