/*
 * Copyright (C) 2021-2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ze_api.h>

#include <cassian/logging/logging.hpp>
#include <cassian/offline_compiler/offline_compiler.hpp>
#include <cassian/runtime/access_qualifier.hpp>
#include <cassian/runtime/device_properties.hpp>
#include <cassian/runtime/feature.hpp>
#include <cassian/runtime/image_properties.hpp>
#include <cassian/runtime/level_zero_utils.hpp>
#include <cassian/runtime/program_descriptor.hpp>
#include <cassian/runtime/runtime.hpp>
#include <cassian/utility/utility.hpp>

#include "level_zero_runtime.hpp"
#include "level_zero_wrapper.hpp"

namespace cassian {
LevelZeroRuntime::~LevelZeroRuntime() {
  if (queue_ != nullptr) {
    wrapper_.zeCommandQueueDestroy(queue_);
  }
  if (context_ != nullptr) {
    wrapper_.zeContextDestroy(context_);
  }
}

void LevelZeroRuntime::initialize() {
  logging::info() << "Runtime: " << name() << '\n';

  ze_result_t result = ZE_RESULT_SUCCESS;

  result = wrapper_.zeInit(0);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to initialize Level Zero");
  }

  uint32_t num_driver_handles = 1;
  result = wrapper_.zeDriverGet(&num_driver_handles, &driver_);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero driver");
  }

  uint32_t num_devices = 1;
  result = wrapper_.zeDeviceGet(driver_, &num_devices, &device_);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero device");
  }

  ze_context_desc_t context_description = {};
  context_description.stype = ZE_STRUCTURE_TYPE_CONTEXT_DESC;
  context_description.pNext = nullptr;
  context_description.flags = 0;

  result = wrapper_.zeContextCreate(driver_, &context_description, &context_);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero context");
  }

  ze_command_queue_desc_t command_queue_description = {};
  command_queue_description.stype = ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC;
  command_queue_description.pNext = nullptr;
  command_queue_description.ordinal = 0;
  command_queue_description.index = 0;
  command_queue_description.flags = 0;
  command_queue_description.mode = ZE_COMMAND_QUEUE_MODE_ASYNCHRONOUS;
  command_queue_description.priority = ZE_COMMAND_QUEUE_PRIORITY_NORMAL;

  result = wrapper_.zeCommandQueueCreate(context_, device_,
                                         &command_queue_description, &queue_);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero command queue");
  }
}

Buffer LevelZeroRuntime::create_buffer(const size_t size,
                                       AccessQualifier /*access*/) {
  ze_device_mem_alloc_desc_t device_memory_allocation_description = {};
  device_memory_allocation_description.stype =
      ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC;
  device_memory_allocation_description.pNext = nullptr;
  device_memory_allocation_description.flags = 0;
  device_memory_allocation_description.ordinal = 0;

  ze_host_mem_alloc_desc_t host_memory_allocation_description = {};
  host_memory_allocation_description.stype =
      ZE_STRUCTURE_TYPE_HOST_MEM_ALLOC_DESC;
  host_memory_allocation_description.pNext = nullptr;
  host_memory_allocation_description.flags = 0;

  void *buffer = nullptr;
  ze_result_t result = wrapper_.zeMemAllocShared(
      context_, &device_memory_allocation_description,
      &host_memory_allocation_description, size, 1, device_, &buffer);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to allocate Level Zero memory");
  }

  auto id = reinterpret_cast<std::uintptr_t>(buffer);
  buffers_[id] = buffer;

  return {id, size};
}

