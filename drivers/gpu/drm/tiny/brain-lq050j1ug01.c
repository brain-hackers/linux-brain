// SPDX-License-Identifier: GPL-2.0+
/*
 * DRM driver for Sharp LQ050J1UG01 panels on Sharp Brain
 *
 * Copyright 2021 Suguru Saito <sg.sgch07@gmail.com>
 *
 * Based on brain.c
 * Copyright 2020 Takumi Sueda <puhitaku@gmail.com>
 *
 * Based on ili9341.c
 * Copyright 2018 David Lechner <david@lechnology.com>
 *
 * Based on mi0283qt.c:
 * Copyright 2016 Noralf Trønnes
 */

#include <linux/backlight.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <video/mipi_display.h>

#include "mxsfb_regs.h"
#define CTRL1_RESET (1 << 0)

#define RESET_TIMEOUT 1000000
#define TX_TIMEOUT 1000000

struct brain_drm_private {
	void __iomem *base;
	struct platform_device *pdev;
	struct clk *clk_lcdif;
	bool enabled;

	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_panel *panel;
};

static const struct {
	u8	payload;
	u8	data;
	u32	delay;
} lcd_regs[] = {
	{ 0x3a, 0, 0 },
		{ 0x55, 1, 0 },
	{ 0xb2, 0, 0 },
		{ 0x45, 1, 0 }, { 0x00, 1, 0 }, { 0xd9, 1, 0 }, { 0x00, 1, 0 },
		{ 0x00, 1, 0 },
	{ 0xb3, 0, 0 },
		{ 0x81, 1, 0 }, { 0x00, 1, 0 }, { 0x01, 1, 0 },
	{ 0xb4, 0, 0 },
		{ 0x00, 1, 0 },
	{ 0xb5, 0, 0 },
		{ 0x02, 1, 0 }, { 0x11, 1, 0 }, { 0x50, 1, 0 }, { 0x00, 1, 0 },
		{ 0x80, 1, 0 }, { 0x45, 1, 0 }, { 0x45, 1, 0 }, { 0x00, 1, 0 },
	{ 0xb6, 0, 0 },
		{ 0x1e, 1, 0 }, { 0x01, 1, 0 }, { 0x90, 1, 0 }, { 0x0a, 1, 0 },
		{ 0x02, 1, 0 }, { 0x58, 1, 0 },
	{ 0xb7, 0, 0 },
		{ 0x2a, 1, 0 }, { 0x91, 1, 0 }, { 0x5c, 1, 0 }, { 0x06, 1, 0 },
		{ 0x08, 1, 0 }, { 0x0c, 1, 0 }, { 0x00, 1, 0 }, { 0x1c, 1, 0 },
		{ 0x06, 1, 0 }, { 0x02, 1, 0 }, { 0x09, 1, 0 },
	{ 0xb9, 0, 0 },
		{ 0x00, 1, 0 }, { 0x32, 1, 0 }, { 0x01, 1, 0 }, { 0x40, 1, 0 },
		{ 0x00, 1, 0 },
	{ 0xc0, 0, 0 },
		{ 0xb7, 1, 0 }, { 0x03, 1, 0 },
	{ 0xc1, 0, 0 },
		{ 0x72, 1, 0 }, { 0x01, 1, 0 },
	{ 0xc2, 0, 0 },
		{ 0x37, 1, 0 }, { 0x2f, 1, 0 }, { 0x0c, 1, 0 },
	{ 0xc3, 0, 0 },
		{ 0x37, 1, 0 }, { 0x03, 1, 0 },
	{ 0xc7, 0, 0 },
		{ 0x01, 1, 0 }, { 0x33, 1, 0 }, { 0x03, 1, 0 },
	{ 0xca, 0, 0 },
		{ 0xbd, 1, 0 }, { 0x17, 1, 0 }, { 0x5b, 1, 0 }, { 0x5b, 1, 0 },
		{ 0x64, 1, 0 }, { 0x11, 1, 0 }, { 0x66, 1, 0 },
	{ 0xde, 0, 0 },
		{ 0x11, 1, 0 }, { 0x00, 1, 0 },
	{ 0xe0, 0, 0 },
		{ 0x24, 1, 0 }, { 0x3f, 1, 0 }, { 0x0e, 1, 0 }, { 0x0e, 1, 0 },
		{ 0x67, 1, 0 }, { 0xee, 1, 0 }, { 0xee, 1, 0 }, { 0xa3, 1, 0 },
		{ 0x04, 1, 0 },
	{ 0xe1, 0, 0 },
		{ 0x24, 1, 0 }, { 0x3f, 1, 0 }, { 0x0f, 1, 0 }, { 0x0e, 1, 0 },
		{ 0x78, 1, 0 }, { 0xee, 1, 0 }, { 0xed, 1, 0 }, { 0x93, 1, 0 },
		{ 0x04, 1, 0 },
	{ 0xe2, 0, 0 },
		{ 0x24, 1, 0 }, { 0x29, 1, 0 }, { 0x14, 1, 0 }, { 0x1c, 1, 0 },
		{ 0x67, 1, 0 }, { 0xdd, 1, 0 }, { 0xdd, 1, 0 }, { 0x97, 1, 0 },
		{ 0x0b, 1, 0 },
	{ 0xe3, 0, 0 },
		{ 0x24, 1, 0 }, { 0x29, 1, 0 }, { 0x14, 1, 0 }, { 0x1c, 1, 0 },
		{ 0x67, 1, 0 }, { 0xdd, 1, 0 }, { 0xdd, 1, 0 }, { 0x97, 1, 0 },
		{ 0x0a, 1, 0 },
	{ 0xe4, 0, 0 },
		{ 0x24, 1, 0 }, { 0x2a, 1, 0 }, { 0x15, 1, 0 }, { 0x1a, 1, 0 },
		{ 0x99, 1, 0 }, { 0xdd, 1, 0 }, { 0xed, 1, 0 }, { 0xa6, 1, 0 },
		{ 0x09, 1, 0 },
	{ 0xe5, 0, 0 },
		{ 0x24, 1, 0 }, { 0x2a, 1, 0 }, { 0x15, 1, 0 }, { 0x1a, 1, 0 },
		{ 0x88, 1, 0 }, { 0xdd, 1, 0 }, { 0xdd, 1, 0 }, { 0x97, 1, 0 },
		{ 0x0c, 1, 0 },
	{ 0x36, 0, 0 },
		{ 0x28, 1, 0 },
	{ 0x2c, 0, 0 },
		{ 0x00, 1, 0 },
};


