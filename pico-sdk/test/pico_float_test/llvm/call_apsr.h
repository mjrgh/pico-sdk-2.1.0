//todo check license
//===-- call_apsr.h - Helpers for ARM EABI floating point tests -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is dual licensed under the MIT and the University of Illinois Open
// Source Licenses. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares helpers for ARM EABI floating point tests for the
// compiler_rt library.
//
//===----------------------------------------------------------------------===//

#ifndef CALL_APSR_H
#define CALL_APSR_H

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error big endian support not implemented
#endif

union cpsr {
    struct {
        uint32_t filler: 28;
        uint32_t v: 1;
        uint32_t c: 1;
        uint32_t z: 1;
        uint32_t n: 1;
    } flags;
    uint32_t value;
};

extern __attribute__((pcs("aapcs")))
uint32_t call_apsr_f(float a, float b, __attribute__((pcs("aapcs"))) void (*fn)(float, float));

extern __attribute__((pcs("aapcs")))
uint32_t call_apsr_d(double a, double b, __attribute__((pcs("aapcs"))) void (*fn)(double, double));

#endif // CALL_APSR_H
