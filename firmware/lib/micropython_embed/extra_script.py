from pathlib import Path

Import("env")  # type: ignore # noqa: F821


def _existing(paths):
    out = []
    seen = set()
    for p in paths:
        p = Path(p)
        if p.is_dir():
            sp = str(p)
            if sp not in seen:
                seen.add(sp)
                out.append(sp)
    return out


def _idf_include_dirs(idf_root: Path, mcu: str):
    components = idf_root / "components"
    if not components.is_dir():
        return []

    needed = [
        "esp_adc",
        "esp_common",
        "esp_hw_support",
        "hal",
        "soc",
        "esp_rom",
    ]
    paths = []
    for comp_name in needed:
        comp = components / comp_name
        if not comp.is_dir():
            continue
        paths.append(comp / "include")
        paths.extend(comp.glob("*/include"))
        paths.append(comp / mcu / "include")
        paths.extend(comp.glob(f"*/{mcu}/include"))
    return _existing(paths)


def _arduino_sdkconfig_include(arduino_root: Path, mcu: str):
    sdk_root = arduino_root / "tools" / "sdk" / mcu
    if not sdk_root.is_dir():
        return None

    preferred = sdk_root / "qio_qspi" / "include" / "sdkconfig.h"
    if preferred.is_file():
        return str(preferred.parent)

    for p in sorted(sdk_root.glob("*/include/sdkconfig.h")):
        if p.is_file():
            return str(p.parent)
    return None


platform = env.PioPlatform()
mcu = env.BoardConfig().get("build.mcu", "esp32s3")

idf_dir = platform.get_package_dir("framework-espidf")
if idf_dir:
    idf_includes = _idf_include_dirs(Path(idf_dir), mcu)
    if idf_includes:
        env.AppendUnique(CPPPATH=idf_includes)

arduino_dir = platform.get_package_dir("framework-arduinoespressif32")
if arduino_dir:
    sdkconfig_inc = _arduino_sdkconfig_include(Path(arduino_dir), mcu)
    if sdkconfig_inc:
        env.AppendUnique(CPPPATH=[sdkconfig_inc])

env.AppendUnique(
    CPPDEFINES=[
        ("ESP_PLATFORM", 1),
        (f"CONFIG_IDF_TARGET_{str(mcu).upper()}", 1),
        ("CONFIG_IDF_TARGET_ARCH_XTENSA", 1),
    ]
)