Image LevelZeroRuntime::create_image(const ImageDimensions dim,
                                     const ImageType type,
                                     const ImageFormat format,
                                     const ImageChannelOrder order,
                                     AccessQualifier access) {
  ze_image_desc_t image_description = {};
  image_description.stype = ZE_STRUCTURE_TYPE_IMAGE_DESC;
  image_description.pNext = nullptr;
  image_description.arraylevels = 0;
  image_description.width = dim.width;
  image_description.height = dim.height;
  image_description.depth = dim.depth;
  image_description.miplevels = 0;
  image_description.type = ze_get_image_type(type);
  image_description.flags =
      access == AccessQualifier::read_only ? 0 : ZE_IMAGE_FLAG_KERNEL_WRITE;
  image_description.format = ze_create_image_format(format, order);

  ze_image_handle_t image = nullptr;
  ze_result_t result =
      wrapper_.zeImageCreate(context_, device_, &image_description, &image);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to allocate Level Zero memory");
  }
  auto id = reinterpret_cast<std::uintptr_t>(image);
  images_[id] = image;
  return {id, dim};
}

Image LevelZeroRuntime::get_image_plane(Image image, ImagePlane plane,
                                        AccessQualifier access) {
  ImageChannelOrder order;
  auto dim = image.dim;

  ze_image_view_planar_exp_desc_t plane_description = {};
  plane_description.stype = ZE_STRUCTURE_TYPE_IMAGE_VIEW_PLANAR_EXP_DESC;
  plane_description.pNext = nullptr;

  switch (plane) {
  case ImagePlane::y:
    order = ImageChannelOrder::r;
    plane_description.planeIndex = 0;
    break;
  case ImagePlane::uv:
    order = ImageChannelOrder::rg;
    plane_description.planeIndex = 1;
    dim.width /= 2;
    dim.height /= 2;
    break;
  }

  ze_image_desc_t image_description = {};
  image_description.stype = ZE_STRUCTURE_TYPE_IMAGE_DESC;
  image_description.pNext = &plane_description;
  image_description.arraylevels = 0;
  image_description.width = dim.width;
  image_description.height = dim.height;
  image_description.depth = dim.depth;
  image_description.miplevels = 0;
  image_description.type = ze_get_image_type(ImageType::t_2d);
  image_description.flags =
      access == AccessQualifier::read_only ? 0 : ZE_IMAGE_FLAG_KERNEL_WRITE;
  image_description.format =
      ze_create_image_format(ImageFormat::unorm_int8, order);

  ze_image_handle_t image_view = nullptr;
  ze_result_t result = wrapper_.zeImageViewCreateExp(
      context_, device_, &image_description, images_[image.id], &image_view);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero image view");
  }
  auto id = reinterpret_cast<std::uintptr_t>(image_view);
  images_[id] = image_view;
  return {id, dim};
}

Sampler LevelZeroRuntime::create_sampler(SamplerCoordinates coordinates,
                                         SamplerAddressingMode address_mode,
                                         SamplerFilterMode filter_mode) {
  ze_sampler_desc_t sampler_description = {};
  sampler_description.stype = ZE_STRUCTURE_TYPE_SAMPLER_DESC;
  sampler_description.pNext = nullptr;

  switch (coordinates) {
  case SamplerCoordinates::unnormalized:
    sampler_description.isNormalized = 0U;
    break;
  case SamplerCoordinates::normalized:
    sampler_description.isNormalized = 1U;
    break;
  }

  switch (address_mode) {
  case SamplerAddressingMode::none:
    sampler_description.addressMode = ZE_SAMPLER_ADDRESS_MODE_NONE;
    break;
  case SamplerAddressingMode::clamp_to_edge:
    sampler_description.addressMode = ZE_SAMPLER_ADDRESS_MODE_CLAMP;
    break;
  case SamplerAddressingMode::clamp:
    sampler_description.addressMode = ZE_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    break;
  case SamplerAddressingMode::repeat:
    sampler_description.addressMode = ZE_SAMPLER_ADDRESS_MODE_REPEAT;
    break;
  case SamplerAddressingMode::mirror:
    sampler_description.addressMode = ZE_SAMPLER_ADDRESS_MODE_MIRROR;
    break;
  }

  switch (filter_mode) {
  case SamplerFilterMode::nearest:
    sampler_description.filterMode = ZE_SAMPLER_FILTER_MODE_NEAREST;
    break;
  case SamplerFilterMode::linear:
    sampler_description.filterMode = ZE_SAMPLER_FILTER_MODE_LINEAR;
    break;
  }

  ze_sampler_handle_t sampler = nullptr;
  ze_result_t result = wrapper_.zeSamplerCreate(context_, device_,
                                                &sampler_description, &sampler);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero sampler");
  }
  auto id = reinterpret_cast<std::uintptr_t>(sampler);
  samplers_[id] = sampler;
  return {id};
}

