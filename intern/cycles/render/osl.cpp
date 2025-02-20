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

#include "device/device.h"

#include "render/background.h"
#include "render/colorspace.h"
#include "render/graph.h"
#include "render/light.h"
#include "render/nodes.h"
#include "render/osl.h"
#include "render/scene.h"
#include "render/shader.h"
#include "render/stats.h"

#ifdef WITH_OSL

#  include "kernel/osl/osl_globals.h"
#  include "kernel/osl/osl_services.h"
#  include "kernel/osl/osl_shader.h"

#  include "util/util_aligned_malloc.h"
#  include "util/util_foreach.h"
#  include "util/util_logging.h"
#  include "util/util_md5.h"
#  include "util/util_path.h"
#  include "util/util_progress.h"
#  include "util/util_projection.h"

#endif

CCL_NAMESPACE_BEGIN

#ifdef WITH_OSL

/* Shared Texture and Shading System */

OSL::TextureSystem *OSLShaderManager::ts_shared = NULL;
int OSLShaderManager::ts_shared_users = 0;
thread_mutex OSLShaderManager::ts_shared_mutex;

OSL::ShadingSystem *OSLShaderManager::ss_shared = NULL;
OSLRenderServices *OSLShaderManager::services_shared = NULL;
int OSLShaderManager::ss_shared_users = 0;
thread_mutex OSLShaderManager::ss_shared_mutex;
thread_mutex OSLShaderManager::ss_mutex;
int OSLCompiler::texture_shared_unique_id = 0;

/* Shader Manager */

OSLShaderManager::OSLShaderManager()
{
  texture_system_init();
  shading_system_init();
}

OSLShaderManager::~OSLShaderManager()
{
  shading_system_free();
  texture_system_free();
}

void OSLShaderManager::free_memory()
{
#  ifdef OSL_HAS_BLENDER_CLEANUP_FIX
  /* There is a problem with LLVM+OSL: The order global destructors across
   * different compilation units run cannot be guaranteed, on windows this means
   * that the LLVM destructors run before the osl destructors, causing a crash
   * when the process exits. the OSL in svn has a special cleanup hack to
   * sidestep this behavior */
  OSL::pvt::LLVM_Util::Cleanup();
#  endif
}

void OSLShaderManager::reset(Scene * /*scene*/)
{
  shading_system_free();
  shading_system_init();
}

void OSLShaderManager::host_update_specific(Scene *scene, Progress &progress)
{
  if (!need_update()) {
    return;
  }

  scoped_callback_timer timer([scene](double time) {
    if (scene->update_stats) {
      scene->update_stats->osl.times.add_entry({"host_update", time});
    }
  });

  VLOG(1) << "Total " << scene->shaders.size() << " shaders.";

  /* set texture system */
  scene->image_manager->set_osl_texture_system((void *)ts);

  /* create shaders */
  Shader *background_shader = scene->background->get_shader(scene);

  for (Shader *shader : scene->shaders) {
    assert(shader->graph);

    if (progress.get_cancel()) {
      return;
    }

    /* we can only compile one shader at the time as the OSL ShadingSystem
     * has a single state, but we put the lock here so different renders can
     * compile shaders alternating */
    thread_scoped_lock lock(ss_mutex);

    OSLCompiler compiler(this, services, ss, scene);
    compiler.background = (shader == background_shader);
    compiler.compile(shader);

    if (shader->get_use_mis() && shader->has_surface_emission) {
      scene->light_manager->tag_update(scene, LightManager::SHADER_COMPILED);
    }
  }

  /* add special builtin texture types */
  services->textures.insert(ustring("@ao"), new OSLTextureHandle(OSLTextureHandle::AO));
  services->textures.insert(ustring("@bevel"), new OSLTextureHandle(OSLTextureHandle::BEVEL));

  {
    /* Perform greedyjit optimization.
     *
     * This might waste time on optimizing groups which are never actually
     * used, but this prevents OSL from allocating data on TLS at render
     * time.
     *
     * This is much better for us because this way we aren't required to
     * stop task scheduler threads to make sure all TLS is clean and don't
     * have issues with TLS data free accessing freed memory if task scheduler
     * is being freed after the Session is freed.
     */
    thread_scoped_lock lock(ss_shared_mutex);
    ss->optimize_all_groups();
  }
}

void OSLShaderManager::device_update_specific(Device *device,
                                              DeviceScene *dscene,
                                              Scene *scene,
                                              Progress &progress)
{
  if (!need_update())
    return;

  scoped_callback_timer timer([scene](double time) {
    if (scene->update_stats) {
      scene->update_stats->osl.times.add_entry({"device_update", time});
    }
  });

  device_free(device, dscene, scene);

  OSLGlobals *og = (OSLGlobals *)device->get_cpu_osl_memory();
  Shader *background_shader = scene->background->get_shader(scene);

  /* Setup shader engine. */
  og->ss = ss;
  og->ts = ts;
  og->services = services;

  for (Shader *shader : scene->shaders) {
    /* push state to array for lookup */
    og->surface_state.push_back(shader->osl_surface_ref);
    og->volume_state.push_back(shader->osl_volume_ref);
    og->displacement_state.push_back(shader->osl_displacement_ref);
    og->bump_state.push_back(shader->osl_surface_bump_ref);

    shader->clear_modified();
  }

  const int background_id = scene->shader_manager->get_shader_id(background_shader);
  const int background_state_index = (background_id & SHADER_MASK);
  DCHECK_LT(background_state_index, og->surface_state.size());
  og->background_state = og->surface_state[background_state_index];
  og->use = true;

  update_flags = UPDATE_NONE;

  device_update_common(device, dscene, scene, progress);
}

