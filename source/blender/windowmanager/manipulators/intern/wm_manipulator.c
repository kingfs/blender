/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2014 Blender Foundation.
 * All rights reserved.
 *
 * Contributor(s): Blender Foundation
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/windowmanager/manipulators/intern/wm_manipulator.c
 *  \ingroup wm
 */

#include "BKE_context.h"

#include "BLI_listbase.h"
#include "BLI_math.h"
#include "BLI_path_util.h"
#include "BLI_string.h"

#include "ED_screen.h"
#include "ED_view3d.h"

#include "GL/glew.h"

#include "MEM_guardedalloc.h"

#include "RNA_access.h"

#include "WM_api.h"
#include "WM_types.h"

/* own includes */
#include "wm_manipulator_wmapi.h"
#include "wm_manipulator_intern.h"

/**
 * Main draw call for ManipulatorDrawInfo data
 */
void manipulator_draw_intern(ManipulatorDrawInfo *info, const bool select)
{
	GLuint buf[3];

	const bool use_lighting = !select && ((U.widget_flag & V3D_SHADED_WIDGETS) != 0);

	if (use_lighting)
		glGenBuffers(3, buf);
	else
		glGenBuffers(2, buf);

	glEnableClientState(GL_VERTEX_ARRAY);
	glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * info->nverts, info->verts, GL_STATIC_DRAW);
	glVertexPointer(3, GL_FLOAT, 0, NULL);

	if (use_lighting) {
		glEnableClientState(GL_NORMAL_ARRAY);
		glBindBuffer(GL_ARRAY_BUFFER, buf[2]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * info->nverts, info->normals, GL_STATIC_DRAW);
		glNormalPointer(GL_FLOAT, 0, NULL);
		glShadeModel(GL_SMOOTH);
	}

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf[1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned short) * (3 * info->ntris), info->indices, GL_STATIC_DRAW);

	glEnable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);

	glDrawElements(GL_TRIANGLES, info->ntris * 3, GL_UNSIGNED_SHORT, NULL);

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glDisableClientState(GL_VERTEX_ARRAY);

	if (use_lighting) {
		glDisableClientState(GL_NORMAL_ARRAY);
		glShadeModel(GL_FLAT);
		glDeleteBuffers(3, buf);
	}
	else {
		glDeleteBuffers(2, buf);
	}
}

/* Still unused */
wmManipulator *WM_manipulator_new(void (*draw)(const bContext *C, wmManipulator *customdata),
                        void (*render_3d_intersection)(const bContext *C, wmManipulator *customdata, int selectionbase),
                        int  (*intersect)(bContext *C, const wmEvent *event, wmManipulator *widget),
                        int  (*handler)(bContext *C, const wmEvent *event, wmManipulator *widget, const int flag))
{
	wmManipulator *widget = MEM_callocN(sizeof(wmManipulator), "widget");

	widget->draw = draw;
	widget->handler = handler;
	widget->intersect = intersect;
	widget->render_3d_intersection = render_3d_intersection;

	/* XXX */
	fix_linking_manipulator_arrow();
	fix_linking_manipulator_arrow2d();
	fix_linking_manipulator_cage();
	fix_linking_manipulator_dial();
//	fix_linking_manipulator_facemap();
	fix_linking_manipulator_primitive();

	return widget;
}

/**
 * Assign an idname that is unique in \a wgroup to \a widget.
 *
 * \param rawname  Name used as basis to define final unique idname.
 */
static void manipulator_unique_idname_set(wmManipulatorGroup *wgroup, wmManipulator *widget, const char *rawname)
{
	if (wgroup->type->idname[0]) {
		BLI_snprintf(widget->idname, sizeof(widget->idname), "%s_%s", wgroup->type->idname, rawname);
	}
	else {
		BLI_strncpy(widget->idname, rawname, sizeof(widget->idname));
	}

	/* ensure name is unique, append '.001', '.002', etc if not */
	BLI_uniquename(&wgroup->widgets, widget, "Widget", '.', offsetof(wmManipulator, idname), sizeof(widget->idname));
}

