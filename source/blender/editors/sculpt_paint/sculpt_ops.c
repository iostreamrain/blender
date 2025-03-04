/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 2006 by Nicholas Bishop. All rights reserved. */

/** \file
 * \ingroup edsculpt
 * Implements the Sculpt Mode tools.
 */

#include "MEM_guardedalloc.h"

#include "BLI_array.h"
#include "BLI_blenlib.h"
#include "BLI_dial_2d.h"
#include "BLI_ghash.h"
#include "BLI_gsqueue.h"
#include "BLI_hash.h"
#include "BLI_link_utils.h"
#include "BLI_linklist.h"
#include "BLI_linklist_stack.h"
#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_math_color_blend.h"
#include "BLI_memarena.h"
#include "BLI_rand.h"
#include "BLI_task.h"
#include "BLI_utildefines.h"
#include "atomic_ops.h"

#include "BLT_translation.h"

#include "PIL_time.h"

#include "DNA_brush_types.h"
#include "DNA_customdata_types.h"
#include "DNA_listBase.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.h"
#include "BKE_brush.h"
#include "BKE_ccg.h"
#include "BKE_colortools.h"
#include "BKE_context.h"
#include "BKE_image.h"
#include "BKE_kelvinlet.h"
#include "BKE_key.h"
#include "BKE_layer.h"
#include "BKE_lib_id.h"
#include "BKE_main.h"
#include "BKE_mesh.h"
#include "BKE_mesh_fair.h"
#include "BKE_mesh_mapping.h"
#include "BKE_mesh_mirror.h"
#include "BKE_modifier.h"
#include "BKE_multires.h"
#include "BKE_node.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_particle.h"
#include "BKE_pbvh.h"
#include "BKE_pointcache.h"
#include "BKE_report.h"
#include "BKE_scene.h"
#include "BKE_screen.h"
#include "BKE_subdiv_ccg.h"
#include "BKE_subsurf.h"

#include "DEG_depsgraph.h"
#include "DEG_depsgraph_query.h"

#include "IMB_colormanagement.h"

#include "GPU_batch.h"
#include "GPU_batch_presets.h"
#include "GPU_immediate.h"
#include "GPU_immediate_util.h"
#include "GPU_matrix.h"
#include "GPU_state.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_toolsystem.h"
#include "WM_types.h"

#include "ED_image.h"
#include "ED_object.h"
#include "ED_screen.h"
#include "ED_sculpt.h"
#include "ED_space_api.h"
#include "ED_transform_snap_object_context.h"
#include "ED_view3d.h"

#include "paint_intern.h"
#include "sculpt_intern.h"

#include "RNA_access.h"
#include "RNA_define.h"
#include "RNA_path.h"

#include "UI_interface.h"
#include "UI_resources.h"

#include "bmesh.h"
#include "bmesh_tools.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Reset the copy of the mesh that is being sculpted on (currently just for the layer brush). */