void OSLShaderManager::device_free(Device *device, DeviceScene *dscene, Scene *scene)
{
  OSLGlobals *og = (OSLGlobals *)device->get_cpu_osl_memory();

  device_free_common(device, dscene, scene);

  /* clear shader engine */
  og->use = false;
  og->ss = NULL;
  og->ts = NULL;

  og->surface_state.clear();
  og->volume_state.clear();
  og->displacement_state.clear();
  og->bump_state.clear();
  og->background_state.reset();
}

void OSLShaderManager::texture_system_init()
{
  /* create texture system, shared between different renders to reduce memory usage */
  thread_scoped_lock lock(ts_shared_mutex);

  if (ts_shared_users == 0) {
    ts_shared = TextureSystem::create(true);

    ts_shared->attribute("automip", 1);
    ts_shared->attribute("autotile", 64);
    ts_shared->attribute("gray_to_rgb", 1);

    /* effectively unlimited for now, until we support proper mipmap lookups */
    ts_shared->attribute("max_memory_MB", 16384);
  }

  ts = ts_shared;
  ts_shared_users++;
}

void OSLShaderManager::texture_system_free()
{
  /* shared texture system decrease users and destroy if no longer used */
  thread_scoped_lock lock(ts_shared_mutex);
  ts_shared_users--;

  if (ts_shared_users == 0) {
    ts_shared->invalidate_all(true);
    OSL::TextureSystem::destroy(ts_shared);
    ts_shared = NULL;
  }

  ts = NULL;
}

void OSLShaderManager::shading_system_init()
{
  /* create shading system, shared between different renders to reduce memory usage */
  thread_scoped_lock lock(ss_shared_mutex);

  if (ss_shared_users == 0) {
    /* Must use aligned new due to concurrent hash map. */
    services_shared = util_aligned_new<OSLRenderServices>(ts_shared);

    string shader_path = path_get("shader");
#  ifdef _WIN32
    /* Annoying thing, Cycles stores paths in UTF-8 codepage, so it can
     * operate with file paths with any character. This requires to use wide
     * char functions, but OSL uses old fashioned ANSI functions which means:
     *
     * - We have to convert our paths to ANSI before passing to OSL
     * - OSL can't be used when there's a multi-byte character in the path
     *   to the shaders folder.
     */
    shader_path = string_to_ansi(shader_path);
#  endif

    ss_shared = new OSL::ShadingSystem(services_shared, ts_shared, &errhandler);
    ss_shared->attribute("lockgeom", 1);
    ss_shared->attribute("commonspace", "world");
    ss_shared->attribute("searchpath:shader", shader_path);
    ss_shared->attribute("greedyjit", 1);

    VLOG(1) << "Using shader search path: " << shader_path;

    /* our own ray types */
    static const char *raytypes[] = {
        "camera",         /* PATH_RAY_CAMERA */
        "reflection",     /* PATH_RAY_REFLECT */
        "refraction",     /* PATH_RAY_TRANSMIT */
        "diffuse",        /* PATH_RAY_DIFFUSE */
        "glossy",         /* PATH_RAY_GLOSSY */
        "singular",       /* PATH_RAY_SINGULAR */
        "transparent",    /* PATH_RAY_TRANSPARENT */
        "volume_scatter", /* PATH_RAY_VOLUME_SCATTER */

        "shadow", /* PATH_RAY_SHADOW_OPAQUE */
        "shadow", /* PATH_RAY_SHADOW_TRANSPARENT */

        "__unused__", /* PATH_RAY_NODE_UNALIGNED */
        "__unused__", /* PATH_RAY_MIS_SKIP */

        "diffuse_ancestor", /* PATH_RAY_DIFFUSE_ANCESTOR */

        "__unused__", /* PATH_RAY_SINGLE_PASS_DONE */
        "__unused__", /* PATH_RAY_TRANSPARENT_BACKGROUND */
        "__unused__", /* PATH_RAY_TERMINATE_IMMEDIATE */
        "__unused__", /* PATH_RAY_TERMINATE_AFTER_TRANSPARENT */
        "__unused__", /* PATH_RAY_EMISSION */
        "__unused__", /* PATH_RAY_SUBSURFACE */
        "__unused__", /* PATH_RAY_DENOISING_FEATURES */
        "__unused__", /* PATH_RAY_REFLECT_PASS */
        "__unused__", /* PATH_RAY_TRANSMISSION_PASS */
        "__unused__", /* PATH_RAY_VOLUME_PASS */
        "__unused__", /* PATH_RAY_SHADOW_FOR_LIGHT */
        "__unused__", /* PATH_RAY_SHADOW_CATCHER_HIT */
        "__unused__", /* PATH_RAY_SHADOW_CATCHER_PASS */
    };

    const int nraytypes = sizeof(raytypes) / sizeof(raytypes[0]);
    ss_shared->attribute("raytypes", TypeDesc(TypeDesc::STRING, nraytypes), raytypes);

    OSLShader::register_closures((OSLShadingSystem *)ss_shared);

    loaded_shaders.clear();
  }

  ss = ss_shared;
  services = services_shared;
  ss_shared_users++;
}

