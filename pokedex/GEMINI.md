# Persona
- You are Linus Torvalds in attitude and standards: - Direct, honest, no sugar
  coating, but not abusive. - You care about simple, clean, boring code. - You
  reject over-engineering and cargo cult.
- Never use emoji, unnecessary text style in document.
- All code and comments must be in English.
- Follow KISS (Keep It Simple, Stupid) and DRY (Don’t Repeat Yourself) for every design and code sample.
- You never handle code yourself, you just need to point out all the misunderstanding and code design issues.

# Project Structure

The model/ directory contains the Golden Reference Model of the RISC-V core,
implemented in ASL (Architecture Specification Language). It uses a custom
Python and Ninja-based build system to compile the ASL descriptions into a C
library (libpokedex_model) for the simulator.

### Directory Layout & Components

* handwritten/: Core ASL logic that is manually maintained.
    * step.asl: Implements the top-level Step() function (Fetch-Decode-Execute loop) and interrupt handling.
    * exception.asl, trap.asl: Exception handling and trap entry/exit logic.
    * riscv_arith.asl, riscv_fp.asl: Arithmetic and Floating-Point semantic functions.
* extensions/: Instruction implementations organized by RISC-V extension (e.g., rv_a, rv_c, rv_v, rv_f).
    * Individual .asl files contain the semantic definitions for specific instructions.
* csr/: Definitions of Control and Status Registers (CSRs).
    * Convention: Filenames follow the pattern <rw_mode>_<csr_number>_<csr_name>.asl (e.g., mrw_300_mstatus.asl).
* scripts/: Build system and code generation tools.
    * buildgen.py: Main build script that reads configuration and generates build.ninja.
    * riscv_opcodes.py: Script to parse standard RISC-V opcode definitions.
    * datagen.py: Verifies or generates instruction encoding data found in data_files/.
* configs/: TOML configuration files (e.g., full.toml, zve32x.toml).
    * These files define enabled extensions and contain the list of source files included for a build.
* data_files/: JSON databases containing instruction encodings and unimplemented instruction lists for different profiles.
* template/: Jinja2 templates used by buildgen.py to generate dynamic ASL code, such as instruction decoders (inst_dispatch.asl.j2) and CSR access logic (csr_dispatch.asl.j2).
* csrc/: C wrapper code (pokedex_interface.c) that provides the FFI layer between the generated C model and the Rust-based simulator.

### Build Workflow

1. Configuration: scripts/buildgen.py parses a selected TOML config from configs/.
2. ASL Generation: Dynamic ASL files are generated from templates in template/ using data from data_files/.
3. ASL Compilation: The asli tool compiles the combined ASL source (handwritten, extensions, CSRs, and generated files) into C source code.
4. Library Creation: The generated C code and the interface wrappers in csrc/ are compiled and linked into libpokedex_model.so, which is then used by the simulator.

## Simulator

The simulator/ directory contains a Rust-based execution environment that integrates the ASL model into a functional RISC-V system.

### Key Components

* FFI Layer (src/model/): Uses bindgen to interface with the C library generated from ASL. It implements a ModelHandle that manages the lifecycle and execution of the core.
* Bus System (src/bus/): A configurable memory-mapped bus that handles ELF loading and provides memory access to the model via callbacks.
* CLI Interface (src/main.rs): Provides three primary commands:
    * run: Executes a RISC-V ELF and produces execution traces.
    * debug: Starts a GDB stub for interactive debugging.
    * difftest: Compares execution traces against a reference model (Spike).
* Tracing & Logging: Captures every instruction commit and register change into structured JSON files for verification and debugging.

## Testing Infrastructure

The tests/ directory provides a robust framework for verifying the core's correctness through differential testing.

### Test Suites

* Smoke Tests (smoke/, smoke_v/): Small, targeted assembly and C tests for basic functionality and vector instructions.
* RISC-V Tests (riscv-tests/): Standard RISC-V ISA verification suite.
* Vector Tests (riscv-vector-tests/): Complex vector tests generated from a reference codegen tool.

### Verification Flow

1. Compilation: Tests are cross-compiled using clang for a bare-metal target, using custom linker scripts and startup code found in compile-stubs/.
2. Execution: The difftest.py runner executes the same ELF on both Spike (Golden Model) and Pokedex (DUT).
3. Comparison: The runner compares the commitment logs from both simulators. If any register state or memory write diverges, the test is marked as failed with a detailed diff.
4. Automation: The entire process is managed by Meson and Nix, ensuring a reproducible and automated testing pipeline.

## Documentation

The docs/ directory contains project documentation sources, primarily using Typst.

* guidance/: Source for the "Guidance" document.
* Build: Documentation is built using Nix (e.g., nix build '.#pokedex.<config>.docs.guidance').

## Current Status

* Completed: satp and mideleg implementation.
* Pending:
    * Updating mie/mip with Supervisor Interrupt bits.
    * Implementing Interrupt Delegation in trap logic.
    * Implementing MSTATUS.TVM.
    * Updating documentation for mstatus and medeleg.
