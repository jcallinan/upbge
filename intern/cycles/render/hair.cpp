/*
 * Copyright 2011-2020 Blender Foundation
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

#include "bvh/bvh.h"

#include "render/curves.h"
#include "render/hair.h"
#include "render/object.h"
#include "render/scene.h"

#include "integrator/shader_eval.h"

#include "util/util_progress.h"

CCL_NAMESPACE_BEGIN

/* Hair Curve */

void Hair::Curve::bounds_grow(const int k,
                              const float3 *curve_keys,
                              const float *curve_radius,
                              BoundBox &bounds) const
{
  float3 P[4];

  P[0] = curve_keys[max(first_key + k - 1, first_key)];
  P[1] = curve_keys[first_key + k];
  P[2] = curve_keys[first_key + k + 1];
  P[3] = curve_keys[min(first_key + k + 2, first_key + num_keys - 1)];

  float3 lower;
  float3 upper;

  curvebounds(&lower.x, &upper.x, P, 0);
  curvebounds(&lower.y, &upper.y, P, 1);
  curvebounds(&lower.z, &upper.z, P, 2);

  float mr = max(curve_radius[first_key + k], curve_radius[first_key + k + 1]);

  bounds.grow(lower, mr);
  bounds.grow(upper, mr);
}

void Hair::Curve::bounds_grow(const int k,
                              const float3 *curve_keys,
                              const float *curve_radius,
                              const Transform &aligned_space,
                              BoundBox &bounds) const
{
  float3 P[4];

  P[0] = curve_keys[max(first_key + k - 1, first_key)];
  P[1] = curve_keys[first_key + k];
  P[2] = curve_keys[first_key + k + 1];
  P[3] = curve_keys[min(first_key + k + 2, first_key + num_keys - 1)];

  P[0] = transform_point(&aligned_space, P[0]);
  P[1] = transform_point(&aligned_space, P[1]);
  P[2] = transform_point(&aligned_space, P[2]);
  P[3] = transform_point(&aligned_space, P[3]);

  float3 lower;
  float3 upper;

  curvebounds(&lower.x, &upper.x, P, 0);
  curvebounds(&lower.y, &upper.y, P, 1);
  curvebounds(&lower.z, &upper.z, P, 2);

  float mr = max(curve_radius[first_key + k], curve_radius[first_key + k + 1]);

  bounds.grow(lower, mr);
  bounds.grow(upper, mr);
}

void Hair::Curve::bounds_grow(float4 keys[4], BoundBox &bounds) const
{
  float3 P[4] = {
      float4_to_float3(keys[0]),
      float4_to_float3(keys[1]),
      float4_to_float3(keys[2]),
      float4_to_float3(keys[3]),
  };

  float3 lower;
  float3 upper;

  curvebounds(&lower.x, &upper.x, P, 0);
  curvebounds(&lower.y, &upper.y, P, 1);
  curvebounds(&lower.z, &upper.z, P, 2);

  float mr = max(keys[1].w, keys[2].w);

  bounds.grow(lower, mr);
  bounds.grow(upper, mr);
}

void Hair::Curve::motion_keys(const float3 *curve_keys,
                              const float *curve_radius,
                              const float3 *key_steps,
                              size_t num_curve_keys,
                              size_t num_steps,
                              float time,
                              size_t k0,
                              size_t k1,
                              float4 r_keys[2]) const
{
  /* Figure out which steps we need to fetch and their interpolation factor. */
  const size_t max_step = num_steps - 1;
  const size_t step = min((int)(time * max_step), max_step - 1);
  const float t = time * max_step - step;
  /* Fetch vertex coordinates. */
  float4 curr_keys[2];
  float4 next_keys[2];
  keys_for_step(
      curve_keys, curve_radius, key_steps, num_curve_keys, num_steps, step, k0, k1, curr_keys);
  keys_for_step(
      curve_keys, curve_radius, key_steps, num_curve_keys, num_steps, step + 1, k0, k1, next_keys);
  /* Interpolate between steps. */
  r_keys[0] = (1.0f - t) * curr_keys[0] + t * next_keys[0];
  r_keys[1] = (1.0f - t) * curr_keys[1] + t * next_keys[1];
}

