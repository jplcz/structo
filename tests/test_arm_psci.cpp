// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/arm_psci.hpp>

#include <cstdint>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo::hypervisor;

TEST(ArmPsciTest, DecodesCpuOnSmc64FunctionId) {
  constexpr std::uint32_t raw = 0xC4000003U; // Fast, SMC64, owner=standard, function 3 (CPU_ON)
  const smccc_function_id id = decode_smccc_function_id(raw);
  const reloco::result<psci_function> fn = decode_psci_function(id);
  ASSERT_TRUE(fn.has_value());
  EXPECT_EQ(*fn, psci_function::cpu_on);
}

TEST(ArmPsciTest, DecodesAllDefinedFunctionNumbers) {
  const psci_function expected[] = {
      psci_function::version,
      psci_function::cpu_suspend,
      psci_function::cpu_off,
      psci_function::cpu_on,
      psci_function::affinity_info,
      psci_function::migrate,
      psci_function::migrate_info_type,
      psci_function::migrate_info_up_cpu,
      psci_function::system_off,
      psci_function::system_reset,
      psci_function::features,
      psci_function::cpu_freeze,
      psci_function::cpu_default_suspend,
      psci_function::node_hw_state,
      psci_function::system_suspend,
      psci_function::set_suspend_mode,
      psci_function::stat_residency,
      psci_function::stat_count,
      psci_function::system_reset2,
      psci_function::mem_protect,
      psci_function::mem_protect_check_range,
  };
  for (std::uint16_t n = 0; n < 21; ++n) {
    smccc_function_id id{};
    id.owner = static_cast<std::uint8_t>(smccc_owner::standard);
    id.function_number = n;
    const reloco::result<psci_function> fn = decode_psci_function(id);
    ASSERT_TRUE(fn.has_value()) << "function number " << n;
    EXPECT_EQ(*fn, expected[n]);
  }
}

TEST(ArmPsciTest, RejectsNonStandardOwner) {
  smccc_function_id id{};
  id.owner = static_cast<std::uint8_t>(smccc_owner::sip);
  id.function_number = 3;
  const reloco::result<psci_function> fn = decode_psci_function(id);
  ASSERT_FALSE(fn.has_value());
  EXPECT_EQ(fn.error(), reloco::error::invalid_argument);
}

TEST(ArmPsciTest, RejectsUnrecognizedFunctionNumber) {
  smccc_function_id id{};
  id.owner = static_cast<std::uint8_t>(smccc_owner::standard);
  id.function_number = 0xFFFF;
  const reloco::result<psci_function> fn = decode_psci_function(id);
  ASSERT_FALSE(fn.has_value());
  EXPECT_EQ(fn.error(), reloco::error::unsupported_operation);
}

TEST(ArmPsciTest, EncodeFunctionIdRoundTripsThroughDecode) {
  const std::uint32_t raw = encode_psci_function_id(psci_function::system_suspend, smccc_convention::smc64);
  const smccc_function_id id = decode_smccc_function_id(raw);
  EXPECT_EQ(id.type, smccc_call_type::fast);
  EXPECT_EQ(id.convention, smccc_convention::smc64);
  EXPECT_EQ(id.owner, static_cast<std::uint8_t>(smccc_owner::standard));
  const reloco::result<psci_function> fn = decode_psci_function(id);
  ASSERT_TRUE(fn.has_value());
  EXPECT_EQ(*fn, psci_function::system_suspend);
}

TEST(ArmPsciTest, Smc64VariantExistsOnlyForFunctionsCarrying64BitPayloads) {
  EXPECT_TRUE(psci_function_has_smc64_variant(psci_function::cpu_on));
  EXPECT_TRUE(psci_function_has_smc64_variant(psci_function::cpu_suspend));
  EXPECT_TRUE(psci_function_has_smc64_variant(psci_function::system_reset2));
  EXPECT_TRUE(psci_function_has_smc64_variant(psci_function::mem_protect_check_range));

  EXPECT_FALSE(psci_function_has_smc64_variant(psci_function::version));
  EXPECT_FALSE(psci_function_has_smc64_variant(psci_function::cpu_off));
  EXPECT_FALSE(psci_function_has_smc64_variant(psci_function::system_off));
  EXPECT_FALSE(psci_function_has_smc64_variant(psci_function::features));
  EXPECT_FALSE(psci_function_has_smc64_variant(psci_function::mem_protect));
}

TEST(ArmPsciTest, EncodePsciReturnCodeSignExtendsFullyForSmc64) {
  const std::uint64_t encoded = encode_psci_return_code<std::uint64_t>(psci_return_code::already_on);
  EXPECT_EQ(encoded, 0xFFFFFFFFFFFFFFFCULL); // -4 sign-extended to 64 bits
}

TEST(ArmPsciTest, EncodePsciReturnCodeIsPlain32BitPatternForSmc32) {
  const std::uint32_t encoded = encode_psci_return_code<std::uint32_t>(psci_return_code::invalid_address);
  EXPECT_EQ(encoded, 0xFFFFFFF7U); // -9
}

TEST(ArmPsciTest, VersionEncodeDecodeRoundTrips) {
  constexpr psci_version v{1, 2};
  constexpr std::uint32_t encoded = encode_psci_version(v);
  EXPECT_EQ(encoded, 0x00010002U);
  const psci_version decoded = decode_psci_version(encoded);
  EXPECT_EQ(decoded.major, 1U);
  EXPECT_EQ(decoded.minor, 2U);
}

TEST(ArmPsciTest, IsConstexprEvaluable) {
  // decode_psci_function() itself isn't constexpr (see its doc comment: reloco::result<T>'s destructor
  // isn't constexpr under C++17 for a non-template free function) -- everything else here is.
  constexpr smccc_function_id id = decode_smccc_function_id(0xC4000003U);
  static_assert(id.convention == smccc_convention::smc64, "must be constexpr");
  constexpr std::uint32_t raw = encode_psci_function_id(psci_function::cpu_on, smccc_convention::smc64);
  static_assert(raw == 0xC4000003U, "must be constexpr");
  static_assert(psci_function_has_smc64_variant(psci_function::cpu_on), "must be constexpr");
  constexpr std::uint64_t encoded_err = encode_psci_return_code<std::uint64_t>(psci_return_code::denied);
  static_assert(encoded_err == 0xFFFFFFFFFFFFFFFDULL, "must be constexpr");
  constexpr std::uint32_t version_raw = encode_psci_version({1, 1});
  static_assert(version_raw == 0x00010001U, "must be constexpr");
}

RELOCO_END_UNSAFE_BUFFER_USAGE