/**
 * Register \a widget.
 *
 * \param name  name used to create a unique idname for \a widget in \a wgroup
 */
bool WM_manipulator_register(wmManipulatorGroup *wgroup, wmManipulator *widget, const char *name)
{
	const float col_default[4] = {1.0f, 1.0f, 1.0f, 1.0f};

	manipulator_unique_idname_set(wgroup, widget, name);

	widget->user_scale = 1.0f;
	widget->line_width = 1.0f;

	/* defaults */
	copy_v4_v4(widget->col, col_default);
	copy_v4_v4(widget->col_hi, col_default);

	/* create at least one property for interaction */
	if (widget->max_prop == 0) {
		widget->max_prop = 1;
	}

	widget->props = MEM_callocN(sizeof(PropertyRNA *) * widget->max_prop, "widget->props");
	widget->ptr = MEM_callocN(sizeof(PointerRNA) * widget->max_prop, "widget->ptr");

	widget->wgroup = wgroup;

	BLI_addtail(&wgroup->widgets, widget);
	return true;
}

/**
 * Free \a widget and unlink from \a widgetlist.
 * \a widgetlist is allowed to be NULL.
 */
void WM_manipulator_delete(ListBase *widgetlist, wmManipulatorMap *wmap, wmManipulator *widget, bContext *C)
{
	if (widget->flag & WM_MANIPULATOR_HIGHLIGHT) {
		wm_manipulatormap_set_highlighted_widget(wmap, C, NULL, 0);
	}
	if (widget->flag & WM_MANIPULATOR_ACTIVE) {
		wm_manipulatormap_set_active_widget(wmap, C, NULL, NULL);
	}
	if (widget->flag & WM_MANIPULATOR_SELECTED) {
		WM_manipulator_deselect(wmap, widget);
	}

	if (widget->opptr.data) {
		WM_operator_properties_free(&widget->opptr);
	}
	MEM_freeN(widget->props);
	MEM_freeN(widget->ptr);

	if (widgetlist)
		BLI_remlink(widgetlist, widget);
	MEM_freeN(widget);
}


/* -------------------------------------------------------------------- */
/** \name Widget Creation API
 *
 * API for defining data on widget creation.
 *
 * \{ */

void WM_manipulator_set_property(wmManipulator *widget, const int slot, PointerRNA *ptr, const char *propname)
{
	if (slot < 0 || slot >= widget->max_prop) {
		fprintf(stderr, "invalid index %d when binding property for widget type %s\n", slot, widget->idname);
		return;
	}

	/* if widget evokes an operator we cannot use it for property manipulation */
	widget->opname = NULL;
	widget->ptr[slot] = *ptr;
	widget->props[slot] = RNA_struct_find_property(ptr, propname);

	if (widget->prop_data_update)
		widget->prop_data_update(widget, slot);
}

PointerRNA *WM_manipulator_set_operator(wmManipulator *widget, const char *opname)
{
	wmOperatorType *ot = WM_operatortype_find(opname, 0);

	if (ot) {
		widget->opname = opname;

		if (widget->opptr.data) {
			WM_operator_properties_free(&widget->opptr);
		}
		WM_operator_properties_create_ptr(&widget->opptr, ot);

		return &widget->opptr;
	}
	else {
		fprintf(stderr, "Error binding operator to widget: operator %s not found!\n", opname);
	}

	return NULL;
}

/**
 * \brief Set widget select callback.
 *
 * Callback is called when widget gets selected/deselected.
 */
void WM_manipulator_set_func_select(wmManipulator *widget, wmManipulatorSelectFunc select)
{
	widget->flag |= WM_MANIPULATOR_SELECTABLE;
	widget->select = select;
}

void WM_manipulator_set_origin(wmManipulator *widget, const float origin[3])
{
	copy_v3_v3(widget->origin, origin);
}

void WM_manipulator_set_offset(wmManipulator *widget, const float offset[3])
{
	copy_v3_v3(widget->offset, offset);
}

void WM_manipulator_set_flag(wmManipulator *widget, const int flag, const bool enable)
{
	if (enable) {
		widget->flag |= flag;
	}
	else {
		widget->flag &= ~flag;
	}
}

