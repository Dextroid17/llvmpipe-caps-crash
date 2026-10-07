# llvmpipe: compute shader SIGSEGVs in JIT code (load from a null address) when the CPU caps are `sse2` or `avx`

**Component:** Drivers/Vulkan/llvmpipe (`vulkan-swrast`)
**Severity:** crash (segfault), silent wrong-code risk

## Summary

A compute shader that runs correctly when llvmpipe targets `sse4_2`, `avx2` or `avx512` SIGSEGVs inside llvmpipe's JIT-compiled shader when llvmpipe targets `sse2` or `avx`. The fault is a load from address 0 — `vpbroadcastd (%rcx),%ymm1` with `%rcx = 0`.

The same SPIR-V module is used in every case (byte-identical, `sha256 3bceca7a51b64ced04b992c04b598c6f33a40f1a96a7ca50fb22c0ece63834b6`, 15900 bytes), so the difference is entirely in the code llvmpipe generates. `spirv-val` reports the module valid.

## Environment

- Mesa `3:26.2.4-1` (Arch/CachyOS `vulkan-swrast`), LLVM 23.1.1
- lavapipe forced with `VK_ICD_FILENAMES`
- Device reported: `llvmpipe (LLVM 23.1.1, 256 bits)`, Vulkan API 1.4.354, subgroup size 8
- Host: CachyOS (Arch), x86-64, glibc

## Reproduction

Attached: `main.cpp` (self-contained, ~250 lines, only needs Vulkan headers + `libvulkan`), `bda_run2.spv` (the shader), `bda_run2.spvasm` (disassembly), `build.sh`.

```
g++ -std=c++20 -O0 -g main.cpp -o bda-repro -I<vulkan headers> -lvulkan
export VK_ICD_FILENAMES=<path to lavapipe icd json>

GALLIUM_OVERRIDE_CPU_CAPS=avx2 ./bda-repro bda_run2.spv   # passes, result=0x55443322
GALLIUM_OVERRIDE_CPU_CAPS=avx  ./bda-repro bda_run2.spv   # SIGSEGV
```

The program creates an instance and a device with `VK_KHR_buffer_device_address` + `VK_KHR_8bit_storage` and `shaderInt64`; creates three storage buffers (a page-table struct at binding 0, a fault struct at binding 1, a 4-byte output at binding 2) plus two small buffers whose device addresses are written into the table; dispatches one workgroup of the shader; and prints the output word.

## Expected vs actual

- **Expected:** dispatch completes, output holds `0x55443322`, fault struct stays zero.
- **Actual** with `GALLIUM_OVERRIDE_CPU_CAPS=avx`: SIGSEGV on a worker thread.

## Result matrix (5 runs each)

| `GALLIUM_OVERRIDE_CPU_CAPS` | result |
| --- | --- |
| `sse2` | **SIGSEGV** 5/5 |
| `sse4_2` | passes 0/5 |
| `avx` | **SIGSEGV** 5/5 |
| `avx2` | passes 0/5 |
| `avx512f` | passes 0/5 |
| unset (native) | passes |

## Faulting instruction

```
Thread 26 "bda-repro" received signal SIGSEGV, Segmentation fault.
0x00007fffec876938:  vpbroadcastd (%rcx),%ymm1        rcx=0
#0  0x00007fffec876938 in ?? ()
#1  0x0000000000000000 in ?? ()
```

`0x7fffec876938` lies inside one of the process's anonymous mappings — the JIT'd shader. The surrounding generated code is a masked gather:

```
vmovdqa %ymm3,0x860(%rsp)
vmovmskps %ymm8,%ecx
tzcnt  %ecx,%esi
and    $0x7,%esi
mov    0x860(%rsp,%rsi,8),%rcx
vpbroadcastd (%rcx),%ymm1        <-- fault, rcx = 0
```

## Why this looks like a driver bug

- The module is valid SPIR-V (`spirv-val` passes) and is byte-identical across every setting above — the only thing that changes between a passing and a failing run is the ISA llvmpipe compiles for.
- The load llvmpipe lowers here is guarded in the SPIR-V: it sits behind an `OpBranchConditional` whose condition guarantees the pointer is non-null, so a correct lowering must not execute it when the branch is not taken. llvmpipe executes it anyway, through a null pointer.
- The two settings that crash are the baseline ISA of each family (`sse2`, `avx`); the ones that add features (`sse4_2`, `avx2`, `avx512`) pass. That pattern points at one instruction-selection path rather than at the shader.

## Attachments

- `main.cpp`, `build.sh` — reproducer
- `bda_run2.spv` — the shader (`sha256 3bceca7a51b64ced04b992c04b598c6f33a40f1a96a7ca50fb22c0ece63834b6`, 15900 bytes)
- `bda_run2.spvasm` — disassembly
