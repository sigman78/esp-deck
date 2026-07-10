idf_build_get_property(target IDF_TARGET)

# CYBERDECK: this is a vendored copy of IDF v5.5.2's esp_hid with four BLE
# HID host fixes (see CYBERDECK_PATCHES.md) — it deliberately SHADOWS the
# SDK component via the documented project-component override. Pin it to
# the IDF version it was copied from so an SDK bump can't silently reuse
# these 5.5.2-era sources against newer IDF internals.
#
# Use the NUMERIC version properties (from esp_idf_version.h), NOT IDF_VER:
# IDF_VER comes from `git describe`, which returns "-128-NOTFOUND" on a tree
# with a broken submodule git config — a false positive that must not fail
# the build. Only assert on a genuine major.minor mismatch.
idf_build_get_property(idf_major IDF_VERSION_MAJOR)
idf_build_get_property(idf_minor IDF_VERSION_MINOR)
if(idf_major AND idf_minor AND NOT (idf_major EQUAL 5 AND idf_minor EQUAL 5))
    message(FATAL_ERROR
        "components/esp_hid was vendored from IDF v5.5.2 but this build uses "
        "IDF ${idf_major}.${idf_minor}.x. Re-derive it per "
        "components/esp_hid/CYBERDECK_PATCHES.md (on >= v5.5.3 re-copy from the "
        "SDK and re-apply only the still-missing patches; on the release "
        "containing all of them, delete this component entirely).")
endif()

if(${target} STREQUAL "linux")
    return() # This component is not supported by the POSIX/Linux simulator
endif()

set(srcs "src/esp_hidd.c"
         "src/esp_hidh.c"
         "src/esp_hid_common.c")

set(include_dirs "include")
set(priv_include_dirs "private")

if(CONFIG_BT_ENABLED)
    if(CONFIG_BT_BLUEDROID_ENABLED)
        list(APPEND srcs
             "src/ble_hidd.c"
             "src/ble_hidh.c"
             "src/bt_hidh.c"
             "src/bt_hidd.c")
    endif()
    if(CONFIG_BT_NIMBLE_ENABLED)
        list(APPEND srcs
                "src/nimble_hidd.c"
                "src/nimble_hidh.c")
    endif()
endif()

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${include_dirs}"
                       PRIV_INCLUDE_DIRS "${priv_include_dirs}"
                       REQUIRES esp_event bt)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