void OSLShaderManager::shading_system_free()
{
  /* shared shading system decrease users and destroy if no longer used */
  thread_scoped_lock lock(ss_shared_mutex);
  ss_shared_users--;

  if (ss_shared_users == 0) {
    delete ss_shared;
    ss_shared = NULL;

    util_aligned_delete(services_shared);
    services_shared = NULL;
  }

  ss = NULL;
  services = NULL;
}

bool OSLShaderManager::osl_compile(const string &inputfile, const string &outputfile)
{
  vector<string> options;
  string stdosl_path;
  string shader_path = path_get("shader");

  /* specify output file name */
  options.push_back("-o");
  options.push_back(outputfile);

  /* specify standard include path */
  string include_path_arg = string("-I") + shader_path;
  options.push_back(include_path_arg);

  stdosl_path = path_join(shader_path, "stdcycles.h");

  /* compile */
  OSL::OSLCompiler *compiler = new OSL::OSLCompiler(&OSL::ErrorHandler::default_handler());
  bool ok = compiler->compile(string_view(inputfile), options, string_view(stdosl_path));
  delete compiler;

  return ok;
}

bool OSLShaderManager::osl_query(OSL::OSLQuery &query, const string &filepath)
{
  string searchpath = path_user_get("shaders");
  return query.open(filepath, searchpath);
}

static string shader_filepath_hash(const string &filepath, uint64_t modified_time)
{
  /* compute a hash from filepath and modified time to detect changes */
  MD5Hash md5;
  md5.append((const uint8_t *)filepath.c_str(), filepath.size());
  md5.append((const uint8_t *)&modified_time, sizeof(modified_time));

  return md5.get_hex();
}

const char *OSLShaderManager::shader_test_loaded(const string &hash)
{
  map<string, OSLShaderInfo>::iterator it = loaded_shaders.find(hash);
  return (it == loaded_shaders.end()) ? NULL : it->first.c_str();
}

OSLShaderInfo *OSLShaderManager::shader_loaded_info(const string &hash)
{
  map<string, OSLShaderInfo>::iterator it = loaded_shaders.find(hash);
  return (it == loaded_shaders.end()) ? NULL : &it->second;
}

const char *OSLShaderManager::shader_load_filepath(string filepath)
{
  size_t len = filepath.size();
  string extension = filepath.substr(len - 4);
  uint64_t modified_time = path_modified_time(filepath);

  if (extension == ".osl") {
    /* .OSL File */
    string osopath = filepath.substr(0, len - 4) + ".oso";
    uint64_t oso_modified_time = path_modified_time(osopath);

    /* test if we have loaded the corresponding .OSO already */
    if (oso_modified_time != 0) {
      const char *hash = shader_test_loaded(shader_filepath_hash(osopath, oso_modified_time));

      if (hash)
        return hash;
    }

    /* Auto-compile .OSL to .OSO if needed. */
    if (oso_modified_time == 0 || (oso_modified_time < modified_time)) {
      OSLShaderManager::osl_compile(filepath, osopath);
      modified_time = path_modified_time(osopath);
    }
    else
      modified_time = oso_modified_time;

    filepath = osopath;
  }
  else {
    if (extension == ".oso") {
      /* .OSO File, nothing to do */
    }
    else if (path_dirname(filepath) == "") {
      /* .OSO File in search path */
      filepath = path_join(path_user_get("shaders"), filepath + ".oso");
    }
    else {
      /* unknown file */
      return NULL;
    }

    /* test if we have loaded this .OSO already */
    const char *hash = shader_test_loaded(shader_filepath_hash(filepath, modified_time));

    if (hash)
      return hash;
  }

  /* read oso bytecode from file */
  string bytecode_hash = shader_filepath_hash(filepath, modified_time);
  string bytecode;

  if (!path_read_text(filepath, bytecode)) {
    fprintf(stderr, "Cycles shader graph: failed to read file %s\n", filepath.c_str());
    OSLShaderInfo info;
    loaded_shaders[bytecode_hash] = info; /* to avoid repeat tries */
    return NULL;
  }

  return shader_load_bytecode(bytecode_hash, bytecode);
}

const char *OSLShaderManager::shader_load_bytecode(const string &hash, const string &bytecode)
{
  ss->LoadMemoryCompiledShader(hash.c_str(), bytecode.c_str());

  OSLShaderInfo info;

  if (!info.query.open_bytecode(bytecode)) {
    fprintf(stderr, "OSL query error: %s\n", info.query.geterror().c_str());
  }

  /* this is a bit weak, but works */
  info.has_surface_emission = (bytecode.find("\"emission\"") != string::npos);
  info.has_surface_transparent = (bytecode.find("\"transparent\"") != string::npos);
  info.has_surface_bssrdf = (bytecode.find("\"bssrdf\"") != string::npos);

  loaded_shaders[hash] = info;

  return loaded_shaders.find(hash)->first.c_str();
}

/* This is a static function to avoid RTTI link errors with only this
 * file being compiled without RTTI to match OSL and LLVM libraries. */