static int sculpt_set_persistent_base_exec(bContext *C, wmOperator *UNUSED(op))
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;

  /* Do not allow in DynTopo just yet. */
  if (!ss || (ss && ss->bm)) {
    return OPERATOR_FINISHED;
  }
  SCULPT_vertex_random_access_ensure(ss);
  BKE_sculpt_update_object_for_edit(depsgraph, ob, false, false, false);

  SculptAttributeParams params = {0};
  params.permanent = true;

  ss->attrs.persistent_co = BKE_sculpt_attribute_ensure(
      ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT3, SCULPT_ATTRIBUTE_NAME(persistent_co), &params);
  ss->attrs.persistent_no = BKE_sculpt_attribute_ensure(
      ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT3, SCULPT_ATTRIBUTE_NAME(persistent_no), &params);
  ss->attrs.persistent_disp = BKE_sculpt_attribute_ensure(
      ob, ATTR_DOMAIN_POINT, CD_PROP_FLOAT, SCULPT_ATTRIBUTE_NAME(persistent_disp), &params);

  const int totvert = SCULPT_vertex_count_get(ss);

  for (int i = 0; i < totvert; i++) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(ss->pbvh, i);

    copy_v3_v3((float *)SCULPT_vertex_attr_get(vertex, ss->attrs.persistent_co),
               SCULPT_vertex_co_get(ss, vertex));
    SCULPT_vertex_normal_get(
        ss, vertex, (float *)SCULPT_vertex_attr_get(vertex, ss->attrs.persistent_no));
    (*(float *)SCULPT_vertex_attr_get(vertex, ss->attrs.persistent_disp)) = 0.0f;
  }

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_set_persistent_base(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Set Persistent Base";
  ot->idname = "SCULPT_OT_set_persistent_base";
  ot->description = "Reset the copy of the mesh that is being sculpted on";

  /* API callbacks. */
  ot->exec = sculpt_set_persistent_base_exec;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/************************* SCULPT_OT_optimize *************************/

static int sculpt_optimize_exec(bContext *C, wmOperator *UNUSED(op))
{
  Object *ob = CTX_data_active_object(C);

  SCULPT_pbvh_clear(ob);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

  return OPERATOR_FINISHED;
}

/* The BVH gets less optimal more quickly with dynamic topology than
 * regular sculpting. There is no doubt more clever stuff we can do to
 * optimize it on the fly, but for now this gives the user a nicer way
 * to recalculate it than toggling modes. */
static void SCULPT_OT_optimize(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Rebuild BVH";
  ot->idname = "SCULPT_OT_optimize";
  ot->description = "Recalculate the sculpt BVH to improve performance";

  /* API callbacks. */
  ot->exec = sculpt_optimize_exec;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/********************* Dynamic topology symmetrize ********************/

static bool sculpt_no_multires_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (SCULPT_mode_poll(C) && ob->sculpt && ob->sculpt->pbvh) {
    return BKE_pbvh_type(ob->sculpt->pbvh) != PBVH_GRIDS;
  }
  return false;
}

static int sculpt_symmetrize_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  const Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  SculptSession *ss = ob->sculpt;
  PBVH *pbvh = ss->pbvh;
  const float dist = RNA_float_get(op->ptr, "merge_tolerance");

  if (!pbvh) {
    return OPERATOR_CANCELLED;
  }

  switch (BKE_pbvh_type(pbvh)) {
    case PBVH_BMESH:
      /* Dyntopo Symmetrize. */

      /* To simplify undo for symmetrize, all BMesh elements are logged
       * as deleted, then after symmetrize operation all BMesh elements
       * are logged as added (as opposed to attempting to store just the
       * parts that symmetrize modifies). */
      SCULPT_undo_push_begin(ob, op);
      SCULPT_undo_push_node(ob, NULL, SCULPT_UNDO_DYNTOPO_SYMMETRIZE);
      BM_log_before_all_removed(ss->bm, ss->bm_log);

      BM_mesh_toolflags_set(ss->bm, true);

      /* Symmetrize and re-triangulate. */
      BMO_op_callf(ss->bm,
                   (BMO_FLAG_DEFAULTS & ~BMO_FLAG_RESPECT_HIDE),
                   "symmetrize input=%avef direction=%i dist=%f use_shapekey=%b",
                   sd->symmetrize_direction,
                   dist,
                   true);
      SCULPT_dynamic_topology_triangulate(ss->bm);

      /* Bisect operator flags edges (keep tags clean for edge queue). */
      BM_mesh_elem_hflag_disable_all(ss->bm, BM_EDGE, BM_ELEM_TAG, false);

      BM_mesh_toolflags_set(ss->bm, false);

      /* Finish undo. */
      BM_log_all_added(ss->bm, ss->bm_log);
      SCULPT_undo_push_end(ob);

      break;
    case PBVH_FACES:
      /* Mesh Symmetrize. */
      ED_sculpt_undo_geometry_begin(ob, op);
      Mesh *mesh = ob->data;

      BKE_mesh_mirror_apply_mirror_on_axis(bmain, mesh, sd->symmetrize_direction, dist);

      ED_sculpt_undo_geometry_end(ob);
      BKE_mesh_normals_tag_dirty(mesh);
      BKE_mesh_batch_cache_dirty_tag(ob->data, BKE_MESH_BATCH_DIRTY_ALL);

      break;
    case PBVH_GRIDS:
      return OPERATOR_CANCELLED;
  }

  /* Redraw. */
  SCULPT_pbvh_clear(ob);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_symmetrize(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Symmetrize";
  ot->idname = "SCULPT_OT_symmetrize";
  ot->description = "Symmetrize the topology modifications";

  /* API callbacks. */
  ot->exec = sculpt_symmetrize_exec;
  ot->poll = sculpt_no_multires_poll;

  RNA_def_float(ot->srna,
                "merge_tolerance",
                0.001f,
                0.0f,
                FLT_MAX,
                "Merge Distance",
                "Distance within which symmetrical vertices are merged",
                0.0f,
                1.0f);
}

/**** Toggle operator for turning sculpt mode on or off ****/

static void sculpt_init_session(Main *bmain, Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  /* Create persistent sculpt mode data. */
  BKE_sculpt_toolsettings_data_ensure(scene);

  /* Create sculpt mode session data. */
  if (ob->sculpt != NULL) {
    BKE_sculptsession_free(ob);
  }
  ob->sculpt = MEM_callocN(sizeof(SculptSession), "sculpt session");
  ob->sculpt->mode_type = OB_MODE_SCULPT;

  /* Trigger evaluation of modifier stack to ensure
   * multires modifier sets .runtime.ccg in
   * the evaluated mesh.
   */
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

  BKE_scene_graph_evaluated_ensure(depsgraph, bmain);

  /* This function expects a fully evaluated depsgraph. */
  BKE_sculpt_update_object_for_edit(depsgraph, ob, false, false, false);

  SculptSession *ss = ob->sculpt;
  if (ss->face_sets) {
    /* Here we can detect geometry that was just added to Sculpt Mode as it has the
     * SCULPT_FACE_SET_NONE assigned, so we can create a new Face Set for it. */
    /* In sculpt mode all geometry that is assigned to SCULPT_FACE_SET_NONE is considered as not
     * initialized, which is used is some operators that modify the mesh topology to perform
     * certain actions in the new polys. After these operations are finished, all polys should have
     * a valid face set ID assigned (different from SCULPT_FACE_SET_NONE) to manage their
     * visibility correctly. */
    /* TODO(pablodp606): Based on this we can improve the UX in future tools for creating new
     * objects, like moving the transform pivot position to the new area or masking existing
     * geometry. */
    const int new_face_set = SCULPT_face_set_next_available_get(ss);
    for (int i = 0; i < ss->totfaces; i++) {
      if (ss->face_sets[i] == SCULPT_FACE_SET_NONE) {
        ss->face_sets[i] = new_face_set;
      }
    }
  }
}

void ED_object_sculptmode_enter_ex(Main *bmain,
                                   Depsgraph *depsgraph,
                                   Scene *scene,
                                   Object *ob,
                                   const bool force_dyntopo,
                                   ReportList *reports)
{
  const int mode_flag = OB_MODE_SCULPT;
  Mesh *me = BKE_mesh_from_object(ob);

  /* Enter sculpt mode. */
  ob->mode |= mode_flag;

  sculpt_init_session(bmain, depsgraph, scene, ob);

  if (!(fabsf(ob->scale[0] - ob->scale[1]) < 1e-4f &&
        fabsf(ob->scale[1] - ob->scale[2]) < 1e-4f)) {
    BKE_report(
        reports, RPT_WARNING, "Object has non-uniform scale, sculpting may be unpredictable");
  }
  else if (is_negative_m4(ob->obmat)) {
    BKE_report(reports, RPT_WARNING, "Object has negative scale, sculpting may be unpredictable");
  }

  Paint *paint = BKE_paint_get_active_from_paintmode(scene, PAINT_MODE_SCULPT);
  BKE_paint_init(bmain, scene, PAINT_MODE_SCULPT, PAINT_CURSOR_SCULPT);

  ED_paint_cursor_start(paint, SCULPT_mode_poll_view3d);

  /* Check dynamic-topology flag; re-enter dynamic-topology mode when changing modes,
   * As long as no data was added that is not supported. */
  if (me->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) {
    MultiresModifierData *mmd = BKE_sculpt_multires_active(scene, ob);

    const char *message_unsupported = NULL;
    if (me->totloop != me->totpoly * 3) {
      message_unsupported = TIP_("non-triangle face");
    }
    else if (mmd != NULL) {
      message_unsupported = TIP_("multi-res modifier");
    }
    else {
      enum eDynTopoWarnFlag flag = SCULPT_dynamic_topology_check(scene, ob);
      if (flag == 0) {
        /* pass */
      }
      else if (flag & DYNTOPO_WARN_VDATA) {
        message_unsupported = TIP_("vertex data");
      }
      else if (flag & DYNTOPO_WARN_EDATA) {
        message_unsupported = TIP_("edge data");
      }
      else if (flag & DYNTOPO_WARN_LDATA) {
        message_unsupported = TIP_("face data");
      }
      else if (flag & DYNTOPO_WARN_MODIFIER) {
        message_unsupported = TIP_("constructive modifier");
      }
      else {
        BLI_assert(0);
      }
    }

    if ((message_unsupported == NULL) || force_dyntopo) {
      /* Needed because we may be entering this mode before the undo system loads. */
      wmWindowManager *wm = bmain->wm.first;
      bool has_undo = wm->undo_stack != NULL;
      /* Undo push is needed to prevent memory leak. */
      if (has_undo) {
        SCULPT_undo_push_begin_ex(ob, "Dynamic topology enable");
      }
      SCULPT_dynamic_topology_enable_ex(bmain, depsgraph, scene, ob);
      if (has_undo) {
        SCULPT_undo_push_node(ob, NULL, SCULPT_UNDO_DYNTOPO_BEGIN);
        SCULPT_undo_push_end(ob);
      }
    }
    else {
      BKE_reportf(
          reports, RPT_WARNING, "Dynamic Topology found: %s, disabled", message_unsupported);
      me->flag &= ~ME_SCULPT_DYNAMIC_TOPOLOGY;
    }
  }

  /* Flush object mode. */
  DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
}

void ED_object_sculptmode_enter(struct bContext *C, Depsgraph *depsgraph, ReportList *reports)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);
  ED_object_sculptmode_enter_ex(bmain, depsgraph, scene, ob, false, reports);
}

