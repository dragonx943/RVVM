/*
riscv_vector.h - RISC-V Vector ISA
Copyright (C) 2025  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RISCV_VECTOR_H
#define RVVM_RISCV_VECTOR_H

#include "riscv_common.h" // IWYU pragma: keep

#if defined(USE_RVV)

slow_path void riscv_emulate_v_opc_op(rvvm_hart_t* vm, const uint32_t insn);

#endif

#endif
