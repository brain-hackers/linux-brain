// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for Sitronix ST7586 panels for Brain 2nd SubLCD
 *
 * Copyright 2021 Suguru Saito <sg.sgch07@gmail.com>
 *
 * based on st7586.c
 * Copyright 2017 David Lechner <david@lechnology.com>
 */

#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_mipi_dbi.h>
#include <drm/drm_rect.h>
#include <drm/drm_vblank.h>

/* controller-specific commands */
#define ST7586_DISP_MODE_GRAY	0x38
#define ST7586_DISP_MODE_MONO	0x39
#define ST7586_ENABLE_DDRAM	0x3a
#define ST7586_SET_DISP_DUTY	0xb0
#define ST7586_SET_OUTPUT_COM	0xb1
#define ST7586_SET_PART_DISP	0xb4
#define ST7586_SET_NLINE_INV	0xb5
#define ST7586_SET_VOP		0xc0
#define ST7586_SET_BIAS_SYSTEM	0xc3
#define ST7586_SET_BOOST_LEVEL	0xc4
#define ST7586_SET_VOP_OFFSET	0xc7
#define ST7586_ENABLE_ANALOG	0xd0
#define ST7586_AUTO_READ_CTRL	0xd7
#define ST7586_OTP_RW_CTRL	0xe0
#define ST7586_OTP_CTRL_OUT	0xe1
#define ST7586_OTP_READ		0xe3

#define ST7586_DISP_CTRL_MX	BIT(6)
#define ST7586_DISP_CTRL_MY	BIT(7)

#define BRAIN_PAGE_OFFSET	0x28

/*
 * The ST7586 controller has an unusual pixel format where 2bpp grayscale is
 * packed 3 pixels per byte with the first two pixels using 3 bits and the 3rd
 * pixel using only 2 bits.
 *
 * |  D7  |  D6  |  D5  ||      |      || 2bpp |
 * | (D4) | (D3) | (D2) ||  D1  |  D0  || GRAY |
 * +------+------+------++------+------++------+
 * |  1   |  1   |  1   ||  1   |  1   || 0  0 | black
 * |  1   |  0   |  0   ||  1   |  0   || 0  1 | dark gray
 * |  0   |  1   |  0   ||  0   |  1   || 1  0 | light gray
 * |  0   |  0   |  0   ||  0   |  0   || 1  1 | white
 */

static const u8 st7586_lookup[] = { 0x7, 0x4, 0x2, 0x0 };

static void st7586_xrgb8888_to_gray332_word(u16 *dst, void *vaddr,
				       struct drm_framebuffer *fb,
				       struct drm_rect *clip)
{
	size_t len = (clip->x2 - clip->x1) * (clip->y2 - clip->y1);
	unsigned int x, y;
	u8 *src, *buf;
	u16 packed;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return;

	drm_fb_xrgb8888_to_gray8(buf, vaddr, fb, clip);
	src = buf;

	for (y = clip->y1; y < clip->y2; y++) {
		for (x = clip->x1; x < clip->x2; x += 3*2) {
			/*
			 * buf: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, a, b, ...
			 * dst: 210543, 876ba9, ...
			 */
			packed = (st7586_lookup[*src++ >> 6] & 0x03) << 8;
			packed |= st7586_lookup[*src++ >> 6] << 10;
			packed |= st7586_lookup[*src++ >> 6] << 13;
			packed |= (st7586_lookup[*src++ >> 6] & 0x03) << 0;
			packed |= st7586_lookup[*src++ >> 6] << 2;
			packed |= st7586_lookup[*src++ >> 6] << 5;

			*dst++ = packed;
		}
	}

	kfree(buf);
}

static int st7586_buf_copy(void *dst, struct drm_framebuffer *fb,
			   struct drm_rect *clip)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct dma_buf_attachment *import_attach = cma_obj->base.import_attach;
	void *src = cma_obj->vaddr;
	int ret = 0;

	if (import_attach) {
		ret = dma_buf_begin_cpu_access(import_attach->dmabuf,
					       DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}

	st7586_xrgb8888_to_gray332_word(dst, src, fb, clip);

	if (import_attach)
		ret = dma_buf_end_cpu_access(import_attach->dmabuf,
					     DMA_FROM_DEVICE);

	return ret;
}

