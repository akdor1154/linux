// SPDX-License-Identifier: GPL-2.0
/*
 * TI SN65DSI83,84,85 driver
 *
 * Currently supported:
 * - SN65DSI83
 *   = 1x Single-link DSI ~ 1x Single-link LVDS
 *   - Supported
 *   - Single-link LVDS mode tested
 * - SN65DSI84
 *   = 1x Single-link DSI ~ 2x Single-link or 1x Dual-link LVDS
 *   - Supported
 *   - Dual-link LVDS mode tested
 *   - 2x Single-link LVDS mode unsupported
 *     (should be easy to add by someone who has the HW)
 * - SN65DSI85
 *   = 2x Single-link or 1x Dual-link DSI ~ 2x Single-link or 1x Dual-link LVDS
 *   - Unsupported
 *     (should be easy to add by someone who has the HW)
 *
 * Copyright (C) 2021 Marek Vasut <marex@denx.de>
 *
 * Based on previous work of:
 * Valentin Raevsky <valentin@compulab.co.il>
 * Philippe Schenker <philippe.schenker@toradex.com>
 */

#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

/* ID registers */
#define REG_ID(n)				(0x00 + (n))
/* Reset and clock registers */
#define REG_RC_RESET				0x09
#define  REG_RC_RESET_SOFT_RESET		BIT(0)
#define REG_RC_LVDS_PLL				0x0a
#define  REG_RC_LVDS_PLL_PLL_EN_STAT		BIT(7)
#define  REG_RC_LVDS_PLL_LVDS_CLK_RANGE(n)	(((n) & 0x7) << 1)
#define  REG_RC_LVDS_PLL_HS_CLK_SRC_DPHY	BIT(0)
#define REG_RC_DSI_CLK				0x0b
#define  REG_RC_DSI_CLK_DSI_CLK_DIVIDER(n)	(((n) & 0x1f) << 3)
#define  REG_RC_DSI_CLK_REFCLK_MULTIPLIER(n)	((n) & 0x3)
#define REG_RC_PLL_EN				0x0d
#define  REG_RC_PLL_EN_PLL_EN			BIT(0)
/* DSI registers */
#define REG_DSI_LANE				0x10
#define  REG_DSI_LANE_LEFT_RIGHT_PIXELS		BIT(7)	/* DSI85-only */
#define  REG_DSI_LANE_DSI_CHANNEL_MODE_DUAL	0	/* DSI85-only */
#define  REG_DSI_LANE_DSI_CHANNEL_MODE_2SINGLE	BIT(6)	/* DSI85-only */
#define  REG_DSI_LANE_DSI_CHANNEL_MODE_SINGLE	BIT(5)
#define  REG_DSI_LANE_CHA_DSI_LANES(n)		(((n) & 0x3) << 3)
#define  REG_DSI_LANE_CHB_DSI_LANES(n)		(((n) & 0x3) << 1)
#define  REG_DSI_LANE_SOT_ERR_TOL_DIS		BIT(0)
#define REG_DSI_EQ				0x11
#define  REG_DSI_EQ_CHA_DSI_DATA_EQ(n)		(((n) & 0x3) << 6)
#define  REG_DSI_EQ_CHA_DSI_CLK_EQ(n)		(((n) & 0x3) << 2)
#define REG_DSI_CLK				0x12
#define  REG_DSI_CLK_CHA_DSI_CLK_RANGE(n)	((n) & 0xff)
/* LVDS registers */
#define REG_LVDS_FMT				0x18
#define  REG_LVDS_FMT_DE_NEG_POLARITY		BIT(7)
#define  REG_LVDS_FMT_HS_NEG_POLARITY		BIT(6)
#define  REG_LVDS_FMT_VS_NEG_POLARITY		BIT(5)
#define  REG_LVDS_FMT_LVDS_LINK_CFG		BIT(4)	/* 0:AB 1:A-only */
#define  REG_LVDS_FMT_CHA_24BPP_MODE		BIT(3)
#define  REG_LVDS_FMT_CHB_24BPP_MODE		BIT(2)
#define  REG_LVDS_FMT_CHA_24BPP_FORMAT1		BIT(1)
#define  REG_LVDS_FMT_CHB_24BPP_FORMAT1		BIT(0)
#define REG_LVDS_VCOM				0x19
#define  REG_LVDS_VCOM_CHA_LVDS_VOCM		BIT(6)
#define  REG_LVDS_VCOM_CHB_LVDS_VOCM		BIT(4)
#define  REG_LVDS_VCOM_CHA_LVDS_VOD_SWING(n)	(((n) & 0x3) << 2)
#define  REG_LVDS_VCOM_CHB_LVDS_VOD_SWING(n)	((n) & 0x3)
#define REG_LVDS_LANE				0x1a
#define  REG_LVDS_LANE_EVEN_ODD_SWAP		BIT(6)
#define  REG_LVDS_LANE_CHA_REVERSE_LVDS		BIT(5)
#define  REG_LVDS_LANE_CHB_REVERSE_LVDS		BIT(4)
#define  REG_LVDS_LANE_CHA_LVDS_TERM		BIT(1)
#define  REG_LVDS_LANE_CHB_LVDS_TERM		BIT(0)
#define REG_LVDS_CM				0x1b
#define  REG_LVDS_CM_CHA_LVDS_CM_ADJUST(n)	(((n) & 0x3) << 4)
#define  REG_LVDS_CM_CHB_LVDS_CM_ADJUST(n)	((n) & 0x3)
/* Video registers */
#define REG_VID_CHA_ACTIVE_LINE_LENGTH_LOW	0x20
#define REG_VID_CHA_ACTIVE_LINE_LENGTH_HIGH	0x21
#define REG_VID_CHA_VERTICAL_DISPLAY_SIZE_LOW	0x24
#define REG_VID_CHA_VERTICAL_DISPLAY_SIZE_HIGH	0x25
#define REG_VID_CHA_SYNC_DELAY_LOW		0x28
#define REG_VID_CHA_SYNC_DELAY_HIGH		0x29
#define REG_VID_CHA_HSYNC_PULSE_WIDTH_LOW	0x2c
#define REG_VID_CHA_HSYNC_PULSE_WIDTH_HIGH	0x2d
#define REG_VID_CHA_VSYNC_PULSE_WIDTH_LOW	0x30
#define REG_VID_CHA_VSYNC_PULSE_WIDTH_HIGH	0x31
#define REG_VID_CHA_HORIZONTAL_BACK_PORCH	0x34
#define REG_VID_CHA_VERTICAL_BACK_PORCH		0x36
#define REG_VID_CHA_HORIZONTAL_FRONT_PORCH	0x38
#define REG_VID_CHA_VERTICAL_FRONT_PORCH	0x3a
#define REG_VID_CHA_TEST_PATTERN		0x3c
/* IRQ registers */
#define REG_IRQ_GLOBAL				0xe0
#define  REG_IRQ_GLOBAL_IRQ_EN			BIT(0)
#define REG_IRQ_EN				0xe1
#define  REG_IRQ_EN_CHA_SYNCH_ERR_EN		BIT(7)
#define  REG_IRQ_EN_CHA_CRC_ERR_EN		BIT(6)
#define  REG_IRQ_EN_CHA_UNC_ECC_ERR_EN		BIT(5)
#define  REG_IRQ_EN_CHA_COR_ECC_ERR_EN		BIT(4)
#define  REG_IRQ_EN_CHA_LLP_ERR_EN		BIT(3)
#define  REG_IRQ_EN_CHA_SOT_BIT_ERR_EN		BIT(2)
#define  REG_IRQ_EN_CHA_PLL_UNLOCK_EN		BIT(0)
#define REG_IRQ_STAT				0xe5
#define  REG_IRQ_STAT_CHA_SYNCH_ERR		BIT(7)
#define  REG_IRQ_STAT_CHA_CRC_ERR		BIT(6)
#define  REG_IRQ_STAT_CHA_UNC_ECC_ERR		BIT(5)
#define  REG_IRQ_STAT_CHA_COR_ECC_ERR		BIT(4)
#define  REG_IRQ_STAT_CHA_LLP_ERR		BIT(3)
#define  REG_IRQ_STAT_CHA_SOT_BIT_ERR		BIT(2)
#define  REG_IRQ_STAT_CHA_PLL_UNLOCK		BIT(0)

