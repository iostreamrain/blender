/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2020 Blender Foundation. All rights reserved. */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_edgehash.h"
#include "BLI_math.h"
#include "BLI_task.h"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_object_types.h"

#include "BKE_brush.h"
#include "BKE_ccg.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_multires.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_scene.h"

#include "paint_intern.h"
#include "sculpt_intern.h"

#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_matrix.h"
#include "GPU_state.h"

#include "bmesh.h"

#include <math.h>
#include <stdlib.h>

#define BOUNDARY_VERTEX_NONE -1
#define BOUNDARY_STEPS_NONE -1

typedef struct BoundaryInitialVertexFloodFillData {
  PBVHVertRef initial_vertex;
  int initial_vertex_i;
  int boundary_initial_vertex_steps;
  PBVHVertRef boundary_initial_vertex;
  int boundary_initial_vertex_i;
  int *floodfill_steps;
  float radius_sq;
} BoundaryInitialVertexFloodFillData;

static bool boundary_initial_vertex_floodfill_cb(
    SculptSession *ss, PBVHVertRef from_v, PBVHVertRef to_v, bool is_duplicate, void *userdata)
{
  BoundaryInitialVertexFloodFillData *data = userdata;

  int from_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, from_v);
  int to_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, to_v);

  if (!SCULPT_vertex_visible_get(ss, to_v)) {
    return false;
  }

  if (!is_duplicate) {
    data->floodfill_steps[to_v_i] = data->floodfill_steps[from_v_i] + 1;
  }
  else {
    data->floodfill_steps[to_v_i] = data->floodfill_steps[from_v_i];
  }

  if (SCULPT_vertex_is_boundary(ss, to_v)) {
    if (data->floodfill_steps[to_v_i] < data->boundary_initial_vertex_steps) {
      data->boundary_initial_vertex_steps = data->floodfill_steps[to_v_i];
      data->boundary_initial_vertex_i = to_v_i;
      data->boundary_initial_vertex = to_v;
    }
  }

  const float len_sq = len_squared_v3v3(SCULPT_vertex_co_get(ss, data->initial_vertex),
                                        SCULPT_vertex_co_get(ss, to_v));
  return len_sq < data->radius_sq;
}

/* From a vertex index anywhere in the mesh, returns the closest vertex in a mesh boundary inside
 * the given radius, if it exists. */
static PBVHVertRef sculpt_boundary_get_closest_boundary_vertex(SculptSession *ss,
                                                               const PBVHVertRef initial_vertex,
                                                               const float radius)
{

  if (SCULPT_vertex_is_boundary(ss, initial_vertex)) {
    return initial_vertex;
  }

  SculptFloodFill flood;
  SCULPT_floodfill_init(ss, &flood);
  SCULPT_floodfill_add_initial(&flood, initial_vertex);

  BoundaryInitialVertexFloodFillData fdata = {
      .initial_vertex = initial_vertex,
      .boundary_initial_vertex = {BOUNDARY_VERTEX_NONE},
      .boundary_initial_vertex_steps = INT_MAX,
      .radius_sq = radius * radius,
  };

  fdata.floodfill_steps = MEM_calloc_arrayN(
      SCULPT_vertex_count_get(ss), sizeof(int), "floodfill steps");

  SCULPT_floodfill_execute(ss, &flood, boundary_initial_vertex_floodfill_cb, &fdata);
  SCULPT_floodfill_free(&flood);

  MEM_freeN(fdata.floodfill_steps);
  return fdata.boundary_initial_vertex;
}

/* Used to allocate the memory of the boundary index arrays. This was decided considered the most
 * common use cases for the brush deformers, taking into account how many vertices those
 * deformations usually need in the boundary. */
static int BOUNDARY_INDICES_BLOCK_SIZE = 300;

static void sculpt_boundary_index_add(SculptBoundary *boundary,
                                      const PBVHVertRef new_vertex,
                                      const int new_index,
                                      const float distance,
                                      GSet *included_verts)
{

  boundary->verts[boundary->verts_num] = new_vertex;

  if (boundary->distance) {
    boundary->distance[new_index] = distance;
  }
  if (included_verts) {
    BLI_gset_add(included_verts, POINTER_FROM_INT(new_index));
  }
  boundary->verts_num++;
  if (boundary->verts_num >= boundary->verts_capacity) {
    boundary->verts_capacity += BOUNDARY_INDICES_BLOCK_SIZE;
    boundary->verts = MEM_reallocN_id(
        boundary->verts, boundary->verts_capacity * sizeof(PBVHVertRef), "boundary indices");
  }
};

static void sculpt_boundary_preview_edge_add(SculptBoundary *boundary,
                                             const PBVHVertRef v1,
                                             const PBVHVertRef v2)
{

  boundary->edges[boundary->edges_num].v1 = v1;
  boundary->edges[boundary->edges_num].v2 = v2;
  boundary->edges_num++;

  if (boundary->edges_num >= boundary->edges_capacity) {
    boundary->edges_capacity += BOUNDARY_INDICES_BLOCK_SIZE;
    boundary->edges = MEM_reallocN_id(boundary->edges,
                                      boundary->edges_capacity * sizeof(SculptBoundaryPreviewEdge),
                                      "boundary edges");
  }
};

/**
 * This function is used to check where the propagation should stop when calculating the boundary,
 * as well as to check if the initial vertex is valid.
 */
