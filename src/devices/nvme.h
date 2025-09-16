/*
nvme.h - Non-Volatile Memory Express
Copyright (C) 2022  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_NVME_H
#define RVVM_NVME_H

#include <rvvm/rvvm_blk.h>

#include "pci-bus.h"

RVVM_PUBLIC pci_dev_t* nvme_init_blk(pci_bus_t* pci_bus, rvvm_blk_dev_t* blk);

RVVM_PUBLIC pci_dev_t* nvme_init(pci_bus_t* pci_bus, const char* image, bool rw);
RVVM_PUBLIC pci_dev_t* nvme_init_auto(rvvm_machine_t* machine, const char* image, bool rw);

#endif
