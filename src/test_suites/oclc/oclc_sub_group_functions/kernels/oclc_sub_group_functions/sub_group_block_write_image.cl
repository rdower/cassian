/*
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */
#pragma OPENCL EXTENSION cl_intel_subgroups : enable
#pragma OPENCL EXTENSION cl_intel_subgroups_char : enable
#pragma OPENCL EXTENSION cl_intel_subgroups_short : enable
#pragma OPENCL EXTENSION cl_intel_subgroups_long : enable
#pragma OPENCL EXTENSION cl_khr_fp16 : enable
#pragma OPENCL EXTENSION cl_khr_fp64 : enable

kernel void
test_kernel_sub_group_block_write_image(__global INPUT_DATA_TYPE *input_data,
                                        write_only image2d_t image) {

  size_t x = get_global_id(0);

  size_t bytes_per_subgroup = get_sub_group_size() * sizeof(INPUT_DATA_TYPE);
  size_t offset_for_subgroup =
      (get_global_id(0) * sizeof(INPUT_DATA_TYPE) / bytes_per_subgroup) *
      bytes_per_subgroup;
  int size_of_scalar = sizeof(INPUT_DATA_TYPE);
  size_t y = get_global_id(1);
  size_t yc = get_global_id(1) * VECTOR_SIZE;
  size_t tidg = get_global_linear_id();
  int2 pos = (int2)(offset_for_subgroup, yc);
  int elements_per_pixel = 1;
  int ulong_size = sizeof(ulong);
  if (size_of_scalar == ulong_size)
    elements_per_pixel = 2;

  size_t img_pixel_width = get_image_width(image) / elements_per_pixel;
  size_t img_byte_offset = x + img_pixel_width * yc;
  DATA_TYPE write_result = 0;
#if VECTOR_SIZE > 1
  for (size_t i = 0; i < VECTOR_SIZE; i++) {

    write_result[i] = input_data[i * img_pixel_width + img_byte_offset];
  }
#else
  write_result = input_data[tidg];
#endif
  FUNC_NAME1(image, pos, write_result);
}