enum sn65dsi83_channel {
	CHANNEL_A,
	CHANNEL_B
};

enum sn65dsi83_lvds_term {
	OHM_100,
	OHM_200
};

enum sn65dsi83_model {
	MODEL_SN65DSI83,
	MODEL_SN65DSI84,
};

struct sn65dsi83 {
	struct drm_bridge		bridge;
	struct device			*dev;
	struct regmap			*regmap;
	struct mipi_dsi_device		*dsi;
	struct drm_bridge		*panel_bridge;
	struct gpio_desc		*enable_gpio;
	struct regulator		*vcc;
	bool				lvds_dual_link;
	bool				lvds_dual_link_even_odd_swap;
	int				lvds_vod_swing_conf[2];
	int				lvds_term_conf[2];
	int				irq;
	struct delayed_work		monitor_work;
	struct work_struct		reset_work;
};

static const struct regmap_range sn65dsi83_readable_ranges[] = {
	regmap_reg_range(REG_ID(0), REG_ID(8)),
	regmap_reg_range(REG_RC_LVDS_PLL, REG_RC_DSI_CLK),
	regmap_reg_range(REG_RC_PLL_EN, REG_RC_PLL_EN),
	regmap_reg_range(REG_DSI_LANE, REG_DSI_CLK),
	regmap_reg_range(REG_LVDS_FMT, REG_LVDS_CM),
	regmap_reg_range(REG_VID_CHA_ACTIVE_LINE_LENGTH_LOW,
			 REG_VID_CHA_ACTIVE_LINE_LENGTH_HIGH),
	regmap_reg_range(REG_VID_CHA_VERTICAL_DISPLAY_SIZE_LOW,
			 REG_VID_CHA_VERTICAL_DISPLAY_SIZE_HIGH),
	regmap_reg_range(REG_VID_CHA_SYNC_DELAY_LOW,
			 REG_VID_CHA_SYNC_DELAY_HIGH),
	regmap_reg_range(REG_VID_CHA_HSYNC_PULSE_WIDTH_LOW,
			 REG_VID_CHA_HSYNC_PULSE_WIDTH_HIGH),
	regmap_reg_range(REG_VID_CHA_VSYNC_PULSE_WIDTH_LOW,
			 REG_VID_CHA_VSYNC_PULSE_WIDTH_HIGH),
	regmap_reg_range(REG_VID_CHA_HORIZONTAL_BACK_PORCH,
			 REG_VID_CHA_HORIZONTAL_BACK_PORCH),
	regmap_reg_range(REG_VID_CHA_VERTICAL_BACK_PORCH,
			 REG_VID_CHA_VERTICAL_BACK_PORCH),
	regmap_reg_range(REG_VID_CHA_HORIZONTAL_FRONT_PORCH,
			 REG_VID_CHA_HORIZONTAL_FRONT_PORCH),
	regmap_reg_range(REG_VID_CHA_VERTICAL_FRONT_PORCH,
			 REG_VID_CHA_VERTICAL_FRONT_PORCH),
	regmap_reg_range(REG_VID_CHA_TEST_PATTERN, REG_VID_CHA_TEST_PATTERN),
	regmap_reg_range(REG_IRQ_GLOBAL, REG_IRQ_EN),
	regmap_reg_range(REG_IRQ_STAT, REG_IRQ_STAT),
};