void ED_object_sculptmode_exit_ex(Main *bmain, Depsgraph *depsgraph, Scene *scene, Object *ob)
{
  const int mode_flag = OB_MODE_SCULPT;
  Mesh *me = BKE_mesh_from_object(ob);

  multires_flush_sculpt_updates(ob);

  /* Not needed for now. */
#if 0
  MultiresModifierData *mmd = BKE_sculpt_multires_active(scene, ob);
  const int flush_recalc = ed_object_sculptmode_flush_recalc_flag(scene, ob, mmd);
#endif

  /* Always for now, so leaving sculpt mode always ensures scene is in
   * a consistent state. */
  if (true || /* flush_recalc || */ (ob->sculpt && ob->sculpt->bm)) {
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
  }

  if (me->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) {
    /* Dynamic topology must be disabled before exiting sculpt
     * mode to ensure the undo stack stays in a consistent
     * state. */
    sculpt_dynamic_topology_disable_with_undo(bmain, depsgraph, scene, ob);

    /* Store so we know to re-enable when entering sculpt mode. */
    me->flag |= ME_SCULPT_DYNAMIC_TOPOLOGY;
  }

  /* Leave sculpt mode. */
  ob->mode &= ~mode_flag;

  BKE_sculptsession_free(ob);

  paint_cursor_delete_textures();

  /* Never leave derived meshes behind. */
  BKE_object_free_derived_caches(ob);

  /* Flush object mode. */
  DEG_id_tag_update(&ob->id, ID_RECALC_COPY_ON_WRITE);
}

void ED_object_sculptmode_exit(bContext *C, Depsgraph *depsgraph)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);
  ED_object_sculptmode_exit_ex(bmain, depsgraph, scene, ob);
}

