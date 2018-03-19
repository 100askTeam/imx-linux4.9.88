/*
 * Copyright 2017 NXP
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <linux/dma-buf.h>
#include <linux/reservation.h>
#include <linux/sort.h>
#include <video/dpu.h>
#include "dpu-plane.h"
#include "imx-drm.h"

static void dpu_drm_output_poll_changed(struct drm_device *dev)
{
	struct imx_drm_device *imxdrm = dev->dev_private;

	drm_fbdev_cma_hotplug_event(imxdrm->fbhelper);
}

static struct drm_plane_state **
dpu_atomic_alloc_tmp_planes_per_crtc(struct drm_device *dev)
{
	int total_planes = dev->mode_config.num_total_plane;
	struct drm_plane_state **states;

	states = kmalloc_array(total_planes, sizeof(*states), GFP_TEMPORARY);
	if (!states)
		return ERR_PTR(-ENOMEM);

	return states;
}

static int zpos_cmp(const void *a, const void *b)
{
	const struct drm_plane_state *sa = *(struct drm_plane_state **)a;
	const struct drm_plane_state *sb = *(struct drm_plane_state **)b;

	return sa->normalized_zpos - sb->normalized_zpos;
}

static int dpu_atomic_sort_planes_per_crtc(struct drm_crtc_state *crtc_state,
					   struct drm_plane_state **states)
{
	struct drm_atomic_state *state = crtc_state->state;
	struct drm_device *dev = state->dev;
	struct drm_plane *plane;
	int n = 0;

	drm_for_each_plane_mask(plane, dev, crtc_state->plane_mask) {
		struct drm_plane_state *plane_state =
			drm_atomic_get_plane_state(state, plane);
		if (IS_ERR(plane_state))
			return PTR_ERR(plane_state);
		states[n++] = plane_state;
	}

	sort(states, n, sizeof(*states), zpos_cmp, NULL);

	return n;
}

static int
dpu_atomic_compute_plane_base_per_crtc(struct drm_plane_state **states, int n)
{
	struct dpu_plane_state *dpstate;
	int i, left, right, top, bottom, tmp;

	/* compute the plane base */
	left   = states[0]->crtc_x;
	top    = states[0]->crtc_y;
	right  = states[0]->crtc_x + states[0]->crtc_w;
	bottom = states[0]->crtc_y + states[0]->crtc_h;

	for (i = 1; i < n; i++) {
		left = min(states[i]->crtc_x, left);
		top =  min(states[i]->crtc_y, top);

		tmp = states[i]->crtc_x + states[i]->crtc_w;
		right = max(tmp, right);

		tmp = states[i]->crtc_y + states[i]->crtc_h;
		bottom = max(tmp, bottom);
	}

	/* BTW, be smart to compute the layer offset */
	for (i = 0; i < n; i++) {
		dpstate = to_dpu_plane_state(states[i]);
		dpstate->layer_x = states[i]->crtc_x - left;
		dpstate->layer_y = states[i]->crtc_y - top;
	}

	/* finally, store the base in plane state */
	dpstate = to_dpu_plane_state(states[0]);
	dpstate->base_x = left;
	dpstate->base_y = top;
	dpstate->base_w = right - left;
	dpstate->base_h = bottom - top;

	return 0;
}

static void
dpu_atomic_set_top_plane_per_crtc(struct drm_plane_state **states, int n)
{
	struct dpu_plane_state *dpstate;
	int i;

	for (i = 0; i < n; i++) {
		dpstate = to_dpu_plane_state(states[i]);
		dpstate->is_top = (i == (n - 1)) ? true : false;
	}
}

static void
dpu_atomic_assign_plane_source_per_crtc(struct drm_plane_state **states, int n)
{
	struct dpu_plane_state *dpstate;
	struct dpu_plane *dplane;
	unsigned int sid;
	int i, j;

	/* for active planes only */
	for (i = 0; i < n; i++) {
		dpstate = to_dpu_plane_state(states[i]);
		dplane = to_dpu_plane(states[i]->plane);
		sid = dplane->stream_id;

		if (sid) {
			j = dplane->grp->hw_plane_num - (n - i);
			dpstate->stage = i ? stages[j - 1] : cf_stages[sid];
			dpstate->source = sources[j];
			dpstate->blend = blends[j];
		} else {
			dpstate->stage = i ? stages[i - 1] : cf_stages[sid];
			dpstate->source = sources[i];
			dpstate->blend = blends[i];
		}
	}
}

