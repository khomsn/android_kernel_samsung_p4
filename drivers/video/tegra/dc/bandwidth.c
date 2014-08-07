/*
 * drivers/video/tegra/dc/bandwidth.c
 *
 * Copyright (C) 2010-2012 NVIDIA Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/math64.h>

#include <mach/clk.h>
#include <mach/dc.h>
#include <mach/fb.h>
#include <mach/mc.h>
#include <linux/nvhost.h>
#include <mach/latency_allowance.h>

#include "dc_reg.h"
#include "dc_config.h"
#include "dc_priv.h"

static int use_dynamic_emc = 1;

module_param_named(use_dynamic_emc, use_dynamic_emc, int, S_IRUGO | S_IWUSR);

/* uses the larger of w->bandwidth or w->new_bandwidth */
static void tegra_dc_set_latency_allowance(struct tegra_dc *dc,
	struct tegra_dc_win *w)
{
	/* windows A, B, C for first and second display */
	static const enum tegra_la_id la_id_tab[2][3] = {
		/* first display */
		{ TEGRA_LA_DISPLAY_0A, TEGRA_LA_DISPLAY_0B,
			TEGRA_LA_DISPLAY_0C },
		/* second display */
		{ TEGRA_LA_DISPLAY_0AB, TEGRA_LA_DISPLAY_0BB,
			TEGRA_LA_DISPLAY_0CB },
	};
#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || defined(CONFIG_ARCH_TEGRA_3x_SOC)
	/* window B V-filter tap for first and second display. */
	static const enum tegra_la_id vfilter_tab[2] = {
		TEGRA_LA_DISPLAY_1B, TEGRA_LA_DISPLAY_1BB,
	};
#endif
	unsigned long bw;

	BUG_ON(dc->ndev->id >= ARRAY_SIZE(la_id_tab));
#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || defined(CONFIG_ARCH_TEGRA_3x_SOC)
	BUG_ON(dc->ndev->id >= ARRAY_SIZE(vfilter_tab));
#endif
	BUG_ON(w->idx >= ARRAY_SIZE(*la_id_tab));

	bw = w->new_bandwidth;

#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || defined(CONFIG_ARCH_TEGRA_3x_SOC)
	/* tegra_dc_get_bandwidth() treats V filter windows as double
	 * bandwidth, but LA has a seperate client for V filter */
	if (w->idx == 1 && win_use_v_filter(dc, w))
		bw /= 2;
#endif

	/* our bandwidth is in kbytes/sec, but LA takes MBps.
	 * round up bandwidth to next 1MBps */
	bw = bw / 1000 + 1;

#ifdef CONFIG_TEGRA_SILICON_PLATFORM
	tegra_set_latency_allowance(la_id_tab[dc->ndev->id][w->idx], bw);
#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || defined(CONFIG_ARCH_TEGRA_3x_SOC)
	/* if window B, also set the 1B client for the 2-tap V filter. */
	if (w->idx == 1)
		tegra_set_latency_allowance(vfilter_tab[dc->ndev->id], bw);
#endif
#endif
}

/* determine if a row is within a window */
static int tegra_dc_line_in_window(int line, struct tegra_dc_win *win)
{
	int win_first = win->out_y;
	int win_last = win_first + win->out_h - 1;

	return (line >= win_first && line <= win_last);
}

/* check overlapping window combinations to find the max bandwidth. */
static unsigned long tegra_dc_find_max_bandwidth(struct tegra_dc_win *wins[],
						 unsigned n)
{
	unsigned i;
	unsigned j;
	long max = 0;

	for (i = 0; i < n; i++) {
		struct tegra_dc_win *a = wins[i];
		long bw = 0;
		int a_first = a->out_y;

		if (!WIN_IS_ENABLED(a))
			continue;
		for (j = 0; j < n; j++) {
			struct tegra_dc_win *b = wins[j];

			if (!WIN_IS_ENABLED(b))
				continue;
			if (tegra_dc_line_in_window(a_first, b))
				bw += b->new_bandwidth;
		}
		if (max < bw)
			max = bw;
	}

	return max;
}

/*
 * Calculate peak EMC bandwidth for each enabled window =
 * pixel_clock * win_bpp * (use_v_filter ? 2 : 1)) * H_scale_factor *
 * (windows_tiling ? 2 : 1)
 *
 * note:
 * (*) We use 2 tap V filter on T2x/T3x, so need double BW if use V filter
 * (*) Tiling mode on T30 and DDR3 requires double BW
 *
 * return:
 * bandwidth in kBps
 */
static unsigned long tegra_dc_calc_win_bandwidth(struct tegra_dc *dc,
	struct tegra_dc_win *w)
{
	u64 ret;
	int tiled_windows_bw_multiplier;
	unsigned long bpp;

	if (!WIN_IS_ENABLED(w))
		return 0;