static int sculpt_mode_toggle_exec(bContext *C, wmOperator *op)
{
  struct wmMsgBus *mbus = CTX_wm_message_bus(C);
  Main *bmain = CTX_data_main(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_on_load(C);
  Scene *scene = CTX_data_scene(C);
  ToolSettings *ts = scene->toolsettings;
  ViewLayer *view_layer = CTX_data_view_layer(C);
  BKE_view_layer_synced_ensure(scene, view_layer);
  Object *ob = BKE_view_layer_active_object_get(view_layer);
  const int mode_flag = OB_MODE_SCULPT;
  const bool is_mode_set = (ob->mode & mode_flag) != 0;

  if (!is_mode_set) {
    if (!ED_object_mode_compat_set(C, ob, mode_flag, op->reports)) {
      return OPERATOR_CANCELLED;
    }
  }

  if (is_mode_set) {
    ED_object_sculptmode_exit_ex(bmain, depsgraph, scene, ob);
  }
  else {
    if (depsgraph) {
      depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
    }
    ED_object_sculptmode_enter_ex(bmain, depsgraph, scene, ob, false, op->reports);
    BKE_paint_toolslots_brush_validate(bmain, &ts->sculpt->paint);

    if (ob->mode & mode_flag) {
      Mesh *me = ob->data;
      /* Dyntopo adds its own undo step. */
      if ((me->flag & ME_SCULPT_DYNAMIC_TOPOLOGY) == 0) {
        /* Without this the memfile undo step is used,
         * while it works it causes lag when undoing the first undo step, see T71564. */
        wmWindowManager *wm = CTX_wm_manager(C);
        if (wm->op_undo_depth <= 1) {
          SCULPT_undo_push_begin(ob, op);
          SCULPT_undo_push_end(ob);
        }
      }
    }
  }

  WM_event_add_notifier(C, NC_SCENE | ND_MODE, scene);

  WM_msg_publish_rna_prop(mbus, &ob->id, ob, Object, mode);

  WM_toolsystem_update_from_context_view3d(C);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_sculptmode_toggle(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Sculpt Mode";
  ot->idname = "SCULPT_OT_sculptmode_toggle";
  ot->description = "Toggle sculpt mode in 3D view";

  /* API callbacks. */
  ot->exec = sculpt_mode_toggle_exec;
  ot->poll = ED_operator_object_active_editable_mesh;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

void SCULPT_geometry_preview_lines_update(bContext *C, SculptSession *ss, float radius)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *ob = CTX_data_active_object(C);

  ss->preview_vert_count = 0;
  int totpoints = 0;

  /* This function is called from the cursor drawing code, so the PBVH may not be build yet. */
  if (!ss->pbvh) {
    return;
  }

  if (!ss->deform_modifiers_active) {
    return;
  }

  if (BKE_pbvh_type(ss->pbvh) == PBVH_GRIDS) {
    return;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);

  if (!ss->pmap) {
    return;
  }

  float brush_co[3];
  copy_v3_v3(brush_co, SCULPT_active_vertex_co_get(ss));

  BLI_bitmap *visited_verts = BLI_BITMAP_NEW(SCULPT_vertex_count_get(ss), "visited_verts");

  /* Assuming an average of 6 edges per vertex in a triangulated mesh. */
  const int max_preview_verts = SCULPT_vertex_count_get(ss) * 3 * 2;

  if (ss->preview_vert_list == NULL) {
    ss->preview_vert_list = MEM_callocN(max_preview_verts * sizeof(PBVHVertRef), "preview lines");
  }

  GSQueue *non_visited_verts = BLI_gsqueue_new(sizeof(PBVHVertRef));
  PBVHVertRef active_v = SCULPT_active_vertex_get(ss);
  BLI_gsqueue_push(non_visited_verts, &active_v);

  while (!BLI_gsqueue_is_empty(non_visited_verts)) {
    PBVHVertRef from_v;

    BLI_gsqueue_pop(non_visited_verts, &from_v);
    SculptVertexNeighborIter ni;
    SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, from_v, ni) {
      if (totpoints + (ni.size * 2) < max_preview_verts) {
        PBVHVertRef to_v = ni.vertex;
        int to_v_i = ni.index;
        ss->preview_vert_list[totpoints] = from_v;
        totpoints++;
        ss->preview_vert_list[totpoints] = to_v;
        totpoints++;
        if (BLI_BITMAP_TEST(visited_verts, to_v_i)) {
          continue;
        }
        BLI_BITMAP_ENABLE(visited_verts, to_v_i);
        const float *co = SCULPT_vertex_co_for_grab_active_get(ss, to_v);
        if (len_squared_v3v3(brush_co, co) < radius * radius) {
          BLI_gsqueue_push(non_visited_verts, &to_v);
        }
      }
    }
    SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
  }

  BLI_gsqueue_free(non_visited_verts);

  MEM_freeN(visited_verts);

  ss->preview_vert_count = totpoints;
}

static int sculpt_sample_color_invoke(bContext *C, wmOperator *op, const wmEvent *UNUSED(e))
{
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  Scene *scene = CTX_data_scene(C);
  Object *ob = CTX_data_active_object(C);
  Brush *brush = BKE_paint_brush(&sd->paint);
  SculptSession *ss = ob->sculpt;
  PBVHVertRef active_vertex = SCULPT_active_vertex_get(ss);
  float active_vertex_color[4];

  if (!SCULPT_handles_colors_report(ss, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  BKE_sculpt_update_object_for_edit(CTX_data_depsgraph_pointer(C), ob, true, false, false);

  /* No color attribute? Set color to white. */
  if (!SCULPT_has_colors(ss)) {
    copy_v4_fl(active_vertex_color, 1.0f);
  }
  else {
    SCULPT_vertex_color_get(ss, active_vertex, active_vertex_color);
  }

  float color_srgb[3];
  IMB_colormanagement_scene_linear_to_srgb_v3(color_srgb, active_vertex_color);
  BKE_brush_color_set(scene, brush, color_srgb);

  WM_event_add_notifier(C, NC_BRUSH | NA_EDITED, brush);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_sample_color(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Sample Color";
  ot->idname = "SCULPT_OT_sample_color";
  ot->description = "Sample the vertex color of the active vertex";

  /* api callbacks */
  ot->invoke = sculpt_sample_color_invoke;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER;
}

/**
 * #sculpt_mask_by_color_delta_get returns values in the (0,1) range that are used to generate the
 * mask based on the difference between two colors (the active color and the color of any other
 * vertex). Ideally, a threshold of 0 should mask only the colors that are equal to the active
 * color and threshold of 1 should mask all colors. In order to avoid artifacts and produce softer
 * falloffs in the mask, the MASK_BY_COLOR_SLOPE defines the size of the transition values between
 * masked and unmasked vertices. The smaller this value is, the sharper the generated mask is going
 * to be.
 */
#define MASK_BY_COLOR_SLOPE 0.25f

static float sculpt_mask_by_color_delta_get(const float *color_a,
                                            const float *color_b,
                                            const float threshold,
                                            const bool invert)
{
  float len = len_v3v3(color_a, color_b);
  /* Normalize len to the (0, 1) range. */
  len = len / M_SQRT3;

  if (len < threshold - MASK_BY_COLOR_SLOPE) {
    len = 1.0f;
  }
  else if (len >= threshold) {
    len = 0.0f;
  }
  else {
    len = (-len + threshold) / MASK_BY_COLOR_SLOPE;
  }

  if (invert) {
    return 1.0f - len;
  }
  return len;
}

static float sculpt_mask_by_color_final_mask_get(const float current_mask,
                                                 const float new_mask,
                                                 const bool invert,
                                                 const bool preserve_mask)
{
  if (preserve_mask) {
    if (invert) {
      return min_ff(current_mask, new_mask);
    }
    return max_ff(current_mask, new_mask);
  }
  return new_mask;
}

typedef struct MaskByColorContiguousFloodFillData {
  float threshold;
  bool invert;
  float *new_mask;
  float initial_color[3];
} MaskByColorContiguousFloodFillData;

static void do_mask_by_color_contiguous_update_nodes_cb(
    void *__restrict userdata, const int n, const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;

  SCULPT_undo_push_node(data->ob, data->nodes[n], SCULPT_UNDO_MASK);
  bool update_node = false;

  const bool invert = data->mask_by_color_invert;
  const bool preserve_mask = data->mask_by_color_preserve_mask;

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    const float current_mask = *vd.mask;
    const float new_mask = data->mask_by_color_floodfill[vd.index];
    *vd.mask = sculpt_mask_by_color_final_mask_get(current_mask, new_mask, invert, preserve_mask);
    if (current_mask == *vd.mask) {
      continue;
    }
    update_node = true;
  }
  BKE_pbvh_vertex_iter_end;
  if (update_node) {
    BKE_pbvh_node_mark_update_mask(data->nodes[n]);
  }
}

static bool sculpt_mask_by_color_contiguous_floodfill_cb(
    SculptSession *ss, PBVHVertRef from_v, PBVHVertRef to_v, bool is_duplicate, void *userdata)
{
  int from_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, from_v);
  int to_v_i = BKE_pbvh_vertex_to_index(ss->pbvh, to_v);

  MaskByColorContiguousFloodFillData *data = userdata;
  float current_color[4];

  SCULPT_vertex_color_get(ss, to_v, current_color);

  float new_vertex_mask = sculpt_mask_by_color_delta_get(
      current_color, data->initial_color, data->threshold, data->invert);
  data->new_mask[to_v_i] = new_vertex_mask;

  if (is_duplicate) {
    data->new_mask[to_v_i] = data->new_mask[from_v_i];
  }

  float len = len_v3v3(current_color, data->initial_color);
  len = len / M_SQRT3;
  return len <= data->threshold;
}

static void sculpt_mask_by_color_contiguous(Object *object,
                                            const PBVHVertRef vertex,
                                            const float threshold,
                                            const bool invert,
                                            const bool preserve_mask)
{
  SculptSession *ss = object->sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);

  float *new_mask = MEM_calloc_arrayN(totvert, sizeof(float), "new mask");

  if (invert) {
    for (int i = 0; i < totvert; i++) {
      new_mask[i] = 1.0f;
    }
  }

  SculptFloodFill flood;
  SCULPT_floodfill_init(ss, &flood);
  SCULPT_floodfill_add_initial(&flood, vertex);

  MaskByColorContiguousFloodFillData ffd;
  ffd.threshold = threshold;
  ffd.invert = invert;
  ffd.new_mask = new_mask;

  float color[4];
  SCULPT_vertex_color_get(ss, vertex, color);

  copy_v3_v3(ffd.initial_color, color);

  SCULPT_floodfill_execute(ss, &flood, sculpt_mask_by_color_contiguous_floodfill_cb, &ffd);
  SCULPT_floodfill_free(&flood);

  int totnode;
  PBVHNode **nodes;
  BKE_pbvh_search_gather(ss->pbvh, NULL, NULL, &nodes, &totnode);

  SculptThreadedTaskData data = {
      .ob = object,
      .nodes = nodes,
      .mask_by_color_floodfill = new_mask,
      .mask_by_color_vertex = vertex,
      .mask_by_color_threshold = threshold,
      .mask_by_color_invert = invert,
      .mask_by_color_preserve_mask = preserve_mask,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(
      0, totnode, &data, do_mask_by_color_contiguous_update_nodes_cb, &settings);

  MEM_SAFE_FREE(nodes);

  MEM_freeN(new_mask);
}

static void do_mask_by_color_task_cb(void *__restrict userdata,
                                     const int n,
                                     const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;

  SCULPT_undo_push_node(data->ob, data->nodes[n], SCULPT_UNDO_MASK);
  bool update_node = false;

  const float threshold = data->mask_by_color_threshold;
  const bool invert = data->mask_by_color_invert;
  const bool preserve_mask = data->mask_by_color_preserve_mask;
  float active_color[4];

  SCULPT_vertex_color_get(ss, data->mask_by_color_vertex, active_color);

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin (ss->pbvh, data->nodes[n], vd, PBVH_ITER_UNIQUE) {
    float col[4];
    SCULPT_vertex_color_get(ss, vd.vertex, col);

    const float current_mask = *vd.mask;
    const float new_mask = sculpt_mask_by_color_delta_get(active_color, col, threshold, invert);
    *vd.mask = sculpt_mask_by_color_final_mask_get(current_mask, new_mask, invert, preserve_mask);

    if (current_mask == *vd.mask) {
      continue;
    }
    update_node = true;
  }
  BKE_pbvh_vertex_iter_end;
  if (update_node) {
    BKE_pbvh_node_mark_update_mask(data->nodes[n]);
  }
}

static void sculpt_mask_by_color_full_mesh(Object *object,
                                           const PBVHVertRef vertex,
                                           const float threshold,
                                           const bool invert,
                                           const bool preserve_mask)
{
  SculptSession *ss = object->sculpt;

  int totnode;
  PBVHNode **nodes;
  BKE_pbvh_search_gather(ss->pbvh, NULL, NULL, &nodes, &totnode);

  SculptThreadedTaskData data = {
      .ob = object,
      .nodes = nodes,
      .mask_by_color_vertex = vertex,
      .mask_by_color_threshold = threshold,
      .mask_by_color_invert = invert,
      .mask_by_color_preserve_mask = preserve_mask,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &data, do_mask_by_color_task_cb, &settings);

  MEM_SAFE_FREE(nodes);
}

static int sculpt_mask_by_color_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  View3D *v3d = CTX_wm_view3d(C);
  if (v3d->shading.type == OB_SOLID) {
    v3d->shading.color_type = V3D_SHADING_VERTEX_COLOR;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);

  /* Color data is not available in multi-resolution or dynamic topology. */
  if (!SCULPT_handles_colors_report(ss, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  SCULPT_vertex_random_access_ensure(ss);

  /* Tools that are not brushes do not have the brush gizmo to update the vertex as the mouse move,
   * so it needs to be updated here. */
  SculptCursorGeometryInfo sgi;
  const float mval_fl[2] = {UNPACK2(event->mval)};
  SCULPT_cursor_geometry_info_update(C, &sgi, mval_fl, false);

  SCULPT_undo_push_begin(ob, op);
  BKE_sculpt_color_layer_create_if_needed(ob);

  const PBVHVertRef active_vertex = SCULPT_active_vertex_get(ss);
  const float threshold = RNA_float_get(op->ptr, "threshold");
  const bool invert = RNA_boolean_get(op->ptr, "invert");
  const bool preserve_mask = RNA_boolean_get(op->ptr, "preserve_previous_mask");

  if (SCULPT_has_loop_colors(ob)) {
    BKE_pbvh_ensure_node_loops(ss->pbvh);
  }

  if (RNA_boolean_get(op->ptr, "contiguous")) {
    sculpt_mask_by_color_contiguous(ob, active_vertex, threshold, invert, preserve_mask);
  }
  else {
    sculpt_mask_by_color_full_mesh(ob, active_vertex, threshold, invert, preserve_mask);
  }

  BKE_pbvh_update_vertex_data(ss->pbvh, PBVH_UpdateMask);
  SCULPT_undo_push_end(ob);

  SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);
  DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);

  return OPERATOR_FINISHED;
}

static void SCULPT_OT_mask_by_color(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Mask by Color";
  ot->idname = "SCULPT_OT_mask_by_color";
  ot->description = "Creates a mask based on the active color attribute";

  /* api callbacks */
  ot->invoke = sculpt_mask_by_color_invoke;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER;

  ot->prop = RNA_def_boolean(
      ot->srna, "contiguous", false, "Contiguous", "Mask only contiguous color areas");

  ot->prop = RNA_def_boolean(ot->srna, "invert", false, "Invert", "Invert the generated mask");
  ot->prop = RNA_def_boolean(
      ot->srna,
      "preserve_previous_mask",
      false,
      "Preserve Previous Mask",
      "Preserve the previous mask and add or subtract the new one generated by the colors");

  RNA_def_float(ot->srna,
                "threshold",
                0.35f,
                0.0f,
                1.0f,
                "Threshold",
                "How much changes in color affect the mask generation",
                0.0f,
                1.0f);
}

typedef enum {
  AUTOMASK_BAKE_MIX,
  AUTOMASK_BAKE_MULTIPLY,
  AUTOMASK_BAKE_DIVIDE,
  AUTOMASK_BAKE_ADD,
  AUTOMASK_BAKE_SUBTRACT,
} CavityBakeMixMode;

typedef struct AutomaskBakeTaskData {
  SculptSession *ss;
  AutomaskingCache *automasking;
  PBVHNode **nodes;
  CavityBakeMixMode mode;
  float factor;
  Object *ob;
} AutomaskBakeTaskData;

static void sculpt_bake_cavity_exec_task_cb(void *__restrict userdata,
                                            const int n,
                                            const TaskParallelTLS *__restrict UNUSED(tls))
{
  AutomaskBakeTaskData *tdata = userdata;
  SculptSession *ss = tdata->ss;
  PBVHNode *node = tdata->nodes[n];
  PBVHVertexIter vd;
  const CavityBakeMixMode mode = tdata->mode;
  const float factor = tdata->factor;

  SCULPT_undo_push_node(tdata->ob, node, SCULPT_UNDO_MASK);

  AutomaskingNodeData automask_data;
  SCULPT_automasking_node_begin(tdata->ob, ss, tdata->automasking, &automask_data, node);

  BKE_pbvh_vertex_iter_begin (ss->pbvh, node, vd, PBVH_ITER_UNIQUE) {
    SCULPT_automasking_node_update(ss, &automask_data, &vd);

    float automask = SCULPT_automasking_factor_get(
        tdata->automasking, ss, vd.vertex, &automask_data);
    float mask;

    switch (mode) {
      case AUTOMASK_BAKE_MIX:
        mask = automask;
        break;
      case AUTOMASK_BAKE_MULTIPLY:
        mask = *vd.mask * automask;
        break;
        break;
      case AUTOMASK_BAKE_DIVIDE:
        mask = automask > 0.00001f ? *vd.mask / automask : 0.0f;
        break;
        break;
      case AUTOMASK_BAKE_ADD:
        mask = *vd.mask + automask;
        break;
      case AUTOMASK_BAKE_SUBTRACT:
        mask = *vd.mask - automask;
        break;
    }

    mask = *vd.mask + (mask - *vd.mask) * factor;
    CLAMP(mask, 0.0f, 1.0f);

    *vd.mask = mask;
  }
  BKE_pbvh_vertex_iter_end;

  BKE_pbvh_node_mark_update_mask(node);
}

static int sculpt_bake_cavity_exec(bContext *C, wmOperator *op)
{
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);
  SCULPT_vertex_random_access_ensure(ss);

  MultiresModifierData *mmd = BKE_sculpt_multires_active(CTX_data_scene(C), ob);
  BKE_sculpt_mask_layers_ensure(ob, mmd);

  SCULPT_undo_push_begin(ob, op);

  CavityBakeMixMode mode = RNA_enum_get(op->ptr, "mix_mode");
  float factor = RNA_float_get(op->ptr, "mix_factor");

  PBVHNode **nodes;
  int totnode;

  BKE_pbvh_search_gather(ss->pbvh, NULL, NULL, &nodes, &totnode);

  AutomaskBakeTaskData tdata;

  /* Set up automasking settings.
   */
  Sculpt sd2 = *sd;

  /* Override cavity mask settings if use_automask_settings is false. */
  if (!RNA_boolean_get(op->ptr, "use_automask_settings")) {
    if (RNA_boolean_get(op->ptr, "invert")) {
      sd2.automasking_flags = BRUSH_AUTOMASKING_CAVITY_INVERTED;
    }
    else {
      sd2.automasking_flags = BRUSH_AUTOMASKING_CAVITY_NORMAL;
    }

    if (RNA_boolean_get(op->ptr, "use_curve")) {
      sd2.automasking_flags |= BRUSH_AUTOMASKING_CAVITY_USE_CURVE;
    }

    sd2.automasking_cavity_blur_steps = RNA_int_get(op->ptr, "blur_steps");
    sd2.automasking_cavity_factor = RNA_float_get(op->ptr, "factor");

    sd2.automasking_cavity_curve = sd->automasking_cavity_curve_op;
  }
  else {
    sd2.automasking_flags &= BRUSH_AUTOMASKING_CAVITY_ALL | BRUSH_AUTOMASKING_CAVITY_USE_CURVE;

    /* Ensure cavity mask is actually enabled. */
    if (!(sd2.automasking_flags & BRUSH_AUTOMASKING_CAVITY_ALL)) {
      sd2.automasking_flags |= BRUSH_AUTOMASKING_CAVITY_NORMAL;
    }
  }

  /* Create copy of brush with cleared automasking settings. */
  Brush brush2 = *brush;
  brush2.automasking_flags = 0;
  brush2.automasking_boundary_edges_propagation_steps = 1;
  brush2.automasking_cavity_curve = sd2.automasking_cavity_curve;

  SCULPT_stroke_id_next(ob);

  tdata.ob = ob;
  tdata.mode = mode;
  tdata.factor = factor;
  tdata.ss = ss;
  tdata.nodes = nodes;
  tdata.automasking = SCULPT_automasking_cache_init(&sd2, &brush2, ob);

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);
  BLI_task_parallel_range(0, totnode, &tdata, sculpt_bake_cavity_exec_task_cb, &settings);

  MEM_SAFE_FREE(nodes);
  SCULPT_automasking_cache_free(tdata.automasking);

  BKE_pbvh_update_vertex_data(ss->pbvh, PBVH_UpdateMask);
  SCULPT_undo_push_end(ob);

  SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);

  /* Unlike other operators we do not tag the ID for update here;
   * it triggers a PBVH rebuild which is too slow and ruins
   * the interactivity of the tool. */

  return OPERATOR_FINISHED;
}