void LevelZeroRuntime::read_buffer(const Buffer &buffer, void *data) {
  void *b = buffers_.at(buffer.id);

  ze_command_list_desc_t command_list_description = {};
  command_list_description.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC;
  command_list_description.pNext = nullptr;
  command_list_description.commandQueueGroupOrdinal = 0;
  command_list_description.flags = 0;

  ze_command_list_handle_t command_list = nullptr;
  ze_result_t result = wrapper_.zeCommandListCreate(
      context_, device_, &command_list_description, &command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero command list");
  }

  result = wrapper_.zeCommandListAppendMemoryCopy(
      command_list, data, b, buffer.size, nullptr, 0, nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException(
        "Failed to append memory copy to Level Zero command list");
  }

  result = wrapper_.zeCommandListClose(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to close Level Zero command list");
  }

  result = wrapper_.zeCommandQueueExecuteCommandLists(queue_, 1, &command_list,
                                                      nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to execute Level Zero command list");
  }

  result = wrapper_.zeCommandQueueSynchronize(queue_, UINT64_MAX);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to synchronize Level Zero command queue");
  }

  result = wrapper_.zeCommandListDestroy(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy Level Zero command list");
  }
}

void LevelZeroRuntime::read_image(const Image &image, void *data) {
  ze_image_handle_t src_image = images_.at(image.id);

  ze_command_list_desc_t command_list_description = {};
  command_list_description.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC;
  command_list_description.pNext = nullptr;
  command_list_description.commandQueueGroupOrdinal = 0;
  command_list_description.flags = 0;

  ze_command_list_handle_t command_list = nullptr;
  ze_result_t result = wrapper_.zeCommandListCreate(
      context_, device_, &command_list_description, &command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero command list");
  }
  ze_image_region_t region = {};
  region.width = image.dim.width;
  region.height = image.dim.height;
  region.depth = image.dim.depth;
  region.originX = 0;
  region.originY = 0;
  region.originZ = 0;
  result = wrapper_.zeCommandListAppendImageCopyToMemory(
      command_list, data, src_image, &region, nullptr, 0, nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException(
        "Failed to append image copy to Level Zero command list");
  }
  result = wrapper_.zeCommandListClose(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to close Level Zero command list");
  }
  result = wrapper_.zeCommandQueueExecuteCommandLists(queue_, 1, &command_list,
                                                      nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to execute Level Zero command list");
  }
  result = wrapper_.zeCommandQueueSynchronize(queue_, UINT64_MAX);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to synchronize Level Zero command queue");
  }

  result = wrapper_.zeCommandListDestroy(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy Level Zero command list");
  }
}

void LevelZeroRuntime::write_buffer(const Buffer &buffer, const void *data) {
  void *b = buffers_.at(buffer.id);

  ze_command_list_desc_t command_list_description = {};
  command_list_description.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC;
  command_list_description.pNext = nullptr;
  command_list_description.commandQueueGroupOrdinal = 0;
  command_list_description.flags = 0;

  ze_command_list_handle_t command_list = nullptr;
  ze_result_t result = wrapper_.zeCommandListCreate(
      context_, device_, &command_list_description, &command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero command list");
  }

  result = wrapper_.zeCommandListAppendMemoryCopy(
      command_list, b, data, buffer.size, nullptr, 0, nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException(
        "Failed to append memory copy to Level Zero command list");
  }

  result = wrapper_.zeCommandListClose(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to close Level Zero command list");
  }

  result = wrapper_.zeCommandQueueExecuteCommandLists(queue_, 1, &command_list,
                                                      nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to execute Level Zero command list");
  }

  result = wrapper_.zeCommandQueueSynchronize(queue_, UINT64_MAX);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to synchronize Level Zero command queue");
  }

  result = wrapper_.zeCommandListDestroy(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy Level Zero command list");
  }
}