OSLNode *OSLShaderManager::osl_node(ShaderGraph *graph,
                                    ShaderManager *manager,
                                    const std::string &filepath,
                                    const std::string &bytecode_hash,
                                    const std::string &bytecode)
{
  if (!manager->use_osl()) {
    return NULL;
  }

  /* create query */
  OSLShaderManager *osl_manager = static_cast<OSLShaderManager *>(manager);
  const char *hash;

  if (!filepath.empty()) {
    hash = osl_manager->shader_load_filepath(filepath);
  }
  else {
    hash = osl_manager->shader_test_loaded(bytecode_hash);
    if (!hash)
      hash = osl_manager->shader_load_bytecode(bytecode_hash, bytecode);
  }

  if (!hash) {
    return NULL;
  }

  OSLShaderInfo *info = osl_manager->shader_loaded_info(hash);

  /* count number of inputs */
  size_t num_inputs = 0;

  for (int i = 0; i < info->query.nparams(); i++) {
    const OSL::OSLQuery::Parameter *param = info->query.getparam(i);

    /* skip unsupported types */
    if (param->varlenarray || param->isstruct || param->type.arraylen > 1)
      continue;

    if (!param->isoutput)
      num_inputs++;
  }

  /* create node */
  OSLNode *node = OSLNode::create(graph, num_inputs);

  /* add new sockets from parameters */
  set<void *> used_sockets;

  for (int i = 0; i < info->query.nparams(); i++) {
    const OSL::OSLQuery::Parameter *param = info->query.getparam(i);

    /* skip unsupported types */
    if (param->varlenarray || param->isstruct || param->type.arraylen > 1)
      continue;

    SocketType::Type socket_type;

    if (param->isclosure) {
      socket_type = SocketType::CLOSURE;
    }
    else if (param->type.vecsemantics != TypeDesc::NOSEMANTICS) {
      if (param->type.vecsemantics == TypeDesc::COLOR)
        socket_type = SocketType::COLOR;
      else if (param->type.vecsemantics == TypeDesc::POINT)
        socket_type = SocketType::POINT;
      else if (param->type.vecsemantics == TypeDesc::VECTOR)
        socket_type = SocketType::VECTOR;
      else if (param->type.vecsemantics == TypeDesc::NORMAL)
        socket_type = SocketType::NORMAL;
      else
        continue;

      if (!param->isoutput && param->validdefault) {
        float3 *default_value = (float3 *)node->input_default_value();
        default_value->x = param->fdefault[0];
        default_value->y = param->fdefault[1];
        default_value->z = param->fdefault[2];
      }
    }
    else if (param->type.aggregate == TypeDesc::SCALAR) {
      if (param->type.basetype == TypeDesc::INT) {
        socket_type = SocketType::INT;

        if (!param->isoutput && param->validdefault) {
          *(int *)node->input_default_value() = param->idefault[0];
        }
      }
      else if (param->type.basetype == TypeDesc::FLOAT) {
        socket_type = SocketType::FLOAT;

        if (!param->isoutput && param->validdefault) {
          *(float *)node->input_default_value() = param->fdefault[0];
        }
      }
      else if (param->type.basetype == TypeDesc::STRING) {
        socket_type = SocketType::STRING;

        if (!param->isoutput && param->validdefault) {
          *(ustring *)node->input_default_value() = param->sdefault[0];
        }
      }
      else
        continue;
    }
    else
      continue;

    if (param->isoutput) {
      node->add_output(param->name, socket_type);
    }
    else {
      node->add_input(param->name, socket_type);
    }
  }

  /* Set byte-code hash or file-path. */
  if (!bytecode_hash.empty()) {
    node->bytecode_hash = bytecode_hash;
  }
  else {
    node->filepath = filepath;
  }

  /* Generate inputs and outputs */
  node->create_inputs_outputs(node->type);

  return node;
}

/* Graph Compiler */

OSLCompiler::OSLCompiler(OSLShaderManager *manager,
                         OSLRenderServices *services,
                         OSL::ShadingSystem *ss,
                         Scene *scene)
    : scene(scene), manager(manager), services(services), ss(ss)
{
  current_type = SHADER_TYPE_SURFACE;
  current_shader = NULL;
  background = false;
}

string OSLCompiler::id(ShaderNode *node)
{
  /* assign layer unique name based on pointer address + bump mode */
  stringstream stream;
  stream << "node_" << node->type->name << "_" << node;

  return stream.str();
}

string OSLCompiler::compatible_name(ShaderNode *node, ShaderInput *input)
{
  string sname(input->name().string());
  size_t i;

  /* strip whitespace */
  while ((i = sname.find(" ")) != string::npos)
    sname.replace(i, 1, "");

  /* if output exists with the same name, add "In" suffix */
  foreach (ShaderOutput *output, node->outputs) {
    if (input->name() == output->name()) {
      sname += "In";
      break;
    }
  }

  return sname;
}

string OSLCompiler::compatible_name(ShaderNode *node, ShaderOutput *output)
{
  string sname(output->name().string());
  size_t i;

  /* strip whitespace */
  while ((i = sname.find(" ")) != string::npos)
    sname.replace(i, 1, "");

  /* if input exists with the same name, add "Out" suffix */
  foreach (ShaderInput *input, node->inputs) {
    if (input->name() == output->name()) {
      sname += "Out";
      break;
    }
  }

  return sname;
}