void Hair::Curve::cardinal_motion_keys(const float3 *curve_keys,
                                       const float *curve_radius,
                                       const float3 *key_steps,
                                       size_t num_curve_keys,
                                       size_t num_steps,
                                       float time,
                                       size_t k0,
                                       size_t k1,
                                       size_t k2,
                                       size_t k3,
                                       float4 r_keys[4]) const
{
  /* Figure out which steps we need to fetch and their interpolation factor. */
  const size_t max_step = num_steps - 1;
  const size_t step = min((int)(time * max_step), max_step - 1);
  const float t = time * max_step - step;
  /* Fetch vertex coordinates. */
  float4 curr_keys[4];
  float4 next_keys[4];
  cardinal_keys_for_step(curve_keys,
                         curve_radius,
                         key_steps,
                         num_curve_keys,
                         num_steps,
                         step,
                         k0,
                         k1,
                         k2,
                         k3,
                         curr_keys);
  cardinal_keys_for_step(curve_keys,
                         curve_radius,
                         key_steps,
                         num_curve_keys,
                         num_steps,
                         step + 1,
                         k0,
                         k1,
                         k2,
                         k3,
                         next_keys);
  /* Interpolate between steps. */
  r_keys[0] = (1.0f - t) * curr_keys[0] + t * next_keys[0];
  r_keys[1] = (1.0f - t) * curr_keys[1] + t * next_keys[1];
  r_keys[2] = (1.0f - t) * curr_keys[2] + t * next_keys[2];
  r_keys[3] = (1.0f - t) * curr_keys[3] + t * next_keys[3];
}

void Hair::Curve::keys_for_step(const float3 *curve_keys,
                                const float *curve_radius,
                                const float3 *key_steps,
                                size_t num_curve_keys,
                                size_t num_steps,
                                size_t step,
                                size_t k0,
                                size_t k1,
                                float4 r_keys[2]) const
{
  k0 = max(k0, 0);
  k1 = min(k1, num_keys - 1);
  const size_t center_step = ((num_steps - 1) / 2);
  if (step == center_step) {
    /* Center step: regular key location. */
    /* TODO(sergey): Consider adding make_float4(float3, float)
     * function.
     */
    r_keys[0] = make_float4(curve_keys[first_key + k0].x,
                            curve_keys[first_key + k0].y,
                            curve_keys[first_key + k0].z,
                            curve_radius[first_key + k0]);
    r_keys[1] = make_float4(curve_keys[first_key + k1].x,
                            curve_keys[first_key + k1].y,
                            curve_keys[first_key + k1].z,
                            curve_radius[first_key + k1]);
  }
  else {
    /* Center step is not stored in this array. */
    if (step > center_step) {
      step--;
    }
    const size_t offset = first_key + step * num_curve_keys;
    r_keys[0] = make_float4(key_steps[offset + k0].x,
                            key_steps[offset + k0].y,
                            key_steps[offset + k0].z,
                            curve_radius[first_key + k0]);
    r_keys[1] = make_float4(key_steps[offset + k1].x,
                            key_steps[offset + k1].y,
                            key_steps[offset + k1].z,
                            curve_radius[first_key + k1]);
  }
}