void LevelZeroRuntime::write_image(const Image &image, const void *data) {
  ze_image_handle_t dst_image = images_.at(image.id);

  ze_command_list_desc_t command_list_description = {};
  command_list_description.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC;
  command_list_description.pNext = nullptr;
  command_list_description.commandQueueGroupOrdinal = 0;
  command_list_description.flags = 0;

  ze_command_list_handle_t command_list = nullptr;
  ze_result_t result = wrapper_.zeCommandListCreate(
      context_, device_, &command_list_description, &command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero command list");
  }
  ze_image_region_t region = {};
  region.width = image.dim.width;
  region.height = image.dim.height;
  region.depth = image.dim.depth;
  region.originX = 0;
  region.originY = 0;
  region.originZ = 0;
  result = wrapper_.zeCommandListAppendImageCopyFromMemory(
      command_list, dst_image, data, &region, nullptr, 0, nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException(
        "Failed to append image copy to Level Zero command list");
  }
  result = wrapper_.zeCommandListClose(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to close Level Zero command list");
  }
  result = wrapper_.zeCommandQueueExecuteCommandLists(queue_, 1, &command_list,
                                                      nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to execute Level Zero command list");
  }
  result = wrapper_.zeCommandQueueSynchronize(queue_, UINT64_MAX);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to synchronize Level Zero command queue");
  }

  result = wrapper_.zeCommandListDestroy(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy Level Zero command list");
  }
}

void LevelZeroRuntime::release_buffer(const Buffer &buffer) {
  void *b = buffers_.at(buffer.id);
  buffers_.erase(buffer.id);

  ze_result_t result = wrapper_.zeMemFree(context_, b);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to free Level Zero memory");
  }
}

void LevelZeroRuntime::release_image(const Image &image) {
  ze_image_handle_t i = images_.at(image.id);
  images_.erase(image.id);
  ze_result_t result = wrapper_.zeImageDestroy(i);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy Level Zero image");
  }
}

void LevelZeroRuntime::release_sampler(const Sampler &sampler) {
  ze_sampler_handle_t s = samplers_.at(sampler.id);
  samplers_.erase(sampler.id);
  ze_result_t result = wrapper_.zeSamplerDestroy(s);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy Level Zero sampler");
  }
}

ze_module_handle_t LevelZeroRuntime::ze_create_module(
    const std::string &source, const std::string &build_options,
    const std::string &program_type,
    const std::optional<std::string> &spirv_options, bool quiet) {
  ze_result_t result = ZE_RESULT_SUCCESS;
  ze_module_handle_t module = nullptr;
  ze_module_build_log_handle_t build_log_handle = nullptr;
  auto f = finally([&]() mutable {
    if (build_log_handle != nullptr) {
      wrapper_.zeModuleBuildLogDestroy(build_log_handle);
    }
  });

  if (program_type == "source") {
    throw RuntimeException(
        "Compilation from source is not supported by Level Zero");
  }

  if (program_type == "spirv") {
    static const auto device_id =
        get_device_property(DeviceProperty::device_id);
    static const auto device_revision =
        get_device_property(DeviceProperty::device_revision);

    const std::vector<uint8_t> spv = generate_spirv_from_source(
        device_id, device_revision, source, build_options, quiet);

    ze_module_desc_t module_description = {};
    module_description.stype = ZE_STRUCTURE_TYPE_MODULE_DESC;
    module_description.pNext = nullptr;
    module_description.format = ZE_MODULE_FORMAT_IL_SPIRV;
    module_description.inputSize = static_cast<uint32_t>(spv.size());
    module_description.pInputModule = spv.data();

    if (spirv_options.has_value()) {
      module_description.pBuildFlags = spirv_options->c_str();
    } else if (build_options.find("-cmc") == 0) {
      module_description.pBuildFlags = "-vc-codegen";
    } else {
      module_description.pBuildFlags = build_options.c_str();
    }

    module_description.pConstants = nullptr;

    result = wrapper_.zeModuleCreate(context_, device_, &module_description,
                                     &module, &build_log_handle);
    if (result != ZE_RESULT_SUCCESS) {
      if (!quiet) {
        const auto build_log = ze_get_module_build_log(build_log_handle);
        logging::error() << "Build log:\n" << build_log << '\n';
      }

      throw RuntimeException("Failed to create Level Zero module");
    }
  } else {
    throw RuntimeException("Invalid program type: " + program_type);
  }

  return module;
}