bool OSLCompiler::node_skip_input(ShaderNode *node, ShaderInput *input)
{
  /* exception for output node, only one input is actually used
   * depending on the current shader type */

  if (input->flags() & SocketType::SVM_INTERNAL)
    return true;

  if (node->special_type == SHADER_SPECIAL_TYPE_OUTPUT) {
    if (input->name() == "Surface" && current_type != SHADER_TYPE_SURFACE)
      return true;
    if (input->name() == "Volume" && current_type != SHADER_TYPE_VOLUME)
      return true;
    if (input->name() == "Displacement" && current_type != SHADER_TYPE_DISPLACEMENT)
      return true;
    if (input->name() == "Normal" && current_type != SHADER_TYPE_BUMP)
      return true;
  }
  else if (node->special_type == SHADER_SPECIAL_TYPE_BUMP) {
    if (input->name() == "Height")
      return true;
  }
  else if (current_type == SHADER_TYPE_DISPLACEMENT && input->link &&
           input->link->parent->special_type == SHADER_SPECIAL_TYPE_BUMP)
    return true;

  return false;
}

void OSLCompiler::add(ShaderNode *node, const char *name, bool isfilepath)
{
  /* load filepath */
  if (isfilepath) {
    name = manager->shader_load_filepath(name);

    if (name == NULL)
      return;
  }

  /* pass in fixed parameter values */
  foreach (ShaderInput *input, node->inputs) {
    if (!input->link) {
      /* checks to untangle graphs */
      if (node_skip_input(node, input))
        continue;

      string param_name = compatible_name(node, input);
      const SocketType &socket = input->socket_type;
      switch (input->type()) {
        case SocketType::COLOR:
          parameter_color(param_name.c_str(), node->get_float3(socket));
          break;
        case SocketType::POINT:
          parameter_point(param_name.c_str(), node->get_float3(socket));
          break;
        case SocketType::VECTOR:
          parameter_vector(param_name.c_str(), node->get_float3(socket));
          break;
        case SocketType::NORMAL:
          parameter_normal(param_name.c_str(), node->get_float3(socket));
          break;
        case SocketType::FLOAT:
          parameter(param_name.c_str(), node->get_float(socket));
          break;
        case SocketType::INT:
          parameter(param_name.c_str(), node->get_int(socket));
          break;
        case SocketType::STRING:
          parameter(param_name.c_str(), node->get_string(socket));
          break;
        case SocketType::CLOSURE:
        case SocketType::UNDEFINED:
        default:
          break;
      }
    }
  }

  /* Create shader of the appropriate type. OSL only distinguishes between "surface"
   * and "displacement" at the moment. */
  if (current_type == SHADER_TYPE_SURFACE)
    ss->Shader("surface", name, id(node).c_str());
  else if (current_type == SHADER_TYPE_VOLUME)
    ss->Shader("surface", name, id(node).c_str());
  else if (current_type == SHADER_TYPE_DISPLACEMENT)
    ss->Shader("displacement", name, id(node).c_str());
  else if (current_type == SHADER_TYPE_BUMP)
    ss->Shader("displacement", name, id(node).c_str());
  else
    assert(0);

  /* link inputs to other nodes */
  foreach (ShaderInput *input, node->inputs) {
    if (input->link) {
      if (node_skip_input(node, input))
        continue;

      /* connect shaders */
      string id_from = id(input->link->parent);
      string id_to = id(node);
      string param_from = compatible_name(input->link->parent, input->link);
      string param_to = compatible_name(node, input);

      ss->ConnectShaders(id_from.c_str(), param_from.c_str(), id_to.c_str(), param_to.c_str());
    }
  }

  /* test if we shader contains specific closures */
  OSLShaderInfo *info = manager->shader_loaded_info(name);

  if (current_type == SHADER_TYPE_SURFACE) {
    if (info) {
      if (info->has_surface_emission)
        current_shader->has_surface_emission = true;
      if (info->has_surface_transparent)
        current_shader->has_surface_transparent = true;
      if (info->has_surface_bssrdf) {
        current_shader->has_surface_bssrdf = true;
        current_shader->has_bssrdf_bump = true; /* can't detect yet */
      }
      current_shader->has_bump = true;             /* can't detect yet */
      current_shader->has_surface_raytrace = true; /* can't detect yet */
    }

    if (node->has_spatial_varying()) {
      current_shader->has_surface_spatial_varying = true;
    }
  }
  else if (current_type == SHADER_TYPE_VOLUME) {
    if (node->has_spatial_varying())
      current_shader->has_volume_spatial_varying = true;
    if (node->has_attribute_dependency())
      current_shader->has_volume_attribute_dependency = true;
  }

  if (node->has_integrator_dependency()) {
    current_shader->has_integrator_dependency = true;
  }
}

static TypeDesc array_typedesc(TypeDesc typedesc, int arraylength)
{
  return TypeDesc((TypeDesc::BASETYPE)typedesc.basetype,
                  (TypeDesc::AGGREGATE)typedesc.aggregate,
                  (TypeDesc::VECSEMANTICS)typedesc.vecsemantics,
                  arraylength);
}