static bool sculpt_boundary_is_vertex_in_editable_boundary(SculptSession *ss,
                                                           const PBVHVertRef initial_vertex)
{

  if (!SCULPT_vertex_visible_get(ss, initial_vertex)) {
    return false;
  }

  int neighbor_count = 0;
  int boundary_vertex_count = 0;
  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, initial_vertex, ni) {
    if (SCULPT_vertex_visible_get(ss, ni.vertex)) {
      neighbor_count++;
      if (SCULPT_vertex_is_boundary(ss, ni.vertex)) {
        boundary_vertex_count++;
      }
    }
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

  /* Corners are ambiguous as it can't be decide which boundary should be active. The flood fill
   * should also stop at corners. */
  if (neighbor_count <= 2) {
    return false;
  }

  /* Non manifold geometry in the mesh boundary.
   * The deformation result will be unpredictable and not very useful. */
  if (boundary_vertex_count > 2) {
    return false;
  }

  return true;
}

/* Flood fill that adds to the boundary data all the vertices from a boundary and its duplicates.
 */

typedef struct BoundaryFloodFillData {
  SculptBoundary *boundary;
  GSet *included_verts;
  EdgeSet *preview_edges;

  PBVHVertRef last_visited_vertex;

} BoundaryFloodFillData;

static bool boundary_floodfill_cb(
    SculptSession *ss, PBVHVertRef from_v, PBVHVertRef to_v, bool is_duplicate, void *userdata)
{
  int from_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, from_v);
  int to_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, to_v);

  BoundaryFloodFillData *data = userdata;
  SculptBoundary *boundary = data->boundary;
  if (!SCULPT_vertex_is_boundary(ss, to_v)) {
    return false;
  }
  const float edge_len = len_v3v3(SCULPT_vertex_co_get(ss, from_v),
                                  SCULPT_vertex_co_get(ss, to_v));
  const float distance_boundary_to_dst = boundary->distance ?
                                             boundary->distance[from_v_i] + edge_len :
                                             0.0f;
  sculpt_boundary_index_add(
      boundary, to_v, to_v_i, distance_boundary_to_dst, data->included_verts);
  if (!is_duplicate) {
    sculpt_boundary_preview_edge_add(boundary, from_v, to_v);
  }
  return sculpt_boundary_is_vertex_in_editable_boundary(ss, to_v);
}

static void sculpt_boundary_indices_init(SculptSession *ss,
                                         SculptBoundary *boundary,
                                         const bool init_boundary_distances,
                                         const PBVHVertRef initial_boundary_vertex)
{

  const int totvert = SCULPT_vertex_count_get(ss);
  boundary->verts = MEM_malloc_arrayN(
      BOUNDARY_INDICES_BLOCK_SIZE, sizeof(PBVHVertRef), "boundary indices");

  if (init_boundary_distances) {
    boundary->distance = MEM_calloc_arrayN(totvert, sizeof(float), "boundary distances");
  }
  boundary->edges = MEM_malloc_arrayN(
      BOUNDARY_INDICES_BLOCK_SIZE, sizeof(SculptBoundaryPreviewEdge), "boundary edges");

  GSet *included_verts = BLI_gset_int_new_ex("included verts", BOUNDARY_INDICES_BLOCK_SIZE);
  SculptFloodFill flood;
  SCULPT_floodfill_init(ss, &flood);

  int initial_boundary_index = BKE_pbvh_vertex_to_index(ss->pbvh, initial_boundary_vertex);

  boundary->initial_vertex = initial_boundary_vertex;
  boundary->initial_vertex_i = initial_boundary_index;

  copy_v3_v3(boundary->initial_vertex_position,
             SCULPT_vertex_co_get(ss, boundary->initial_vertex));
  sculpt_boundary_index_add(
      boundary, initial_boundary_vertex, initial_boundary_index, 0.0f, included_verts);
  SCULPT_floodfill_add_initial(&flood, boundary->initial_vertex);

  BoundaryFloodFillData fdata = {
      .boundary = boundary,
      .included_verts = included_verts,
      .last_visited_vertex = {BOUNDARY_VERTEX_NONE},

  };

  SCULPT_floodfill_execute(ss, &flood, boundary_floodfill_cb, &fdata);
  SCULPT_floodfill_free(&flood);

  /* Check if the boundary loops into itself and add the extra preview edge to close the loop. */
  if (fdata.last_visited_vertex.i != BOUNDARY_VERTEX_NONE &&
      sculpt_boundary_is_vertex_in_editable_boundary(ss, fdata.last_visited_vertex)) {
    SculptVertexNeighborIter ni;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, fdata.last_visited_vertex, ni) {
      if (BLI_gset_haskey(included_verts, POINTER_FROM_INT(ni.index)) &&
          sculpt_boundary_is_vertex_in_editable_boundary(ss, ni.vertex)) {
        sculpt_boundary_preview_edge_add(boundary, fdata.last_visited_vertex, ni.vertex);
        boundary->forms_loop = true;
      }
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
  }

  BLI_gset_free(included_verts, NULL);
}

/**
 * This functions initializes all data needed to calculate falloffs and deformation from the
 * boundary into the mesh into a #SculptBoundaryEditInfo array. This includes how many steps are
 * needed to go from a boundary vertex to an interior vertex and which vertex of the boundary is
 * the closest one.
 */