static const struct regmap_access_table sn65dsi83_readable_table = {
	.yes_ranges = sn65dsi83_readable_ranges,
	.n_yes_ranges = ARRAY_SIZE(sn65dsi83_readable_ranges),
};

static const struct regmap_range sn65dsi83_writeable_ranges[] = {
	regmap_reg_range(REG_RC_RESET, REG_RC_DSI_CLK),
	regmap_reg_range(REG_RC_PLL_EN, REG_RC_PLL_EN),
	regmap_reg_range(REG_DSI_LANE, REG_DSI_CLK),
	regmap_reg_range(REG_LVDS_FMT, REG_LVDS_CM),
	regmap_reg_range(REG_VID_CHA_ACTIVE_LINE_LENGTH_LOW,
			 REG_VID_CHA_ACTIVE_LINE_LENGTH_HIGH),
	regmap_reg_range(REG_VID_CHA_VERTICAL_DISPLAY_SIZE_LOW,
			 REG_VID_CHA_VERTICAL_DISPLAY_SIZE_HIGH),
	regmap_reg_range(REG_VID_CHA_SYNC_DELAY_LOW,
			 REG_VID_CHA_SYNC_DELAY_HIGH),
	regmap_reg_range(REG_VID_CHA_HSYNC_PULSE_WIDTH_LOW,
			 REG_VID_CHA_HSYNC_PULSE_WIDTH_HIGH),
	regmap_reg_range(REG_VID_CHA_VSYNC_PULSE_WIDTH_LOW,
			 REG_VID_CHA_VSYNC_PULSE_WIDTH_HIGH),
	regmap_reg_range(REG_VID_CHA_HORIZONTAL_BACK_PORCH,
			 REG_VID_CHA_HORIZONTAL_BACK_PORCH),
	regmap_reg_range(REG_VID_CHA_VERTICAL_BACK_PORCH,
			 REG_VID_CHA_VERTICAL_BACK_PORCH),
	regmap_reg_range(REG_VID_CHA_HORIZONTAL_FRONT_PORCH,
			 REG_VID_CHA_HORIZONTAL_FRONT_PORCH),
	regmap_reg_range(REG_VID_CHA_VERTICAL_FRONT_PORCH,
			 REG_VID_CHA_VERTICAL_FRONT_PORCH),
	regmap_reg_range(REG_VID_CHA_TEST_PATTERN, REG_VID_CHA_TEST_PATTERN),
	regmap_reg_range(REG_IRQ_GLOBAL, REG_IRQ_EN),
	regmap_reg_range(REG_IRQ_STAT, REG_IRQ_STAT),
};

static const struct regmap_access_table sn65dsi83_writeable_table = {
	.yes_ranges = sn65dsi83_writeable_ranges,
	.n_yes_ranges = ARRAY_SIZE(sn65dsi83_writeable_ranges),
};

static const struct regmap_range sn65dsi83_volatile_ranges[] = {
	regmap_reg_range(REG_RC_RESET, REG_RC_RESET),
	regmap_reg_range(REG_RC_LVDS_PLL, REG_RC_LVDS_PLL),
	regmap_reg_range(REG_IRQ_STAT, REG_IRQ_STAT),
};

static const struct regmap_access_table sn65dsi83_volatile_table = {
	.yes_ranges = sn65dsi83_volatile_ranges,
	.n_yes_ranges = ARRAY_SIZE(sn65dsi83_volatile_ranges),
};

static const struct regmap_config sn65dsi83_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.rd_table = &sn65dsi83_readable_table,
	.wr_table = &sn65dsi83_writeable_table,
	.volatile_table = &sn65dsi83_volatile_table,
	.cache_type = REGCACHE_MAPLE,
	.max_register = REG_IRQ_STAT,
};

static const int lvds_vod_swing_data_table[2][4][2] = {
	{	/* 100 Ohm */
		{ 180000, 313000 },
		{ 215000, 372000 },
		{ 250000, 430000 },
		{ 290000, 488000 },
	},
	{	/* 200 Ohm */
		{ 150000, 261000 },
		{ 200000, 346000 },
		{ 250000, 428000 },
		{ 300000, 511000 },
	},
};

static const int lvds_vod_swing_clock_table[2][4][2] = {
	{	/* 100 Ohm */
		{ 140000, 244000 },
		{ 168000, 290000 },
		{ 195000, 335000 },
		{ 226000, 381000 },
	},
	{	/* 200 Ohm */
		{ 117000, 204000 },
		{ 156000, 270000 },
		{ 195000, 334000 },
		{ 234000, 399000 },
	},
};

static struct sn65dsi83 *bridge_to_sn65dsi83(struct drm_bridge *bridge)
{
	return container_of(bridge, struct sn65dsi83, bridge);
}

static int sn65dsi83_attach(struct drm_bridge *bridge,
			    struct drm_encoder *encoder,
			    enum drm_bridge_attach_flags flags)
{
	struct sn65dsi83 *ctx = bridge_to_sn65dsi83(bridge);

	return drm_bridge_attach(encoder, ctx->panel_bridge,
				 &ctx->bridge, flags);
}

static void sn65dsi83_detach(struct drm_bridge *bridge)
{
	struct sn65dsi83 *ctx = bridge_to_sn65dsi83(bridge);

	if (!ctx->dsi)
		return;

	ctx->dsi = NULL;
}