void Hair::Curve::cardinal_keys_for_step(const float3 *curve_keys,
                                         const float *curve_radius,
                                         const float3 *key_steps,
                                         size_t num_curve_keys,
                                         size_t num_steps,
                                         size_t step,
                                         size_t k0,
                                         size_t k1,
                                         size_t k2,
                                         size_t k3,
                                         float4 r_keys[4]) const
{
  k0 = max(k0, 0);
  k3 = min(k3, num_keys - 1);
  const size_t center_step = ((num_steps - 1) / 2);
  if (step == center_step) {
    /* Center step: regular key location. */
    r_keys[0] = make_float4(curve_keys[first_key + k0].x,
                            curve_keys[first_key + k0].y,
                            curve_keys[first_key + k0].z,
                            curve_radius[first_key + k0]);
    r_keys[1] = make_float4(curve_keys[first_key + k1].x,
                            curve_keys[first_key + k1].y,
                            curve_keys[first_key + k1].z,
                            curve_radius[first_key + k1]);
    r_keys[2] = make_float4(curve_keys[first_key + k2].x,
                            curve_keys[first_key + k2].y,
                            curve_keys[first_key + k2].z,
                            curve_radius[first_key + k2]);
    r_keys[3] = make_float4(curve_keys[first_key + k3].x,
                            curve_keys[first_key + k3].y,
                            curve_keys[first_key + k3].z,
                            curve_radius[first_key + k3]);
  }
  else {
    /* Center step is not stored in this array. */
    if (step > center_step) {
      step--;
    }
    const size_t offset = first_key + step * num_curve_keys;
    r_keys[0] = make_float4(key_steps[offset + k0].x,
                            key_steps[offset + k0].y,
                            key_steps[offset + k0].z,
                            curve_radius[first_key + k0]);
    r_keys[1] = make_float4(key_steps[offset + k1].x,
                            key_steps[offset + k1].y,
                            key_steps[offset + k1].z,
                            curve_radius[first_key + k1]);
    r_keys[2] = make_float4(key_steps[offset + k2].x,
                            key_steps[offset + k2].y,
                            key_steps[offset + k2].z,
                            curve_radius[first_key + k2]);
    r_keys[3] = make_float4(key_steps[offset + k3].x,
                            key_steps[offset + k3].y,
                            key_steps[offset + k3].z,
                            curve_radius[first_key + k3]);
  }
}

/* Hair */

NODE_DEFINE(Hair)
{
  NodeType *type = NodeType::add("hair", create, NodeType::NONE, Geometry::get_node_base_type());

  SOCKET_POINT_ARRAY(curve_keys, "Curve Keys", array<float3>());
  SOCKET_FLOAT_ARRAY(curve_radius, "Curve Radius", array<float>());
  SOCKET_INT_ARRAY(curve_first_key, "Curve First Key", array<int>());
  SOCKET_INT_ARRAY(curve_shader, "Curve Shader", array<int>());

  return type;
}

Hair::Hair() : Geometry(get_node_type(), Geometry::HAIR)
{
  curve_key_offset = 0;
  curve_segment_offset = 0;
  curve_shape = CURVE_RIBBON;
}

Hair::~Hair()
{
}

void Hair::resize_curves(int numcurves, int numkeys)
{
  curve_keys.resize(numkeys);
  curve_radius.resize(numkeys);
  curve_first_key.resize(numcurves);
  curve_shader.resize(numcurves);

  attributes.resize();
}

void Hair::reserve_curves(int numcurves, int numkeys)
{
  curve_keys.reserve(numkeys);
  curve_radius.reserve(numkeys);
  curve_first_key.reserve(numcurves);
  curve_shader.reserve(numcurves);

  attributes.resize(true);
}

void Hair::clear(bool preserve_shaders)
{
  Geometry::clear(preserve_shaders);

  curve_keys.clear();
  curve_radius.clear();
  curve_first_key.clear();
  curve_shader.clear();

  attributes.clear();
}

void Hair::add_curve_key(float3 co, float radius)
{
  curve_keys.push_back_reserved(co);
  curve_radius.push_back_reserved(radius);

  tag_curve_keys_modified();
  tag_curve_radius_modified();
}

void Hair::add_curve(int first_key, int shader)
{
  curve_first_key.push_back_reserved(first_key);
  curve_shader.push_back_reserved(shader);

  tag_curve_first_key_modified();
  tag_curve_shader_modified();
}

void Hair::copy_center_to_motion_step(const int motion_step)
{
  Attribute *attr_mP = attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
  if (attr_mP) {
    float3 *keys = &curve_keys[0];
    size_t numkeys = curve_keys.size();
    memcpy(attr_mP->data_float3() + motion_step * numkeys, keys, sizeof(float3) * numkeys);
  }
}

void Hair::get_uv_tiles(ustring map, unordered_set<int> &tiles)
{
  Attribute *attr;

  if (map.empty()) {
    attr = attributes.find(ATTR_STD_UV);
  }
  else {
    attr = attributes.find(map);
  }

  if (attr) {
    attr->get_uv_tiles(this, ATTR_PRIM_GEOMETRY, tiles);
  }
}

