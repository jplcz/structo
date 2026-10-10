// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/arch/riscv/sbi.hpp>
#include <structo/boot/arm_tf_fip.hpp>
#include <structo/boot/linux_arm.hpp>
#include <structo/boot/linux_image_header.hpp>
#include <structo/boot/linux_x86.hpp>
#include <structo/boot/uboot.hpp>
#include <structo/boot/uefi.hpp>
#include <structo/boot_memory_map.hpp>

#include <reloco/array.hpp>
#include <reloco/string_view.hpp>

using reloco::error;
using reloco::span;

namespace {

using bytes_t = span<std::byte>;
using cbytes_t = span<const std::byte>;
using string_view_t = reloco::string_view;

template <std::size_t N> struct buffer {
  reloco::array<std::byte, N> data = {};
  bytes_t rw() noexcept { return bytes_t(data); }
  cbytes_t ro() const noexcept { return cbytes_t(data); }
};

void put_le(bytes_t b, std::size_t off, std::uint64_t v, int n) {
  for (int i = 0; i < n; ++i)
    b[off + static_cast<std::size_t>(i)] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}

class BootProtocolsTest : public ::testing::Test {};

// ---- boot_memory_map -------------------------------------------------------

TEST_F(BootProtocolsTest, MemoryMapTryAddRoutesByKind) {
  structo::boot_memory_map<8> map;
  ASSERT_TRUE(map.try_add(structo::boot::memory_kind::usable, 0x1000u, 0x1000u));
  ASSERT_TRUE(map.try_add(structo::boot::memory_kind::bootloader_reclaimable, 0x2000u, 0x1000u));
  ASSERT_TRUE(map.try_add(structo::boot::memory_kind::mmio, 0x9000u, 0x1000u)); // ignored
  EXPECT_EQ(map.free.size(), 1u);
  EXPECT_EQ(map.full.size(), 1u); // adjacent ranges coalesce
}

// ---- UEFI ------------------------------------------------------------------

TEST_F(BootProtocolsTest, UefiMemoryMapHonoursDescriptorStride) {
  constexpr std::size_t stride = 48; // firmware may pad descriptors
  buffer<stride * 2> map;
  put_le(map.rw(), 0, 7, 4); // EfiConventionalMemory
  put_le(map.rw(), 8, 0x100000, 8);
  put_le(map.rw(), 24, 16, 8);    // pages
  put_le(map.rw(), stride, 0, 4); // EfiReservedMemoryType
  put_le(map.rw(), stride + 8, 0x200000, 8);
  put_le(map.rw(), stride + 24, 1, 8);

  auto reader = structo::boot::uefi::memory_map_reader::try_create(map.ro(), stride);
  ASSERT_TRUE(reader);
  EXPECT_EQ(reader->count(), 2u);

  structo::boot_memory_map<8> out;
  auto reader2 = structo::boot::uefi::memory_map_reader::try_create(map.ro(), stride);
  ASSERT_TRUE(structo::boot::uefi::try_fill_memory_map(*reader2, out));
  EXPECT_EQ(out.free.size(), 1u);
}

TEST_F(BootProtocolsTest, UefiRejectsBadDescriptorSize) {
  buffer<64> map;
  EXPECT_FALSE(structo::boot::uefi::memory_map_reader::try_create(map.ro(), 16));
  EXPECT_FALSE(structo::boot::uefi::memory_map_reader::try_create(map.ro(), 44));
}

// ---- Linux x86 -------------------------------------------------------------

TEST_F(BootProtocolsTest, LinuxX86WriterReaderRoundTrip) {
  buffer<4096> page;
  // A real loader copies the kernel's setup header into the zero page; fake the fields the reader validates.
  put_le(page.rw(), 0x1FE, 0xAA55, 2);
  put_le(page.rw(), 0x202, 0x53726448, 4);
  put_le(page.rw(), 0x206, 0x020F, 2);
  structo::boot::linux_x86::boot_params_writer w(page.rw());
  ASSERT_TRUE(w.set_cmdline(0x20000));
  ASSERT_TRUE(w.set_ramdisk(0x3000000, 0x1000));
  ASSERT_TRUE(w.add_e820(0x100000, 0x7ff00000, structo::boot::linux_x86::e820_type::ram));

  auto r = structo::boot::linux_x86::boot_params_reader::try_create(page.ro());
  ASSERT_TRUE(r);
  EXPECT_EQ(r->e820_count(), 1u);
  auto cmd = r->try_cmdline_address();
  ASSERT_TRUE(cmd);
  EXPECT_EQ(*cmd, 0x20000u);

  structo::boot_memory_map<8> map;
  ASSERT_TRUE(r->try_fill_memory_map(map));
  EXPECT_EQ(map.free.size(), 1u);
}

TEST_F(BootProtocolsTest, LinuxX86RejectsShortPage) {
  buffer<100> page;
  EXPECT_FALSE(structo::boot::linux_x86::boot_params_reader::try_create(page.ro()));
}

// ---- Linux ARM / arm64 / RISC-V -------------------------------------------

TEST_F(BootProtocolsTest, ZImageHeaderParses) {
  buffer<64> img;
  put_le(img.rw(), 0x24, 0x016F2818, 4);
  put_le(img.rw(), 0x28, 0, 4);
  put_le(img.rw(), 0x2C, 0x400000, 4);
  put_le(img.rw(), 0x30, 0x04030201, 4);
  auto h = structo::boot::linux_arm::try_parse_zimage_header(img.ro());
  ASSERT_TRUE(h);
  EXPECT_EQ(h->image_size(), 0x400000u);
  EXPECT_TRUE(h->little_endian);
  put_le(img.rw(), 0x24, 0, 4);
  EXPECT_FALSE(structo::boot::linux_arm::try_parse_zimage_header(img.ro()));
}

TEST_F(BootProtocolsTest, AtagsRoundTrip) {
  buffer<256> list;
  structo::boot::linux_arm::atag_writer w(list.rw());
  ASSERT_TRUE(w.add_core());
  ASSERT_TRUE(w.add_mem(0x80000000u, 0x20000000u));
  ASSERT_TRUE(w.add_cmdline("console=ttyAMA0"));
  ASSERT_TRUE(w.add_initrd2(0x82000000u, 0x1000u));
  ASSERT_TRUE(w.finish());

  EXPECT_EQ(structo::boot::linux_arm::classify_kernel_arg(list.ro()), structo::boot::linux_arm::kernel_arg_kind::atags);

  auto reader = structo::boot::linux_arm::atag_reader::try_create(list.ro());
  ASSERT_TRUE(reader);
  int seen = 0;
  for (auto t : *reader) {
    ASSERT_TRUE(t);
    ++seen;
    if (t->tag == structo::boot::linux_arm::atag_tag::cmdline) {
      auto c = structo::boot::linux_arm::try_decode_cmdline(*t);
      ASSERT_TRUE(c);
      EXPECT_EQ(c->size(), 15u);
    }
  }
  EXPECT_EQ(seen, 5); // core, mem, cmdline, initrd2, none

  structo::boot_memory_map<4> map;
  auto again = structo::boot::linux_arm::atag_reader::try_create(list.ro());
  ASSERT_TRUE(structo::boot::linux_arm::try_fill_memory_map(*again, map));
  EXPECT_EQ(map.free.size(), 1u);
}

TEST_F(BootProtocolsTest, Arm64ImageHeader) {
  buffer<64> img;
  put_le(img.rw(), 8, 0x80000, 8);
  put_le(img.rw(), 16, 0x1000000, 8);
  put_le(img.rw(), 24, 0x2, 8); // 4K pages
  put_le(img.rw(), 56, 0x644d5241, 4);
  auto h = structo::boot::linux_arm64::try_parse_image_header(img.ro());
  ASSERT_TRUE(h);
  EXPECT_EQ(h->page_size(), 4096u);
  EXPECT_EQ(h->effective_text_offset(), 0x80000u);
  EXPECT_FALSE(h->big_endian());
}

TEST_F(BootProtocolsTest, RiscvImageHeader) {
  buffer<64> img;
  put_le(img.rw(), 8, 0x200000, 8);
  put_le(img.rw(), 16, 0x800000, 8);
  put_le(img.rw(), 32, 0x00000002, 4);
  put_le(img.rw(), 56, 0x05435352, 4);
  auto h = structo::boot::linux_riscv::try_parse_image_header(img.ro());
  ASSERT_TRUE(h);
  EXPECT_EQ(h->version_major(), 0u);
  EXPECT_EQ(h->version_minor(), 2u);
  put_le(img.rw(), 16, 0, 8);
  EXPECT_FALSE(structo::boot::linux_riscv::try_parse_image_header(img.ro()));
}

// ---- SBI -------------------------------------------------------------------

struct fake_sbi {
  std::uint32_t last_eid = 0;
  std::uint32_t last_fid = 0;
  std::uint64_t last_a0 = 0;
  structo::riscv::sbi::sbiret reply{};
  structo::riscv::sbi::sbiret call(std::uint32_t eid, std::uint32_t fid, std::uint64_t a0, std::uint64_t, std::uint64_t,
                                   std::uint64_t, std::uint64_t, std::uint64_t) noexcept {
    last_eid = eid;
    last_fid = fid;
    last_a0 = a0;
    return reply;
  }
};

TEST_F(BootProtocolsTest, SbiSpecVersionAndProbe) {
  fake_sbi f;
  f.reply = {0, 0x02000001};
  structo::riscv::sbi::client<fake_sbi> c{f};
  auto v = c.get_spec_version();
  ASSERT_TRUE(v);
  EXPECT_EQ(v->major, 2u);
  EXPECT_EQ(v->minor, 1u);
  f.reply = {0, 1};
  structo::riscv::sbi::client<fake_sbi> c2{f};
  auto p = c2.probe_extension(structo::riscv::sbi::eid_time);
  ASSERT_TRUE(p);
  EXPECT_TRUE(*p);
  EXPECT_EQ(c2.backend().last_a0, structo::riscv::sbi::eid_time);
}

TEST_F(BootProtocolsTest, SbiMapsErrors) {
  fake_sbi f;
  f.reply = {static_cast<std::int64_t>(structo::riscv::sbi::status::not_supported), 0};
  structo::riscv::sbi::client<fake_sbi> c{f};
  auto r = c.hart_start(1, 0x80200000, 0);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), error::unsupported_operation);
}