static u8 sn65dsi83_get_lvds_range(struct sn65dsi83 *ctx,
				   const struct drm_display_mode *mode)
{
	/*
	 * The encoding of the LVDS_CLK_RANGE is as follows:
	 * 000 - 25 MHz <= LVDS_CLK < 37.5 MHz
	 * 001 - 37.5 MHz <= LVDS_CLK < 62.5 MHz
	 * 010 - 62.5 MHz <= LVDS_CLK < 87.5 MHz
	 * 011 - 87.5 MHz <= LVDS_CLK < 112.5 MHz
	 * 100 - 112.5 MHz <= LVDS_CLK < 137.5 MHz
	 * 101 - 137.5 MHz <= LVDS_CLK <= 154 MHz
	 * which is a range of 12.5MHz..162.5MHz in 50MHz steps, except that
	 * the ends of the ranges are clamped to the supported range. Since
	 * sn65dsi83_mode_valid() already filters the valid modes and limits
	 * the clock to 25..154 MHz, the range calculation can be simplified
	 * as follows:
	 */
	int mode_clock = mode->clock;

	if (ctx->lvds_dual_link)
		mode_clock /= 2;

	return (mode_clock - 12500) / 25000;
}

static u8 sn65dsi83_get_dsi_range(struct sn65dsi83 *ctx,
				  const struct drm_display_mode *mode)
{
	/*
	 * The encoding of the CHA_DSI_CLK_RANGE is as follows:
	 * 0x00 through 0x07 - Reserved
	 * 0x08 - 40 <= DSI_CLK < 45 MHz
	 * 0x09 - 45 <= DSI_CLK < 50 MHz
	 * ...
	 * 0x63 - 495 <= DSI_CLK < 500 MHz
	 * 0x64 - 500 MHz
	 * 0x65 through 0xFF - Reserved
	 * which is DSI clock in 5 MHz steps, clamped to 40..500 MHz.
	 * The DSI clock are calculated as:
	 *  DSI_CLK = mode clock * bpp / dsi_data_lanes / 2
	 * the 2 is there because the bus is DDR.
	 */
	return DIV_ROUND_UP(clamp((unsigned int)mode->clock *
			    mipi_dsi_pixel_format_to_bpp(ctx->dsi->format) /
			    ctx->dsi->lanes / 2, 40000U, 500000U), 5000U);
}

static u8 sn65dsi83_get_dsi_div(struct sn65dsi83 *ctx)
{
	/* The divider is (DSI_CLK / LVDS_CLK) - 1, which really is: */
	unsigned int dsi_div = mipi_dsi_pixel_format_to_bpp(ctx->dsi->format);

	dsi_div /= ctx->dsi->lanes;

	if (!ctx->lvds_dual_link)
		dsi_div /= 2;

	return dsi_div - 1;
}

static int sn65dsi83_reset_pipe(struct sn65dsi83 *sn65dsi83)
{
	struct drm_modeset_acquire_ctx ctx;
	int err;

	/*
	 * Reset active outputs of the related CRTC.
	 *
	 * This way, drm core will reconfigure each components in the CRTC
	 * outputs path. In our case, this will force the previous component to
	 * go back in LP11 mode and so allow the reconfiguration of SN65DSI83
	 * bridge.
	 *
	 * Keep the lock during the whole operation to be atomic.
	 */

	drm_modeset_acquire_init(&ctx, 0);

	dev_warn(sn65dsi83->dev, "reset the pipe\n");

retry:
	err = drm_bridge_helper_reset_crtc(&sn65dsi83->bridge, &ctx);
	if (err == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	}

	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	return 0;
}

static void sn65dsi83_reset_work(struct work_struct *ws)
{
	struct sn65dsi83 *ctx = container_of(ws, struct sn65dsi83, reset_work);
	int ret;

	/* Reset the pipe */
	ret = sn65dsi83_reset_pipe(ctx);
	if (ret) {
		dev_err(ctx->dev, "reset pipe failed %pe\n", ERR_PTR(ret));
		return;
	}
	if (ctx->irq)
		enable_irq(ctx->irq);
}

static void sn65dsi83_handle_errors(struct sn65dsi83 *ctx)
{
	unsigned int irq_stat;
	int ret;

	/*
	 * Schedule a reset in case of:
	 *  - the bridge doesn't answer
	 *  - the bridge signals an error
	 */

	ret = regmap_read(ctx->regmap, REG_IRQ_STAT, &irq_stat);
	if (ret || irq_stat) {
		/*
		 * IRQ acknowledged is not always possible (the bridge can be in
		 * a state where it doesn't answer anymore). To prevent an
		 * interrupt storm, disable interrupt. The interrupt will be
		 * after the reset.
		 */
		if (ctx->irq)
			disable_irq_nosync(ctx->irq);

		schedule_work(&ctx->reset_work);
	}
}

static void sn65dsi83_monitor_work(struct work_struct *work)
{
	struct sn65dsi83 *ctx = container_of(to_delayed_work(work),
					     struct sn65dsi83, monitor_work);

	sn65dsi83_handle_errors(ctx);

	schedule_delayed_work(&ctx->monitor_work, msecs_to_jiffies(1000));
}

static void sn65dsi83_monitor_start(struct sn65dsi83 *ctx)
{
	schedule_delayed_work(&ctx->monitor_work, msecs_to_jiffies(1000));
}

static void sn65dsi83_monitor_stop(struct sn65dsi83 *ctx)
{
	cancel_delayed_work_sync(&ctx->monitor_work);
}

static void sn65dsi83_atomic_pre_enable(struct drm_bridge *bridge,
					struct drm_atomic_state *state)
{
	struct sn65dsi83 *ctx = bridge_to_sn65dsi83(bridge);
	const struct drm_bridge_state *bridge_state;
	const struct drm_crtc_state *crtc_state;
	const struct drm_display_mode *mode;
	struct drm_connector *connector;
	struct drm_crtc *crtc;
	bool lvds_format_24bpp;
	bool lvds_format_jeida;
	unsigned int pval;
	__le16 le16val;
	u16 val;
	int ret;

	ret = regulator_enable(ctx->vcc);
	if (ret) {
		dev_err(ctx->dev, "Failed to enable vcc: %d\n", ret);
		return;
	}