static void sculpt_boundary_edit_data_init(SculptSession *ss,
                                           SculptBoundary *boundary,
                                           const PBVHVertRef initial_vertex,
                                           const float radius)
{
  const int totvert = SCULPT_vertex_count_get(ss);

  const bool has_duplicates = BKE_pbvh_type(ss->pbvh) == PBVH_GRIDS;

  boundary->edit_info = MEM_malloc_arrayN(
      totvert, sizeof(SculptBoundaryEditInfo), "Boundary edit info");

  for (int i = 0; i < totvert; i++) {
    boundary->edit_info[i].original_vertex_i = BOUNDARY_VERTEX_NONE;
    boundary->edit_info[i].propagation_steps_num = BOUNDARY_STEPS_NONE;
  }

  GSQueue *current_iteration = BLI_gsqueue_new(sizeof(PBVHVertRef));
  GSQueue *next_iteration = BLI_gsqueue_new(sizeof(PBVHVertRef));

  /* Initialized the first iteration with the vertices already in the boundary. This is propagation
   * step 0. */
  BLI_bitmap *visited_verts = BLI_BITMAP_NEW(SCULPT_vertex_count_get(ss), "visited_verts");
  for (int i = 0; i < boundary->verts_num; i++) {
    int index = BKE_pbvh_vertex_to_index(ss->pbvh, boundary->verts[i]);

    boundary->edit_info[index].original_vertex_i = BKE_pbvh_vertex_to_index(ss->pbvh,
                                                                            boundary->verts[i]);
    boundary->edit_info[index].propagation_steps_num = 0;

    /* This ensures that all duplicate vertices in the boundary have the same original_vertex
     * index, so the deformation for them will be the same. */
    if (has_duplicates) {
      SculptVertexNeighborIter ni_duplis;
      SCULPT_VERTEX_DUPLICATES_AND_NEIGHBORS_ITER_BEGIN (ss, boundary->verts[i], ni_duplis) {
        if (ni_duplis.is_duplicate) {
          boundary->edit_info[ni_duplis.index].original_vertex_i = BKE_pbvh_vertex_to_index(
              ss->pbvh, boundary->verts[i]);
        }
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni_duplis);
    }

    BLI_gsqueue_push(current_iteration, &boundary->verts[i]);
  }

  int propagation_steps_num = 0;
  float accum_distance = 0.0f;

  while (true) {
    /* Stop adding steps to edit info. This happens when a steps is further away from the boundary
     * than the brush radius or when the entire mesh was already processed. */
    if (accum_distance > radius || BLI_gsqueue_is_empty(current_iteration)) {
      boundary->max_propagation_steps = propagation_steps_num;
      break;
    }

    while (!BLI_gsqueue_is_empty(current_iteration)) {
      PBVHVertRef from_v;
      BLI_gsqueue_pop(current_iteration, &from_v);

      int from_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, from_v);

      SculptVertexNeighborIter ni;
      SCULPT_VERTEX_DUPLICATES_AND_NEIGHBORS_ITER_BEGIN (ss, from_v, ni) {
        const bool is_visible = SCULPT_vertex_visible_get(ss, ni.vertex);
        if (!is_visible ||
            boundary->edit_info[ni.index].propagation_steps_num != BOUNDARY_STEPS_NONE) {
          continue;
        }
        boundary->edit_info[ni.index].original_vertex_i =
            boundary->edit_info[from_v_i].original_vertex_i;

        BLI_BITMAP_ENABLE(visited_verts, ni.index);

        if (ni.is_duplicate) {
          /* Grids duplicates handling. */
          boundary->edit_info[ni.index].propagation_steps_num =
              boundary->edit_info[from_v_i].propagation_steps_num;
        }
        else {
          boundary->edit_info[ni.index].propagation_steps_num =
              boundary->edit_info[from_v_i].propagation_steps_num + 1;

          BLI_gsqueue_push(next_iteration, &ni.vertex);

          /* When copying the data to the neighbor for the next iteration, it has to be copied to
           * all its duplicates too. This is because it is not possible to know if the updated
           * neighbor or one if its uninitialized duplicates is going to come first in order to
           * copy the data in the from_v neighbor iterator. */
          if (has_duplicates) {
            SculptVertexNeighborIter ni_duplis;
            SCULPT_VERTEX_DUPLICATES_AND_NEIGHBORS_ITER_BEGIN (ss, ni.vertex, ni_duplis) {
              if (ni_duplis.is_duplicate) {
                boundary->edit_info[ni_duplis.index].original_vertex_i =
                    boundary->edit_info[from_v_i].original_vertex_i;
                boundary->edit_info[ni_duplis.index].propagation_steps_num =
                    boundary->edit_info[from_v_i].propagation_steps_num + 1;
              }
            }
            SCULPT_VERTEX_NEIGHBORS_ITER_END(ni_duplis);
          }

          /* Check the distance using the vertex that was propagated from the initial vertex that
           * was used to initialize the boundary. */
          if (boundary->edit_info[from_v_i].original_vertex_i ==
              BKE_pbvh_vertex_to_index(ss->pbvh, initial_vertex)) {
            boundary->pivot_vertex = ni.vertex;
            copy_v3_v3(boundary->initial_pivot_position, SCULPT_vertex_co_get(ss, ni.vertex));
            accum_distance += len_v3v3(SCULPT_vertex_co_get(ss, from_v),
                                       SCULPT_vertex_co_get(ss, ni.vertex));
          }
        }
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
    }

    /* Copy the new vertices to the queue to be processed in the next iteration. */
    while (!BLI_gsqueue_is_empty(next_iteration)) {
      PBVHVertRef next_v;
      BLI_gsqueue_pop(next_iteration, &next_v);
      BLI_gsqueue_push(current_iteration, &next_v);
    }

    propagation_steps_num++;
  }

  MEM_SAFE_FREE(visited_verts);

  BLI_gsqueue_free(current_iteration);
  BLI_gsqueue_free(next_iteration);
}

