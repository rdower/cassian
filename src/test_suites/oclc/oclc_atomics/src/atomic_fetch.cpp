/*
 * Copyright (C) 2021 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include <algorithm>
#include <cassian/random/random.hpp>
#include <cassian/runtime/openclc_types.hpp>
#include <cassian/runtime/runtime.hpp>
#include <cassian/test_harness/test_harness.hpp>
#include <cassian/utility/utility.hpp>
#include <cassian/vector/vector.hpp>
#include <catch2/catch.hpp>
#include <cmath>
#include <common.hpp>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <test_config.hpp>
#include <vector>

namespace ca = cassian;

namespace {

template <typename T, typename U> struct Input {
  std::vector<T> value;
  std::vector<U> operand;
};

template <typename T, typename U> struct TestCase {
  using test_type = T;
  using test_host_type = typename T::host_type;
  using operand_type = U;
  using operand_host_type = typename U::host_type;

  Operation operation = Operation::addition;
  std::function<test_host_type(test_host_type, operand_host_type)>
      reference_function;

  MemoryType memory_type = MemoryType::global;

  FunctionType function_type = FunctionType::implicit;
  MemoryOrder memory_order = MemoryOrder::relaxed;
  MemoryScope memory_scope = MemoryScope::device;

  int global_work_size = 0;
  int local_work_size = 0;

  ca::Runtime *runtime = nullptr;
  std::string program_type;

  Input<test_host_type, operand_host_type> input;
  std::function<void(std::vector<test_host_type>, std::vector<test_host_type>)>
      compare_function;
};

template <typename TEST_CASE_TYPE, typename REFERENCE_FUNCTION>
TEST_CASE_TYPE create_test_case(const TestConfig &config,
                                const Operation operation,
                                REFERENCE_FUNCTION reference_function) {
  TEST_CASE_TYPE test_case;
  test_case.runtime = config.runtime();
  test_case.global_work_size = config.work_size();
  test_case.local_work_size =
      get_local_work_size(test_case.global_work_size, *test_case.runtime);
  test_case.program_type = config.program_type();
  test_case.operation = operation;
  test_case.reference_function = reference_function;
  return test_case;
}

template <typename TEST_CASE_TYPE>
bool should_skip(const TEST_CASE_TYPE &test_case) {
  ca::Requirements requirements;
  using test_host_type = typename TEST_CASE_TYPE::test_host_type;
  requirements.atomic_add<test_host_type>();
  return ca::should_skip_test(requirements, *test_case.runtime);
}

template <typename T, typename U> Input<T, U> generate_input(const int size) {
  Input<T, U> input;
  input.value = ca::generate_vector<T>(size, 0);
  input.operand = ca::generate_vector<U>(size, 0);
  return input;
}

std::string get_kernel_path(const MemoryType memory_type) {
  switch (memory_type) {
  case MemoryType::global:
    return "kernels/oclc_atomics/atomic_fetch_global.cl";
  case MemoryType::local:
    return "kernels/oclc_atomics/atomic_fetch_local.cl";
  default:
    return "unknown";
  }
}

std::string implicit_function_build_option(const Operation operation) {
  return " -D FUNCTION=atomic_fetch_" + to_string(operation);
}

std::string explicit_function_build_option(const Operation operation) {
  return implicit_function_build_option(operation) + "_explicit";
}

template <typename T> std::string atomic_type_build_option() {
  return std::string(" -D ATOMIC_TYPE=") + T::device_atomic_type;
}

template <typename T> std::string data_type_build_option() {
  return std::string(" -D DATA_TYPE=") + T::device_type;
}

template <typename T> std::string operand_type_build_option() {
  return std::string(" -D OPERAND_TYPE=") + T::device_type;
}

std::string memory_order_build_option(const MemoryOrder memory_order) {
  return std::string(" -D MEMORY_ORDER=") + to_string(memory_order);
}

std::string memory_scope_build_option(const MemoryScope memory_scope) {
  return std::string(" -D MEMORY_SCOPE=") + to_string(memory_scope);
}

std::string work_group_size_build_option(const int size) {
  return std::string(" -D WORK_GROUP_SIZE=") + std::to_string(size);
}

template <typename T> std::string extra_extension_build_option() { return ""; }

template <typename TEST_TYPE, typename OPERAND_TYPE>
std::string
get_build_options(const int local_work_size, const FunctionType function_type,
                  const Operation operation, const MemoryOrder memory_order,
                  const MemoryScope memory_scope) {
  std::string build_options = " -cl-std=CL3.0" +
                              atomic_type_build_option<TEST_TYPE>() +
                              operand_type_build_option<OPERAND_TYPE>() +
                              data_type_build_option<TEST_TYPE>() +
                              work_group_size_build_option(local_work_size) +
                              extra_extension_build_option<TEST_TYPE>();

  if (function_type == FunctionType::implicit) {
    build_options += implicit_function_build_option(operation);
  } else if (function_type == FunctionType::explicit_memory_order) {
    build_options += explicit_function_build_option(operation) +
                     memory_order_build_option(memory_order);
  } else if (function_type == FunctionType::explicit_memory_scope) {
    build_options += explicit_function_build_option(operation) +
                     memory_order_build_option(memory_order) +
                     memory_scope_build_option(memory_scope);
  }

  return build_options;
}

ca::Kernel create_kernel(const std::string &path,
                         const std::string &build_options, ca::Runtime *runtime,
                         const std::string &program_type) {
  const std::string source = ca::load_text_file(ca::get_asset(path));
  const std::string kernel_name = "test_kernel";
  return runtime->create_kernel(kernel_name, source, build_options,
                                program_type);
}

template <typename T> struct Output {
  std::vector<T> result;
  std::vector<T> fetched;
};

template <typename T, typename U>
Output<T> run_kernel(const ca::Kernel &kernel, const int global_work_size,
                     const int local_work_size, const Input<T, U> &input,
                     ca::Runtime *runtime) {
  std::vector<ca::Buffer> buffers;

  ca::Buffer value_buffer =
      runtime->create_buffer(sizeof(T) * input.value.size());
  runtime->write_buffer_from_vector(value_buffer, input.value);
  buffers.push_back(value_buffer);

  ca::Buffer operand_buffer =
      runtime->create_buffer(sizeof(U) * input.operand.size());
  runtime->write_buffer_from_vector(operand_buffer, input.operand);
  buffers.push_back(operand_buffer);

  ca::Buffer fetched_buffer =
      runtime->create_buffer(sizeof(T) * input.value.size());
  buffers.push_back(fetched_buffer);

  for (size_t i = 0; i < buffers.size(); ++i) {
    runtime->set_kernel_argument(kernel, static_cast<int>(i), buffers[i]);
  }

  runtime->run_kernel(kernel, global_work_size, local_work_size);

  Output<T> output;
  output.result = runtime->read_buffer_to_vector<T>(value_buffer);
  output.fetched = runtime->read_buffer_to_vector<T>(fetched_buffer);

  for (auto &buffer : buffers) {
    runtime->release_buffer(buffer);
  }

  return output;
}

template <typename T, typename U, typename REF_FUNCTION>
std::vector<T> get_reference(const Input<T, U> input,
                             REF_FUNCTION reference_function) {
  std::vector<T> output(input.value.size());
  for (size_t i = 0; i < output.size(); ++i) {
    output[i] = reference_function(input.value[i], input.operand[i]);
  }
  return output;
}

template <typename T, Operation O> struct OperandType { using value = T; };

template <> struct OperandType<ca::clc_intptr_t, Operation::addition> {
  using value = ca::clc_ptrdiff_t;
};
template <> struct OperandType<ca::clc_intptr_t, Operation::subtraction> {
  using value = ca::clc_ptrdiff_t;
};

template <> struct OperandType<ca::clc_uintptr_t, Operation::addition> {
  using value = ca::clc_ptrdiff_t;
};
template <> struct OperandType<ca::clc_uintptr_t, Operation::subtraction> {
  using value = ca::clc_ptrdiff_t;
};

template <typename T, Operation O>
using operand_type_v = typename OperandType<T, O>::value;

template <typename T, typename cassian::EnableIfIsIntegral<T> = 0>
void compare(const std::vector<T> &output, const std::vector<T> &reference) {
  REQUIRE_THAT(output, Catch::Equals(reference));
}

template <typename TEST_CASE_TYPE> void run_test(TEST_CASE_TYPE test_case) {
  using test_type = typename TEST_CASE_TYPE::test_type;
  using operand_type = typename TEST_CASE_TYPE::operand_type;
  using test_host_type = typename TEST_CASE_TYPE::test_host_type;
  using operand_host_type = typename TEST_CASE_TYPE::operand_host_type;

  const std::string kernel_path = get_kernel_path(test_case.memory_type);
  const std::string build_options = get_build_options<test_type, operand_type>(
      test_case.local_work_size, test_case.function_type, test_case.operation,
      test_case.memory_order, test_case.memory_scope);
  const ca::Kernel kernel = create_kernel(
      kernel_path, build_options, test_case.runtime, test_case.program_type);

  const Output<test_host_type> output =
      run_kernel(kernel, test_case.global_work_size, test_case.local_work_size,
                 test_case.input, test_case.runtime);
  test_case.runtime->release_kernel(kernel);

  const std::vector<test_host_type> reference =
      get_reference(test_case.input, test_case.reference_function);

  test_case.compare_function(output.fetched, test_case.input.value);
  test_case.compare_function(output.result, reference);
}

template <typename TEST_CASE_TYPE>
void test_signatures(TEST_CASE_TYPE test_case,
                     const std::vector<MemoryType> &memory_types,
                     const std::vector<FunctionType> &function_types,
                     const std::vector<MemoryOrder> &memory_orders,
                     const std::vector<MemoryScope> &memory_scopes) {
  for (const auto memory_type : memory_types) {
    test_case.memory_type = memory_type;
    SECTION(to_string(memory_type)) {
      for (const auto function_type : function_types) {
        test_case.function_type = function_type;
        SECTION(to_string(function_type)) {
          if (function_type == FunctionType::implicit) {
            run_test(test_case);
          } else if (function_type == FunctionType::explicit_memory_order) {
            for (const auto memory_order : memory_orders) {
              test_case.memory_order = memory_order;
              SECTION(to_string(memory_order)) { run_test(test_case); }
            }
          } else if (function_type == FunctionType::explicit_memory_scope) {
            for (const auto memory_scope : memory_scopes) {
              test_case.memory_scope = memory_scope;
              SECTION(to_string(memory_scope)) {
                for (const auto memory_order : memory_orders) {
                  test_case.memory_order = memory_order;
                  SECTION(to_string(memory_order)) { run_test(test_case); }
                }
              }
            }
          }
        }
      }
    }
  }
}

TEMPLATE_TEST_CASE("atomic_fetch_add_signatures", "", ca::clc_int_t,
                   ca::clc_uint_t, ca::clc_long_t, ca::clc_ulong_t,
                   ca::clc_intptr_t, ca::clc_uintptr_t, ca::clc_size_t,
                   ca::clc_ptrdiff_t) {
  const TestConfig &config = get_test_config();
  const Operation operation = Operation::addition;
  const auto reference_function = [](auto a, auto b) { return a + b; };

  using test_case_type =
      TestCase<TestType, operand_type_v<TestType, operation>>;
  auto test_case =
      create_test_case<test_case_type>(config, operation, reference_function);
  if (should_skip(test_case)) {
    return;
  }

  test_case.input = generate_input<typename test_case_type::test_host_type,
                                   typename test_case_type::operand_host_type>(
      test_case.global_work_size);
  test_case.compare_function = [](auto a, auto b) { compare(a, b); };
  test_signatures(test_case, memory_types_all, function_types_all,
                  memory_orders_all, memory_scopes_all);
}

TEMPLATE_TEST_CASE("atomic_fetch_sub_signatures", "", ca::clc_int_t,
                   ca::clc_uint_t, ca::clc_long_t, ca::clc_ulong_t,
                   ca::clc_intptr_t, ca::clc_uintptr_t, ca::clc_size_t,
                   ca::clc_ptrdiff_t) {
  const TestConfig &config = get_test_config();
  const Operation operation = Operation::subtraction;
  const auto reference_function = [](auto a, auto b) { return a - b; };

  using test_case_type =
      TestCase<TestType, operand_type_v<TestType, operation>>;
  auto test_case =
      create_test_case<test_case_type>(config, operation, reference_function);
  if (should_skip(test_case)) {
    return;
  }

  test_case.input = generate_input<typename test_case_type::test_host_type,
                                   typename test_case_type::operand_host_type>(
      test_case.global_work_size);
  test_case.compare_function = [](auto a, auto b) { compare(a, b); };
  test_signatures(test_case, memory_types_all, function_types_all,
                  memory_orders_all, memory_scopes_all);
}

TEMPLATE_TEST_CASE("atomic_fetch_or_signatures", "", ca::clc_int_t,
                   ca::clc_uint_t, ca::clc_long_t, ca::clc_ulong_t,
                   ca::clc_intptr_t, ca::clc_uintptr_t, ca::clc_size_t,
                   ca::clc_ptrdiff_t) {
  const TestConfig &config = get_test_config();
  const Operation operation = Operation::bitwise_or;
  const auto reference_function = [](auto a, auto b) { return a | b; };

  using test_case_type =
      TestCase<TestType, operand_type_v<TestType, operation>>;
  auto test_case =
      create_test_case<test_case_type>(config, operation, reference_function);
  if (should_skip(test_case)) {
    return;
  }

  test_case.input = generate_input<typename test_case_type::test_host_type,
                                   typename test_case_type::operand_host_type>(
      test_case.global_work_size);
  test_case.compare_function = [](auto a, auto b) { compare(a, b); };
  test_signatures(test_case, memory_types_all, function_types_all,
                  memory_orders_all, memory_scopes_all);
}

TEMPLATE_TEST_CASE("atomic_fetch_xor_signatures", "", ca::clc_int_t,
                   ca::clc_uint_t, ca::clc_long_t, ca::clc_ulong_t,
                   ca::clc_intptr_t, ca::clc_uintptr_t, ca::clc_size_t,
                   ca::clc_ptrdiff_t) {
  const TestConfig &config = get_test_config();
  const Operation operation = Operation::bitwise_xor;
  const auto reference_function = [](auto a, auto b) { return a ^ b; };

  using test_case_type =
      TestCase<TestType, operand_type_v<TestType, operation>>;
  auto test_case =
      create_test_case<test_case_type>(config, operation, reference_function);
  if (should_skip(test_case)) {
    return;
  }

  test_case.input = generate_input<typename test_case_type::test_host_type,
                                   typename test_case_type::operand_host_type>(
      test_case.global_work_size);
  test_case.compare_function = [](auto a, auto b) { compare(a, b); };
  test_signatures(test_case, memory_types_all, function_types_all,
                  memory_orders_all, memory_scopes_all);
}

TEMPLATE_TEST_CASE("atomic_fetch_and_signatures", "", ca::clc_int_t,
                   ca::clc_uint_t, ca::clc_long_t, ca::clc_ulong_t,
                   ca::clc_intptr_t, ca::clc_uintptr_t, ca::clc_size_t,
                   ca::clc_ptrdiff_t) {
  const TestConfig &config = get_test_config();
  const Operation operation = Operation::bitwise_and;
  const auto reference_function = [](auto a, auto b) { return a & b; };

  using test_case_type =
      TestCase<TestType, operand_type_v<TestType, operation>>;
  auto test_case =
      create_test_case<test_case_type>(config, operation, reference_function);
  if (should_skip(test_case)) {
    return;
  }

  test_case.input = generate_input<typename test_case_type::test_host_type,
                                   typename test_case_type::operand_host_type>(
      test_case.global_work_size);
  test_case.compare_function = [](auto a, auto b) { compare(a, b); };
  test_signatures(test_case, memory_types_all, function_types_all,
                  memory_orders_all, memory_scopes_all);
}

TEMPLATE_TEST_CASE("atomic_fetch_min_signatures", "", ca::clc_int_t,
                   ca::clc_uint_t, ca::clc_long_t, ca::clc_ulong_t,
                   ca::clc_intptr_t, ca::clc_uintptr_t, ca::clc_size_t,
                   ca::clc_ptrdiff_t) {
  const TestConfig &config = get_test_config();
  const Operation operation = Operation::compute_min;
  const auto reference_function = [](auto a, auto b) { return std::min(a, b); };

  using test_case_type =
      TestCase<TestType, operand_type_v<TestType, operation>>;
  auto test_case =
      create_test_case<test_case_type>(config, operation, reference_function);
  if (should_skip(test_case)) {
    return;
  }

  test_case.input = generate_input<typename test_case_type::test_host_type,
                                   typename test_case_type::operand_host_type>(
      test_case.global_work_size);
  test_case.compare_function = [](auto a, auto b) { compare(a, b); };
  test_signatures(test_case, memory_types_all, function_types_all,
                  memory_orders_all, memory_scopes_all);
}

TEMPLATE_TEST_CASE("atomic_fetch_max_signatures", "", ca::clc_int_t,
                   ca::clc_uint_t, ca::clc_long_t, ca::clc_ulong_t,
                   ca::clc_intptr_t, ca::clc_uintptr_t, ca::clc_size_t,
                   ca::clc_ptrdiff_t) {
  const TestConfig &config = get_test_config();
  const Operation operation = Operation::compute_max;
  const auto reference_function = [](auto a, auto b) { return std::max(a, b); };

  using test_case_type =
      TestCase<TestType, operand_type_v<TestType, operation>>;
  auto test_case =
      create_test_case<test_case_type>(config, operation, reference_function);
  if (should_skip(test_case)) {
    return;
  }

  test_case.input = generate_input<typename test_case_type::test_host_type,
                                   typename test_case_type::operand_host_type>(
      test_case.global_work_size);
  test_case.compare_function = [](auto a, auto b) { compare(a, b); };
  test_signatures(test_case, memory_types_all, function_types_all,
                  memory_orders_all, memory_scopes_all);
}

} // namespace