Kernel LevelZeroRuntime::create_kernel(
    const std::string &kernel_name, const std::string &source,
    const std::string &build_options, const std::string &program_type,
    const std::optional<std::string> &spirv_options, bool quiet) {
  ze_result_t result = ZE_RESULT_SUCCESS;
  ze_module_handle_t module = ze_create_module(
      source, build_options, program_type, spirv_options, quiet);

  ze_kernel_desc_t kernel_description = {};
  kernel_description.stype = ZE_STRUCTURE_TYPE_KERNEL_DESC;
  kernel_description.pNext = nullptr;
  kernel_description.pKernelName = kernel_name.c_str();
  kernel_description.flags = 0;

  ze_kernel_handle_t kernel = nullptr;
  result = wrapper_.zeKernelCreate(module, &kernel_description, &kernel);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero kernel");
  }

  auto id = reinterpret_cast<std::uintptr_t>(kernel);
  kernels_[id] = kernel;
  modules_.emplace(id, module);

  return Kernel(id);
}

Kernel LevelZeroRuntime::create_kernel_from_multiple_programs(
    const std::string &kernel_name,
    const std::vector<ProgramDescriptor> &program_descriptors,
    const std::string & /*linker_options*/, bool quiet) {
  ze_result_t result = ZE_RESULT_SUCCESS;
  ze_module_build_log_handle_t link_log_handle = nullptr;
  std::vector<ze_module_handle_t> modules;

  auto f = finally([&]() mutable {
    if (link_log_handle != nullptr) {
      wrapper_.zeModuleBuildLogDestroy(link_log_handle);
    }

    for (auto *m : modules) {
      wrapper_.zeModuleDestroy(m);
    }
  });

  std::transform(std::begin(program_descriptors), std::end(program_descriptors),
                 std::back_inserter(modules), [this, quiet](const auto &desc) {
                   return ze_create_module(desc.source, desc.compiler_options,
                                           desc.program_type,
                                           desc.spirv_options, quiet);
                 });

  result = wrapper_.zeModuleDynamicLink(modules.size(), modules.data(),
                                        &link_log_handle);

  if (result != ZE_RESULT_SUCCESS) {
    if (!quiet) {
      const auto link_log = ze_get_module_build_log(link_log_handle);
      logging::error() << "Link log:\n" << link_log << '\n';
    }

    throw RuntimeException("Failed to link Level Zero modules");
  }

  ze_kernel_desc_t kernel_description = {};
  kernel_description.stype = ZE_STRUCTURE_TYPE_KERNEL_DESC;
  kernel_description.pNext = nullptr;
  kernel_description.pKernelName = kernel_name.c_str();
  kernel_description.flags = 0;

  ze_kernel_handle_t kernel = nullptr;

  auto it =
      std::find_if(std::begin(modules), std::end(modules), [&](auto module) {
        kernel = nullptr;

        auto status =
            wrapper_.zeKernelCreate(module, &kernel_description, &kernel);

        if (status == ZE_RESULT_SUCCESS) {
          return true;
        }
        if (status == ZE_RESULT_ERROR_INVALID_KERNEL_NAME) {
          return false;
        }
        throw RuntimeException("Failed to create Level Zero modules");
      });

  if (it == std::end(modules)) {
    throw RuntimeException("Failed to create Level Zero modules");
  }

  auto id = reinterpret_cast<std::uintptr_t>(kernel);
  kernels_[id] = kernel;

  for (auto *m : modules) {
    modules_.emplace(id, m);
  }

  modules.clear();

  return Kernel(id);
}

