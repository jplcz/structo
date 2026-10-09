<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Boot protocols

Header-only, allocation-free, bounds-checked readers and writers for the
hand-off structures bootloaders use, all folding their memory map into
`structo::boot_memory_map` through `try_add(memory_kind, base, size)`.
No raw pointers are exposed: inputs are `reloco::span`s or integer
physical addresses.

| Header | Protocol |
|---|---|
| `boot/limine.hpp` | Limine (requests + responses) |
| `boot/uefi.hpp` | UEFI memory map, config tables, GOP |
| `boot/linux_x86.hpp` | Linux x86 / x86_64 zero page |
| `boot/linux_arm.hpp` | Linux 32-bit ARM (`zImage`, ATAGS) |
| `boot/linux_image_header.hpp` | Linux arm64 and RISC-V `Image` headers |
| `arch/riscv/sbi.hpp` | RISC-V SBI |
| `boot/uboot.hpp` | U-Boot `uImage`, FIT detection, environment |
| `boot/arm_tf_fip.hpp` | Arm Trusted Firmware FIP (ToC parse, UUID lookup, ToC writer) |

Each header's file comment carries a complete example; the highlights:

```cpp
// UEFI: why - turn the firmware map into the kernel's physical memory view.
// map_bytes: the GetMemoryMap buffer; desc_size: DescriptorSize returned by firmware
// (NOT sizeof(descriptor): firmware may pad each entry).
auto reader = structo::boot::uefi::memory_map_reader::try_create(map_bytes, desc_size);
structo::boot_memory_map<128> map;       // 128 = max number of disjoint ranges kept
(void)structo::boot::uefi::try_fill_memory_map(*reader, map);

// Linux ARM: why - hand an old (non-devicetree) kernel its memory and cmdline.
structo::boot::linux_arm::atag_writer w(atag_buffer);  // caller-owned buffer in the first 16 KiB of RAM
(void)w.add_core();                                    // mandatory first tag
(void)w.add_mem(0x80000000, 0x20000000);               // start, size
(void)w.add_cmdline("console=ttyAMA0");                // kernel command line
(void)w.finish();                                      // ATAG_NONE terminator
// Jump with r0 = 0, r1 = machine type (~0 for DT-only), r2 = address of atag_buffer.

// U-Boot: why - never trust an image before its CRCs check out.
auto img = structo::boot::uboot::try_parse_image(file_bytes);  // file_bytes: whole uImage
if (img)
  use(img->header.entry, img->payload);                        // payload CRC already verified

// SBI: why - S-mode asks firmware to start a secondary hart.
// Backend wraps the architecture's `ecall`; see sbi.hpp for a full one.
structo::riscv::sbi::client<ecall_backend> sbi{ecall_backend{}};
(void)sbi.hart_start(/*hartid=*/1, /*entry_phys=*/0x80200000, /*a1 value=*/0);
```

```cpp
// FIP: why - find BL31/BL33 in the package TF-A's BL2 loaded.
auto fip = structo::boot::arm_tf_fip::fip_reader::try_create(fip_bytes);  // checks the 0xAA640001 signature
auto bl33 = fip->find(structo::boot::arm_tf_fip::uuids::non_trusted_firmware_bl33);
if (bl33)
  boot(bl33->payload);  // span into fip_bytes; offset/size already bounds-checked
```

## Notes

- Linux `Image` headers: arm64 entry is `x0 = DTB`; RISC-V is `a0 = hartid,
  a1 = DTB`; 32-bit ARM is `r0 = 0, r1 = machine, r2 = DTB/ATAGS`.
- Limine requests must live in `volatile` storage the bootloader patches;
  the reader takes a resolver mapping a response address to a span (the
  `LIMINE_NO_POINTERS` form), so no pointer is ever dereferenced.
- FIT images are devicetrees: after `uboot::detect_format()` returns `fit`,
  walk them with `fdt_reader`.