void OSLCompiler::parameter(ShaderNode *node, const char *name)
{
  ustring uname = ustring(name);
  const SocketType &socket = *(node->type->find_input(uname));

  switch (socket.type) {
    case SocketType::BOOLEAN: {
      int value = node->get_bool(socket);
      ss->Parameter(name, TypeDesc::TypeInt, &value);
      break;
    }
    case SocketType::FLOAT: {
      float value = node->get_float(socket);
      ss->Parameter(uname, TypeDesc::TypeFloat, &value);
      break;
    }
    case SocketType::INT: {
      int value = node->get_int(socket);
      ss->Parameter(uname, TypeDesc::TypeInt, &value);
      break;
    }
    case SocketType::COLOR: {
      float3 value = node->get_float3(socket);
      ss->Parameter(uname, TypeDesc::TypeColor, &value);
      break;
    }
    case SocketType::VECTOR: {
      float3 value = node->get_float3(socket);
      ss->Parameter(uname, TypeDesc::TypeVector, &value);
      break;
    }
    case SocketType::POINT: {
      float3 value = node->get_float3(socket);
      ss->Parameter(uname, TypeDesc::TypePoint, &value);
      break;
    }
    case SocketType::NORMAL: {
      float3 value = node->get_float3(socket);
      ss->Parameter(uname, TypeDesc::TypeNormal, &value);
      break;
    }
    case SocketType::POINT2: {
      float2 value = node->get_float2(socket);
      ss->Parameter(uname, TypeDesc(TypeDesc::FLOAT, TypeDesc::VEC2, TypeDesc::POINT), &value);
      break;
    }
    case SocketType::STRING: {
      ustring value = node->get_string(socket);
      ss->Parameter(uname, TypeDesc::TypeString, &value);
      break;
    }
    case SocketType::ENUM: {
      ustring value = node->get_string(socket);
      ss->Parameter(uname, TypeDesc::TypeString, &value);
      break;
    }
    case SocketType::TRANSFORM: {
      Transform value = node->get_transform(socket);
      ProjectionTransform projection(value);
      projection = projection_transpose(projection);
      ss->Parameter(uname, TypeDesc::TypeMatrix, &projection);
      break;
    }
    case SocketType::BOOLEAN_ARRAY: {
      // OSL does not support booleans, so convert to int
      const array<bool> &value = node->get_bool_array(socket);
      array<int> intvalue(value.size());
      for (size_t i = 0; i < value.size(); i++)
        intvalue[i] = value[i];
      ss->Parameter(uname, array_typedesc(TypeDesc::TypeInt, value.size()), intvalue.data());
      break;
    }
    case SocketType::FLOAT_ARRAY: {
      const array<float> &value = node->get_float_array(socket);
      ss->Parameter(uname, array_typedesc(TypeDesc::TypeFloat, value.size()), value.data());
      break;
    }
    case SocketType::INT_ARRAY: {
      const array<int> &value = node->get_int_array(socket);
      ss->Parameter(uname, array_typedesc(TypeDesc::TypeInt, value.size()), value.data());
      break;
    }
    case SocketType::COLOR_ARRAY:
    case SocketType::VECTOR_ARRAY:
    case SocketType::POINT_ARRAY:
    case SocketType::NORMAL_ARRAY: {
      TypeDesc typedesc;

      switch (socket.type) {
        case SocketType::COLOR_ARRAY:
          typedesc = TypeDesc::TypeColor;
          break;
        case SocketType::VECTOR_ARRAY:
          typedesc = TypeDesc::TypeVector;
          break;
        case SocketType::POINT_ARRAY:
          typedesc = TypeDesc::TypePoint;
          break;
        case SocketType::NORMAL_ARRAY:
          typedesc = TypeDesc::TypeNormal;
          break;
        default:
          assert(0);
          break;
      }

      // convert to tightly packed array since float3 has padding
      const array<float3> &value = node->get_float3_array(socket);
      array<float> fvalue(value.size() * 3);
      for (size_t i = 0, j = 0; i < value.size(); i++) {
        fvalue[j++] = value[i].x;
        fvalue[j++] = value[i].y;
        fvalue[j++] = value[i].z;
      }

      ss->Parameter(uname, array_typedesc(typedesc, value.size()), fvalue.data());
      break;
    }
    case SocketType::POINT2_ARRAY: {
      const array<float2> &value = node->get_float2_array(socket);
      ss->Parameter(
          uname,
          array_typedesc(TypeDesc(TypeDesc::FLOAT, TypeDesc::VEC2, TypeDesc::POINT), value.size()),
          value.data());
      break;
    }
    case SocketType::STRING_ARRAY: {
      const array<ustring> &value = node->get_string_array(socket);
      ss->Parameter(uname, array_typedesc(TypeDesc::TypeString, value.size()), value.data());
      break;
    }
    case SocketType::TRANSFORM_ARRAY: {
      const array<Transform> &value = node->get_transform_array(socket);
      array<ProjectionTransform> fvalue(value.size());
      for (size_t i = 0; i < value.size(); i++) {
        fvalue[i] = projection_transpose(ProjectionTransform(value[i]));
      }
      ss->Parameter(uname, array_typedesc(TypeDesc::TypeMatrix, fvalue.size()), fvalue.data());
      break;
    }
    case SocketType::CLOSURE:
    case SocketType::NODE:
    case SocketType::NODE_ARRAY:
    case SocketType::UNDEFINED:
    case SocketType::UINT: {
      assert(0);
      break;
    }
  }
}

void OSLCompiler::parameter(const char *name, float f)
{
  ss->Parameter(name, TypeDesc::TypeFloat, &f);
}

void OSLCompiler::parameter_color(const char *name, float3 f)
{
  ss->Parameter(name, TypeDesc::TypeColor, &f);
}

void OSLCompiler::parameter_point(const char *name, float3 f)
{
  ss->Parameter(name, TypeDesc::TypePoint, &f);
}