static void cavity_bake_ui(bContext *C, wmOperator *op)
{
  uiLayout *layout = op->layout;
  Scene *scene = CTX_data_scene(C);
  Sculpt *sd = scene->toolsettings ? scene->toolsettings->sculpt : NULL;

  uiLayoutSetPropSep(layout, true);
  uiLayoutSetPropDecorate(layout, false);
  bool use_curve;

  if (!sd || !RNA_boolean_get(op->ptr, "use_automask_settings")) {
    uiItemR(layout, op->ptr, "mix_mode", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "mix_factor", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "use_automask_settings", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "factor", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "blur_steps", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "invert", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "use_curve", 0, NULL, ICON_NONE);

    use_curve = RNA_boolean_get(op->ptr, "use_curve");
  }
  else {
    PointerRNA sculpt_ptr;

    RNA_pointer_create(&scene->id, &RNA_Sculpt, sd, &sculpt_ptr);
    uiItemR(layout, op->ptr, "mix_mode", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "mix_factor", 0, NULL, ICON_NONE);
    uiItemR(layout, op->ptr, "use_automask_settings", 0, NULL, ICON_NONE);

    use_curve = RNA_boolean_get(&sculpt_ptr, "use_automasking_custom_cavity_curve");
  }

  if (use_curve) {
    PointerRNA sculpt_ptr;

    const char *curve_prop;

    if (RNA_boolean_get(op->ptr, "use_automask_settings")) {
      curve_prop = "automasking_cavity_curve";
    }
    else {
      curve_prop = "automasking_cavity_curve_op";
    }

    if (scene->toolsettings && scene->toolsettings->sculpt) {
      RNA_pointer_create(&scene->id, &RNA_Sculpt, scene->toolsettings->sculpt, &sculpt_ptr);
      uiTemplateCurveMapping(layout, &sculpt_ptr, curve_prop, 'v', false, false, false, false);
    }
  }
}

