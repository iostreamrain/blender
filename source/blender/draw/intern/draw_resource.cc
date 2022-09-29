/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2022 Blender Foundation. */

/** \file
 * \ingroup draw
 */

#include "DNA_particle_types.h"
#include "RNA_access.h"
#include "RNA_path.h"
#include "RNA_types.h"

#include "draw_handle.hh"
#include "draw_manager.hh"
#include "draw_shader_shared.h"

/* -------------------------------------------------------------------- */
/** \name ObjectAttributes
 * \{ */

/**
 * Extract object attribute from RNA property.
 * Returns true if the attribute was correctly extracted.
 * This function mirrors lookup_property in cycles/blender/object.cpp
 */
static bool ptr_property_lookup(PointerRNA *id_ptr, const char *name, float *r_value)
{
  PointerRNA ptr;
  PropertyRNA *prop;

  if (id_ptr->data == nullptr) {
    return false;
  }

  if (!RNA_path_resolve(id_ptr, name, &ptr, &prop)) {
    return false;
  }

  if (prop == nullptr) {
    return false;
  }

  PropertyType type = RNA_property_type(prop);
  int array_len = RNA_property_array_length(&ptr, prop);

  if (array_len == 0) {
    float value;

    if (type == PROP_FLOAT) {
      value = RNA_property_float_get(&ptr, prop);
    }
    else if (type == PROP_INT) {
      value = static_cast<float>(RNA_property_int_get(&ptr, prop));
    }
    else {
      return false;
    }

    *reinterpret_cast<float4 *>(r_value) = float4(value, value, value, 1.0f);
    return true;
  }

  if (type == PROP_FLOAT && array_len <= 4) {
    *reinterpret_cast<float4 *>(r_value) = float4(0.0f, 0.0f, 0.0f, 1.0f);
    RNA_property_float_get_array(&ptr, prop, r_value);
    return true;
  }
  return false;
}

bool ObjectAttribute::id_property_lookup(ID *id, const char *name)
{
  PointerRNA id_ptr;

  RNA_id_pointer_create(id, &id_ptr);

  return ptr_property_lookup(&id_ptr, name, &data_x);
}

/**
 * Go through all possible source of the given object uniform attribute.
 * Returns true if the attribute was correctly filled.
 * This function mirrors lookup_instance_property in cycles/blender/blender_object.cpp
 */
bool ObjectAttribute::sync(const blender::draw::ObjectRef &ref, const GPUUniformAttr &attr)
{
  hash_code = attr.hash_code;

  /* If requesting instance data, check the parent particle system and object. */
  if (attr.use_dupli) {
    if ((ref.dupli_object != nullptr) &&
        object_dupli_lookup_attribute(ref.dupli_object, attr.name, &data_x)) {
      return true;
    }
    if ((ref.dupli_object != nullptr) && (ref.dupli_object->particle_system != nullptr)) {
      ParticleSettings *settings = ref.dupli_object->particle_system->part;
      if (this->id_property_lookup((ID *)settings, attr.name_id_prop) ||
          this->id_property_lookup((ID *)settings, attr.name)) {
        return true;
      }
    }
    if (this->id_property_lookup((ID *)ref.dupli_parent, attr.name_id_prop) ||
        this->id_property_lookup((ID *)ref.dupli_parent, attr.name)) {
      return true;
    }
  }

  /* Check the object and mesh. */
  if (ref.object != nullptr) {
    if (this->id_property_lookup((ID *)ref.object, attr.name_id_prop) ||
        this->id_property_lookup((ID *)ref.object, attr.name) ||
        this->id_property_lookup((ID *)ref.object->data, attr.name_id_prop) ||
        this->id_property_lookup((ID *)ref.object->data, attr.name)) {
      return true;
    }
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name LayerAttributes
 * \{ */

bool LayerAttribute::sync(PointerRNA *scene,
                          PointerRNA *layer,
                          PointerRNA *world,
                          const GPULayerAttr &attr)
{
  hash_code = attr.hash_code;

  return ptr_property_lookup(layer, attr.name_id_prop, &data.x) ||
         ptr_property_lookup(layer, attr.name, &data.x) ||
         ptr_property_lookup(scene, attr.name_id_prop, &data.x) ||
         ptr_property_lookup(scene, attr.name, &data.x) ||
         ptr_property_lookup(world, attr.name_id_prop, &data.x) ||
         ptr_property_lookup(world, attr.name, &data.x);
}

/** \} */