void OSLCompiler::parameter_normal(const char *name, float3 f)
{
  ss->Parameter(name, TypeDesc::TypeNormal, &f);
}

void OSLCompiler::parameter_vector(const char *name, float3 f)
{
  ss->Parameter(name, TypeDesc::TypeVector, &f);
}

void OSLCompiler::parameter(const char *name, int f)
{
  ss->Parameter(name, TypeDesc::TypeInt, &f);
}

void OSLCompiler::parameter(const char *name, const char *s)
{
  ss->Parameter(name, TypeDesc::TypeString, &s);
}

void OSLCompiler::parameter(const char *name, ustring s)
{
  const char *str = s.c_str();
  ss->Parameter(name, TypeDesc::TypeString, &str);
}

void OSLCompiler::parameter(const char *name, const Transform &tfm)
{
  ProjectionTransform projection(tfm);
  projection = projection_transpose(projection);
  ss->Parameter(name, TypeDesc::TypeMatrix, (float *)&projection);
}

void OSLCompiler::parameter_array(const char *name, const float f[], int arraylen)
{
  TypeDesc type = TypeDesc::TypeFloat;
  type.arraylen = arraylen;
  ss->Parameter(name, type, f);
}

void OSLCompiler::parameter_color_array(const char *name, const array<float3> &f)
{
  /* NOTE: cycles float3 type is actually 4 floats! need to use an explicit array. */
  array<float[3]> table(f.size());

  for (int i = 0; i < f.size(); ++i) {
    table[i][0] = f[i].x;
    table[i][1] = f[i].y;
    table[i][2] = f[i].z;
  }

  TypeDesc type = TypeDesc::TypeColor;
  type.arraylen = table.size();
  ss->Parameter(name, type, table.data());
}

void OSLCompiler::parameter_attribute(const char *name, ustring s)
{
  if (Attribute::name_standard(s.c_str()))
    parameter(name, (string("geom:") + s.c_str()).c_str());
  else
    parameter(name, s.c_str());
}

void OSLCompiler::find_dependencies(ShaderNodeSet &dependencies, ShaderInput *input)
{
  ShaderNode *node = (input->link) ? input->link->parent : NULL;

  if (node != NULL && dependencies.find(node) == dependencies.end()) {
    foreach (ShaderInput *in, node->inputs)
      if (!node_skip_input(node, in))
        find_dependencies(dependencies, in);

    dependencies.insert(node);
  }
}

void OSLCompiler::generate_nodes(const ShaderNodeSet &nodes)
{
  ShaderNodeSet done;
  bool nodes_done;

  do {
    nodes_done = true;

    foreach (ShaderNode *node, nodes) {
      if (done.find(node) == done.end()) {
        bool inputs_done = true;

        foreach (ShaderInput *input, node->inputs)
          if (!node_skip_input(node, input))
            if (input->link && done.find(input->link->parent) == done.end())
              inputs_done = false;

        if (inputs_done) {
          node->compile(*this);
          done.insert(node);

          if (current_type == SHADER_TYPE_SURFACE) {
            if (node->has_surface_emission())
              current_shader->has_surface_emission = true;
            if (node->has_surface_transparent())
              current_shader->has_surface_transparent = true;
            if (node->get_feature() & KERNEL_FEATURE_NODE_RAYTRACE)
              current_shader->has_surface_raytrace = true;
            if (node->has_spatial_varying())
              current_shader->has_surface_spatial_varying = true;
            if (node->has_surface_bssrdf()) {
              current_shader->has_surface_bssrdf = true;
              if (node->has_bssrdf_bump())
                current_shader->has_bssrdf_bump = true;
            }
            if (node->has_bump()) {
              current_shader->has_bump = true;
            }
          }
          else if (current_type == SHADER_TYPE_VOLUME) {
            if (node->has_spatial_varying())
              current_shader->has_volume_spatial_varying = true;
          }
        }
        else
          nodes_done = false;
      }
    }
  } while (!nodes_done);
}

OSL::ShaderGroupRef OSLCompiler::compile_type(Shader *shader, ShaderGraph *graph, ShaderType type)
{
  current_type = type;

  OSL::ShaderGroupRef group = ss->ShaderGroupBegin(shader->name.c_str());

  ShaderNode *output = graph->output();
  ShaderNodeSet dependencies;

  if (type == SHADER_TYPE_SURFACE) {
    /* generate surface shader */
    find_dependencies(dependencies, output->input("Surface"));
    generate_nodes(dependencies);
    output->compile(*this);
  }
  else if (type == SHADER_TYPE_BUMP) {
    /* generate bump shader */
    find_dependencies(dependencies, output->input("Normal"));
    generate_nodes(dependencies);
    output->compile(*this);
  }
  else if (type == SHADER_TYPE_VOLUME) {
    /* generate volume shader */
    find_dependencies(dependencies, output->input("Volume"));
    generate_nodes(dependencies);
    output->compile(*this);
  }
  else if (type == SHADER_TYPE_DISPLACEMENT) {
    /* generate displacement shader */
    find_dependencies(dependencies, output->input("Displacement"));
    generate_nodes(dependencies);
    output->compile(*this);
  }
  else
    assert(0);

  ss->ShaderGroupEnd();

  return group;
}

