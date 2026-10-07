# llvmpipe compute-shader crash gated on `GALLIUM_OVERRIDE_CPU_CAPS`

Minimal reproducer for a segfault in llvmpipe's JIT-compiled shader. Filed upstream as
[mesa/mesa#16515](https://gitlab.freedesktop.org/mesa/mesa/-/work_items/16515).

A compute shader that runs correctly when llvmpipe targets `sse4_2`, `avx2` or `avx512`
SIGSEGVs when llvmpipe targets `sse2` or `avx`. The fault is a load from address 0 —
`vpbroadcastd (%rcx),%ymm1` with `%rcx = 0`. The SPIR-V module is byte-identical in every
case (`sha256 3bceca7a51b64ced04b992c04b598c6f33a40f1a96a7ca50fb22c0ece63834b6`, 15900 bytes), so the difference is entirely in the code
llvmpipe generates.

## Environment where it was found

- Mesa `3:26.2.4-1` (Arch/CachyOS `vulkan-swrast`), LLVM 23.1.1
- lavapipe forced with `VK_ICD_FILENAMES`
- Device: `llvmpipe (LLVM 23.1.1, 256 bits)`, Vulkan 1.4.354, subgroup size 8
- Host: CachyOS (Arch), x86-64, glibc

## Files

- `main.cpp` — self-contained reproducer (~250 lines; needs only Vulkan headers and `libvulkan`)
- `build.sh` — build helper
- `bda_run2.spv` — the shader (`sha256 3bceca7a51b64ced04b992c04b598c6f33a40f1a96a7ca50fb22c0ece63834b6`, 15900 bytes)
- `bda_run2.spvasm` — its disassembly

## Build and run

```sh
g++ -std=c++20 -O0 -g main.cpp -o bda-repro -I<vulkan headers> -lvulkan
export VK_ICD_FILENAMES=<path to lavapipe icd json>

GALLIUM_OVERRIDE_CPU_CAPS=avx2 ./bda-repro bda_run2.spv   # passes, result=0x55443322
GALLIUM_OVERRIDE_CPU_CAPS=avx  ./bda-repro bda_run2.spv   # SIGSEGV
```

## Result matrix (5 runs each)

| `GALLIUM_OVERRIDE_CPU_CAPS` | result |
| --- | --- |
| `sse2` | SIGSEGV 5/5 |
| `sse4_2` | passes 0/5 |
| `avx` | SIGSEGV 5/5 |
| `avx2` | passes 0/5 |
| `avx512f` | passes 0/5 |
| unset (native) | passes |

## Fault

```
Thread 26 "bda-repro" received signal SIGSEGV, Segmentation fault.
0x00007fffec876938:  vpbroadcastd (%rcx),%ymm1        rcx=0
#0  0x00007fffec876938 in ?? ()
#1  0x0000000000000000 in ?? ()
```

`0x7fffec876938` is inside one of the process's anonymous mappings — the JIT'd shader.
The surrounding generated code is a masked gather:

```
vmovdqa %ymm3,0x860(%rsp)
vmovmskps %ymm8,%ecx
tzcnt  %ecx,%esi
and    $0x7,%esi
mov    0x860(%rsp,%rsi,8),%rcx
vpbroadcastd (%rcx),%ymm1        <-- fault, rcx = 0
```

`spirv-val` reports the module valid, and the load llvmpipe lowers here sits behind an
`OpBranchConditional` that guarantees the pointer is non-null.

## Origin

The module is emitted by [AnyPS5](https://github.com/boykopovar/AnyPS5) (a PS5 relinker) for
one of its GPU tests, found there as
[boykopovar/AnyPS5#1044](https://github.com/boykopovar/AnyPS5/issues/1044). Nothing in this
reproducer depends on AnyPS5 — only the emitted SPIR-V and a plain Vulkan harness.