void Hair::compute_bounds()
{
  BoundBox bnds = BoundBox::empty;
  size_t curve_keys_size = curve_keys.size();

  if (curve_keys_size > 0) {
    for (size_t i = 0; i < curve_keys_size; i++)
      bnds.grow(curve_keys[i], curve_radius[i]);

    Attribute *curve_attr = attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);
    if (use_motion_blur && curve_attr) {
      size_t steps_size = curve_keys.size() * (motion_steps - 1);
      float3 *key_steps = curve_attr->data_float3();

      for (size_t i = 0; i < steps_size; i++)
        bnds.grow(key_steps[i]);
    }

    if (!bnds.valid()) {
      bnds = BoundBox::empty;

      /* skip nan or inf coordinates */
      for (size_t i = 0; i < curve_keys_size; i++)
        bnds.grow_safe(curve_keys[i], curve_radius[i]);

      if (use_motion_blur && curve_attr) {
        size_t steps_size = curve_keys.size() * (motion_steps - 1);
        float3 *key_steps = curve_attr->data_float3();

        for (size_t i = 0; i < steps_size; i++)
          bnds.grow_safe(key_steps[i]);
      }
    }
  }

  if (!bnds.valid()) {
    /* empty mesh */
    bnds.grow(zero_float3());
  }

  bounds = bnds;
}

void Hair::apply_transform(const Transform &tfm, const bool apply_to_motion)
{
  /* compute uniform scale */
  float3 c0 = transform_get_column(&tfm, 0);
  float3 c1 = transform_get_column(&tfm, 1);
  float3 c2 = transform_get_column(&tfm, 2);
  float scalar = powf(fabsf(dot(cross(c0, c1), c2)), 1.0f / 3.0f);

  /* apply transform to curve keys */
  for (size_t i = 0; i < curve_keys.size(); i++) {
    float3 co = transform_point(&tfm, curve_keys[i]);
    float radius = curve_radius[i] * scalar;

    /* scale for curve radius is only correct for uniform scale */
    curve_keys[i] = co;
    curve_radius[i] = radius;
  }

  tag_curve_keys_modified();
  tag_curve_radius_modified();

  if (apply_to_motion) {
    Attribute *curve_attr = attributes.find(ATTR_STD_MOTION_VERTEX_POSITION);

    if (curve_attr) {
      /* apply transform to motion curve keys */
      size_t steps_size = curve_keys.size() * (motion_steps - 1);
      float4 *key_steps = curve_attr->data_float4();

      for (size_t i = 0; i < steps_size; i++) {
        float3 co = transform_point(&tfm, float4_to_float3(key_steps[i]));
        float radius = key_steps[i].w * scalar;

        /* scale for curve radius is only correct for uniform scale */
        key_steps[i] = float3_to_float4(co);
        key_steps[i].w = radius;
      }
    }
  }
}

void Hair::pack_curves(Scene *scene,
                       float4 *curve_key_co,
                       KernelCurve *curves,
                       KernelCurveSegment *curve_segments)
{
  size_t curve_keys_size = curve_keys.size();

  /* pack curve keys */
  if (curve_keys_size) {
    float3 *keys_ptr = curve_keys.data();
    float *radius_ptr = curve_radius.data();

    for (size_t i = 0; i < curve_keys_size; i++)
      curve_key_co[i] = make_float4(keys_ptr[i].x, keys_ptr[i].y, keys_ptr[i].z, radius_ptr[i]);
  }

  /* pack curve segments */
  const PrimitiveType type = primitive_type();

  size_t curve_num = num_curves();
  size_t index = 0;

  for (size_t i = 0; i < curve_num; i++) {
    Curve curve = get_curve(i);
    int shader_id = curve_shader[i];
    Shader *shader = (shader_id < used_shaders.size()) ?
                         static_cast<Shader *>(used_shaders[shader_id]) :
                         scene->default_surface;
    shader_id = scene->shader_manager->get_shader_id(shader, false);

    curves[i].shader_id = shader_id;
    curves[i].first_key = curve_key_offset + curve.first_key;
    curves[i].num_keys = curve.num_keys;
    curves[i].type = type;

    for (int k = 0; k < curve.num_segments(); ++k, ++index) {
      curve_segments[index].prim = prim_offset + i;
      curve_segments[index].type = PRIMITIVE_PACK_SEGMENT(type, k);
    }
  }
}