/* This functions assigns a falloff factor to each one of the SculptBoundaryEditInfo structs based
 * on the brush curve and its propagation steps. The falloff goes from the boundary into the mesh.
 */
static void sculpt_boundary_falloff_factor_init(SculptSession *ss,
                                                SculptBoundary *boundary,
                                                Brush *brush,
                                                const float radius)
{
  const int totvert = SCULPT_vertex_count_get(ss);
  BKE_curvemapping_init(brush->curve);

  for (int i = 0; i < totvert; i++) {
    if (boundary->edit_info[i].propagation_steps_num != -1) {
      boundary->edit_info[i].strength_factor = BKE_brush_curve_strength(
          brush, boundary->edit_info[i].propagation_steps_num, boundary->max_propagation_steps);
    }

    if (boundary->edit_info[i].original_vertex_i ==
        BKE_pbvh_vertex_to_index(ss->pbvh, boundary->initial_vertex)) {
      /* All vertices that are propagated from the original vertex won't be affected by the
       * boundary falloff, so there is no need to calculate anything else. */
      continue;
    }

    if (!boundary->distance) {
      /* There are falloff modes that do not require to modify the previously calculated falloff
       * based on boundary distances. */
      continue;
    }

    const float boundary_distance = boundary->distance[boundary->edit_info[i].original_vertex_i];
    float falloff_distance = 0.0f;
    float direction = 1.0f;

    switch (brush->boundary_falloff_type) {
      case BRUSH_BOUNDARY_FALLOFF_RADIUS:
        falloff_distance = boundary_distance;
        break;
      case BRUSH_BOUNDARY_FALLOFF_LOOP: {
        const int div = boundary_distance / radius;
        const float mod = fmodf(boundary_distance, radius);
        falloff_distance = div % 2 == 0 ? mod : radius - mod;
      } break;
      case BRUSH_BOUNDARY_FALLOFF_LOOP_INVERT: {
        const int div = boundary_distance / radius;
        const float mod = fmodf(boundary_distance, radius);
        falloff_distance = div % 2 == 0 ? mod : radius - mod;
        /* Inverts the faloff in the intervals 1 2 5 6 9 10 ... */
        if (((div - 1) & 2) == 0) {
          direction = -1.0f;
        }
      } break;
      case BRUSH_BOUNDARY_FALLOFF_CONSTANT:
        /* For constant falloff distances are not allocated, so this should never happen. */
        BLI_assert(false);
    }

    boundary->edit_info[i].strength_factor *= direction * BKE_brush_curve_strength(
                                                              brush, falloff_distance, radius);
  }
}

SculptBoundary *SCULPT_boundary_data_init(Object *object,
                                          Brush *brush,
                                          const PBVHVertRef initial_vertex,
                                          const float radius)
{
  SculptSession *ss = object->sculpt;

  if (initial_vertex.i == PBVH_REF_NONE) {
    return NULL;
  }

  SCULPT_vertex_random_access_ensure(ss);
  SCULPT_boundary_info_ensure(object);

  const PBVHVertRef boundary_initial_vertex = sculpt_boundary_get_closest_boundary_vertex(
      ss, initial_vertex, radius);

  if (boundary_initial_vertex.i == BOUNDARY_VERTEX_NONE) {
    return NULL;
  }

  /* Starting from a vertex that is the limit of a boundary is ambiguous, so return NULL instead of
   * forcing a random active boundary from a corner. */
  if (!sculpt_boundary_is_vertex_in_editable_boundary(ss, initial_vertex)) {
    return NULL;
  }

  SculptBoundary *boundary = MEM_callocN(sizeof(SculptBoundary), "Boundary edit data");

  const bool init_boundary_distances = brush ? brush->boundary_falloff_type !=
                                                   BRUSH_BOUNDARY_FALLOFF_CONSTANT :
                                               false;

  sculpt_boundary_indices_init(ss, boundary, init_boundary_distances, boundary_initial_vertex);

  const float boundary_radius = brush ? radius * (1.0f + brush->boundary_offset) : radius;
  sculpt_boundary_edit_data_init(ss, boundary, boundary_initial_vertex, boundary_radius);

  return boundary;
}

void SCULPT_boundary_data_free(SculptBoundary *boundary)
{
  MEM_SAFE_FREE(boundary->verts);
  MEM_SAFE_FREE(boundary->edges);
  MEM_SAFE_FREE(boundary->distance);
  MEM_SAFE_FREE(boundary->edit_info);
  MEM_SAFE_FREE(boundary->bend.pivot_positions);
  MEM_SAFE_FREE(boundary->bend.pivot_rotation_axis);
  MEM_SAFE_FREE(boundary->slide.directions);
  MEM_SAFE_FREE(boundary);
}

/* These functions initialize the required vectors for the desired deformation using the
 * SculptBoundaryEditInfo. They calculate the data using the vertices that have the
 * max_propagation_steps value and them this data is copied to the rest of the vertices using the
 * original vertex index. */