	if (dfixed_trunc(w->w) == 0 || dfixed_trunc(w->h) == 0 ||
	    w->out_w == 0 || w->out_h == 0)
		return 0;

	tiled_windows_bw_multiplier =
		tegra_mc_get_tiled_memory_bandwidth_multiplier();

	/* all of tegra's YUV formats(420 and 422) fetch 2 bytes per pixel,
	 * but the size reported by tegra_dc_fmt_bpp for the planar version
	 * is of the luma plane's size only. */
	bpp = tegra_dc_is_yuv_planar(w->fmt) ?
		2 * tegra_dc_fmt_bpp(w->fmt) : tegra_dc_fmt_bpp(w->fmt);
	ret = (dc->mode.pclk / 1000UL * bpp / 8);
#if defined(CONFIG_ARCH_TEGRA_2x_SOC) || defined(CONFIG_ARCH_TEGRA_3x_SOC)
	ret *= (win_use_v_filter(dc, w) ? 2 : 1);
#endif
	ret *=	div_u64(dfixed_trunc(w->w), w->out_w * (WIN_IS_TILED(w) ?
			tiled_windows_bw_multiplier : 1));

#ifdef CONFIG_ARCH_TEGRA_2x_SOC
	/*
	 * Assuming 60% efficiency: i.e. if we calculate we need 70MBps, we
	 * will request 117MBps from EMC.
	 */
	ret = ret + (17 * div_u64(ret, 25));
#else
/*
	 * Assuming ~35% efficiency: i.e. if we calculate we need 70MBps, we
	 * will request 200MBps from EMC.
 */
	ret = ret * 29 / 10;
#endif

	return ret;
}

static unsigned long tegra_dc_get_bandwidth(
	struct tegra_dc_win *windows[], int n)
{
	int i;

	BUG_ON(n > DC_N_WINDOWS);

	/* emc rate and latency allowance both need to know per window
	 * bandwidths */
	for (i = 0; i < n; i++) {
		struct tegra_dc_win *w = windows[i];

		if (w)
			w->new_bandwidth =
				tegra_dc_calc_win_bandwidth(w->dc, w);
	}

	return tegra_dc_find_max_bandwidth(windows, n);
}

/* to save power, call when display memory clients would be idle */
void tegra_dc_clear_bandwidth(struct tegra_dc *dc)
{
	trace_printk("%s:%s rate=%d\n", dc->ndev->name, __func__,
		dc->emc_clk_rate);
	if (tegra_is_clk_enabled(dc->emc_clk))
		clk_disable(dc->emc_clk);
	dc->emc_clk_rate = 0;
}

/* use the larger of dc->emc_clk_rate or dc->new_emc_clk_rate, and copies
 * dc->new_emc_clk_rate into dc->emc_clk_rate.
 * calling this function both before and after a flip is sufficient to select
 * set use_new to true to force dc->new_emc_clk_rate programming.
 */
void tegra_dc_program_bandwidth(struct tegra_dc *dc, bool use_new)
{
	unsigned i;

	if (use_new || dc->emc_clk_rate != dc->new_emc_clk_rate) {
		/* going from 0 to non-zero */
		if (!dc->emc_clk_rate && !tegra_is_clk_enabled(dc->emc_clk))
			clk_enable(dc->emc_clk);

		dc->emc_clk_rate = dc->new_emc_clk_rate;
		clk_set_rate(dc->emc_clk, dc->emc_clk_rate);

		/* going from non-zero to 0 */
		if (!dc->new_emc_clk_rate && tegra_is_clk_enabled(dc->emc_clk))
			clk_disable(dc->emc_clk);
	}

	for (i = 0; i < DC_N_WINDOWS; i++) {
		struct tegra_dc_win *w = &dc->windows[i];

		if ((use_new || w->bandwidth != w->new_bandwidth) &&
			w->new_bandwidth != 0)
			tegra_dc_set_latency_allowance(dc, w);
		trace_printk("%s:win%u bandwidth=%d\n", dc->ndev->name, w->idx,
			w->bandwidth);
		w->bandwidth = w->new_bandwidth;
	}
}

int tegra_dc_set_dynamic_emc(struct tegra_dc_win *windows[], int n)
{
	unsigned long new_rate;
	struct tegra_dc *dc;

	if (!use_dynamic_emc)
		return 0;

	dc = windows[0]->dc;

	/* calculate the new rate based on this POST */
	new_rate = tegra_dc_get_bandwidth(windows, n);
	if (WARN_ONCE(new_rate > (ULONG_MAX / 1000), "bandwidth maxed out\n"))
		new_rate = ULONG_MAX;
	else
		new_rate = EMC_BW_TO_FREQ(new_rate * 1000);

	if (tegra_dc_has_multiple_dc())
		new_rate = ULONG_MAX;

	trace_printk("%s:new_emc_clk_rate=%ld\n", dc->ndev->name, new_rate);
	dc->new_emc_clk_rate = new_rate;

	return 0;
}