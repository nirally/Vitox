# Upstream Vita3K findings (local notes for later upstream review; Vita3K NOT patched)

Vita3K commit: `7224a0fcc769fea781e4166e5f16a2456552f9f5`

## U1. SceSysmem `allocated_*` accounting subtracts the info-struct size, not the block size
**Status:** confirmed on physical Xbox (M4.1, accepted by ChatGPT 2026-10-18).

**Source:**
- Allocation stores `block->size = sizeof(SceKernelMemBlockInfo)` (0x18) and adds the real size:
  - `vita3k/modules/SceSysmem/SceSysmemForDriver.cpp:151` (`ksceKernelAllocMemBlock`) → `allocated_{user,cdram,phycont} += size`
  - `vita3k/modules/SceSysmem/SceSysmem.cpp:107` (`sceKernelAllocMemBlockForVM`) → `allocated_user += size`
- Free subtracts `block->second->size`, which is the 0x18 struct-size field, instead of `mappedSize`:
  - `SceSysmemForDriver.cpp:270`, `:273`, `:277`, `:280` (`ksceKernelFreeMemBlock`, every memblock type)
  - `SceSysmem.cpp:164` (`sceKernelFreeMemBlockForVM`)

**Hardware evidence** (run `XBOX-RUNS\20261018-162531_m41-xbox`, `logs\core_log.txt`):
- `sceKernelAllocMemBlock(... size=0x10000)` → uid=1, SysmemState allocated_user=0x10000
- `sceKernelFreeMemBlock(uid=1)` → 0, blocks=0, **allocated_user=0xFFE8** (expected 0)

**Effect:** the counters only grow (by `size - 0x18` per alloc/free cycle). `sceKernelGetFreeMemorySize` (`SceSysmem.cpp:171`) reports less and less free memory, and can clamp to 0 after enough cycles.

**Likely fix:** subtract `block->second->mappedSize` in both free paths.

## U2. `sceKernelFreeMemBlockForVM` erases a `vm_blocks` iterator from `blocks`
**Status:** source review only; not exercised on hardware.

**Source:** `vita3k/modules/SceSysmem/SceSysmem.cpp:160-166`.
```cpp
const Blocks::const_iterator block = state->vm_blocks.find(uid);
...
state->blocks.erase(block);     // iterator belongs to vm_blocks: undefined behaviour
state->vm_blocks.erase(block);
```
**Likely fix:** `state->blocks.erase(uid); state->vm_blocks.erase(block);`. U1 also applies on line 164.

## U3. Path canonicalisation fails on Xbox (Boost.Filesystem and std::filesystem): Vita3K's `fs::relative` users
**Status:** confirmed on physical Xbox (run `XBOX-RUNS\20261018-164513_m42b-xbox`, 0.0.8.1). This is a platform behaviour, not a Vita3K bug, but it affects Vita3K code paths.

**Hardware evidence:**
- `CreateFileFromAppW`/`CreateFile2` open the handle fine (`FILE_FLAG_BACKUP_SEMANTICS`).
- `GetFinalPathNameByHandleW(h, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)` returns 0 with **`ERROR_ACCESS_DENIED` (5)** on D:\DevelopmentFiles AND on LocalState (Q:). With `VOLUME_NAME_NT` it succeeds (`\Device\HarddiskVolume6\DevelopmentFiles\...`, `\Device\HarddiskVolume4\Users\...`). The app container apparently cannot map volumes to drive letters.
- Boost.Filesystem's Windows `canonical()` (`libs/filesystem/src/operations.cpp:2709-2725`) retries with `VOLUME_NAME_NT` only on `ERROR_PATH_NOT_FOUND`. So `canonical`, `weakly_canonical` and `relative` fail with ec=5. MSVC `std::filesystem::canonical`/`weakly_canonical`/`relative` fail the same way. `lexically_relative` works.

**Vita3K users of `fs::relative`** (all would fail on Xbox):
- `vita3k/util/src/fs_utils.cpp:101` (`copy_directory_contents`, used by `io.cpp` `copy_path`)
- `vita3k/packages/src/sce_utils.cpp:1143`, `:1151` (package/content install)

**Options (later):**
1. Use `lexically_relative` where both paths are already absolute and normalised (upstream-style).
2. A Vitox compat wrapper for `GetFinalPathNameByHandleW` in the Boost TUs: on `ERROR_ACCESS_DENIED` with `VOLUME_NAME_DOS`, retry with `VOLUME_NAME_NT` and map the `\Device\HarddiskVolumeN` prefix back to the input path's drive. This needs care.
