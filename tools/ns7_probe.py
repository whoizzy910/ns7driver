#!/usr/bin/env python3
"""
ns7_probe.py - Step 1 of a userspace NS7 driver for Apple Silicon.

  python3 ns7_probe.py              # list USB devices, find the NS7, dump its descriptors
  python3 ns7_probe.py --listen     # read every non-audio IN endpoint and print the bytes
  python3 ns7_probe.py --listen --log ns7.log   # also save to a file

Setup (once):
  brew install libusb
  pip3 install pyusb

Numark's USB vendor ID is 0x15E4. If the NS7 shows a different one,
pass --vid 0xXXXX --pid 0xYYYY.
"""
import argparse
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    sys.exit("pyusb not installed: pip3 install pyusb  (and: brew install libusb)")

NUMARK_VID = 0x15E4

XFER = {0: "control", 1: "isochronous", 2: "bulk", 3: "interrupt"}


def find_device(vid, pid):
    if pid is not None:
        return usb.core.find(idVendor=vid, idProduct=pid)
    return usb.core.find(idVendor=vid)


def list_all():
    print("USB devices:")
    for d in usb.core.find(find_all=True):
        try:
            name = usb.util.get_string(d, d.iProduct) if d.iProduct else ""
        except Exception:
            name = "?"
        print(f"  {d.idVendor:04x}:{d.idProduct:04x}  {name}")


def dump(dev):
    def s(i):
        try:
            return usb.util.get_string(dev, i) if i else ""
        except Exception:
            return "?"

    print(f"\nDevice {dev.idVendor:04x}:{dev.idProduct:04x}")
    print(f"  Manufacturer: {s(dev.iManufacturer)}")
    print(f"  Product:      {s(dev.iProduct)}")
    print(f"  Class:        0x{dev.bDeviceClass:02x}")
    for cfg in dev:
        print(f"  Config {cfg.bConfigurationValue}")
        for intf in cfg:
            print(f"    Interface {intf.bInterfaceNumber} alt {intf.bAlternateSetting}"
                  f"  class 0x{intf.bInterfaceClass:02x}"
                  f" sub 0x{intf.bInterfaceSubClass:02x}"
                  f" proto 0x{intf.bInterfaceProtocol:02x}")
            for ep in intf:
                direction = "IN " if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN else "OUT"
                kind = XFER[usb.util.endpoint_type(ep.bmAttributes)]
                print(f"      EP 0x{ep.bEndpointAddress:02x} {direction} {kind:<11}"
                      f" maxpkt {ep.wMaxPacketSize} interval {ep.bInterval}")


def listen(dev, logfile):
    try:
        dev.set_configuration()
    except usb.core.USBError as e:
        print(f"set_configuration: {e} (continuing)")

    cfg = dev.get_active_configuration()
    endpoints = []
    for intf in cfg:
        if intf.bAlternateSetting != 0:
            continue
        ins = [ep for ep in intf
               if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN
               and usb.util.endpoint_type(ep.bmAttributes) in (2, 3)]  # bulk / interrupt only
        if not ins:
            continue
        try:
            if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                dev.detach_kernel_driver(intf.bInterfaceNumber)
        except (NotImplementedError, usb.core.USBError):
            pass
        try:
            usb.util.claim_interface(dev, intf.bInterfaceNumber)
        except usb.core.USBError as e:
            print(f"Could not claim interface {intf.bInterfaceNumber}: {e}")
            continue
        endpoints += ins

    if not endpoints:
        sys.exit("No bulk/interrupt IN endpoints could be opened. "
                 "Send me the descriptor dump; the device may need an init command first.")

    print("Listening on: " + ", ".join(f"0x{e.bEndpointAddress:02x}" for e in endpoints))
    print("Move ONE control at a time. Ctrl-C to stop.\n")

    last = {}
    repeats = {}
    t0 = time.time()
    log = open(logfile, "a") if logfile else None
    try:
        while True:
            for ep in endpoints:
                try:
                    data = bytes(ep.read(ep.wMaxPacketSize, timeout=5))
                except usb.core.USBTimeoutError:
                    continue
                except usb.core.USBError as e:
                    if e.errno in (60, 110):  # timeout codes on macOS / Linux
                        continue
                    print(f"EP 0x{ep.bEndpointAddress:02x} error: {e}")
                    continue
                if not data:
                    continue
                addr = ep.bEndpointAddress
                if data == last.get(addr):
                    repeats[addr] = repeats.get(addr, 0) + 1
                    continue
                if repeats.get(addr):
                    print(f"           (previous packet repeated {repeats[addr]}x)")
                    repeats[addr] = 0
                last[addr] = data
                line = f"{time.time() - t0:9.3f}  EP 0x{addr:02x}  [{len(data):3}]  {data.hex(' ')}"
                print(line)
                if log:
                    log.write(line + "\n")
                    log.flush()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        if log:
            log.close()
        usb.util.dispose_resources(dev)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--vid", type=lambda x: int(x, 0), default=NUMARK_VID)
    p.add_argument("--pid", type=lambda x: int(x, 0))
    p.add_argument("--listen", action="store_true")
    p.add_argument("--log")
    a = p.parse_args()

    try:
        dev = find_device(a.vid, a.pid)
    except usb.core.NoBackendError:
        sys.exit("libusb not found: brew install libusb")

    if dev is None:
        print(f"No device with vendor ID 0x{a.vid:04x} found.")
        list_all()
        print("\nIf the NS7 is in that list, rerun with --vid/--pid. "
              "If it's not, check the USB cable and that the NS7 is powered on.")
        return

    dump(dev)
    if a.listen:
        listen(dev, a.log)


if __name__ == "__main__":
    main()