	/* Deassert reset */
	gpiod_set_value_cansleep(ctx->enable_gpio, 1);
	usleep_range(10000, 11000);

	/* Get the LVDS format from the bridge state. */
	bridge_state = drm_atomic_get_new_bridge_state(state, bridge);

	switch (bridge_state->output_bus_cfg.format) {
	case MEDIA_BUS_FMT_RGB666_1X7X3_SPWG:
		lvds_format_24bpp = false;
		lvds_format_jeida = true;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA:
		lvds_format_24bpp = true;
		lvds_format_jeida = true;
		break;
	case MEDIA_BUS_FMT_RGB888_1X7X4_SPWG:
		lvds_format_24bpp = true;
		lvds_format_jeida = false;
		break;
	default:
		/*
		 * Some bridges still don't set the correct
		 * LVDS bus pixel format, use SPWG24 default
		 * format until those are fixed.
		 */
		lvds_format_24bpp = true;
		lvds_format_jeida = false;
		dev_warn(ctx->dev,
			 "Unsupported LVDS bus format 0x%04x, please check output bridge driver. Falling back to SPWG24.\n",
			 bridge_state->output_bus_cfg.format);
		break;
	}

	/*
	 * Retrieve the CRTC adjusted mode. This requires a little dance to go
	 * from the bridge to the encoder, to the connector and to the CRTC.
	 */
	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);
	crtc = drm_atomic_get_new_connector_state(state, connector)->crtc;
	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	mode = &crtc_state->adjusted_mode;

	/* Clear reset, disable PLL */
	regmap_write(ctx->regmap, REG_RC_RESET, 0x00);
	regmap_write(ctx->regmap, REG_RC_PLL_EN, 0x00);

	/* Reference clock derived from DSI link clock. */
	regmap_write(ctx->regmap, REG_RC_LVDS_PLL,
		     REG_RC_LVDS_PLL_LVDS_CLK_RANGE(sn65dsi83_get_lvds_range(ctx, mode)) |
		     REG_RC_LVDS_PLL_HS_CLK_SRC_DPHY);
	regmap_write(ctx->regmap, REG_DSI_CLK,
		     REG_DSI_CLK_CHA_DSI_CLK_RANGE(sn65dsi83_get_dsi_range(ctx, mode)));
	regmap_write(ctx->regmap, REG_RC_DSI_CLK,
		     REG_RC_DSI_CLK_DSI_CLK_DIVIDER(sn65dsi83_get_dsi_div(ctx)));

	/* Set number of DSI lanes and LVDS link config. */
	regmap_write(ctx->regmap, REG_DSI_LANE,
		     REG_DSI_LANE_DSI_CHANNEL_MODE_SINGLE |
		     REG_DSI_LANE_CHA_DSI_LANES(~(ctx->dsi->lanes - 1)) |
		     /* CHB is DSI85-only, set to default on DSI83/DSI84 */
		     REG_DSI_LANE_CHB_DSI_LANES(3));
	/* No equalization. */
	regmap_write(ctx->regmap, REG_DSI_EQ, 0x00);

	/* Set up sync signal polarity. */
	val = (mode->flags & DRM_MODE_FLAG_NHSYNC ?
	       REG_LVDS_FMT_HS_NEG_POLARITY : 0) |
	      (mode->flags & DRM_MODE_FLAG_NVSYNC ?
	       REG_LVDS_FMT_VS_NEG_POLARITY : 0);
	val |= bridge_state->output_bus_cfg.flags & DRM_BUS_FLAG_DE_LOW ?
	       REG_LVDS_FMT_DE_NEG_POLARITY : 0;

	/* Set up bits-per-pixel, 18bpp or 24bpp. */
	if (lvds_format_24bpp) {
		val |= REG_LVDS_FMT_CHA_24BPP_MODE;
		if (ctx->lvds_dual_link)
			val |= REG_LVDS_FMT_CHB_24BPP_MODE;
	}

	/* Set up LVDS format, JEIDA/Format 1 or SPWG/Format 2 */
	if (lvds_format_jeida) {
		val |= REG_LVDS_FMT_CHA_24BPP_FORMAT1;
		if (ctx->lvds_dual_link)
			val |= REG_LVDS_FMT_CHB_24BPP_FORMAT1;
	}

	/* Set up LVDS output config (DSI84,DSI85) */
	if (!ctx->lvds_dual_link)
		val |= REG_LVDS_FMT_LVDS_LINK_CFG;

	regmap_write(ctx->regmap, REG_LVDS_FMT, val);
	regmap_write(ctx->regmap, REG_LVDS_VCOM,
			REG_LVDS_VCOM_CHA_LVDS_VOD_SWING(ctx->lvds_vod_swing_conf[CHANNEL_A]) |
			REG_LVDS_VCOM_CHB_LVDS_VOD_SWING(ctx->lvds_vod_swing_conf[CHANNEL_B]));
	regmap_write(ctx->regmap, REG_LVDS_LANE,
		     (ctx->lvds_dual_link_even_odd_swap ?
		      REG_LVDS_LANE_EVEN_ODD_SWAP : 0) |
		     (ctx->lvds_term_conf[CHANNEL_A] ?
			  REG_LVDS_LANE_CHA_LVDS_TERM : 0) |
		     (ctx->lvds_term_conf[CHANNEL_B] ?
			  REG_LVDS_LANE_CHB_LVDS_TERM : 0));
	regmap_write(ctx->regmap, REG_LVDS_CM, 0x00);

	le16val = cpu_to_le16(mode->hdisplay);
	regmap_bulk_write(ctx->regmap, REG_VID_CHA_ACTIVE_LINE_LENGTH_LOW,
			  &le16val, 2);
	le16val = cpu_to_le16(mode->vdisplay);
	regmap_bulk_write(ctx->regmap, REG_VID_CHA_VERTICAL_DISPLAY_SIZE_LOW,
			  &le16val, 2);
	/* 32 + 1 pixel clock to ensure proper operation */
	le16val = cpu_to_le16(32 + 1);
	regmap_bulk_write(ctx->regmap, REG_VID_CHA_SYNC_DELAY_LOW, &le16val, 2);
	le16val = cpu_to_le16(mode->hsync_end - mode->hsync_start);
	regmap_bulk_write(ctx->regmap, REG_VID_CHA_HSYNC_PULSE_WIDTH_LOW,
			  &le16val, 2);
	le16val = cpu_to_le16(mode->vsync_end - mode->vsync_start);
	regmap_bulk_write(ctx->regmap, REG_VID_CHA_VSYNC_PULSE_WIDTH_LOW,
			  &le16val, 2);
	regmap_write(ctx->regmap, REG_VID_CHA_HORIZONTAL_BACK_PORCH,
		     mode->htotal - mode->hsync_end);
	regmap_write(ctx->regmap, REG_VID_CHA_VERTICAL_BACK_PORCH,
		     mode->vtotal - mode->vsync_end);
	regmap_write(ctx->regmap, REG_VID_CHA_HORIZONTAL_FRONT_PORCH,
		     mode->hsync_start - mode->hdisplay);
	regmap_write(ctx->regmap, REG_VID_CHA_VERTICAL_FRONT_PORCH,
		     mode->vsync_start - mode->vdisplay);
	regmap_write(ctx->regmap, REG_VID_CHA_TEST_PATTERN, 0x00);

	/* Enable PLL */
	regmap_write(ctx->regmap, REG_RC_PLL_EN, REG_RC_PLL_EN_PLL_EN);
	usleep_range(3000, 4000);
	ret = regmap_read_poll_timeout(ctx->regmap, REG_RC_LVDS_PLL, pval,
				       pval & REG_RC_LVDS_PLL_PLL_EN_STAT,
				       1000, 100000);
	if (ret) {
		dev_err(ctx->dev, "failed to lock PLL, ret=%i\n", ret);
		/* On failure, disable PLL again and exit. */
		regmap_write(ctx->regmap, REG_RC_PLL_EN, 0x00);
		return;
	}

	/* Trigger reset after CSR register update. */
	regmap_write(ctx->regmap, REG_RC_RESET, REG_RC_RESET_SOFT_RESET);

	/* Wait for 10ms after soft reset as specified in datasheet */
	usleep_range(10000, 12000);
}

