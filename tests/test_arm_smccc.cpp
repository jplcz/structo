// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/arm_smccc.hpp>

#include <cstdint>

using namespace structo::hypervisor;

TEST(ArmSmcccTest, DecodesFastSmc64PsciStyleFunctionId) {
  // PSCI CPU_SUSPEND (64-bit): Fast call, SMC64, owner=standard(4), function 0x0001.
  constexpr std::uint32_t raw = 0xC4000001U;
  const smccc_function_id id = decode_smccc_function_id(raw);
  EXPECT_EQ(id.type, smccc_call_type::fast);
  EXPECT_EQ(id.convention, smccc_convention::smc64);
  EXPECT_EQ(id.owner, static_cast<std::uint8_t>(smccc_owner::standard));
  EXPECT_EQ(id.function_number, 0x0001U);
}

TEST(ArmSmcccTest, DecodesYieldingSmc32FunctionId) {
  // Yielding call, SMC32, owner=sip(2), function 0x1234.
  constexpr std::uint32_t raw = 0x02001234U;
  const smccc_function_id id = decode_smccc_function_id(raw);
  EXPECT_EQ(id.type, smccc_call_type::yielding);
  EXPECT_EQ(id.convention, smccc_convention::smc32);
  EXPECT_EQ(id.owner, static_cast<std::uint8_t>(smccc_owner::sip));
  EXPECT_EQ(id.function_number, 0x1234U);
}

TEST(ArmSmcccTest, EncodeIsTheInverseOfDecode) {
  for (const std::uint32_t raw : {0xC4000001U, 0x02001234U, 0x84000002U, 0x40000001U}) {
    const smccc_function_id id = decode_smccc_function_id(raw);
    EXPECT_EQ(encode_smccc_function_id(id.type, id.convention, id.owner, id.function_number), raw);
  }
}

TEST(ArmSmcccTest, OwnerRangeHelpersClassifyTrustedAppAndOsRanges) {
  EXPECT_TRUE(is_trusted_app_owner(48));
  EXPECT_TRUE(is_trusted_app_owner(49));
  EXPECT_FALSE(is_trusted_app_owner(47));
  EXPECT_FALSE(is_trusted_app_owner(50));

  EXPECT_TRUE(is_trusted_os_owner(50));
  EXPECT_TRUE(is_trusted_os_owner(63));
  EXPECT_TRUE(is_trusted_os_owner(55));
  EXPECT_FALSE(is_trusted_os_owner(49));
  EXPECT_FALSE(is_trusted_os_owner(64)); // out of the 6-bit field entirely; still correctly false
}

TEST(ArmSmcccTest, Aarch64MonitorSupportsBothConventions) {
  EXPECT_TRUE(smccc_convention_supported<std::uint64_t>(smccc_convention::smc32));
  EXPECT_TRUE(smccc_convention_supported<std::uint64_t>(smccc_convention::smc64));
}

TEST(ArmSmcccTest, Armv7MonitorOnlySupportsSmc32) {
  EXPECT_TRUE(smccc_convention_supported<std::uint32_t>(smccc_convention::smc32));
  EXPECT_FALSE(smccc_convention_supported<std::uint32_t>(smccc_convention::smc64));
}

TEST(ArmSmcccTest, EncodeReturnCodeSignExtendsFullyForSmc64) {
  const std::uint64_t encoded = encode_smccc_return_code<std::uint64_t>(smccc_return_code::not_supported);
  EXPECT_EQ(encoded, 0xFFFFFFFFFFFFFFFFULL); // -1 sign-extended to 64 bits
}

TEST(ArmSmcccTest, EncodeReturnCodeIsPlain32BitPatternForSmc32) {
  const std::uint32_t encoded = encode_smccc_return_code<std::uint32_t>(smccc_return_code::not_supported);
  EXPECT_EQ(encoded, 0xFFFFFFFFU);
}

TEST(ArmSmcccTest, EncodeReturnCodeSuccessIsZeroForBothWidths) {
  EXPECT_EQ(encode_smccc_return_code<std::uint32_t>(smccc_return_code::success), 0U);
  EXPECT_EQ(encode_smccc_return_code<std::uint64_t>(smccc_return_code::success), 0U);
}

TEST(ArmSmcccTest, IsConstexprEvaluable) {
  constexpr smccc_function_id id = decode_smccc_function_id(0xC4000001U);
  static_assert(id.convention == smccc_convention::smc64, "must be constexpr");
  constexpr std::uint32_t raw = encode_smccc_function_id(smccc_call_type::fast, smccc_convention::smc64,
                                                         static_cast<std::uint8_t>(smccc_owner::standard), 1);
  static_assert(raw == 0xC4000001U, "must be constexpr");
  constexpr bool supported32 = smccc_convention_supported<std::uint32_t>(smccc_convention::smc64);
  static_assert(!supported32, "must be constexpr");
  constexpr std::uint64_t not_supported_x0 = encode_smccc_return_code<std::uint64_t>(smccc_return_code::not_supported);
  static_assert(not_supported_x0 == 0xFFFFFFFFFFFFFFFFULL, "must be constexpr");
}
