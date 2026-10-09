# STM32 USB Device Library — local changes

SOUP item: STMicroelectronics `stm32_mw_usb_device`
(https://github.com/STMicroelectronics/stm32_mw_usb_device).

Upstream version in this tree: **v2.11.6** (`Core/` and `Class/DFU/`; only the DFU
class is built). Updated from v2.11.5 on 2026-10-06 to pick up the
`usbd_ctlreq.c` endpoint-index fix (CVA R7, issue #3): `USBD_StdEPReq` now
rejects an endpoint number above 15 before indexing `ep_in[]`/`ep_out[]`, and
`USBD_GetString` is bounded by the destination length.

Every file is byte-identical to upstream v2.11.6 except the one below. Verify
with a `diff` against the upstream tag before recording a new version here.

## Class/DFU/Src/usbd_dfu.c — manifestation-tolerant descriptor

`USBD_DFU_CfgDesc` `bmAttributes` is `0x0F` instead of upstream `0x0B`:
`bitManifestationTolerant` is set.

Why: with the upstream value the device self-resets inside `DFU_Leave()` after a
download, so the host's final GET_STATUS fails (`dfu-util`: "Error during
download get_status"). With manifestation tolerance the device returns to
`dfuIDLE`, the host reads a clean status, and the bootloader main loop performs
the reboot once `DFU_ImageDownloadComplete()` reports manifestation complete
(see `USB_DEVICE/App/usbd_dfu_if.c` and `Core/Src/main.c`). The bootloader also
uses this window for its post-download anti-rollback check.

This change is functional, not a security fix, and must be re-applied on every
library update.