static void sn65dsi83_atomic_enable(struct drm_bridge *bridge,
				    struct drm_atomic_state *state)
{
	struct sn65dsi83 *ctx = bridge_to_sn65dsi83(bridge);
	unsigned int pval;

	/* Clear all errors that got asserted during initialization. */
	regmap_read(ctx->regmap, REG_IRQ_STAT, &pval);
	regmap_write(ctx->regmap, REG_IRQ_STAT, pval);

	/* Wait for 1ms and check for errors in status register */
	usleep_range(1000, 1100);
	regmap_read(ctx->regmap, REG_IRQ_STAT, &pval);
	if (pval)
		dev_err(ctx->dev, "Unexpected link status 0x%02x\n", pval);

	if (ctx->irq) {
		/* Enable irq to detect errors */
		regmap_write(ctx->regmap, REG_IRQ_GLOBAL, REG_IRQ_GLOBAL_IRQ_EN);
		regmap_write(ctx->regmap, REG_IRQ_EN, 0xff);
	} else {
		/* Use the polling task */
		sn65dsi83_monitor_start(ctx);
	}
}

static void sn65dsi83_atomic_disable(struct drm_bridge *bridge,
				     struct drm_atomic_state *state)
{
	struct sn65dsi83 *ctx = bridge_to_sn65dsi83(bridge);
	int ret;

	if (ctx->irq) {
		/* Disable irq */
		regmap_write(ctx->regmap, REG_IRQ_EN, 0x0);
		regmap_write(ctx->regmap, REG_IRQ_GLOBAL, 0x0);
	} else {
		/* Stop the polling task */
		sn65dsi83_monitor_stop(ctx);
	}

	/* Put the chip in reset, pull EN line low, and assure 10ms reset low timing. */
	gpiod_set_value_cansleep(ctx->enable_gpio, 0);
	usleep_range(10000, 11000);

	ret = regulator_disable(ctx->vcc);
	if (ret)
		dev_err(ctx->dev, "Failed to disable vcc: %d\n", ret);

	regcache_mark_dirty(ctx->regmap);
}