static void st7586_fb_dirty(struct drm_framebuffer *fb, struct drm_rect *rect)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(fb->dev);
	struct mipi_dbi *dbi = &dbidev->dbi;
	int start_col, end_col, start_page, end_page, len, idx, ret = 0;

	if (!dbidev->enabled)
		return;

	if (!drm_dev_enter(fb->dev, &idx))
		return;

	/* 3 pixels per byte, so grow clip to nearest multiple of 3 */
	rect->x1 = rounddown(rect->x1, 3);
	rect->x2 = roundup(rect->x2, 3);

	DRM_DEBUG_KMS("Flushing [FB:%d] " DRM_RECT_FMT "\n", fb->base.id, DRM_RECT_ARG(rect));

	ret = st7586_buf_copy(dbidev->tx_buf, fb, rect);
	if (ret)
		goto err_msg;

	/* Pixels are packed 3 per byte */
	start_col = rect->x1 / 3;
	end_col = rect->x2 / 3;

	len = (end_col - start_col) * (rect->y2 - rect->y1) / 2;

	/* Write to top-side DDRAM */
	start_page = rect->y1 + BRAIN_PAGE_OFFSET;
	end_page = (rect->y2 - rect->y1) / 2 + BRAIN_PAGE_OFFSET;
	mipi_dbi_command(dbi, MIPI_DCS_SET_COLUMN_ADDRESS,
			 (start_col >> 8) & 0xFF, start_col & 0xFF,
			 (end_col >> 8) & 0xFF, (end_col - 1) & 0xFF);
	mipi_dbi_command(dbi, MIPI_DCS_SET_PAGE_ADDRESS,
			 (start_page >> 8) & 0xFF, start_page & 0xFF,
			 (end_page >> 8) & 0xFF, (end_page - 1) & 0xFF);

	ret = mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START,
				   (u8 *)dbidev->tx_buf, len);
	if (ret)
		goto err_msg;

	/* Write to bottom-side DDRAM */
	start_page = (rect->y2 - rect->y1) / 2 + BRAIN_PAGE_OFFSET;
	end_page = rect->y2 + BRAIN_PAGE_OFFSET;
	mipi_dbi_command(dbi, MIPI_DCS_SET_COLUMN_ADDRESS,
			 (start_col >> 8) & 0xFF, start_col & 0xFF,
			 (end_col >> 8) & 0xFF, (end_col - 1) & 0xFF);
	mipi_dbi_command(dbi, MIPI_DCS_SET_PAGE_ADDRESS,
			 (start_page >> 8) & 0xFF, start_page & 0xFF,
			 (end_page >> 8) & 0xFF, (end_page - 1) & 0xFF);

	ret = mipi_dbi_command_buf(dbi, MIPI_DCS_WRITE_MEMORY_START,
				   ((u8 *)dbidev->tx_buf) + len, len);
err_msg:
	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n", ret);

	drm_dev_exit(idx);
}

static void st7586_pipe_update(struct drm_simple_display_pipe *pipe,
			       struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_rect rect;

	if (drm_atomic_helper_damage_merged(old_state, state, &rect))
		st7586_fb_dirty(state->fb, &rect);

	if (crtc->state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		spin_unlock_irq(&crtc->dev->event_lock);
		crtc->state->event = NULL;
	}
}

static void st7586_pipe_enable(struct drm_simple_display_pipe *pipe,
			       struct drm_crtc_state *crtc_state,
			       struct drm_plane_state *plane_state)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);
	struct drm_framebuffer *fb = plane_state->fb;
	struct mipi_dbi *dbi = &dbidev->dbi;
	struct drm_rect rect = {
		.x1 = 0,
		.x2 = fb->width,
		.y1 = 0,
		.y2 = fb->height,
	};
	int idx, ret;
	u8 addr_mode;

	if (!drm_dev_enter(pipe->crtc.dev, &idx))
		return;

	DRM_DEBUG_KMS("\n");

	ret = mipi_dbi_poweron_reset(dbidev);
	if (ret)
		goto out_exit;

	mipi_dbi_command(dbi, ST7586_AUTO_READ_CTRL, 0x9f);
	mipi_dbi_command(dbi, ST7586_OTP_RW_CTRL, 0x00);

	msleep(10);

	mipi_dbi_command(dbi, ST7586_OTP_READ);

	msleep(20);

	mipi_dbi_command(dbi, ST7586_OTP_CTRL_OUT);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_SLEEP_MODE);
	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_OFF);

	msleep(50);

	mipi_dbi_command(dbi, ST7586_SET_VOP_OFFSET, 0x00);
	mipi_dbi_command(dbi, ST7586_SET_VOP, 0x19, 0x01);
	mipi_dbi_command(dbi, ST7586_SET_BIAS_SYSTEM, 0x03);
	mipi_dbi_command(dbi, ST7586_SET_BOOST_LEVEL, 0x07);
	mipi_dbi_command(dbi, ST7586_ENABLE_ANALOG, 0x1d);
	mipi_dbi_command(dbi, ST7586_SET_NLINE_INV, 0x00);
	mipi_dbi_command(dbi, ST7586_DISP_MODE_GRAY);
	mipi_dbi_command(dbi, ST7586_ENABLE_DDRAM, 0x02);

	switch (dbidev->rotation) {
	default:
		addr_mode = 0x00;
		break;
	case 90:
		addr_mode = ST7586_DISP_CTRL_MY;
		break;
	case 180:
		addr_mode = ST7586_DISP_CTRL_MX | ST7586_DISP_CTRL_MY;
		break;
	case 270:
		addr_mode = ST7586_DISP_CTRL_MX;
		break;
	}
	mipi_dbi_command(dbi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	mipi_dbi_command(dbi, ST7586_SET_DISP_DUTY, 0x77);
	mipi_dbi_command(dbi, ST7586_SET_OUTPUT_COM, BRAIN_PAGE_OFFSET);
	mipi_dbi_command(dbi, ST7586_SET_PART_DISP, 0xa0);
	mipi_dbi_command(dbi, MIPI_DCS_SET_PARTIAL_AREA, 0x00, 0x00, 0x00, 0x9f);
	mipi_dbi_command(dbi, MIPI_DCS_EXIT_INVERT_MODE);

	msleep(100);

	dbidev->enabled = true;
	st7586_fb_dirty(fb, &rect);

	mipi_dbi_command(dbi, MIPI_DCS_SET_DISPLAY_ON);
out_exit:
	drm_dev_exit(idx);
}