// ---- U-Boot ----------------------------------------------------------------

TEST_F(BootProtocolsTest, UbootImageRoundTripAndCorruption) {
  buffer<64 + 8> blob;
  for (std::size_t i = 0; i < 8; ++i)
    blob.data[64 + i] = static_cast<std::byte>(i + 1);
  structo::boot::uboot::image_header fields;
  fields.load = 0x80008000;
  fields.entry = 0x80008000;
  fields.os_ = structo::boot::uboot::os::linux_;
  fields.arch_ = structo::boot::uboot::arch::arm64;
  fields.type = structo::boot::uboot::image_type::kernel;
  fields.name[0] = 't';
  fields.name[1] = 'e';
  fields.name[2] = 's';
  fields.name[3] = 't';
  ASSERT_TRUE(structo::boot::uboot::try_write_header(blob.rw(), blob.ro().subspan(64), fields));

  EXPECT_EQ(structo::boot::uboot::detect_format(blob.ro()), structo::boot::uboot::image_format::legacy);
  auto img = structo::boot::uboot::try_parse_image(blob.ro());
  ASSERT_TRUE(img);
  EXPECT_EQ(img->payload.size(), 8u);
  EXPECT_EQ(img->header.entry, 0x80008000u);
  EXPECT_STREQ(img->header.name.data(), "test");

  blob.data[70] = std::byte{0xFF}; // corrupt the payload
  EXPECT_FALSE(structo::boot::uboot::try_parse_image(blob.ro()));
}

