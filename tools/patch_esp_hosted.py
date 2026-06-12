#!/usr/bin/env python3
"""Re-apply the mandatory esp_hosted patch after the component manager
re-fetches managed_components (e.g. after `idf.py fullclean` or a version
bump). Idempotent — safe to run any time; the build also runs it via CMake.

Why: esp_hosted auto-initializes from a C constructor, which runs before the
startup-stack RAM regions are returned to the heap. With this firmware's
static footprint that leaves <16KB internal DMA RAM, the ~48KB SDIO mempool
allocation fails and the device boot-loops on an assert. We defer the init
to main/wifi.c instead. See README "已知坑位".
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / ("managed_components/espressif__esp_hosted/host/port/esp/"
                 "freertos/src/port_esp_hosted_host_init.c")

ORIG = "static void __attribute__((constructor)) esp_hosted_host_init(void)"
PATCHED = """/* PATCHED (tab5_ssh_term): C-constructor auto-init runs before the
 * startup-stack RAM regions are returned to the heap. With this app's large
 * static footprint that leaves <16KB internal DMA RAM, so the SDIO mempool
 * (~48KB) fails and the boot loops on an assert. esp_hosted_init() is called
 * explicitly from main/wifi.c instead, where the full heap is available. */
static void __attribute__((unused)) esp_hosted_host_init(void)"""

def main() -> int:
    if not TARGET.exists():
        print(f"esp_hosted not fetched yet ({TARGET}); nothing to do")
        return 0
    src = TARGET.read_text()
    if "PATCHED (tab5_ssh_term)" in src:
        print("esp_hosted patch already applied")
        return 0
    if ORIG not in src:
        print("ERROR: constructor pattern not found — esp_hosted layout changed, "
              "re-port the patch by hand!", file=sys.stderr)
        return 1
    TARGET.write_text(src.replace(ORIG, PATCHED))
    print("esp_hosted patch applied")
    return 0

if __name__ == "__main__":
    sys.exit(main())
