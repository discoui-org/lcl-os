#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include "lcl-ui/core/window_app.hpp"
#include "lcl-ui/widgets/backdrop_surface.hpp"
#include "lcl-ui/widgets/container.hpp"
#include "lcl-ui/widgets/button.hpp"
#include "lcl-ui/widgets/menu.hpp"
#include "lcl-ui/widgets/popover.hpp"
#include "lcl-ui/widgets/scroll_view.hpp"
#include "lcl-ui/widgets/text.hpp"
#include "lcl-ui/widgets/text_field.hpp"
#include "lcl-ui/widgets/toggle.hpp"
#include "render/raster_canvas.hpp"

using namespace lcl::ui;

namespace {

void applyTypography(Text &text, const lcl::theme::TypographyRole &role) {
  text.setFontSize(role.fontSize);
  text.setTextColor(role.foreground);
}

} // namespace

int main() {
  std::cout << "========================================\n";
  std::cout << "  LCL OS - lcl-ui ScrollView Live Demo  \n";
  std::cout << "========================================\n";

  // 1. Initialize WindowApp Application Pipeline (800x600)
  WindowApp app(lcl::render::makeRasterCanvas(), 800, 600,
                "LCL-UI ScrollView Interactive Demo App");
  app.setAppId("org.lcl.uidemo");
  app.setWindowCornerStyle(20.0f, 2.0f);
  app.setEdgeToEdge(true);
  const auto &theme = app.getTheme();
  const auto &metrics = theme.metrics;
  Popover popover(app, [] { return lcl::render::makeRasterCanvas(); });
  Menu menu(app, [] { return lcl::render::makeRasterCanvas(); });

  // 2. Build Centered Flexbox Layout Tree in User-Space App
  auto rootContainer = std::make_unique<Container>();
  rootContainer->getYogaNode().setWidth(800.0f);
  rootContainer->getYogaNode().setHeight(600.0f);

  auto backdrop = std::make_unique<BackdropSurface>();
  backdrop->setInteractive(false);
  // The material fills the outer surface behind system insets. Widget
  // content remains in the compositor-provided safe content rect.
  backdrop->setEffectBounds(EffectBounds::OuterSurface);
  backdrop->addFilter(lcl::protocol::FilterType::Blur, 50.0f);
  backdrop->addFilter(lcl::protocol::FilterType::Saturation, 2.0f);
  backdrop->addFilter(lcl::protocol::FilterType::Brightness, 1.1f);
  backdrop->setTint(theme.colors.materialTint);
  backdrop->getYogaNode().setPositionType(YGPositionTypeAbsolute);
  backdrop->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
  backdrop->getYogaNode().setPosition(YGEdgeTop, 0.0f);
  backdrop->getYogaNode().setPosition(YGEdgeRight, 0.0f);
  backdrop->getYogaNode().setPosition(YGEdgeBottom, 0.0f);

  auto content = std::make_unique<Container>();
  content->getYogaNode().setPositionType(YGPositionTypeAbsolute);
  content->getYogaNode().setPosition(YGEdgeLeft, 0.0f);
  content->getYogaNode().setPosition(YGEdgeTop, 0.0f);
  content->getYogaNode().setPosition(YGEdgeRight, 0.0f);
  content->getYogaNode().setPosition(YGEdgeBottom, 0.0f);
  content->getYogaNode().setDirection(YGFlexDirectionColumn);
  content->getYogaNode().setJustifyContent(YGJustifyCenter);
  content->getYogaNode().setAlignItems(YGAlignCenter);
  content->getYogaNode().setGap(YGGutterAll, 16.0f);

  // Main Card Container
  auto cardContainer = std::make_unique<Container>();
  cardContainer->getYogaNode().setDirection(YGFlexDirectionColumn);
  cardContainer->getYogaNode().setAlignItems(YGAlignCenter);
  cardContainer->useStyle(theme.primarySurface);
  cardContainer->getYogaNode().setPadding(YGEdgeAll, metrics.cardPadding);
  cardContainer->getYogaNode().setGap(YGGutterAll, 12.0f);

  // Title & Status Label
  auto titleText = std::make_unique<Text>("ScrollView Test Paneli");
  applyTypography(*titleText, theme.typography.title);

  auto statusText = std::make_unique<Text>("Seçilen: Henüz yok (Tıklama: 0)");
  Text *textPtr = statusText.get();
  applyTypography(*statusText, theme.typography.description);

  auto popoverTitle = std::make_unique<Text>("Popover v1");
  applyTypography(*popoverTitle, theme.typography.body);

  auto popoverStatus = std::make_unique<Text>("Popover: kapalı");
  Text *popoverStatusPtr = popoverStatus.get();
  applyTypography(*popoverStatus, theme.typography.description);
  popoverStatus->getYogaNode().setWidth(300.0f);
  popoverStatus->getYogaNode().setHeight(18.0f);

  auto makePopoverContent = [&popover, popoverStatusPtr, &theme](
                                const std::string &message) {
    auto handleSlot = std::make_shared<TransientHandle>(0);
    auto panelContent = std::make_unique<Container>();
    panelContent->getYogaNode().setDirection(YGFlexDirectionColumn);
    panelContent->getYogaNode().setGap(YGGutterAll, 10.0f);

    auto messageText = std::make_unique<Text>(message);
    applyTypography(*messageText, theme.typography.body);

    auto popupField = std::make_unique<TextField>();
    popupField->setPlaceholder("Popover TextField");

    auto closeButton = std::make_unique<Button>("Kapat");
    closeButton->setOnClick([&popover, popoverStatusPtr, handleSlot] {
      if (popover.close(*handleSlot)) {
        popoverStatusPtr->setText("Popover: dismissed");
      }
    });

    panelContent->addChild(std::move(messageText));
    panelContent->addChild(std::move(popupField));
    panelContent->addChild(std::move(closeButton));
    return std::pair{std::move(panelContent), std::move(handleSlot)};
  };

  auto localPopoverButton = std::make_unique<Button>("Popover aç");
  Button *localPopoverButtonPtr = localPopoverButton.get();
  localPopoverButton->useStyle(theme.quietButton);
  localPopoverButton->setWidth(220.0f);
  localPopoverButton->setHeight(metrics.regularControlHeight);
  localPopoverButton->setOnClick(
      [&popover, localPopoverButtonPtr, popoverStatusPtr,
       &makePopoverContent] {
        auto [panel, handleSlot] =
            makePopoverContent("Parent-bound transient surface içerik");
        const auto result = popover.show(
            *localPopoverButtonPtr, std::move(panel),
            PopoverOptions{
                .width = 240.0f,
                .height = 150.0f,
                .onDismissed = [popoverStatusPtr] {
                  popoverStatusPtr->setText("Popover: dismissed");
                },
            });
        *handleSlot = result.handle;
        popoverStatusPtr->setText(
            !result ? "Popover: açılamadı"
                    : "Popover: opened popup-surface");
      });

  // ScrollView Viewport (340x280)
  auto scrollView = std::make_unique<ScrollView>();
  scrollView->getYogaNode().setWidth(340.0f);
  scrollView->getYogaNode().setHeight(280.0f);

  // Scrollable Content Container
  auto scrollContent = std::make_unique<Container>();
  scrollContent->getYogaNode().setDirection(YGFlexDirectionColumn);
  scrollContent->getYogaNode().setGap(YGGutterAll, 8.0f);
  scrollContent->useStyle(theme.groupedSurface);
  scrollContent->getYogaNode().setPadding(YGEdgeAll, metrics.groupPadding);

  // TextField Widget Lab section
  auto textFieldTitle = std::make_unique<Text>("TextField");
  applyTypography(*textFieldTitle, theme.typography.body);

  auto textFieldStatus =
      std::make_unique<Text>("Son değişiklik: Henüz yok");
  Text *textFieldStatusPtr = textFieldStatus.get();
  applyTypography(*textFieldStatus, theme.typography.description);
  textFieldStatus->getYogaNode().setWidth(300.0f);
  textFieldStatus->getYogaNode().setHeight(18.0f);
  textFieldStatus->setClipsToBounds(true);

  const auto makeTextField = [textFieldStatusPtr](
                                 const std::string &name,
                                 const std::string &value) {
    auto field = std::make_unique<TextField>(value);
    field->setOnChange([textFieldStatusPtr, name](const std::string &next) {
      textFieldStatusPtr->setText("Son değişiklik — " + name + ": " +
                                  (next.empty() ? "<boş>" : next));
      std::cout << "[lcl_ui_demo] TextField " << name << " changed: "
                << next << std::endl;
    });
    return field;
  };

  auto emptyTextField = makeTextField("Boş", "");
  emptyTextField->setPlaceholder("Buraya yazın...");

  auto prefilledTextField =
      makeTextField("Dolu", "Önceden girilmiş metin");

  auto longTextField = makeTextField(
      "Uzun",
      "Bu çok uzun TextField metni yatay kaydırma ve caret görünürlüğünü test eder.");

  auto utf8TextField =
      makeTextField("UTF-8", "Türkçe ığüşöç — Lazca ǩ ž ʒ");

  scrollContent->addChild(std::move(textFieldTitle));
  scrollContent->addChild(std::move(emptyTextField));
  scrollContent->addChild(std::move(prefilledTextField));
  scrollContent->addChild(std::move(longTextField));
  scrollContent->addChild(std::move(utf8TextField));
  scrollContent->addChild(std::move(textFieldStatus));

  auto buttonStyleTitle = std::make_unique<Text>("Native Button Styles");
  applyTypography(*buttonStyleTitle, theme.typography.body);

  auto buttonStyleRow = std::make_unique<Container>();
  buttonStyleRow->setWidth(300.0f);
  buttonStyleRow->setHeight(metrics.largeControlHeight);
  buttonStyleRow->getYogaNode().setDirection(YGFlexDirectionRow);
  buttonStyleRow->getYogaNode().setGap(YGGutterAll, 10.0f);

  auto defaultButton = std::make_unique<Button>("Default");
  defaultButton->setWidth(140.0f);
  defaultButton->setHeight(metrics.regularControlHeight);

  auto styledButton = std::make_unique<Button>("useStyle");
  styledButton->setWidth(140.0f);
  styledButton->setHeight(metrics.regularControlHeight);
  styledButton->useStyle(theme.quietButton);

  buttonStyleRow->addChild(std::move(defaultButton));
  buttonStyleRow->addChild(std::move(styledButton));
  scrollContent->addChild(std::move(buttonStyleTitle));
  scrollContent->addChild(std::move(buttonStyleRow));

  auto toggleTitle = std::make_unique<Text>("Toggle v1");
  applyTypography(*toggleTitle, theme.typography.body);
  auto toggleStatus = std::make_unique<Text>("Toggle: henüz değişiklik yok");
  Text *toggleStatusPtr = toggleStatus.get();
  applyTypography(*toggleStatus, theme.typography.description);
  toggleStatus->setHeight(18.0f);

  const auto makeToggleRow = [toggleStatusPtr, &theme, &metrics](
                                 const std::string &label,
                                 bool initialValue,
                                 bool enabled) {
    auto row = std::make_unique<Container>();
    row->setWidth(300.0f);
    row->setHeight(metrics.largeControlHeight);
    row->getYogaNode().setDirection(YGFlexDirectionRow);
    row->getYogaNode().setAlignItems(YGAlignCenter);
    row->getYogaNode().setJustifyContent(YGJustifySpaceBetween);

    auto rowLabel = std::make_unique<Text>(label);
    applyTypography(*rowLabel, enabled ? theme.typography.body
                                      : theme.typography.caption);

    auto toggle = std::make_unique<Toggle>(initialValue);
    toggle->setEnabled(enabled);
    toggle->setOnChange([toggleStatusPtr, label](bool value) {
      toggleStatusPtr->setText("Toggle — " + label + ": " +
                               (value ? "On" : "Off"));
    });

    row->addChild(std::move(rowLabel));
    row->addChild(std::move(toggle));
    return row;
  };

  scrollContent->addChild(std::move(toggleTitle));
  scrollContent->addChild(std::move(toggleStatus));
  scrollContent->addChild(makeToggleRow("Normal", false, true));
  scrollContent->addChild(makeToggleRow("Initially ON", true, true));
  scrollContent->addChild(makeToggleRow("Disabled", false, false));

  auto menuTitle = std::make_unique<Text>("Menu v1");
  applyTypography(*menuTitle, theme.typography.body);
  auto menuStatus = std::make_unique<Text>("Menu: henüz seçim yok");
  Text *menuStatusPtr = menuStatus.get();
  applyTypography(*menuStatus, theme.typography.description);
  menuStatus->setHeight(18.0f);
  auto menuButton = std::make_unique<Button>("Open Menu");
  Button *menuButtonPtr = menuButton.get();
  menuButton->setHeight(metrics.regularControlHeight);
  menuButton->setOnClick([&menu, menuButtonPtr, menuStatusPtr, &metrics] {
    const auto setStatus = [menuStatusPtr](const std::string &item) {
      menuStatusPtr->setText("Menu: " + item);
    };
    const auto result = menu.show(
        *menuButtonPtr,
        {
            MenuItem{"New", true, [setStatus] { setStatus("New"); }},
            MenuItem{"Open", true, [setStatus] { setStatus("Open"); }},
            MenuItem{"Save", true, [setStatus] { setStatus("Save"); }},
            MenuItem{"Disabled Item", false,
                     [setStatus] { setStatus("Disabled Item"); }},
            MenuItem{"Quit", true, [setStatus] { setStatus("Quit"); }},
        },
        MenuOptions{
            .width = 220.0f,
            .itemHeight = metrics.regularControlHeight,
            .onDismissed = [menuStatusPtr] {
              menuStatusPtr->setText("Menu: dismissed");
            },
        });
    if (!result) menuStatusPtr->setText("Menu: açılamadı");
  });
  scrollContent->addChild(std::move(menuTitle));
  scrollContent->addChild(std::move(menuStatus));
  scrollContent->addChild(std::move(menuButton));

  // Normal form traversal area. FocusScope is reserved for transient traps.
  auto focusGroup = std::make_unique<Container>();
  focusGroup->getYogaNode().setDirection(YGFlexDirectionColumn);
  focusGroup->getYogaNode().setGap(YGGutterAll, 8.0f);
  focusGroup->useStyle(theme.secondarySurface);
  focusGroup->setPadding(YGEdgeAll, metrics.groupPadding);

  auto focusTitle = std::make_unique<Text>("Focus Traversal");
  applyTypography(*focusTitle, theme.typography.body);
  auto focusHint = std::make_unique<Text>("Tab / Shift+Tab (normal window sırası)");
  applyTypography(*focusHint, theme.typography.caption);

  auto focusButtonA = std::make_unique<Button>("Button A");
  auto focusFieldA = std::make_unique<TextField>();
  focusFieldA->setPlaceholder("TextField A");
  auto focusButtonB = std::make_unique<Button>("Button B");
  auto focusFieldB = std::make_unique<TextField>();
  focusFieldB->setPlaceholder("TextField B");

  focusGroup->addChild(std::move(focusTitle));
  focusGroup->addChild(std::move(focusHint));
  focusGroup->addChild(std::move(focusButtonA));
  focusGroup->addChild(std::move(focusFieldA));
  focusGroup->addChild(std::move(focusButtonB));
  focusGroup->addChild(std::move(focusFieldB));
  scrollContent->addChild(std::move(focusGroup));

  // Add 18 Interactive Row Buttons
  static int clickCounter = 0;
  for (int i = 1; i <= 18; ++i) {
    std::string rowName =
        "Satır #" + std::to_string(i) + (i % 2 == 0 ? " (Çift)" : " (Tek)");
    auto rowBtn = std::make_unique<Button>(rowName);
    rowBtn->useStyle(theme.quietButton);
    rowBtn->setHeight(metrics.compactControlHeight);

    rowBtn->setOnClick([i, textPtr]() {
      clickCounter++;
      std::string newText = "Seçilen: Satır #" + std::to_string(i) +
                            " (Tıklama: " + std::to_string(clickCounter) + ")";
      textPtr->setText(newText);
      std::cout << "[lcl_ui_demo] Scrolled Row #" << i
                << " clicked! Total clicks: " << clickCounter << std::endl;
    });

    scrollContent->addChild(std::move(rowBtn));
  }

  scrollView->setContent(std::move(scrollContent));

  cardContainer->addChild(std::move(titleText));
  cardContainer->addChild(std::move(statusText));
  cardContainer->addChild(std::move(popoverTitle));
  cardContainer->addChild(std::move(popoverStatus));
  cardContainer->addChild(std::move(localPopoverButton));
  cardContainer->addChild(std::move(scrollView));
  content->addChild(std::move(cardContainer));
  rootContainer->addChild(std::move(backdrop));
  rootContainer->addChild(std::move(content));

  auto edgePopoverButton =
      std::make_unique<Button>("Edge popover");
  Button *edgePopoverButtonPtr = edgePopoverButton.get();
  edgePopoverButton->useStyle(theme.quietButton);
  edgePopoverButton->getYogaNode().setPositionType(YGPositionTypeAbsolute);
  edgePopoverButton->setPosition(YGEdgeLeft, 670.0f);
  edgePopoverButton->setPosition(YGEdgeTop, 530.0f);
  edgePopoverButton->setWidth(120.0f);
  edgePopoverButton->setHeight(metrics.regularControlHeight);
  edgePopoverButton->setOnClick(
      [&popover, edgePopoverButtonPtr, popoverStatusPtr,
       &makePopoverContent] {
        auto [panel, handleSlot] =
            makePopoverContent("Window dışına taşan PopupSurface içerik");
        const auto result = popover.show(
            *edgePopoverButtonPtr, std::move(panel),
            PopoverOptions{
                .width = 240.0f,
                .height = 160.0f,
                .onDismissed = [popoverStatusPtr] {
                  popoverStatusPtr->setText("Popover: dismissed");
                },
            });
        *handleSlot = result.handle;
        popoverStatusPtr->setText(
            !result ? "Popover: açılamadı"
                    : "Popover: opened popup-surface");
      });
  rootContainer->addChild(std::move(edgePopoverButton));

  app.setRootWidget(std::move(rootContainer));

  // 3. Connect to Compositor IPC & run live window event loop
  if (app.connectCompositor()) {
    std::cout << "[lcl_ui_demo] App connected to compositor! Running live "
                 "desktop UI loop...\n";
    app.runEventLoop();
  }

  return 0;
}