static void SCULPT_OT_mask_from_cavity(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Mask From Cavity";
  ot->idname = "SCULPT_OT_mask_from_cavity";
  ot->description = "Creates a mask based on the curvature of the surface";
  ot->ui = cavity_bake_ui;

  static EnumPropertyItem mix_modes[] = {
      {AUTOMASK_BAKE_MIX, "MIX", ICON_NONE, "Mix", ""},
      {AUTOMASK_BAKE_MULTIPLY, "MULTIPLY", ICON_NONE, "Multiply", ""},
      {AUTOMASK_BAKE_DIVIDE, "DIVIDE", ICON_NONE, "Divide", ""},
      {AUTOMASK_BAKE_ADD, "ADD", ICON_NONE, "Add", ""},
      {AUTOMASK_BAKE_SUBTRACT, "SUBTRACT", ICON_NONE, "Subtract", ""},
      {0, NULL, 0, NULL, NULL},
  };

  /* api callbacks */
  ot->exec = sculpt_bake_cavity_exec;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna, "mix_mode", mix_modes, AUTOMASK_BAKE_MIX, "Mode", "Mix mode");
  RNA_def_float(ot->srna, "mix_factor", 1.0f, 0.0f, 5.0f, "Mix Factor", "", 0.0f, 1.0f);

  RNA_def_boolean(ot->srna,
                  "use_automask_settings",
                  false,
                  "Use Automask Settings",
                  "Use default settings from Options panel in sculpt mode");

  RNA_def_float(ot->srna,
                "factor",
                0.5f,
                0.0f,
                5.0f,
                "Cavity Factor",
                "The contrast of the cavity mask",
                0.0f,
                1.0f);
  RNA_def_int(ot->srna,
              "blur_steps",
              2,
              0,
              25,
              "Cavity Blur",
              "The number of times the cavity mask is blurred",
              0,
              25);
  RNA_def_boolean(ot->srna, "use_curve", false, "Use Curve", "");

  RNA_def_boolean(ot->srna, "invert", false, "Cavity (Inverted)", "");
}