PrimitiveType Hair::primitive_type() const
{
  return has_motion_blur() ?
             ((curve_shape == CURVE_RIBBON) ? PRIMITIVE_MOTION_CURVE_RIBBON :
                                              PRIMITIVE_MOTION_CURVE_THICK) :
             ((curve_shape == CURVE_RIBBON) ? PRIMITIVE_CURVE_RIBBON : PRIMITIVE_CURVE_THICK);
}

/* Fill in coordinates for curve transparency shader evaluation on device. */
static int fill_shader_input(const Hair *hair,
                             const int object_index,
                             device_vector<KernelShaderEvalInput> &d_input)
{
  int d_input_size = 0;
  KernelShaderEvalInput *d_input_data = d_input.data();

  const int num_curves = hair->num_curves();
  for (int i = 0; i < num_curves; i++) {
    const Hair::Curve curve = hair->get_curve(i);
    const int num_segments = curve.num_segments();

    for (int j = 0; j < num_segments + 1; j++) {
      KernelShaderEvalInput in;
      in.object = object_index;
      in.prim = hair->prim_offset + i;
      in.u = (j < num_segments) ? 0.0f : 1.0f;
      in.v = (j < num_segments) ? __int_as_float(j) : __int_as_float(j - 1);
      d_input_data[d_input_size++] = in;
    }
  }

  return d_input_size;
}

/* Read back curve transparency shader output. */
static void read_shader_output(float *shadow_transparency,
                               bool &is_fully_opaque,
                               const device_vector<float> &d_output)
{
  const int num_keys = d_output.size();
  const float *output_data = d_output.data();
  bool is_opaque = true;

  for (int i = 0; i < num_keys; i++) {
    shadow_transparency[i] = output_data[i];
    if (shadow_transparency[i] > 0.0f) {
      is_opaque = false;
    }
  }

  is_fully_opaque = is_opaque;
}

bool Hair::need_shadow_transparency()
{
  for (const Node *node : used_shaders) {
    const Shader *shader = static_cast<const Shader *>(node);
    if (shader->has_surface_transparent && shader->get_use_transparent_shadow()) {
      return true;
    }
  }

  return false;
}

bool Hair::update_shadow_transparency(Device *device, Scene *scene, Progress &progress)
{
  if (!need_shadow_transparency()) {
    /* If no shaders with shadow transparency, remove attribute. */
    Attribute *attr = attributes.find(ATTR_STD_SHADOW_TRANSPARENCY);
    if (attr) {
      attributes.remove(attr);
      return true;
    }
    else {
      return false;
    }
  }

  string msg = string_printf("Computing Shadow Transparency %s", name.c_str());
  progress.set_status("Updating Hair", msg);

  /* Create shadow transparency attribute. */
  Attribute *attr = attributes.find(ATTR_STD_SHADOW_TRANSPARENCY);
  const bool attribute_exists = (attr != nullptr);
  if (!attribute_exists) {
    attr = attributes.add(ATTR_STD_SHADOW_TRANSPARENCY);
  }

  float *attr_data = attr->data_float();

  /* Find object index. */
  size_t object_index = OBJECT_NONE;

  for (size_t i = 0; i < scene->objects.size(); i++) {
    if (scene->objects[i]->get_geometry() == this) {
      object_index = i;
      break;
    }
  }

  /* Evaluate shader on device. */
  ShaderEval shader_eval(device, progress);
  bool is_fully_opaque = false;
  shader_eval.eval(SHADER_EVAL_CURVE_SHADOW_TRANSPARENCY,
                   num_keys(),
                   1,
                   function_bind(&fill_shader_input, this, object_index, _1),
                   function_bind(&read_shader_output, attr_data, is_fully_opaque, _1));

  if (is_fully_opaque) {
    attributes.remove(attr);
    return attribute_exists;
  }

  return true;
}

CCL_NAMESPACE_END