static void st7586_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct mipi_dbi_dev *dbidev = drm_to_mipi_dbi_dev(pipe->crtc.dev);

	/*
	 * This callback is not protected by drm_dev_enter/exit since we want to
	 * turn off the display on regular driver unload. It's highly unlikely
	 * that the underlying SPI controller is gone should this be called after
	 * unplug.
	 */

	DRM_DEBUG_KMS("\n");

	if (!dbidev->enabled)
		return;

	mipi_dbi_command(&dbidev->dbi, MIPI_DCS_SET_DISPLAY_OFF);
	dbidev->enabled = false;
}

static const u32 st7586_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static const struct drm_simple_display_pipe_funcs st7586_pipe_funcs = {
	.enable		= st7586_pipe_enable,
	.disable	= st7586_pipe_disable,
	.update		= st7586_pipe_update,
	.prepare_fb	= drm_gem_fb_simple_display_pipe_prepare_fb,
};

static const struct drm_display_mode st7586_mode = {
	DRM_SIMPLE_MODE(240, 120, 51, 26),
};

DEFINE_DRM_GEM_CMA_FOPS(st7586_fops);

static struct drm_driver st7586_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &st7586_fops,
	.release		= mipi_dbi_release,
	DRM_GEM_CMA_VMAP_DRIVER_OPS,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "brain-st7586",
	.desc			= "Sitronix ST7586 for Sharp Brain 2nd generation SubLCD",
	.date			= "20211002",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id st7586_of_match[] = {
	{ .compatible = "brain,st7586" },
	{},
};
MODULE_DEVICE_TABLE(of, st7586_of_match);

static const struct spi_device_id st7586_id[] = {
	{ "brain-slcd", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, st7586_id);

static int st7586_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct mipi_dbi_dev *dbidev;
	struct drm_device *drm;
	struct mipi_dbi *dbi;
	u32 rotation = 0;
	size_t bufsize;
	int ret;

	dbidev = kzalloc(sizeof(*dbidev), GFP_KERNEL);
	if (!dbidev)
		return -ENOMEM;

	dbi = &dbidev->dbi;
	drm = &dbidev->drm;
	ret = devm_drm_dev_init(dev, drm, &st7586_driver);
	if (ret) {
		kfree(dbidev);
		return ret;
	}

	drm_mode_config_init(drm);

	bufsize = (st7586_mode.vdisplay + 2) / 3 * st7586_mode.hdisplay;

	dbi->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(dbi->reset)) {
		DRM_DEV_ERROR(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(dbi->reset);
	}

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, dbi, NULL);
	if (ret)
		return ret;

	/* Cannot read from this controller via SPI */
	dbi->read_commands = NULL;

	ret = mipi_dbi_dev_init_with_formats(dbidev, &st7586_pipe_funcs,
					     st7586_formats, ARRAY_SIZE(st7586_formats),
					     &st7586_mode, rotation, bufsize);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	spi_set_drvdata(spi, drm);

	drm_fbdev_generic_setup(drm, 0);

	return 0;
}

static int st7586_remove(struct spi_device *spi)
{
	struct drm_device *drm = spi_get_drvdata(spi);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	return 0;
}

static void st7586_shutdown(struct spi_device *spi)
{
	drm_atomic_helper_shutdown(spi_get_drvdata(spi));
}

static struct spi_driver st7586_spi_driver = {
	.driver = {
		.name = "brain-st7586",
		.owner = THIS_MODULE,
		.of_match_table = st7586_of_match,
	},
	.id_table = st7586_id,
	.probe = st7586_probe,
	.remove = st7586_remove,
	.shutdown = st7586_shutdown,
};
module_spi_driver(st7586_spi_driver);

MODULE_DESCRIPTION("Sitronix ST7586 DRM driver for Sharp Brain 2nd generation SubLCD");
MODULE_AUTHOR("Suguru Saito <sg.sgch07@gmail.com>");
MODULE_LICENSE("GPL");