static enum drm_mode_status
sn65dsi83_mode_valid(struct drm_bridge *bridge,
		     const struct drm_display_info *info,
		     const struct drm_display_mode *mode)
{
	/* LVDS output clock range 25..154 MHz */
	if (mode->clock < 25000)
		return MODE_CLOCK_LOW;
	if (mode->clock > 154000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

#define MAX_INPUT_SEL_FORMATS	1

static u32 *
sn65dsi83_atomic_get_input_bus_fmts(struct drm_bridge *bridge,
				    struct drm_bridge_state *bridge_state,
				    struct drm_crtc_state *crtc_state,
				    struct drm_connector_state *conn_state,
				    u32 output_fmt,
				    unsigned int *num_input_fmts)
{
	u32 *input_fmts;

	*num_input_fmts = 0;

	input_fmts = kcalloc(MAX_INPUT_SEL_FORMATS, sizeof(*input_fmts),
			     GFP_KERNEL);
	if (!input_fmts)
		return NULL;

	/* This is the DSI-end bus format */
	input_fmts[0] = MEDIA_BUS_FMT_RGB888_1X24;
	*num_input_fmts = 1;

	return input_fmts;
}

static const struct drm_bridge_funcs sn65dsi83_funcs = {
	.attach			= sn65dsi83_attach,
	.detach			= sn65dsi83_detach,
	.atomic_enable		= sn65dsi83_atomic_enable,
	.atomic_pre_enable	= sn65dsi83_atomic_pre_enable,
	.atomic_disable		= sn65dsi83_atomic_disable,
	.mode_valid		= sn65dsi83_mode_valid,

	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.atomic_get_input_bus_fmts = sn65dsi83_atomic_get_input_bus_fmts,
};

static int sn65dsi83_select_lvds_vod_swing(struct device *dev,
	u32 lvds_vod_swing_data[2], u32 lvds_vod_swing_clk[2], u8 lvds_term)
{
	int i;

	for (i = 0; i <= 3; i++) {
		if (lvds_vod_swing_data_table[lvds_term][i][0]  >= lvds_vod_swing_data[0] &&
		    lvds_vod_swing_data_table[lvds_term][i][1]  <= lvds_vod_swing_data[1] &&
		    lvds_vod_swing_clock_table[lvds_term][i][0] >= lvds_vod_swing_clk[0] &&
		    lvds_vod_swing_clock_table[lvds_term][i][1] <= lvds_vod_swing_clk[1])
			return i;
	}

	dev_err(dev, "failed to find appropriate LVDS_VOD_SWING configuration\n");
	return -EINVAL;
}

static int sn65dsi83_parse_lvds_endpoint(struct sn65dsi83 *ctx, int channel)
{
	struct device *dev = ctx->dev;
	struct device_node *endpoint;
	int endpoint_reg;
	/* Set so the property can be freely selected if not defined */
	u32 lvds_vod_swing_data[2] = { 0, 1000000 };
	u32 lvds_vod_swing_clk[2] = { 0, 1000000 };
	/* Set default near end terminataion to 200 Ohm */
	u32 lvds_term = 200;
	int lvds_vod_swing_conf;
	int ret = 0;
	int ret_data;
	int ret_clock;

	if (channel == CHANNEL_A)
		endpoint_reg = 2;
	else
		endpoint_reg = 3;

	endpoint = of_graph_get_endpoint_by_regs(dev->of_node, endpoint_reg, -1);

	of_property_read_u32(endpoint, "ti,lvds-termination-ohms", &lvds_term);
	if (lvds_term == 100)
		ctx->lvds_term_conf[channel] = OHM_100;
	else if (lvds_term == 200)
		ctx->lvds_term_conf[channel] = OHM_200;
	else {
		ret = -EINVAL;
		goto exit;
	}

	ret_data = of_property_read_u32_array(endpoint, "ti,lvds-vod-swing-data-microvolt",
					lvds_vod_swing_data, ARRAY_SIZE(lvds_vod_swing_data));
	if (ret_data != 0 && ret_data != -EINVAL) {
		ret = ret_data;
		goto exit;
	}

	ret_clock = of_property_read_u32_array(endpoint, "ti,lvds-vod-swing-clock-microvolt",
					lvds_vod_swing_clk, ARRAY_SIZE(lvds_vod_swing_clk));
	if (ret_clock != 0 && ret_clock != -EINVAL) {
		ret = ret_clock;
		goto exit;
	}

	/* Use default value if both properties are NOT defined. */
	if (ret_data == -EINVAL && ret_clock == -EINVAL)
		lvds_vod_swing_conf = 0x1;

	/* Use lookup table if any of the two properties is defined. */
	if (!ret_data || !ret_clock) {
		lvds_vod_swing_conf = sn65dsi83_select_lvds_vod_swing(dev, lvds_vod_swing_data,
						lvds_vod_swing_clk, ctx->lvds_term_conf[channel]);
		if (lvds_vod_swing_conf < 0) {
			ret = lvds_vod_swing_conf;
			goto exit;
		}
	}

	ctx->lvds_vod_swing_conf[channel] = lvds_vod_swing_conf;
	ret = 0;
exit:
	of_node_put(endpoint);
	return ret;
}

static int sn65dsi83_parse_dt(struct sn65dsi83 *ctx, enum sn65dsi83_model model)
{
	struct drm_bridge *panel_bridge;
	struct device *dev = ctx->dev;
	int ret;

	ret = sn65dsi83_parse_lvds_endpoint(ctx, CHANNEL_A);
	if (ret < 0)
		return ret;

	ret = sn65dsi83_parse_lvds_endpoint(ctx, CHANNEL_B);
	if (ret < 0)
		return ret;

	ctx->lvds_dual_link = false;
	ctx->lvds_dual_link_even_odd_swap = false;
	if (model != MODEL_SN65DSI83) {
		struct device_node *port2, *port3;
		int dual_link;

		port2 = of_graph_get_port_by_id(dev->of_node, 2);
		port3 = of_graph_get_port_by_id(dev->of_node, 3);
		dual_link = drm_of_lvds_get_dual_link_pixel_order(port2, port3);
		of_node_put(port2);
		of_node_put(port3);

		if (dual_link == DRM_LVDS_DUAL_LINK_ODD_EVEN_PIXELS) {
			ctx->lvds_dual_link = true;
			/* Odd pixels to LVDS Channel A, even pixels to B */
			ctx->lvds_dual_link_even_odd_swap = false;
		} else if (dual_link == DRM_LVDS_DUAL_LINK_EVEN_ODD_PIXELS) {
			ctx->lvds_dual_link = true;
			/* Even pixels to LVDS Channel A, odd pixels to B */
			ctx->lvds_dual_link_even_odd_swap = true;
		}
	}

	panel_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 2, 0);
	if (IS_ERR(panel_bridge))
		return dev_err_probe(dev, PTR_ERR(panel_bridge), "Failed to get panel bridge\n");

	ctx->panel_bridge = panel_bridge;

	ctx->vcc = devm_regulator_get(dev, "vcc");
	if (IS_ERR(ctx->vcc))
		return dev_err_probe(dev, PTR_ERR(ctx->vcc),
				     "Failed to get supply 'vcc'\n");

	return 0;
}

static int sn65dsi83_host_attach(struct sn65dsi83 *ctx)
{
	struct device *dev = ctx->dev;
	struct device_node *host_node;
	struct device_node *endpoint;
	struct mipi_dsi_device *dsi;
	struct mipi_dsi_host *host;
	const struct mipi_dsi_device_info info = {
		.type = "sn65dsi83",
		.channel = 0,
		.node = NULL,
	};
	int dsi_lanes, ret;

	endpoint = of_graph_get_endpoint_by_regs(dev->of_node, 0, -1);
	dsi_lanes = drm_of_get_data_lanes_count(endpoint, 1, 4);
	host_node = of_graph_get_remote_port_parent(endpoint);
	host = of_find_mipi_dsi_host_by_node(host_node);
	of_node_put(host_node);
	of_node_put(endpoint);

	if (!host)
		return -EPROBE_DEFER;

	if (dsi_lanes < 0)
		return dsi_lanes;

	dsi = devm_mipi_dsi_device_register_full(dev, host, &info);
	if (IS_ERR(dsi))
		return dev_err_probe(dev, PTR_ERR(dsi),
				     "failed to create dsi device\n");

	ctx->dsi = dsi;

	dsi->lanes = dsi_lanes;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_NO_HFP | MIPI_DSI_MODE_VIDEO_NO_HBP |
			  MIPI_DSI_MODE_VIDEO_NO_HSA | MIPI_DSI_MODE_NO_EOT_PACKET;

	ret = devm_mipi_dsi_attach(dev, dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach dsi to host: %d\n", ret);
		return ret;
	}

	return 0;
}

static irqreturn_t sn65dsi83_irq(int irq, void *data)
{
	struct sn65dsi83 *ctx = data;

	sn65dsi83_handle_errors(ctx);
	return IRQ_HANDLED;
}

static int sn65dsi83_probe(struct i2c_client *client)
{
	const struct i2c_device_id *id = i2c_client_get_device_id(client);
	struct device *dev = &client->dev;
	enum sn65dsi83_model model;
	struct sn65dsi83 *ctx;
	int ret;

	ctx = devm_drm_bridge_alloc(dev, struct sn65dsi83, bridge, &sn65dsi83_funcs);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->dev = dev;
	INIT_WORK(&ctx->reset_work, sn65dsi83_reset_work);
	INIT_DELAYED_WORK(&ctx->monitor_work, sn65dsi83_monitor_work);

	if (dev->of_node) {
		model = (enum sn65dsi83_model)(uintptr_t)
			of_device_get_match_data(dev);
	} else {
		model = id->driver_data;
	}

	/* Put the chip in reset, pull EN line low, and assure 10ms reset low timing. */
	ctx->enable_gpio = devm_gpiod_get_optional(ctx->dev, "enable",
						   GPIOD_OUT_LOW);
	if (IS_ERR(ctx->enable_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->enable_gpio), "failed to get enable GPIO\n");

	usleep_range(10000, 11000);

	ret = sn65dsi83_parse_dt(ctx, model);
	if (ret)
		return ret;

	ctx->regmap = devm_regmap_init_i2c(client, &sn65dsi83_regmap_config);
	if (IS_ERR(ctx->regmap))
		return dev_err_probe(dev, PTR_ERR(ctx->regmap), "failed to get regmap\n");

	if (client->irq) {
		ctx->irq = client->irq;
		ret = devm_request_threaded_irq(ctx->dev, ctx->irq, NULL, sn65dsi83_irq,
						IRQF_ONESHOT, dev_name(ctx->dev), ctx);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request irq\n");
	}

	dev_set_drvdata(dev, ctx);
	i2c_set_clientdata(client, ctx);

	ctx->bridge.of_node = dev->of_node;
	ctx->bridge.pre_enable_prev_first = true;
	ctx->bridge.type = DRM_MODE_CONNECTOR_LVDS;
	drm_bridge_add(&ctx->bridge);

	ret = sn65dsi83_host_attach(ctx);
	if (ret) {
		dev_err_probe(dev, ret, "failed to attach DSI host\n");
		goto err_remove_bridge;
	}

	return 0;

err_remove_bridge:
	drm_bridge_remove(&ctx->bridge);
	return ret;
}

static void sn65dsi83_remove(struct i2c_client *client)
{
	struct sn65dsi83 *ctx = i2c_get_clientdata(client);

	drm_bridge_remove(&ctx->bridge);
}

static const struct i2c_device_id sn65dsi83_id[] = {
	{ "ti,sn65dsi83", MODEL_SN65DSI83 },
	{ "ti,sn65dsi84", MODEL_SN65DSI84 },
	{},
};
MODULE_DEVICE_TABLE(i2c, sn65dsi83_id);

static const struct of_device_id sn65dsi83_match_table[] = {
	{ .compatible = "ti,sn65dsi83", .data = (void *)MODEL_SN65DSI83 },
	{ .compatible = "ti,sn65dsi84", .data = (void *)MODEL_SN65DSI84 },
	{},
};
MODULE_DEVICE_TABLE(of, sn65dsi83_match_table);

static struct i2c_driver sn65dsi83_driver = {
	.probe = sn65dsi83_probe,
	.remove = sn65dsi83_remove,
	.id_table = sn65dsi83_id,
	.driver = {
		.name = "sn65dsi83",
		.of_match_table = sn65dsi83_match_table,
	},
};
module_i2c_driver(sn65dsi83_driver);

MODULE_AUTHOR("Marek Vasut <marex@denx.de>");
MODULE_DESCRIPTION("TI SN65DSI83 DSI to LVDS bridge driver");
MODULE_LICENSE("GPL v2");
