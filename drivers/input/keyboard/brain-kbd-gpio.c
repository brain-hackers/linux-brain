// SPDX-License-Identifier: GPL-2.0-only
/*
 * SHARP Brain keyboard (I2C) driver
 *
 * Copyright 2021 Takumi Sueda
 *
 * Author: Takumi Sueda <puhitaku@gmail.com>
 *
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/input-polldev.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#define DEV_NAME "brain-kbd-gpio"

#define BK_CMD_KEYCODE 0x04

#define BK_IS_PRESSED(val) ((~val & 0x40) >> 6)

struct bk_gpio_data {
	struct input_polled_dev *polldev;
	struct gpio_desc* in[8];
	struct gpio_desc* out[7];
	unsigned int km[7][7];
	unsigned int km_symbol[7][7];
	int kmlen;
	int kmlen_symbol;
	unsigned int sym_key_bank;
	unsigned int sym_key_num;

	bool symbol;
	ulong last_in[7];
	bool pressed[7][7];
};

static void bk_gpio_read_keys(struct input_polled_dev *polldev, ulong* result)
{
	struct bk_gpio_data *kbd = polldev->private;
	struct device *dev = &polldev->input->dev;
	int try, i, err;
	ulong in[10][7];

	for (try = 0; try < ARRAY_SIZE(in); try++) {
		for (i = 0; i < ARRAY_SIZE(in[0]); i++) {
			gpiod_set_value(kbd->out[i], 1);
			udelay(100);
			in[try][i] = 0;
			err = gpiod_get_array_value(8, kbd->in, NULL, &in[try][i]);
			if (err) {
				dev_err(dev, "failed to get array value: %d\n", err);
			}
			in[try][i] = ~((in[try][i] ^ (in[try][i] >> 1)) & 0x1f ^ (in[try][i] >> 1)) & 0x7f;
			gpiod_set_value(kbd->out[i], 0);
		}

		if (try < 3) {
			continue;
		}

		if (
			memcmp(in[try-2], in[try-1], sizeof(in[0])) == 0 &&
			memcmp(in[try-1], in[try], sizeof(in[0])) == 0
		) {
			for (i = 0; i < ARRAY_SIZE(in[0]); i++) {
				result[i] = in[try][i];
			}
			break;
		}

		// not to exceed 10ms in total
		mdelay(1);
	}

	if (try == ARRAY_SIZE(in)) {
		dev_dbg(dev, "failed to debounce: exceeded %d tries\n", ARRAY_SIZE(in));
		for (i = 0; i < ARRAY_SIZE(in[0]); i++) {
			result[i] = 0;
		}
	}
}

static void bk_gpio_poll(struct input_polled_dev *polldev)
{
	struct bk_gpio_data *kbd = polldev->private;
	struct device *dev = &polldev->input->dev;
	int i, j, bank;
	ulong in[7], agg;

	bk_gpio_read_keys(polldev, in);

	if (memcmp(in, kbd->last_in, sizeof(in)) == 0) {
		goto skip;
	}

	dev_dbg(
		dev,
		"%02lx %02lx %02lx %02lx %02lx %02lx %02lx",
		in[0], in[1], in[2], in[3], in[4], in[5], in[6]
	);

	agg = 0;
	for (i = 0; i < 7; i++) {
		agg |= in[i];
	}

	for (i = 0; i < 7; i++) {
		bank = 1 << i;
		if (agg & bank) {
			for (j = 0; j < 7; j++) {
				// skip invalid keymap
				if (kbd->km[i][j] == UINT_MAX) {
					continue;
				}

				if ((in[j] & bank) == 0) {
					if (!kbd->pressed[i][j]) {
						if (i == kbd->sym_key_bank && j == kbd->sym_key_num) {
							dev_dbg(dev, "symbol\n");
							kbd->symbol = true;
						} else {
							dev_dbg(dev, "P: %04x\n", kbd->km[i][j]);
							input_report_key(
								polldev->input,
								kbd->symbol ? kbd->km_symbol[i][j] : kbd->km[i][j],
								1
							);
						}
					}
					kbd->pressed[i][j] = true;
				} else {
					kbd->pressed[i][j] = false;
				}
			}
		} else {
			// flush deasserted bank
			for (j = 0; j < 7; j++) {
				if (!kbd->pressed[i][j]) {
					continue;
				}
				kbd->pressed[i][j] = false;

				if (i == kbd->sym_key_bank && j == kbd->sym_key_num) {
					kbd->symbol = false;
				} else {
					dev_dbg(dev, "R: %04x\n", kbd->km[i][j]);
					input_report_key(polldev->input, kbd->km[i][j], 0);
					input_report_key(polldev->input, kbd->km_symbol[i][j], 0);
				}
			}
		}
	}

	input_sync(polldev->input);
skip:
	memcpy(kbd->last_in, in, sizeof(in));
}

static int bk_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bk_gpio_data *kbd;
	int i, j, count, err, cells, len, offset;
	struct gpio_desc *gpio;
	u32 keydef[3];

	kbd = devm_kzalloc(dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd) {
		return -ENOMEM;
	}

	count = gpiod_count(dev, "matrix-in");
	if (count != 8) {
		dev_err(dev, "expected length of matrix-in-gpios is 8, not %d\n", count);
		return -EINVAL;
	}

	for (i = 0; i < gpiod_count(dev, "matrix-in"); i++) {
		gpio = devm_gpiod_get_index(dev, "matrix-in", i, GPIOD_IN);
		if (IS_ERR(gpio)) {
			dev_err(dev, "failed to get input gpio %d\n", i);
			return -EINVAL;
		}
		kbd->in[i] = gpio;
	}

	count = gpiod_count(dev, "matrix-out");
	if (count != 7) {
		dev_err(dev, "expected length of matrix-out-gpios is 7, not %d\n", count);
		return -EINVAL;
	}

	for (i = 0; i < gpiod_count(dev, "matrix-out"); i++) {
		gpio = devm_gpiod_get_index(dev, "matrix-out", i, GPIOD_OUT_LOW);
		if (IS_ERR(gpio)) {
			dev_err(dev, "failed to get output gpio %d\n", i);
			return -EINVAL;
		}
		gpiod_set_value(gpio, 0);
		kbd->out[i] = gpio;
	}

	// Init input device

	kbd->polldev = devm_input_allocate_polled_device(dev);
	if (!kbd->polldev) {
		dev_err(dev, "failed to allocate inpute device\n");
		return -ENOMEM;
	}

	kbd->polldev->private = kbd;
	kbd->polldev->poll = bk_gpio_poll;
	kbd->polldev->poll_interval = 100;
	kbd->polldev->input->name = DEV_NAME;
	kbd->polldev->input->id.bustype = BUS_HOST;

	__set_bit(EV_KEY, kbd->polldev->input->evbit); /* FIXME: is it really necessary? */
	__set_bit(EV_REP, kbd->polldev->input->evbit); /* autorepeat */

	// Parse keymap

	cells = 3;
	if (!of_get_property(dev->of_node, "keymap", &len)) {
		dev_err(dev, "DT node has no keymap\n");
		return -EINVAL;
	}

	len /= sizeof(u32) * cells;
	kbd->kmlen = len;

	for (i = 0; i < 7; i++) {
		for (j = 0; j < 7; j++) {
			// sentinel value to indicate unused mapping
			kbd->km[i][j] = UINT_MAX;
		}
	}

	for (i = 0; i < len; i++) {
		offset = i * cells;
		for (j = 0; j < 3; j++) {
			if (of_property_read_u32_index(dev->of_node, "keymap",
						       offset + j, keydef + j)) {
				dev_err(dev,
					"could not read DT property (brain keycode)\n");
				return -EINVAL;
			}
		}
		kbd->km[keydef[0] & 0xff][keydef[1] & 0xff] = keydef[2];
		dev_dbg(dev, "normal: brain: %x %x, kernel: %x",
			keydef[0], keydef[1], keydef[2]);

		input_set_capability(kbd->polldev->input, EV_KEY, keydef[2]);
	}

	if (!of_get_property(dev->of_node, "keymap-symbol", &len)) {
		dev_err(dev, "DT node has no keymap (symbol)\n");
		return -EINVAL;
	}

	len /= sizeof(u32) * cells;
	kbd->kmlen_symbol = len;

	for (i = 0; i < 7; i++) {
		for (j = 0; j < 7; j++) {
			// sentinel value to indicate unused mapping
			kbd->km_symbol[i][j] = UINT_MAX;
		}
	}

	for (i = 0; i < len; i++) {
		offset = i * cells;
		for (j = 0; j < 3; j++) {
			if (of_property_read_u32_index(dev->of_node, "keymap-symbol",
						       offset + j, keydef + j)) {
				dev_err(dev,
					"could not read DT property (brain keycode)\n");
				return -EINVAL;
			}
		}
		kbd->km_symbol[keydef[0] & 0xff][keydef[1] & 0xff] = keydef[2];
		dev_dbg(dev, "symbol: brain: %02x %02x, kernel: %02x",
			keydef[0], keydef[1], keydef[2]);

		input_set_capability(kbd->polldev->input, EV_KEY, keydef[2]);
	}

	if (of_property_read_u32_index(dev->of_node, "symbol-key", 0, &kbd->sym_key_bank)) {
		dev_err(dev, "could not read symbol key bank\n");
		return -EINVAL;
	}
	dev_dbg(dev, "sym_key_bank = %d\n", kbd->sym_key_bank);

	if (of_property_read_u32_index(dev->of_node, "symbol-key", 1, &kbd->sym_key_num)) {
		dev_err(dev, "could not read symbol key num\n");
		return -EINVAL;
	}
	dev_dbg(dev, "sym_key_num = %d\n", kbd->sym_key_num);

	for (i = 0; i < 7; i++) {
		for (j = 0; j < 7; j++) {
			kbd->pressed[i][j] = false;
		}
	}

	err = input_register_polled_device(kbd->polldev);
	if (err) {
		dev_err(dev, "failed to register input device: %d\n",
			err);
		return err;
	}

	return 0;
}

static const struct of_device_id bk_gpio_of_match[] = {
	{
		.compatible = "sharp,brain-kbd-gpio",
	},
	{ /* sentinel */ },
};

static struct platform_driver bk_gpio_driver = {
	.driver =  {
		.name = DEV_NAME,
		.of_match_table = of_match_ptr(bk_gpio_of_match),
	},
	.probe = bk_gpio_probe,
};
module_platform_driver(bk_gpio_driver);

MODULE_AUTHOR("Takumi Sueda <puhitaku@gmail.com>");
MODULE_DESCRIPTION("SHARP Brain keyboard driver");
MODULE_LICENSE("GPL v2");