static void sculpt_boundary_bend_data_init(SculptSession *ss, SculptBoundary *boundary)
{
  const int totvert = SCULPT_vertex_count_get(ss);
  boundary->bend.pivot_rotation_axis = MEM_calloc_arrayN(
      totvert, sizeof(float[3]), "pivot rotation axis");
  boundary->bend.pivot_positions = MEM_calloc_arrayN(totvert, sizeof(float[3]), "pivot positions");

  for (int i = 0; i < totvert; i++) {
    if (boundary->edit_info[i].propagation_steps_num != boundary->max_propagation_steps) {
      continue;
    }

    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    float dir[3];
    float normal[3];
    SCULPT_vertex_normal_get(ss, vertex, normal);
    sub_v3_v3v3(
        dir,
        SCULPT_vertex_co_get(
            ss, BKE_pbvh_index_to_vertex(ss->pbvh, boundary->edit_info[i].original_vertex_i)),
        SCULPT_vertex_co_get(ss, vertex));
    cross_v3_v3v3(
        boundary->bend.pivot_rotation_axis[boundary->edit_info[i].original_vertex_i], dir, normal);
    normalize_v3(boundary->bend.pivot_rotation_axis[boundary->edit_info[i].original_vertex_i]);
    copy_v3_v3(boundary->bend.pivot_positions[boundary->edit_info[i].original_vertex_i],
               SCULPT_vertex_co_get(ss, vertex));
  }

  for (int i = 0; i < totvert; i++) {
    if (boundary->edit_info[i].propagation_steps_num == BOUNDARY_STEPS_NONE) {
      continue;
    }
    copy_v3_v3(boundary->bend.pivot_positions[i],
               boundary->bend.pivot_positions[boundary->edit_info[i].original_vertex_i]);
    copy_v3_v3(boundary->bend.pivot_rotation_axis[i],
               boundary->bend.pivot_rotation_axis[boundary->edit_info[i].original_vertex_i]);
  }
}

static void sculpt_boundary_slide_data_init(SculptSession *ss, SculptBoundary *boundary)
{
  const int totvert = SCULPT_vertex_count_get(ss);
  boundary->slide.directions = MEM_calloc_arrayN(totvert, sizeof(float[3]), "slide directions");

  for (int i = 0; i < totvert; i++) {
    if (boundary->edit_info[i].propagation_steps_num != boundary->max_propagation_steps) {
      continue;
    }
    sub_v3_v3v3(
        boundary->slide.directions[boundary->edit_info[i].original_vertex_i],
        SCULPT_vertex_co_get(
            ss, BKE_pbvh_index_to_vertex(ss->pbvh, boundary->edit_info[i].original_vertex_i)),
        SCULPT_vertex_co_get(ss, BKE_pbvh_index_to_vertex(ss->pbvh, i)));
    normalize_v3(boundary->slide.directions[boundary->edit_info[i].original_vertex_i]);
  }

  for (int i = 0; i < totvert; i++) {
    if (boundary->edit_info[i].propagation_steps_num == BOUNDARY_STEPS_NONE) {
      continue;
    }
    copy_v3_v3(boundary->slide.directions[i],
               boundary->slide.directions[boundary->edit_info[i].original_vertex_i]);
  }
}

static void sculpt_boundary_twist_data_init(SculptSession *ss, SculptBoundary *boundary)
{
  zero_v3(boundary->twist.pivot_position);
  float(*poly_verts)[3] = MEM_malloc_arrayN(boundary->verts_num, sizeof(float[3]), "poly verts");
  for (int i = 0; i < boundary->verts_num; i++) {
    add_v3_v3(boundary->twist.pivot_position, SCULPT_vertex_co_get(ss, boundary->verts[i]));
    copy_v3_v3(poly_verts[i], SCULPT_vertex_co_get(ss, boundary->verts[i]));
  }
  mul_v3_fl(boundary->twist.pivot_position, 1.0f / boundary->verts_num);
  if (boundary->forms_loop) {
    normal_poly_v3(boundary->twist.rotation_axis, poly_verts, boundary->verts_num);
  }
  else {
    sub_v3_v3v3(boundary->twist.rotation_axis,
                SCULPT_vertex_co_get(ss, boundary->pivot_vertex),
                SCULPT_vertex_co_get(ss, boundary->initial_vertex));
    normalize_v3(boundary->twist.rotation_axis);
  }
  MEM_freeN(poly_verts);
}

static float sculpt_boundary_displacement_from_grab_delta_get(SculptSession *ss,
                                                              SculptBoundary *boundary)
{
  float plane[4];
  float pos[3];
  float normal[3];
  sub_v3_v3v3(normal, ss->cache->initial_location, boundary->initial_pivot_position);
  normalize_v3(normal);
  plane_from_point_normal_v3(plane, ss->cache->initial_location, normal);
  add_v3_v3v3(pos, ss->cache->initial_location, ss->cache->grab_delta_symmetry);
  return dist_signed_to_plane_v3(pos, plane);
}

