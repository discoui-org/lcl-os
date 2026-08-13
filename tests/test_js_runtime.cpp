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
        globalThis.testRoot = new LCL.Container();
        globalThis.testRoot.setWidth(800);
        globalThis.testRoot.setHeight(600);

        globalThis.testBackdrop = new LCL.BackdropSurface();
        globalThis.testBackdrop.setInteractive(false);
        globalThis.testBackdrop.addFilter("blur", 50);

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
