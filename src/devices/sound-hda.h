/*
sound-hda.h - HD Audio
Copyright (C) 2025  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef SOUND_HDA_H
#define SOUND_HDA_H

#include "rvvmlib.h"
#include "pci-bus.h"

PUBLIC pci_dev_t* sound_hda_init(pci_bus_t* pci_bus);
PUBLIC pci_dev_t* sound_hda_init_auto(rvvm_machine_t* machine);

#endif