/* Deformation tasks callbacks. */
static void do_boundary_brush_bend_task_cb_ex(void *__restrict userdata,
                                              const int n,
                                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const int symm_area = ss->cache->mirror_symmetry_pass;
  SculptBoundary *boundary = ss->cache->boundaries[symm_area];
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(data->ob);
  const Brush *brush = data->brush;

  const float strength = ss->cache->bstrength;

  PBVHVertexIter vd;
  SculptOrigVertData orig_data;
  SCULPT_orig_vert_data_init(&orig_data, data->ob, data->nodes[n], SCULPT_UNDO_COORDS);

  const float disp = strength * sculpt_boundary_displacement_from_grab_delta_get(ss, boundary);
  float angle_factor = disp / ss->cache->radius;
  /* Angle Snapping when inverting the brush. */
  if (ss->cache->invert) {
    angle_factor = floorf(angle_factor * 10) / 10.0f;
  }
  const float angle = angle_factor * M_PI;
  AutomaskingNodeData automask_data;
  SCULPT_automasking_node_begin(
      data->ob, ss, ss->cache->automasking, &automask_data, data->nodes[n]);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (boundary->edit_info[vd.index].propagation_steps_num == -1) {
      continue;
    }

    SCULPT_automasking_node_update(ss, &automask_data, &vd);
    SCULPT_orig_vert_data_update(&orig_data, &vd);
    if (!SCULPT_check_vertex_pivot_symmetry(
            orig_data.co, boundary->initial_vertex_position, symm)) {
      continue;
    }

    const float mask = vd.mask ? 1.0f - *vd.mask : 1.0f;
    const float automask = SCULPT_automasking_factor_get(
        ss->cache->automasking, ss, vd.vertex, &automask_data);
    float t_orig_co[3];
    float *target_co = SCULPT_brush_deform_target_vertex_co_get(ss, brush->deform_target, &vd);
    sub_v3_v3v3(t_orig_co, orig_data.co, boundary->bend.pivot_positions[vd.index]);
    rotate_v3_v3v3fl(target_co,
                     t_orig_co,
                     boundary->bend.pivot_rotation_axis[vd.index],
                     angle * boundary->edit_info[vd.index].strength_factor * mask * automask);
    add_v3_v3(target_co, boundary->bend.pivot_positions[vd.index]);

    if (vd.mvert) {
      BKE_pbvh_vert_tag_update_normal(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_boundary_brush_slide_task_cb_ex(void *__restrict userdata,
                                               const int n,
                                               const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const int symm_area = ss->cache->mirror_symmetry_pass;
  SculptBoundary *boundary = ss->cache->boundaries[symm_area];
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(data->ob);
  const Brush *brush = data->brush;

  const float strength = ss->cache->bstrength;

  PBVHVertexIter vd;
  SculptOrigVertData orig_data;
  SCULPT_orig_vert_data_init(&orig_data, data->ob, data->nodes[n], SCULPT_UNDO_COORDS);

  const float disp = sculpt_boundary_displacement_from_grab_delta_get(ss, boundary);
  AutomaskingNodeData automask_data;
  SCULPT_automasking_node_begin(
      data->ob, ss, ss->cache->automasking, &automask_data, data->nodes[n]);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (boundary->edit_info[vd.index].propagation_steps_num == -1) {
      continue;
    }

    SCULPT_automasking_node_update(ss, &automask_data, &vd);
    SCULPT_orig_vert_data_update(&orig_data, &vd);
    if (!SCULPT_check_vertex_pivot_symmetry(
            orig_data.co, boundary->initial_vertex_position, symm)) {
      continue;
    }

    const float mask = vd.mask ? 1.0f - *vd.mask : 1.0f;
    const float automask = SCULPT_automasking_factor_get(
        ss->cache->automasking, ss, vd.vertex, &automask_data);
    float *target_co = SCULPT_brush_deform_target_vertex_co_get(ss, brush->deform_target, &vd);
    madd_v3_v3v3fl(target_co,
                   orig_data.co,
                   boundary->slide.directions[vd.index],
                   boundary->edit_info[vd.index].strength_factor * disp * mask * automask *
                       strength);

    if (vd.mvert) {
      BKE_pbvh_vert_tag_update_normal(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_boundary_brush_inflate_task_cb_ex(void *__restrict userdata,
                                                 const int n,
                                                 const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const int symm_area = ss->cache->mirror_symmetry_pass;
  SculptBoundary *boundary = ss->cache->boundaries[symm_area];
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(data->ob);
  const Brush *brush = data->brush;

  const float strength = ss->cache->bstrength;

  PBVHVertexIter vd;
  SculptOrigVertData orig_data;
  SCULPT_orig_vert_data_init(&orig_data, data->ob, data->nodes[n], SCULPT_UNDO_COORDS);
  AutomaskingNodeData automask_data;
  SCULPT_automasking_node_begin(
      data->ob, ss, ss->cache->automasking, &automask_data, data->nodes[n]);

  const float disp = sculpt_boundary_displacement_from_grab_delta_get(ss, boundary);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (boundary->edit_info[vd.index].propagation_steps_num == -1) {
      continue;
    }

    SCULPT_automasking_node_update(ss, &automask_data, &vd);
    SCULPT_orig_vert_data_update(&orig_data, &vd);
    if (!SCULPT_check_vertex_pivot_symmetry(
            orig_data.co, boundary->initial_vertex_position, symm)) {
      continue;
    }

    const float mask = vd.mask ? 1.0f - *vd.mask : 1.0f;
    const float automask = SCULPT_automasking_factor_get(
        ss->cache->automasking, ss, vd.vertex, &automask_data);
    float *target_co = SCULPT_brush_deform_target_vertex_co_get(ss, brush->deform_target, &vd);
    madd_v3_v3v3fl(target_co,
                   orig_data.co,
                   orig_data.no,
                   boundary->edit_info[vd.index].strength_factor * disp * mask * automask *
                       strength);

    if (vd.mvert) {
      BKE_pbvh_vert_tag_update_normal(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_boundary_brush_grab_task_cb_ex(void *__restrict userdata,
                                              const int n,
                                              const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const int symm_area = ss->cache->mirror_symmetry_pass;
  SculptBoundary *boundary = ss->cache->boundaries[symm_area];
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(data->ob);
  const Brush *brush = data->brush;

  const float strength = ss->cache->bstrength;

  PBVHVertexIter vd;
  SculptOrigVertData orig_data;
  SCULPT_orig_vert_data_init(&orig_data, data->ob, data->nodes[n], SCULPT_UNDO_COORDS);
  AutomaskingNodeData automask_data;
  SCULPT_automasking_node_begin(
      data->ob, ss, ss->cache->automasking, &automask_data, data->nodes[n]);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (boundary->edit_info[vd.index].propagation_steps_num == -1) {
      continue;
    }

    SCULPT_automasking_node_update(ss, &automask_data, &vd);
    SCULPT_orig_vert_data_update(&orig_data, &vd);
    if (!SCULPT_check_vertex_pivot_symmetry(
            orig_data.co, boundary->initial_vertex_position, symm)) {
      continue;
    }

    const float mask = vd.mask ? 1.0f - *vd.mask : 1.0f;
    const float automask = SCULPT_automasking_factor_get(
        ss->cache->automasking, ss, vd.vertex, &automask_data);
    float *target_co = SCULPT_brush_deform_target_vertex_co_get(ss, brush->deform_target, &vd);
    madd_v3_v3v3fl(target_co,
                   orig_data.co,
                   ss->cache->grab_delta_symmetry,
                   boundary->edit_info[vd.index].strength_factor * mask * automask * strength);

    if (vd.mvert) {
      BKE_pbvh_vert_tag_update_normal(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_boundary_brush_twist_task_cb_ex(void *__restrict userdata,
                                               const int n,
                                               const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const int symm_area = ss->cache->mirror_symmetry_pass;
  SculptBoundary *boundary = ss->cache->boundaries[symm_area];
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(data->ob);
  const Brush *brush = data->brush;

  const float strength = ss->cache->bstrength;

  PBVHVertexIter vd;
  SculptOrigVertData orig_data;
  SCULPT_orig_vert_data_init(&orig_data, data->ob, data->nodes[n], SCULPT_UNDO_COORDS);
  AutomaskingNodeData automask_data;
  SCULPT_automasking_node_begin(
      data->ob, ss, ss->cache->automasking, &automask_data, data->nodes[n]);

  const float disp = strength * sculpt_boundary_displacement_from_grab_delta_get(ss, boundary);
  float angle_factor = disp / ss->cache->radius;
  /* Angle Snapping when inverting the brush. */
  if (ss->cache->invert) {
    angle_factor = floorf(angle_factor * 10) / 10.0f;
  }
  const float angle = angle_factor * M_PI;

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (boundary->edit_info[vd.index].propagation_steps_num == -1) {
      continue;
    }

    SCULPT_automasking_node_update(ss, &automask_data, &vd);
    SCULPT_orig_vert_data_update(&orig_data, &vd);
    if (!SCULPT_check_vertex_pivot_symmetry(
            orig_data.co, boundary->initial_vertex_position, symm)) {
      continue;
    }

    const float mask = vd.mask ? 1.0f - *vd.mask : 1.0f;
    const float automask = SCULPT_automasking_factor_get(
        ss->cache->automasking, ss, vd.vertex, &automask_data);
    float t_orig_co[3];
    float *target_co = SCULPT_brush_deform_target_vertex_co_get(ss, brush->deform_target, &vd);
    sub_v3_v3v3(t_orig_co, orig_data.co, boundary->twist.pivot_position);
    rotate_v3_v3v3fl(target_co,
                     t_orig_co,
                     boundary->twist.rotation_axis,
                     angle * mask * automask * boundary->edit_info[vd.index].strength_factor);
    add_v3_v3(target_co, boundary->twist.pivot_position);

    if (vd.mvert) {
      BKE_pbvh_vert_tag_update_normal(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static void do_boundary_brush_smooth_task_cb_ex(void *__restrict userdata,
                                                const int n,
                                                const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  const int symmetry_pass = ss->cache->mirror_symmetry_pass;
  const SculptBoundary *boundary = ss->cache->boundaries[symmetry_pass];
  const ePaintSymmetryFlags symm = SCULPT_mesh_symmetry_xyz_get(data->ob);
  const Brush *brush = data->brush;

  const float strength = ss->cache->bstrength;

  PBVHVertexIter vd;
  SculptOrigVertData orig_data;
  SCULPT_orig_vert_data_init(&orig_data, data->ob, data->nodes[n], SCULPT_UNDO_COORDS);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    if (boundary->edit_info[vd.index].propagation_steps_num == -1) {
      continue;
    }

    SCULPT_orig_vert_data_update(&orig_data, &vd);
    if (!SCULPT_check_vertex_pivot_symmetry(
            orig_data.co, boundary->initial_vertex_position, symm)) {
      continue;
    }

    float coord_accum[3] = {0.0f, 0.0f, 0.0f};
    int total_neighbors = 0;
    const int current_propagation_steps = boundary->edit_info[vd.index].propagation_steps_num;
    SculptVertexNeighborIter ni;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd.vertex, ni) {
      if (current_propagation_steps == boundary->edit_info[ni.index].propagation_steps_num) {
        add_v3_v3(coord_accum, SCULPT_vertex_co_get(ss, ni.vertex));
        total_neighbors++;
      }
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

    if (total_neighbors == 0) {
      continue;
    }
    float disp[3];
    float avg[3];
    const float mask = vd.mask ? 1.0f - *vd.mask : 1.0f;
    mul_v3_v3fl(avg, coord_accum, 1.0f / total_neighbors);
    sub_v3_v3v3(disp, avg, vd.co);
    float *target_co = SCULPT_brush_deform_target_vertex_co_get(ss, brush->deform_target, &vd);
    madd_v3_v3v3fl(
        target_co, vd.co, disp, boundary->edit_info[vd.index].strength_factor * mask * strength);

    if (vd.mvert) {
      BKE_pbvh_vert_tag_update_normal(ss->pbvh, vd.vertex);
    }
  }
  BKE_pbvh_vertex_iter_end;
}

void SCULPT_do_boundary_brush(Sculpt *sd, Object *ob, PBVHNode **nodes, int totnode)
{
  SculptSession *ss = ob->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);

  const int symm_area = ss->cache->mirror_symmetry_pass;
  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(ss->cache)) {

    PBVHVertRef initial_vertex;

    if (ss->cache->mirror_symmetry_pass == 0) {
      initial_vertex = SCULPT_active_vertex_get(ss);
    }
    else {
      float location[3];
      flip_v3_v3(location, SCULPT_active_vertex_co_get(ss), symm_area);
      initial_vertex = SCULPT_nearest_vertex_get(
          sd, ob, location, ss->cache->radius_squared, false);
    }

    ss->cache->boundaries[symm_area] = SCULPT_boundary_data_init(
        ob, brush, initial_vertex, ss->cache->initial_radius);

    if (ss->cache->boundaries[symm_area]) {

      switch (brush->boundary_deform_type) {
        case BRUSH_BOUNDARY_DEFORM_BEND:
          sculpt_boundary_bend_data_init(ss, ss->cache->boundaries[symm_area]);
          break;
        case BRUSH_BOUNDARY_DEFORM_EXPAND:
          sculpt_boundary_slide_data_init(ss, ss->cache->boundaries[symm_area]);
          break;
        case BRUSH_BOUNDARY_DEFORM_TWIST:
          sculpt_boundary_twist_data_init(ss, ss->cache->boundaries[symm_area]);
          break;
        case BRUSH_BOUNDARY_DEFORM_INFLATE:
        case BRUSH_BOUNDARY_DEFORM_GRAB:
          /* Do nothing. These deform modes don't need any extra data to be precomputed. */
          break;
      }

      sculpt_boundary_falloff_factor_init(
          ss, ss->cache->boundaries[symm_area], brush, ss->cache->initial_radius);
    }
  }

  /* No active boundary under the cursor. */
  if (!ss->cache->boundaries[symm_area]) {
    return;
  }

  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .brush = brush,
      .nodes = nodes,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);

  switch (brush->boundary_deform_type) {
    case BRUSH_BOUNDARY_DEFORM_BEND:
      BLI_task_parallel_range(0, totnode, &data, do_boundary_brush_bend_task_cb_ex, &settings);
      break;
    case BRUSH_BOUNDARY_DEFORM_EXPAND:
      BLI_task_parallel_range(0, totnode, &data, do_boundary_brush_slide_task_cb_ex, &settings);
      break;
    case BRUSH_BOUNDARY_DEFORM_INFLATE:
      BLI_task_parallel_range(0, totnode, &data, do_boundary_brush_inflate_task_cb_ex, &settings);
      break;
    case BRUSH_BOUNDARY_DEFORM_GRAB:
      BLI_task_parallel_range(0, totnode, &data, do_boundary_brush_grab_task_cb_ex, &settings);
      break;
    case BRUSH_BOUNDARY_DEFORM_TWIST:
      BLI_task_parallel_range(0, totnode, &data, do_boundary_brush_twist_task_cb_ex, &settings);
      break;
    case BRUSH_BOUNDARY_DEFORM_SMOOTH:
      BLI_task_parallel_range(0, totnode, &data, do_boundary_brush_smooth_task_cb_ex, &settings);
      break;
  }
}

void SCULPT_boundary_edges_preview_draw(const uint gpuattr,
                                        SculptSession *ss,
                                        const float outline_col[3],
                                        const float outline_alpha)
{
  if (!ss->boundary_preview) {
    return;
  }
  immUniformColor3fvAlpha(outline_col, outline_alpha);
  GPU_line_width(2.0f);
  immBegin(GPU_PRIM_LINES, ss->boundary_preview->edges_num * 2);
  for (int i = 0; i < ss->boundary_preview->edges_num; i++) {
    immVertex3fv(gpuattr, SCULPT_vertex_co_get(ss, ss->boundary_preview->edges[i].v1));
    immVertex3fv(gpuattr, SCULPT_vertex_co_get(ss, ss->boundary_preview->edges[i].v2));
  }
  immEnd();
}

void SCULPT_boundary_pivot_line_preview_draw(const uint gpuattr, SculptSession *ss)
{
  if (!ss->boundary_preview) {
    return;
  }
  immUniformColor4f(1.0f, 1.0f, 1.0f, 0.8f);
  GPU_line_width(2.0f);
  immBegin(GPU_PRIM_LINES, 2);
  immVertex3fv(gpuattr, SCULPT_vertex_co_get(ss, ss->boundary_preview->pivot_vertex));
  immVertex3fv(gpuattr, SCULPT_vertex_co_get(ss, ss->boundary_preview->initial_vertex));
  immEnd();
}
