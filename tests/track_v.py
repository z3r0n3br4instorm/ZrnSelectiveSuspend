#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Track V: lending a card to a virtual machine and taking it back, in a QEMU guest.

The same guest as Track K, with an emulated IOMMU and the kernel's passthrough
driver in it. The "virtual machine" is a process in the guest that holds the
passthrough device's group open, which is all the host side of a hand-over
can see of one. Nothing is loaded into the host's kernel.

Usage: track_v.py [--keep] [--verbose]
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import track_k  # noqa: E402

if __name__ == "__main__":
    why = track_k.build_module()
    if why:
        print(f"SKIP  track V ({why})")
        sys.exit(77)
    os.environ["ZSS_BUILD"] = track_k.BUILD
    os.environ["ZSS_GUEST_SCRIPT"] = os.path.join(HERE, "track_v_guest.py")
    os.environ["ZSS_GUEST_IOMMU"] = "1"
    # A second display device, on the root bus: the host keeps one when the card under test is lent.
    os.environ["ZSS_GUEST_QEMU"] = "-device bochs-display,id=hostgpu"
    os.environ["ZSS_GUEST_MODULES"] = ",".join([
        "drivers/iommu/iommufd/iommufd", "drivers/vfio/vfio", "virt/lib/irqbypass", "drivers/vfio/vfio_iommu_type1",
        "drivers/vfio/pci/vfio-pci-core", "drivers/vfio/pci/vfio-pci"])
    import track_b
    sys.exit(track_b.main())