static struct brain_drm_private *pipe_to_private(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct brain_drm_private, pipe);
}

static struct brain_drm_private *connector_to_private(struct drm_connector *connector)
{
	return container_of(connector, struct brain_drm_private, connector);
}

static int brain_clear_poll(const void __iomem *addr, u32 mask, u32 timeout)
{
	u32 reg;
	return readl_poll_timeout(addr, reg, !(reg & mask), 0, timeout);
}

static int brain_write_clear_poll(void __iomem *addr, u32 mask, u32 timeout)
{
	u32 reg;
	writel(mask, addr + REG_CLR);
	return readl_poll_timeout(addr, reg, !(reg & mask), 0, timeout);
}

static int brain_write_byte(struct brain_drm_private *priv, u8 payload, const u8 is_data)
{
	
	if (brain_clear_poll(priv->base + LCDC_CTRL, CTRL_RUN, TX_TIMEOUT)) {
		return -ETIMEDOUT;
	}
	writel(TRANSFER_COUNT_SET_VCOUNT(1) | TRANSFER_COUNT_SET_HCOUNT(1),
		   priv->base + LCDC_V4_TRANSFER_COUNT);

	writel(CTRL_DATA_SELECT | CTRL_RUN, priv->base + LCDC_CTRL + REG_CLR);

	if (is_data) {
		writel(CTRL_DATA_SELECT,  priv->base + LCDC_CTRL + REG_SET);
	}

	writel(CTRL_RUN, priv->base + LCDC_CTRL + REG_SET);

	if (brain_clear_poll(priv->base + LCDC_STAT, STAT_LFIFO_FULL, TX_TIMEOUT)) {
		return -ETIMEDOUT;
	}
	writel(payload, priv->base + LCDC_DATA);
	
	if (brain_clear_poll(priv->base + LCDC_CTRL, CTRL_RUN, TX_TIMEOUT)) {
		return -ETIMEDOUT;
	}
	return 0;
}

static int brain_reset_block(void __iomem *reset_addr)
{
	int ret;

	ret = brain_write_clear_poll(reset_addr, CTRL_SFTRST, RESET_TIMEOUT);
	if (ret)
		return ret;

	writel(CTRL_CLKGATE, reset_addr + REG_CLR);

	ret = brain_write_clear_poll(reset_addr, CTRL_SFTRST, RESET_TIMEOUT);
	if (ret)
		return ret;
	
	return brain_write_clear_poll(reset_addr, CTRL_CLKGATE, RESET_TIMEOUT);
}