static int dpu_drm_atomic_check(struct drm_device *dev,
				struct drm_atomic_state *state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_plane *plane;
	struct dpu_plane *dpu_plane;
	struct dpu_plane_grp *grp[MAX_DPU_PLANE_GRP];
	int ret, i, grp_id, active_plane[MAX_DPU_PLANE_GRP];

	for (i = 0; i < MAX_DPU_PLANE_GRP; i++) {
		active_plane[i] = 0;
		grp[i] = NULL;
	}

	drm_for_each_crtc(crtc, dev) {
		crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(crtc_state))
			return PTR_ERR(crtc_state);

		ret = drm_atomic_add_affected_planes(state, crtc);
		if (ret)
			return ret;

		drm_atomic_crtc_state_for_each_plane(plane, crtc_state) {
			dpu_plane = to_dpu_plane(plane);
			grp_id = dpu_plane->grp->id;
			active_plane[grp_id]++;

			if (grp[grp_id] == NULL)
				grp[grp_id] = dpu_plane->grp;
		}
	}

	for (i = 0; i < MAX_DPU_PLANE_GRP; i++)
		if (grp[i] && active_plane[i] > grp[i]->hw_plane_num)
			return -EINVAL;

	ret = drm_atomic_helper_check_modeset(dev, state);
	if (ret)
		return ret;

	ret = drm_atomic_normalize_zpos(dev, state);
	if (ret)
		return ret;

	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		struct drm_plane_state **states;
		int n;

		states = dpu_atomic_alloc_tmp_planes_per_crtc(dev);
		if (IS_ERR(states))
			return PTR_ERR(states);

		n = dpu_atomic_sort_planes_per_crtc(crtc_state, states);
		if (n < 0) {
			kfree(states);
			return n;
		}

		/* no active planes? */
		if (n == 0) {
			kfree(states);
			continue;
		}

		/* 'zpos = 0' means primary plane */
		if (states[0]->plane->type != DRM_PLANE_TYPE_PRIMARY) {
			kfree(states);
			return -EINVAL;
		}

		ret = dpu_atomic_compute_plane_base_per_crtc(states, n);
		if (ret) {
			kfree(states);
			return ret;
		}

		dpu_atomic_set_top_plane_per_crtc(states, n);

		dpu_atomic_assign_plane_source_per_crtc(states, n);

		kfree(states);
	}

	ret = drm_atomic_helper_check_planes(dev, state);
	if (ret)
		return ret;

	return ret;
}

static int dpu_drm_atomic_commit(struct drm_device *dev,
				 struct drm_atomic_state *state,
				 bool nonblock)
{
	struct drm_plane_state *plane_state;
	struct drm_plane *plane;
	struct dma_buf *dma_buf;
	int i;

	/*
	 * If the plane fb has an dma-buf attached, fish out the exclusive
	 * fence for the atomic helper to wait on.
	 */
	for_each_plane_in_state(state, plane, plane_state, i) {
		if ((plane->state->fb != plane_state->fb) && plane_state->fb) {
			dma_buf = drm_fb_cma_get_gem_obj(plane_state->fb,
							 0)->base.dma_buf;
			if (!dma_buf)
				continue;
			plane_state->fence =
				reservation_object_get_excl_rcu(dma_buf->resv);
		}
	}

	return drm_atomic_helper_commit(dev, state, nonblock);
}

const struct drm_mode_config_funcs dpu_drm_mode_config_funcs = {
	.fb_create = drm_fb_cma_create,
	.output_poll_changed = dpu_drm_output_poll_changed,
	.atomic_check = dpu_drm_atomic_check,
	.atomic_commit = dpu_drm_atomic_commit,
};