std::vector<uint8_t> LevelZeroRuntime::create_program_and_get_native_binary(
    const std::string &source, const std::string &build_options,
    const std::string &program_type,
    const std::optional<std::string> &spirv_options, bool quiet) {

  ze_module_handle_t module = ze_create_module(
      source, build_options, program_type, spirv_options, quiet);

  size_t binary_size = 0;
  ze_result_t status = ZE_RESULT_SUCCESS;
  status = wrapper_.zeModuleGetNativeBinary(module, &binary_size, nullptr);
  if (status != ZE_RESULT_SUCCESS) {
    throw RuntimeException(
        "Failed to get size using zeModuleGetNativeBinary command");
  }
  std::vector<uint8_t> program_bytes(binary_size);
  status = wrapper_.zeModuleGetNativeBinary(module, &binary_size,
                                            program_bytes.data());
  if (status != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get pModuleNativeBinary using "
                           "zeModuleGetNativeBinary command");
  }
  status = wrapper_.zeModuleDestroy(module);
  if (status != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy module");
  }
  return program_bytes;
}

void LevelZeroRuntime::set_kernel_argument(const Kernel &kernel,
                                           const int argument_index,
                                           const Buffer &buffer) {
  void *b = buffers_.at(buffer.id);
  set_kernel_argument(kernel, argument_index, sizeof(b), &b);
}

void LevelZeroRuntime::set_kernel_argument(const Kernel &kernel,
                                           const int argument_index,
                                           const Image &image) {
  ze_image_handle_t i = images_.at(image.id);
  set_kernel_argument(kernel, argument_index, sizeof(i), &i);
}

void LevelZeroRuntime::set_kernel_argument(const Kernel &kernel,
                                           const int argument_index,
                                           const Sampler &sampler) {
  ze_sampler_handle_t s = samplers_.at(sampler.id);
  set_kernel_argument(kernel, argument_index, sizeof(s), &s);
}

void LevelZeroRuntime::set_kernel_argument(const Kernel &kernel,
                                           const int argument_index,
                                           const size_t argument_size,
                                           const void *argument) {
  ze_kernel_handle_t k = kernels_.at(kernel.id);
  ze_result_t result = wrapper_.zeKernelSetArgumentValue(
      k, argument_index, argument_size, argument);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to set Level Zero kernel argument");
  }
}