static int brain_set_pixel_fmt(struct brain_drm_private *ili, struct drm_plane_state *plane_state)
{
	struct drm_crtc *crtc = &ili->pipe.crtc;
	struct drm_device *drm = crtc->dev;
	const u32 format = plane_state->fb->format->format;
	u32 ctrl, ctrl1;

	ctrl = CTRL_BYPASS_COUNT | CTRL_MASTER;

	/* CTRL1 contains IRQ config and status bits, preserve those. */
	ctrl1 = readl(ili->base + LCDC_CTRL1);
	ctrl1 &= CTRL1_CUR_FRAME_DONE_IRQ_EN | CTRL1_CUR_FRAME_DONE_IRQ;

	switch (format) {
	case DRM_FORMAT_RGB565:
		dev_dbg(drm->dev, "Setting up RGB565 mode\n");
		ctrl |= CTRL_SET_WORD_LENGTH(0);
		ctrl1 |= CTRL1_SET_BYTE_PACKAGING(0xf);
		break;
	case DRM_FORMAT_XRGB8888:
		dev_dbg(drm->dev, "Setting up XRGB8888 mode\n");
		ctrl |= CTRL_SET_WORD_LENGTH(3);
		/* Do not use packed pixels = one pixel per word instead. */
		ctrl1 |= CTRL1_SET_BYTE_PACKAGING(0x7);
		break;
	default:
		dev_err(drm->dev, "Unhandled pixel format %08x\n", format);
		return -EINVAL;
	}

	writel(ctrl1, ili->base + LCDC_CTRL1);
	writel(ctrl, ili->base + LCDC_CTRL);

	return 0;
}

static void brain_set_bus_fmt(struct brain_drm_private *ili)
{
	struct drm_crtc *crtc = &ili->pipe.crtc;
	struct drm_device *drm = crtc->dev;
	u32 bus_format = MEDIA_BUS_FMT_RGB565_1X16;
	u32 reg;

	reg = readl(ili->base + LCDC_CTRL);

	if (ili->connector.display_info.num_bus_formats)
		bus_format = ili->connector.display_info.bus_formats[0];

	reg &= ~CTRL_BUS_WIDTH_MASK;
	switch (bus_format) {
	case MEDIA_BUS_FMT_RGB565_1X16:
		reg |= CTRL_SET_BUS_WIDTH(STMLCDIF_16BIT);
		break;
	case MEDIA_BUS_FMT_RGB666_1X18:
		reg |= CTRL_SET_BUS_WIDTH(STMLCDIF_18BIT);
		break;
	case MEDIA_BUS_FMT_RGB888_1X24:
		reg |= CTRL_SET_BUS_WIDTH(STMLCDIF_24BIT);
		break;
	default:
		dev_err(drm->dev, "Unknown media bus format %d\n", bus_format);
		break;
	}
	writel(reg, ili->base + LCDC_CTRL);
}


static void brain_enable(struct drm_simple_display_pipe *pipe,
			     struct drm_crtc_state *crtc_state,
			     struct drm_plane_state *plane_state)
{
	struct brain_drm_private *ili;
	struct drm_display_mode *m = &crtc_state->adjusted_mode;
	int i, ret, idx;
	u32 valid;
#if 0
	u8 mac = 0;
#endif

	ili = pipe_to_private(pipe);
	m = &ili->pipe.crtc.state->adjusted_mode;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	clk_prepare_enable(ili->clk_lcdif);

	ret = brain_reset_block(ili->base);
	if (ret)
		return;

	writel(CTRL1_FIFO_CLEAR, ili->base + LCDC_CTRL1 + REG_SET);

	ret = brain_set_pixel_fmt(ili, plane_state);
	if (ret)
		return;

	/* Unset DOTCLK */
	writel(CTRL_MASTER | CTRL_DOTCLK_MODE | CTRL_BYPASS_COUNT, ili->base + LCDC_CTRL + REG_CLR);

	brain_set_bus_fmt(ili);

	/* Timing */
	writel(TIMING_CMD_HOLD(1) | TIMING_CMD_SETUP(1) | TIMING_DATA_HOLD(1) | TIMING_DATA_SETUP(1),
		ili->base + LCDC_TIMING);

