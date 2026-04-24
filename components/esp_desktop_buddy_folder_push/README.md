# esp_desktop_buddy_folder_push

Optional `char_*` folder-push implementation for `esp_desktop_buddy`. Callers provide the sink that accepts or rejects content.

Public header: `include/esp_desktop_buddy/folder_push.h`

Dependency:

```cmake
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_desktop_buddy_folder_push
)
```