void LevelZeroRuntime::run_kernel_common(
    const Kernel &kernel, const std::array<size_t, 3> global_work_size,
    const std::array<size_t, 3> *local_work_size) {
  ze_kernel_handle_t k = kernels_.at(kernel.id);

  ze_command_list_desc_t command_list_description = {};
  command_list_description.stype = ZE_STRUCTURE_TYPE_COMMAND_LIST_DESC;
  command_list_description.pNext = nullptr;
  command_list_description.commandQueueGroupOrdinal = 0;
  command_list_description.flags = 0;

  ze_command_list_handle_t command_list = nullptr;
  ze_result_t result = wrapper_.zeCommandListCreate(
      context_, device_, &command_list_description, &command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to create Level Zero command list");
  }

  ze_group_count_t thread_group_dimensions = {};
  std::array<uint32_t, 3> local_ws = {1, 1, 1};
  if (local_work_size == nullptr) {
    result = wrapper_.zeKernelSuggestGroupSize(
        k, global_work_size[0], global_work_size[1], global_work_size[2],
        &local_ws[0], &local_ws[1], &local_ws[2]);
    if (result != ZE_RESULT_SUCCESS) {
      throw RuntimeException("Failed to get Level Zero suggested group size");
    }

    result =
        wrapper_.zeKernelSetGroupSize(k, local_ws[0], local_ws[1], local_ws[2]);
    if (result != ZE_RESULT_SUCCESS) {
      throw RuntimeException("Failed to set Level Zero group size");
    }

    thread_group_dimensions.groupCountX =
        std::max(global_work_size[0] / local_ws[0], static_cast<size_t>(1));
    thread_group_dimensions.groupCountY =
        std::max(global_work_size[1] / local_ws[1], static_cast<size_t>(1));
    thread_group_dimensions.groupCountZ =
        std::max(global_work_size[2] / local_ws[2], static_cast<size_t>(1));
  } else {
    result = wrapper_.zeKernelSetGroupSize(k, local_work_size->at(0),
                                           local_work_size->at(1),
                                           local_work_size->at(2));
    if (result != ZE_RESULT_SUCCESS) {
      throw RuntimeException("Failed to set Level Zero group size");
    }

    thread_group_dimensions.groupCountX = std::max(
        global_work_size[0] / local_work_size->at(0), static_cast<size_t>(1));
    thread_group_dimensions.groupCountY = std::max(
        global_work_size[1] / local_work_size->at(1), static_cast<size_t>(1));
    thread_group_dimensions.groupCountZ = std::max(
        global_work_size[2] / local_work_size->at(2), static_cast<size_t>(1));
  }

  logging::debug() << "Running kernel with global_work_size = "
                   << to_string(global_work_size) << " and local_work_size = "
                   << (local_work_size != nullptr ? to_string(*local_work_size)
                                                  : to_string(local_ws))
                   << '\n';
  result = wrapper_.zeCommandListAppendLaunchKernel(
      command_list, k, &thread_group_dimensions, nullptr, 0, nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException(
        "Failed to append kernel to Level Zero command list");
  }

  result = wrapper_.zeCommandListClose(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to close Level Zero command list");
  }

  result = wrapper_.zeCommandQueueExecuteCommandLists(queue_, 1, &command_list,
                                                      nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to execute Level Zero command list");
  }

  result = wrapper_.zeCommandQueueSynchronize(queue_, UINT64_MAX);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to synchronize Level Zero command queue");
  }

  result = wrapper_.zeCommandListDestroy(command_list);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to destroy Level Zero command list");
  }
}

void LevelZeroRuntime::release_kernel(const Kernel &kernel) {
  ze_kernel_handle_t k = kernels_.at(kernel.id);
  kernels_.erase(kernel.id);

  ze_result_t result = wrapper_.zeKernelDestroy(k);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to release Level Zero kernel");
  }

  auto modules_for_kernel = modules_.equal_range(kernel.id);
  std::vector<std::pair<std::uintptr_t, ze_module_handle_t>> modules_to_destroy;

  auto is_module_in_use = [&](auto p) {
    return std::find_if(std::begin(modules_), std::end(modules_),
                        [id = p.first, m = p.second](auto p) {
                          return p.first != id && p.second == m;
                        }) != std::end(modules_);
  };

  std::copy_if(modules_for_kernel.first, modules_for_kernel.second,
               std::back_inserter(modules_to_destroy), is_module_in_use);

  modules_.erase(kernel.id);

  for (auto m : modules_to_destroy) {
    result = wrapper_.zeModuleDestroy(m.second);
    if (result != ZE_RESULT_SUCCESS) {
      throw RuntimeException("Failed to release Level Zero module");
    }
  }
}