	/* Initialize LCD */
	/* Decrease the bus width to 8-bit temporarily */
	valid = CTRL1_GET_BYTE_PACKAGING(readl(ili->base + LCDC_CTRL1));
	writel(CTRL1_SET_BYTE_PACKAGING(0xf), ili->base + LCDC_CTRL1 + REG_CLR);
	writel(CTRL1_SET_BYTE_PACKAGING(0x3), ili->base + LCDC_CTRL1 + REG_SET);

	/* Reset LCD Controller */
	writel(CTRL1_RESET, ili->base + LCDC_CTRL1 + REG_SET);
	mdelay(30);
	writel(CTRL1_RESET, ili->base + LCDC_CTRL1 + REG_CLR);
	mdelay(30);
	writel(CTRL1_RESET, ili->base + LCDC_CTRL1 + REG_SET);
	mdelay(30);

	for (i = 0; i < ARRAY_SIZE(lcd_regs); i++) {
		brain_write_byte(ili, lcd_regs[i].payload, lcd_regs[i].data);
		mdelay(lcd_regs[i].delay);
	}

#if 0
	if (of_property_read_bool(ili->pdev->dev.of_node, "sharp,mac-flip-x")) {
		mac |= ILI9805_MADCTL_MX;
	}

	if (of_property_read_bool(ili->pdev->dev.of_node, "sharp,mac-flip-y")) {
		mac |= ILI9805_MADCTL_MY;
	}

	if (of_property_read_bool(ili->pdev->dev.of_node, "sharp,mac-flip-y-gs")) {
		mac |= ILI9805_MADCTL_GS;
	}

	if (of_property_read_bool(ili->pdev->dev.of_node, "sharp,mac-transpose")) {
		mac |= ILI9805_MADCTL_MV;
	}

	if (of_property_read_bool(ili->pdev->dev.of_node, "sharp,mac-bgr")) {
		mac |= ILI9805_MADCTL_BGR;
	}

	brain_write_byte(ili, 0x36, 0); /* Memory Access Control */
	brain_write_byte(ili, mac, 1);

	if (of_property_read_bool(ili->pdev->dev.of_node, "sharp,mac-inversion")) {
		brain_write_byte(ili, 0x21, 0); /* Display Inversion On */
	}
#endif
	brain_write_byte(ili, 0x11, 0); /* Sleep Out */
	mdelay(120);

	brain_write_byte(ili, 0x34, 0);

	brain_write_byte(ili, 0x29, 0); /* Display On */
	mdelay(120);

	brain_write_byte(ili, 0x2a, 0); /* Column Address Set */
	brain_write_byte(ili, 0x00, 1); /* Start Column in 2 Bytes */
	brain_write_byte(ili, 0x00, 1);
#if 0
	brain_write_byte(ili, (m->hdisplay & 0xff00) >> 8, 1); /* End Column in 2 Bytes */
	brain_write_byte(ili, (m->hdisplay & 0x00ff) >> 0, 1);
#else
	brain_write_byte(ili, (m->vdisplay & 0xff00) >> 8, 1);
	brain_write_byte(ili, (m->vdisplay & 0x00ff) >> 0, 1);
#endif

	brain_write_byte(ili, 0x2b, 0); /* Page Address Set */
	brain_write_byte(ili, 0x00, 1); /* Start Page in 2 Bytes */
	brain_write_byte(ili, 0x00, 1);
#if 0
	brain_write_byte(ili, (m->vdisplay & 0xff00) >> 8, 1); /* End Page in 2 Bytes */
	brain_write_byte(ili, (m->vdisplay & 0x00ff) >> 0, 1);
#else
	brain_write_byte(ili, (m->hdisplay & 0xff00) >> 8, 1);
	brain_write_byte(ili, (m->hdisplay & 0x00ff) >> 0, 1);
#endif

	brain_write_byte(ili, 0x2c, 0); /* Memory Write */

	writel(CTRL1_SET_BYTE_PACKAGING(0xf), ili->base + LCDC_CTRL1 + REG_CLR);
	writel(CTRL1_SET_BYTE_PACKAGING(valid), ili->base + LCDC_CTRL1 + REG_SET);

	/* Ready to transmit */
	writel(CTRL_MASTER, ili->base + LCDC_CTRL + REG_SET);
	ili->enabled = true;
}

static void brain_disable(struct drm_simple_display_pipe *pipe)
{
	struct brain_drm_private *ili = pipe_to_private(pipe);

	clk_disable_unprepare(ili->clk_lcdif);
	return;
}

