# ADR-0002: Write device code in C99; host tools may use Python, TypeScript or Rust

- **Status:** Proposed (2026-10-10).
- **Context:** [hardware toolchain](../research/fm1-hardware.md#4-toolchain) · [dev tooling](../research/dev-tooling.md)

## Decision
All code that runs on the device (`core/` and `targets/fm1/`) is **C99/gnu99**. Vendored DSP may use a C++11 subset built with `-fno-exceptions -fno-rtti`. Host-only tools can be written in Python, Node/TypeScript or Rust.

## Why
- The pi32v2 CPU has no upstream LLVM backend and no Rust target. The only compiler is JieLi's 2017 Clang 4.0.1 fork.
- Felucca and every one of its forks are C, so reusing their code needs no glue layer.

## Build-time checks that enforce the constraints
- No heap allocation.
- No 64-bit division.
- No `double` anywhere on the device. The AC79 has a **single-precision FPU** (X0X builds with `-mcpu=r3 -mfprev1`; see dinghy#1), so `float` is allowed. New `core/` code should still prefer integers, which keeps output bit-exact across host, WASM and device.
- No calls into chip ROM.
- `-ffp-contract=off` everywhere, so host, WASM and device output are bit-exact.
- RAM and flash budgets.