void WM_manipulator_set_scale(wmManipulator *widget, const float scale)
{
	widget->user_scale = scale;
}

void WM_manipulator_set_line_width(wmManipulator *widget, const float line_width)
{
	widget->line_width = line_width;
}

/**
 * Set widget rgba colors.
 *
 * \param col  Normal state color.
 * \param col_hi  Highlighted state color.
 */
void WM_manipulator_set_colors(wmManipulator *widget, const float col[4], const float col_hi[4])
{
	copy_v4_v4(widget->col, col);
	copy_v4_v4(widget->col_hi, col_hi);
}

/** \} */ // Widget Creation API


/* -------------------------------------------------------------------- */

/**
 * Remove \a widget from selection.
 * Reallocates memory for selected widgets so better not call for selecting multiple ones.
 *
 * \return if the selection has changed.
 */
bool WM_manipulator_deselect(wmManipulatorMap *wmap, wmManipulator *widget)
{
	if (!wmap->wmap_context.selected_widgets)
		return false;

	wmManipulator ***sel = &wmap->wmap_context.selected_widgets;
	int *tot_selected = &wmap->wmap_context.tot_selected;
	bool changed = false;

	/* caller should check! */
	BLI_assert(widget->flag & WM_MANIPULATOR_SELECTED);

	/* remove widget from selected_widgets array */
	for (int i = 0; i < (*tot_selected); i++) {
		if ((*sel)[i] == widget) {
			for (int j = i; j < ((*tot_selected) - 1); j++) {
				(*sel)[j] = (*sel)[j + 1];
			}
			changed = true;
			break;
		}
	}

	/* update array data */
	if ((*tot_selected) <= 1) {
		WM_manipulatormap_selected_delete(wmap);
	}
	else {
		*sel = MEM_reallocN(*sel, sizeof(**sel) * (*tot_selected));
		(*tot_selected)--;
	}

	widget->flag &= ~WM_MANIPULATOR_SELECTED;
	return changed;
}

/**
 * Add \a widget to selection.
 * Reallocates memory for selected widgets so better not call for selecting multiple ones.
 *
 * \return if the selection has changed.
 */
bool WM_manipulator_select(bContext *C, wmManipulatorMap *wmap, wmManipulator *widget)
{
	wmManipulator ***sel = &wmap->wmap_context.selected_widgets;
	int *tot_selected = &wmap->wmap_context.tot_selected;

	if (!widget || (widget->flag & WM_MANIPULATOR_SELECTED))
		return false;

	(*tot_selected)++;

	*sel = MEM_reallocN(*sel, sizeof(wmManipulator *) * (*tot_selected));
	(*sel)[(*tot_selected) - 1] = widget;

	widget->flag |= WM_MANIPULATOR_SELECTED;
	if (widget->select) {
		widget->select(C, widget, SEL_SELECT);
	}
	wm_manipulatormap_set_highlighted_widget(wmap, C, widget, widget->highlighted_part);

	return true;
}

void WM_manipulator_calculate_scale(wmManipulator *widget, const bContext *C)
{
	const RegionView3D *rv3d = CTX_wm_region_view3d(C);
	float scale = 1.0f;

	if (widget->flag & WM_MANIPULATOR_SCALE_3D) {
		if (rv3d && (U.widget_flag & V3D_3D_WIDGETS) == 0) {
			if (widget->get_final_position) {
				float position[3];

				widget->get_final_position(widget, position);
				scale = ED_view3d_pixel_size(rv3d, position) * (float)U.widget_scale;
			}
			else {
				scale = ED_view3d_pixel_size(rv3d, widget->origin) * (float)U.widget_scale;
			}
		}
		else {
			scale = U.widget_scale * 0.02f;
		}
	}

	widget->scale = scale * widget->user_scale;
}

void WM_manipulator_update_prop_data(wmManipulator *widget)
{
	/* widget property might have been changed, so update widget */
	if (widget->props && widget->prop_data_update) {
		for (int i = 0; i < widget->max_prop; i++) {
			if (widget->props[i]) {
				widget->prop_data_update(widget, i);
			}
		}
	}
}