static void brain_fb_dirty_full(struct brain_drm_private *ili, struct drm_framebuffer *fb, struct drm_rect *rect) {
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	int idx;

	u16 width = fb->width;
	u16 height = fb->height;
//
	u32 valid;
//

	if (!ili->enabled)
		return;

	if (!drm_dev_enter(fb->dev, &idx))
		return;

	if (brain_clear_poll(ili->base + LCDC_CTRL, CTRL_RUN, TX_TIMEOUT)) {
		dev_warn(&ili->pdev->dev, "Exceeded timeout\n");
		goto timeout_exit;
	}

#if 1
	writel(CTRL_MASTER | CTRL_DATA_SELECT, ili->base + LCDC_CTRL + REG_CLR);

	valid = CTRL1_GET_BYTE_PACKAGING(readl(ili->base + LCDC_CTRL1));
	writel(CTRL1_SET_BYTE_PACKAGING(0xf), ili->base + LCDC_CTRL1 + REG_CLR);
	writel(CTRL1_SET_BYTE_PACKAGING(0x3), ili->base + LCDC_CTRL1 + REG_SET);

	brain_write_byte(ili, 0x2a, 0); /* Column Address Set */
	brain_write_byte(ili, 0x00, 1); /* Start Column in 2 Bytes */
	brain_write_byte(ili, 0x00, 1);
	brain_write_byte(ili, (height & 0xff00) >> 8, 1);
	brain_write_byte(ili, (height & 0x00ff) >> 0, 1);

	brain_write_byte(ili, 0x2b, 0); /* Page Address Set */
	brain_write_byte(ili, 0x00, 1); /* Start Page in 2 Bytes */
	brain_write_byte(ili, 0x00, 1);
	brain_write_byte(ili, (width & 0xff00) >> 8, 1);
	brain_write_byte(ili, (width & 0x00ff) >> 0, 1);

	brain_write_byte(ili, 0x2c, 0); /* Memory Write */

	writel(CTRL1_SET_BYTE_PACKAGING(0xf), ili->base + LCDC_CTRL1 + REG_CLR);
	writel(CTRL1_SET_BYTE_PACKAGING(valid), ili->base + LCDC_CTRL1 + REG_SET);

	writel(CTRL_MASTER, ili->base + LCDC_CTRL + REG_SET);
#endif

	writel(cma_obj->paddr, ili->base + LCDC_V4_CUR_BUF);
	writel(cma_obj->paddr, ili->base + LCDC_V4_NEXT_BUF);
	writel(TRANSFER_COUNT_SET_VCOUNT(height) | TRANSFER_COUNT_SET_HCOUNT(width),
	       ili->base + LCDC_V4_TRANSFER_COUNT);
	writel(CTRL_DATA_SELECT | CTRL_RUN, ili->base + LCDC_CTRL + REG_SET);

timeout_exit:
	drm_dev_exit(idx);
}

static void brain_update(struct drm_simple_display_pipe *pipe,
			  struct drm_plane_state *old_state)
{
	struct brain_drm_private *ili = pipe_to_private(pipe);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		brain_fb_dirty_full(ili, state->fb, &rect);

	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}
}

static const struct drm_simple_display_pipe_funcs brain_pipe_funcs = {
	.enable = brain_enable,
	.disable = brain_disable,
	.update = brain_update,
	.prepare_fb = drm_gem_fb_simple_display_pipe_prepare_fb,
};

DEFINE_DRM_GEM_CMA_FOPS(brain_fops);

static int brain_connector_get_modes(struct drm_connector *connector)
{
	struct brain_drm_private *ili;
	static struct drm_display_mode modei;
	struct drm_display_mode* mode;
	u32 hd, vd, hd_mm, vd_mm;

	ili = connector_to_private(connector);

	of_property_read_u32(ili->pdev->dev.of_node, "sharp,lcd-width", &hd);
	of_property_read_u32(ili->pdev->dev.of_node, "sharp,lcd-height", &vd);
	of_property_read_u32(ili->pdev->dev.of_node, "sharp,lcd-width-mm", &hd_mm);
	of_property_read_u32(ili->pdev->dev.of_node, "sharp,lcd-height-mm", &vd_mm);

	// Equivalent to DRM_SIMPLE_MODE macro
	modei.type = DRM_MODE_TYPE_DRIVER;
	modei.clock = 1;
	modei.hdisplay = hd;
	modei.hsync_start = hd;
	modei.hsync_end = hd;
	modei.htotal = hd;
	modei.vdisplay = vd;
	modei.vsync_start = vd;
	modei.vsync_end = vd;
	modei.vtotal = vd;
	modei.width_mm = hd_mm;
	modei.height_mm = vd_mm;

	mode = drm_mode_duplicate(connector->dev, &modei);
	if (!mode) {
		DRM_ERROR("Failed to duplicate mode\n");
		return 0;
	}

	if (mode->name[0] == '\0')
		drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	if (mode->width_mm) {
		connector->display_info.width_mm = mode->width_mm;
		connector->display_info.height_mm = mode->height_mm;
	}

	return 1;
}