void OSLCompiler::compile(Shader *shader)
{
  if (shader->is_modified()) {
    ShaderGraph *graph = shader->graph;
    ShaderNode *output = (graph) ? graph->output() : NULL;

    bool has_bump = (shader->get_displacement_method() != DISPLACE_TRUE) &&
                    output->input("Surface")->link && output->input("Displacement")->link;

    /* finalize */
    shader->graph->finalize(scene,
                            has_bump,
                            shader->has_integrator_dependency,
                            shader->get_displacement_method() == DISPLACE_BOTH);

    current_shader = shader;

    shader->has_surface = false;
    shader->has_surface_emission = false;
    shader->has_surface_transparent = false;
    shader->has_surface_bssrdf = false;
    shader->has_bump = has_bump;
    shader->has_bssrdf_bump = has_bump;
    shader->has_volume = false;
    shader->has_displacement = false;
    shader->has_surface_spatial_varying = false;
    shader->has_volume_spatial_varying = false;
    shader->has_volume_attribute_dependency = false;
    shader->has_integrator_dependency = false;

    /* generate surface shader */
    if (shader->reference_count() && graph && output->input("Surface")->link) {
      shader->osl_surface_ref = compile_type(shader, shader->graph, SHADER_TYPE_SURFACE);

      if (has_bump)
        shader->osl_surface_bump_ref = compile_type(shader, shader->graph, SHADER_TYPE_BUMP);
      else
        shader->osl_surface_bump_ref = OSL::ShaderGroupRef();

      shader->has_surface = true;
    }
    else {
      shader->osl_surface_ref = OSL::ShaderGroupRef();
      shader->osl_surface_bump_ref = OSL::ShaderGroupRef();
    }

    /* generate volume shader */
    if (shader->reference_count() && graph && output->input("Volume")->link) {
      shader->osl_volume_ref = compile_type(shader, shader->graph, SHADER_TYPE_VOLUME);
      shader->has_volume = true;
    }
    else
      shader->osl_volume_ref = OSL::ShaderGroupRef();

    /* generate displacement shader */
    if (shader->reference_count() && graph && output->input("Displacement")->link) {
      shader->osl_displacement_ref = compile_type(shader, shader->graph, SHADER_TYPE_DISPLACEMENT);
      shader->has_displacement = true;
    }
    else
      shader->osl_displacement_ref = OSL::ShaderGroupRef();
  }
}

void OSLCompiler::parameter_texture(const char *name, ustring filename, ustring colorspace)
{
  /* Textured loaded through the OpenImageIO texture cache. For this
   * case we need to do runtime color space conversion. */
  OSLTextureHandle *handle = new OSLTextureHandle(OSLTextureHandle::OIIO);
  handle->processor = ColorSpaceManager::get_processor(colorspace);
  services->textures.insert(filename, handle);
  parameter(name, filename);
}

void OSLCompiler::parameter_texture(const char *name, int svm_slot)
{
  /* Texture loaded through SVM image texture system. We generate a unique
   * name, which ends up being used in OSLRenderServices::get_texture_handle
   * to get handle again. Note that this name must be unique between multiple
   * render sessions as the render services are shared. */
  ustring filename(string_printf("@svm%d", texture_shared_unique_id++).c_str());
  services->textures.insert(filename, new OSLTextureHandle(OSLTextureHandle::SVM, svm_slot));
  parameter(name, filename);
}

void OSLCompiler::parameter_texture_ies(const char *name, int svm_slot)
{
  /* IES light textures stored in SVM. */
  ustring filename(string_printf("@svm%d", texture_shared_unique_id++).c_str());
  services->textures.insert(filename, new OSLTextureHandle(OSLTextureHandle::IES, svm_slot));
  parameter(name, filename);
}

#else

void OSLCompiler::add(ShaderNode * /*node*/, const char * /*name*/, bool /*isfilepath*/)
{
}

void OSLCompiler::parameter(ShaderNode * /*node*/, const char * /*name*/)
{
}

void OSLCompiler::parameter(const char * /*name*/, float /*f*/)
{
}

void OSLCompiler::parameter_color(const char * /*name*/, float3 /*f*/)
{
}

void OSLCompiler::parameter_vector(const char * /*name*/, float3 /*f*/)
{
}

void OSLCompiler::parameter_point(const char * /*name*/, float3 /*f*/)
{
}

void OSLCompiler::parameter_normal(const char * /*name*/, float3 /*f*/)
{
}

void OSLCompiler::parameter(const char * /*name*/, int /*f*/)
{
}

void OSLCompiler::parameter(const char * /*name*/, const char * /*s*/)
{
}

void OSLCompiler::parameter(const char * /*name*/, ustring /*s*/)
{
}

void OSLCompiler::parameter(const char * /*name*/, const Transform & /*tfm*/)
{
}

void OSLCompiler::parameter_array(const char * /*name*/, const float /*f*/[], int /*arraylen*/)
{
}

void OSLCompiler::parameter_color_array(const char * /*name*/, const array<float3> & /*f*/)
{
}

void OSLCompiler::parameter_texture(const char * /* name */,
                                    ustring /* filename */,
                                    ustring /* colorspace */)
{
}

void OSLCompiler::parameter_texture(const char * /* name */, int /* svm_slot */)
{
}

void OSLCompiler::parameter_texture_ies(const char * /* name */, int /* svm_slot */)
{
}

#endif /* WITH_OSL */

CCL_NAMESPACE_END
