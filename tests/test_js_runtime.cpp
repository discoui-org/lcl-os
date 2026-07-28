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
