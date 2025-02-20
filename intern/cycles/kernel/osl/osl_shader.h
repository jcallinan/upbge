/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __OSL_SHADER_H__
#define __OSL_SHADER_H__

#ifdef WITH_OSL

/* OSL Shader Engine
 *
 * Holds all variables to execute and use OSL shaders from the kernel. These
 * are initialized externally by OSLShaderManager before rendering starts.
 *
 * Before/after a thread starts rendering, thread_init/thread_free must be
 * called, which will store any per thread OSL state in thread local storage.
 * This means no thread state must be passed along in the kernel itself.
 */

#  include "kernel/kernel_types.h"

CCL_NAMESPACE_BEGIN

class Scene;

struct ShaderClosure;
struct ShaderData;
struct IntegratorStateCPU;
struct differential3;
struct KernelGlobalsCPU;

struct OSLGlobals;
struct OSLShadingSystem;

class OSLShader {
 public:
  /* init */
  static void register_closures(OSLShadingSystem *ss);

  /* per thread data */
  static void thread_init(KernelGlobalsCPU *kg, OSLGlobals *osl_globals);
  static void thread_free(KernelGlobalsCPU *kg);

  /* eval */
  static void eval_surface(const KernelGlobalsCPU *kg,
                           const void *state,
                           ShaderData *sd,
                           uint32_t path_flag);
  static void eval_background(const KernelGlobalsCPU *kg,
                              const void *state,
                              ShaderData *sd,
                              uint32_t path_flag);
  static void eval_volume(const KernelGlobalsCPU *kg,
                          const void *state,
                          ShaderData *sd,
                          uint32_t path_flag);
  static void eval_displacement(const KernelGlobalsCPU *kg, const void *state, ShaderData *sd);

  /* attributes */
  static int find_attribute(const KernelGlobalsCPU *kg,
                            const ShaderData *sd,
                            uint id,
                            AttributeDescriptor *desc);
};

CCL_NAMESPACE_END

#endif

#endif /* __OSL_SHADER_H__ */
