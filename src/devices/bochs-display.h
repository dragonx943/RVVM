/*
bochs-display.h - Bochs Display
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_BOCHS_DISPLAY_H
#define RVVM_BOCHS_DISPLAY_H

#include <rvvm/rvvm_fb.h>

#include "pci-bus.h"

// Use 16MiB VRAM
#define RVVM_BOCHS_DISPLAY_VRAM 0x1000000U

// Passed fbdev should have at least 16MiB of VRAM reserved
RVVM_PUBLIC pci_dev_t* rvvm_bochs_display_init(pci_bus_t* pci_bus, rvvm_fbdev_t* fbdev);

RVVM_PUBLIC pci_dev_t* rvvm_bochs_display_init_auto(rvvm_machine_t* machine, rvvm_fbdev_t* fbdev);

#endif