static const struct drm_connector_helper_funcs brain_connector_helper_funcs = {
	.get_modes = brain_connector_get_modes,
};

static const struct drm_connector_funcs brain_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs brain_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static struct drm_driver brain_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &brain_fops,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
	.name			= "brain",
	.desc			= "Sharp LQ050J1UG01",
	.date			= "20210417",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id brain_of_match[] = {
	{ .compatible = "sharp,brainlcd" },
	{},
};
MODULE_DEVICE_TABLE(of, brain_of_match);

static int brain_probe(struct platform_device *pdev)
{
	struct drm_device *drm;
	struct brain_drm_private *ili;
	struct resource *res;
	struct gpio_desc *en;
	u32 width, height;
	int i, ret;

	const struct of_device_id *of_id =
		of_match_device(brain_of_match, &pdev->dev);

	static const uint32_t formats[] = {
		DRM_FORMAT_RGB565,
	};

	if (!pdev->dev.of_node)
		return -ENODEV;
	
	if (of_id)
		pdev->id_entry = of_id->data;

	drm = drm_dev_alloc(&brain_driver, &pdev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	ili = devm_kzalloc(&pdev->dev, sizeof(*ili), GFP_KERNEL);
	if (!ili) {
		drm_dev_put(drm);
		return -ENOMEM;
	}

	ili->pdev = pdev;
	drm->dev_private = ili;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ili->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ili->base)) {
		drm_dev_put(drm);
		return PTR_ERR(ili->base);
	}

	ili->enabled = false;

	ili->clk_lcdif = devm_clk_get(drm->dev, NULL);
	if (IS_ERR(ili->clk_lcdif)) {
		drm_dev_put(drm);
		return PTR_ERR(ili->clk_lcdif);
	}

	drm_mode_config_init(drm);
	drm->mode_config.funcs = &brain_mode_config_funcs;

	for (i = 0; i < gpiod_count(&pdev->dev, "sharp,en"); i++) {
		en = devm_gpiod_get_index(&pdev->dev, "sharp,en", i, GPIOD_OUT_HIGH);
		if (IS_ERR(en))
			dev_err(&pdev->dev, "failed to get gpio %d\n", i);
			continue;
		ret = gpiod_direction_output(en, 1);
		if (IS_ERR(en))
			dev_err(&pdev->dev, "failed to output gpio %d\n", i);
	}
	
	of_property_read_u32(ili->pdev->dev.of_node, "sharp,lcd-width", &width);
	of_property_read_u32(ili->pdev->dev.of_node, "sharp,lcd-height", &height);

	drm->mode_config.min_width = width;
	drm->mode_config.max_width = width;
	drm->mode_config.min_height = height;
	drm->mode_config.max_height = height;

	drm_connector_helper_add(&ili->connector, &brain_connector_helper_funcs);
	ret = drm_connector_init(drm, &ili->connector, &brain_connector_funcs, DRM_MODE_CONNECTOR_DPI);
	if (ret) {
		drm_dev_put(drm);
		return ret;
	}

	ret = drm_simple_display_pipe_init(drm, &ili->pipe, &brain_pipe_funcs,
					   formats, ARRAY_SIZE(formats),
					   NULL, &ili->connector);
	if (ret) {
		drm_dev_put(drm);
		return ret;
	}

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		drm_dev_put(drm);
		return ret;
	}

	platform_set_drvdata(pdev, drm);
	drm_fbdev_generic_setup(drm, 16);
	return 0;
}

static int brain_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	return 0;
}

static void brain_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static struct platform_driver brain_platform_driver = {
	.probe	= brain_probe,
	.remove	= brain_remove,
	.shutdown = brain_shutdown,
	.driver = {
		.name   = "brain",
		.of_match_table = brain_of_match,
	},
};

module_platform_driver(brain_platform_driver);

MODULE_DESCRIPTION("DRM driver for Sharp LQ050J1UG01 panels on Sharp Brain");
MODULE_AUTHOR("Suguru Saito <sg.sgch07@gmail.com>");
MODULE_LICENSE("GPL");