TEST_F(BootProtocolsTest, UbootEnvLookup) {
  const string_view_t vars("bootargs=console=ttyS0\0bootdelay=3\0", 35);
  buffer<4 + 36> env;
  for (std::size_t i = 0; i < vars.size(); ++i)
    env.data[4 + i] = static_cast<std::byte>(vars[i]);
  put_le(env.rw(), 0, structo::boot::detail::crc32(env.ro().subspan(4)), 4);
  auto r = structo::boot::uboot::env_reader::try_create(env.ro(), false);
  ASSERT_TRUE(r);
  auto delay = r->find("bootdelay");
  ASSERT_TRUE(delay.has_value());
  EXPECT_EQ(delay->size(), 1u);
  EXPECT_FALSE(r->find("missing").has_value());
  env.data[5] = std::byte{'X'};
  EXPECT_FALSE(structo::boot::uboot::env_reader::try_create(env.ro(), false));
}

// ---- Arm TF-A FIP ----------------------------------------------------------

TEST_F(BootProtocolsTest, FipFindsEntriesAndRejectsBadOnes) {
  namespace fip = structo::boot::arm_tf_fip;
  buffer<256> blob;
  fip::toc_writer w(blob.rw().first(16 + 3 * 40), 0x12345678, std::uint64_t{0x1} << 32);
  ASSERT_TRUE(w.add(fip::uuids::trusted_boot_firmware_bl2, 200, 4, 0));
  ASSERT_TRUE(w.add(fip::uuids::non_trusted_firmware_bl33, 210, 8, 0));
  ASSERT_TRUE(w.finish());
  EXPECT_FALSE(w.add(fip::uuids::fw_config, 0, 0, 0)); // no room left
  blob.data[200] = std::byte{0xB2};
  blob.data[210] = std::byte{0x33};

  auto r = fip::fip_reader::try_create(blob.ro());
  ASSERT_TRUE(r);
  EXPECT_EQ(r->header().platform_flags(), 1u);
  auto bl33 = r->find(fip::uuids::non_trusted_firmware_bl33);
  ASSERT_TRUE(bl33);
  EXPECT_EQ(bl33->payload.size(), 8u);
  EXPECT_EQ(bl33->payload[0], std::byte{0x33});
  auto missing = r->find(fip::uuids::el3_runtime_firmware_bl31);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error(), error::not_found);

  int n = 0;
  auto it = r->entries();
  for (auto e : it) {
    ASSERT_TRUE(e);
    ++n;
  }
  EXPECT_EQ(n, 2);

  // Payload outside the blob is rejected, as is a missing terminator.
  buffer<256> bad;
  fip::toc_writer bw(bad.rw().first(16 + 2 * 40));
  ASSERT_TRUE(bw.add(fip::uuids::hw_config, 250, 100, 0));
  ASSERT_TRUE(bw.finish());
  auto br = fip::fip_reader::try_create(bad.ro());
  ASSERT_TRUE(br);
  EXPECT_FALSE(br->find(fip::uuids::hw_config));

  EXPECT_EQ(fip::uuids::trusted_boot_firmware_bl2.hi, 0x5ff9ec0b4d223e4dull);
  put_le(bad.rw(), 0, 0, 4);
  EXPECT_FALSE(fip::fip_reader::try_create(bad.ro()));
}

} // namespace
