from flash_worker.activities import _looks_like_badge_device, _needs_usb_bootloader_recovery


def test_debug_console_port_is_not_badge_candidate():
    assert not _looks_like_badge_device({
        "port": "/dev/cu.debug-console",
        "description": "n/a",
        "hwid": "n/a",
    })


def test_bluetooth_port_is_not_badge_candidate():
    assert not _looks_like_badge_device({
        "port": "/dev/cu.Bluetooth-Incoming-Port",
        "description": "n/a",
        "hwid": "n/a",
    })


def test_usbmodem_port_is_badge_candidate():
    assert _looks_like_badge_device({
        "port": "/dev/cu.usbmodem11244301",
        "description": "n/a",
        "hwid": "n/a",
    })


def test_espressif_usb_jtag_metadata_is_badge_candidate():
    assert _looks_like_badge_device({
        "port": "/dev/cu.anything",
        "description": "USB JTAG/serial debug unit",
        "hwid": "USB VID:PID=303A:1001 SER=123456 LOCATION=1-2",
    })


def test_long_form_usbmodem_port_needs_bootloader_recovery():
    assert _needs_usb_bootloader_recovery("/dev/cu.usbmodemE83DC1F94D9C1")


def test_short_usbmodem_port_can_use_normal_esptool_reset():
    assert not _needs_usb_bootloader_recovery("/dev/cu.usbmodem1101")