bool LevelZeroRuntime::is_feature_supported(const Feature feature) const {
  ze_device_module_properties_t device_module_properties = {};
  device_module_properties.stype = ZE_STRUCTURE_TYPE_DEVICE_MODULE_PROPERTIES;
  device_module_properties.pNext = nullptr;

  ze_result_t result =
      wrapper_.zeDeviceGetModuleProperties(device_, &device_module_properties);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero kernel properties");
  }

  ze_device_image_properties_t device_image_properties = {};
  result =
      wrapper_.zeDeviceGetImageProperties(device_, &device_image_properties);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero image properties");
  }

  switch (feature) {
  case Feature::fp16:
    return (device_module_properties.flags & ZE_DEVICE_MODULE_FLAG_FP16) != 0;
  case Feature::fp64:
    return (device_module_properties.flags & ZE_DEVICE_MODULE_FLAG_FP64) != 0;
  case Feature::read_write_images:
    return (device_image_properties.maxReadImageArgs != 0U) &&
           (device_image_properties.maxWriteImageArgs != 0U);
  case Feature::image:
  case Feature::image2d:
    return get_device_property(DeviceProperty::image2d) != 0;
  case Feature::sampling:
    return get_device_property(DeviceProperty::max_num_samplers) != 0;
  case Feature::int64_atomics:
    return (device_module_properties.flags &
            ZE_DEVICE_MODULE_FLAG_INT64_ATOMICS) != 0;
  default:
    return false;
  }
}

int LevelZeroRuntime::get_device_property(const DeviceProperty property) const {
  ze_device_properties_t device_properties = {};
  device_properties.stype = ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES;
  device_properties.pNext = nullptr;
  ze_result_t result =
      wrapper_.zeDeviceGetProperties(device_, &device_properties);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero device properties");
  }

  ze_device_compute_properties_t device_compute_properties = {};
  device_compute_properties.stype = ZE_STRUCTURE_TYPE_DEVICE_COMPUTE_PROPERTIES;
  device_compute_properties.pNext = nullptr;
  result = wrapper_.zeDeviceGetComputeProperties(device_,
                                                 &device_compute_properties);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException(
        "Failed to get Level Zero device compute properties");
  }

  ze_device_image_properties_t device_image_properties = {};
  device_image_properties.stype = ZE_STRUCTURE_TYPE_DEVICE_IMAGE_PROPERTIES;
  device_image_properties.pNext = nullptr;
  result =
      wrapper_.zeDeviceGetImageProperties(device_, &device_image_properties);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero device image properties");
  }

  switch (property) {
  case DeviceProperty::max_group_size_x:
    return static_cast<int>(device_compute_properties.maxGroupSizeX);
  case DeviceProperty::max_group_size_y:
    return static_cast<int>(device_compute_properties.maxGroupSizeY);
  case DeviceProperty::max_group_size_z:
    return static_cast<int>(device_compute_properties.maxGroupSizeZ);
  case DeviceProperty::max_total_group_size:
    return static_cast<int>(device_compute_properties.maxTotalGroupSize);
  case DeviceProperty::max_num_samplers:
    return static_cast<int>(device_image_properties.maxSamplers);
  case DeviceProperty::image:
    return static_cast<int>(device_image_properties.maxImageDims1D != 0U &&
                            device_image_properties.maxImageDims2D != 0U &&
                            device_image_properties.maxImageDims3D != 0U);
  case DeviceProperty::image2d:
    return static_cast<int>(device_image_properties.maxImageDims2D);
  case DeviceProperty::max_local_memory_size:
    return static_cast<int>(device_compute_properties.maxSharedLocalMemory);
  case DeviceProperty::device_id:
    return static_cast<int>(device_properties.deviceId);
  case DeviceProperty::device_revision:
    return -1;
  case DeviceProperty::simd_width:
    return static_cast<int>(device_properties.physicalEUSimdWidth);
  default:
    throw RuntimeException("Failed to find device property");
  }
}

std::string LevelZeroRuntime::name() const { return "L0"; }

std::string LevelZeroRuntime::ze_get_module_build_log(
    const ze_module_build_log_handle_t &build_log_handle) const {
  size_t log_size = 0;
  ze_result_t result =
      wrapper_.zeModuleBuildLogGetString(build_log_handle, &log_size, nullptr);
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero build log size");
  }
  std::vector<char> log_vector(log_size);
  result = wrapper_.zeModuleBuildLogGetString(build_log_handle, &log_size,
                                              log_vector.data());
  if (result != ZE_RESULT_SUCCESS) {
    throw RuntimeException("Failed to get Level Zero build log");
  }
  std::string log(log_vector.begin(), log_vector.end());
  return log;
}

} // namespace cassian
