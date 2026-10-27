#include "eui_neo.h"

#include <iostream>

namespace app 
{
    const DslAppConfig& dslAppConfig() {
        static const DslAppConfig config = DslAppConfig{}
            .title("Hello EUI-NEO")
            .windowSize(800, 520);
        return config;
    }

    void compose(eui::Ui& ui, const eui::Screen& screen) {
        ui.column("root")
            .size(screen.width, screen.height)
            .padding(32.0f)
            .gap(18.0f)
            .content([&] {
                ui.text("title")
                    .text("Hello, EUI-NEO!")
                    .fontSize(32.0f)
                    .build();

                components::button(ui, "button")
                    .text("Click me")
                    .onClick([] {
                        std::cout << "Button clicked" << std::endl;
                    })
                    .build();
            })
            .build();
    }
}