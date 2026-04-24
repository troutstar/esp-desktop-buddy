# esp_desktop_buddy

Reusable Buddy protocol core. It owns framing, snapshot state, prompt state, command routing, and typed events.

It implements the public Buddy [`protocol`](https://github.com/anthropics/claude-desktop-buddy/blob/main/REFERENCE.md) defined in the upstream [`claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy) repository.

Public entry point: `include/esp_desktop_buddy/esp_desktop_buddy.h`

Dependency:

```cmake
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_desktop_buddy
)
```