void ED_operatortypes_sculpt(void)
{
  WM_operatortype_append(SCULPT_OT_brush_stroke);
  WM_operatortype_append(SCULPT_OT_sculptmode_toggle);
  WM_operatortype_append(SCULPT_OT_set_persistent_base);
  WM_operatortype_append(SCULPT_OT_dynamic_topology_toggle);
  WM_operatortype_append(SCULPT_OT_optimize);
  WM_operatortype_append(SCULPT_OT_symmetrize);
  WM_operatortype_append(SCULPT_OT_detail_flood_fill);
  WM_operatortype_append(SCULPT_OT_sample_detail_size);
  WM_operatortype_append(SCULPT_OT_set_detail_size);
  WM_operatortype_append(SCULPT_OT_mesh_filter);
  WM_operatortype_append(SCULPT_OT_mask_filter);
  WM_operatortype_append(SCULPT_OT_mask_expand);
  WM_operatortype_append(SCULPT_OT_set_pivot_position);
  WM_operatortype_append(SCULPT_OT_face_sets_create);
  WM_operatortype_append(SCULPT_OT_face_sets_change_visibility);
  WM_operatortype_append(SCULPT_OT_face_sets_randomize_colors);
  WM_operatortype_append(SCULPT_OT_face_sets_init);
  WM_operatortype_append(SCULPT_OT_cloth_filter);
  WM_operatortype_append(SCULPT_OT_face_sets_edit);
  WM_operatortype_append(SCULPT_OT_face_set_lasso_gesture);
  WM_operatortype_append(SCULPT_OT_face_set_box_gesture);
  WM_operatortype_append(SCULPT_OT_trim_box_gesture);
  WM_operatortype_append(SCULPT_OT_trim_lasso_gesture);
  WM_operatortype_append(SCULPT_OT_project_line_gesture);

  WM_operatortype_append(SCULPT_OT_sample_color);
  WM_operatortype_append(SCULPT_OT_color_filter);
  WM_operatortype_append(SCULPT_OT_mask_by_color);
  WM_operatortype_append(SCULPT_OT_dyntopo_detail_size_edit);
  WM_operatortype_append(SCULPT_OT_mask_init);

  WM_operatortype_append(SCULPT_OT_expand);
  WM_operatortype_append(SCULPT_OT_mask_from_cavity);
}
