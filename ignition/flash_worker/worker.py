#!/usr/bin/env python3
"""Temporal worker for badge build and flash operations.

Managed automatically by start.sh. To run manually:
    cd ignition && python3 -m flash_worker.worker
"""
import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from temporalio.client import Client
from temporalio.worker import Worker

from flash_worker.activities import (
    build_firmware,
    detect_badge_devices,
    detect_badge_ports,
    flash_badge_factory_image,
    flash_badge_images,
    flash_badge_firmware,
    flash_badge_filesystem,
    prepare_badge_devices_for_flashing,
    prepare_flash_artifacts,
    resolve_badge_port,
    sync_badge_clock,
    verify_badge_boot_marker,
    verify_badge_ble,
)
from flash_worker.workflows import (
    MAX_CONCURRENT_FLASHES,
    TASK_QUEUE,
    BadgeFlashWorkflow,
    BuildFirmwareWorkflow,
    DetectBadgesWorkflow,
    FlashBadgesWorkflow,
)


async def main() -> None:
    try:
        client = await Client.connect("localhost:7233")
    except Exception:
        print("  error: Cannot connect to Temporal at localhost:7233")
        print("  Start it with: temporal server start-dev")
        sys.exit(1)

    worker = Worker(
        client,
        task_queue=TASK_QUEUE,
        workflows=[
            BuildFirmwareWorkflow,
            DetectBadgesWorkflow,
            BadgeFlashWorkflow,
            FlashBadgesWorkflow,
        ],
        activities=[
            build_firmware,
            detect_badge_devices,
            detect_badge_ports,
            flash_badge_factory_image,
            flash_badge_images,
            flash_badge_firmware,
            flash_badge_filesystem,
            prepare_badge_devices_for_flashing,
            prepare_flash_artifacts,
            resolve_badge_port,
            sync_badge_clock,
            verify_badge_boot_marker,
            verify_badge_ble,
        ],
        max_concurrent_activities=MAX_CONCURRENT_FLASHES,
        max_concurrent_activity_task_polls=MAX_CONCURRENT_FLASHES,
    )

    print(f"  Badge flash worker ready  •  queue: {TASK_QUEUE}")
    print(f"  Temporal UI: http://localhost:8233")
    print()
    await worker.run()


if __name__ == "__main__":
    asyncio.run(main())
