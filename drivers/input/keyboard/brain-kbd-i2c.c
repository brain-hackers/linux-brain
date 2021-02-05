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
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#define BRAIN_KBD_I2C_DEV_NAME "brain-kbd-i2c"

#define BK_CMD_KEYCODE 0x04

#define BK_IS_PRESSED(val) ((~val & 0x40) >> 6)

struct keymap_def {
	u8 brain_keycode;
	unsigned int kernel_keycode;
};

struct bk_i2c_data {
	struct i2c_client *cli;
	struct input_dev *idev;
	struct keymap_def *km;
	struct keymap_def *km_symbol;
	int kmlen;
	int kmlen_symbol;

	bool symbol;
};

static bool detect_key(struct bk_i2c_data *kbd, u8 keycode)
{
	int i;

	if ((keycode & 0x3f) == 0x19) {
		if (BK_IS_PRESSED(keycode)) {
			dev_dbg(&kbd->cli->dev, "symbol pressed!\n");
			kbd->symbol = true;
		} else {
			dev_dbg(&kbd->cli->dev, "symbol released!\n");
			kbd->symbol = false;
		}
		return true;
	}

	if (kbd->symbol) {
		for (i = 0; i < kbd->kmlen_symbol; i++) {
			if ((keycode & 0x3f) ==
			    kbd->km_symbol[i].brain_keycode) {
				dev_dbg(&kbd->cli->dev,
					"symbol: pressed %02x\n",
					kbd->km_symbol[i].brain_keycode);
				input_report_key(
					kbd->idev,
					kbd->km_symbol[i].kernel_keycode,
					BK_IS_PRESSED(keycode));
				return true;
			}
		}
	} else {
		for (i = 0; i < kbd->kmlen; i++) {
			if ((keycode & 0x3f) == kbd->km[i].brain_keycode) {
				dev_dbg(&kbd->cli->dev,
					"normal: pressed %02x\n",
					kbd->km[i].brain_keycode);
				input_report_key(kbd->idev,
						 kbd->km[i].kernel_keycode,
						 BK_IS_PRESSED(keycode));
				return true;
			}
		}
	}
	return false;
}

static irqreturn_t bk_i2c_irq_handler(int irq, void *devid)
{
	struct bk_i2c_data *kbd = devid;
	s32 raw;
	u8 n, k1, k2, k3;

	raw = i2c_smbus_read_word_swapped(kbd->cli, BK_CMD_KEYCODE);
	if (raw < 0) {
		dev_err(&kbd->cli->dev, "failed to read keycode: %d\n", raw);
		goto err;
	}

	n = raw >> 8;
	k1 = raw & 0xff;

	if (k1 == 0x00) {
		dev_dbg(&kbd->cli->dev,
			"interrupted but no key press was found\n");
		goto err;
	}

	if (n == 1) {
		if (detect_key(kbd, k1)) {
			goto done;
		} else {
			dev_dbg(&kbd->cli->dev,
				"unknown key was pressed: %02x\n", (k1 & 0x3f));
			goto err;
		}
	}

	raw = i2c_smbus_read_word_swapped(kbd->cli, BK_CMD_KEYCODE);
	if (raw < 0) {
		dev_err(&kbd->cli->dev, "failed to read 2nd/3rd keycode: %d\n",
			raw);
		goto err;
	}

	k2 = (raw & 0xff00) >> 8;
	k3 = raw & 0xff;

	detect_key(kbd, k2);

	if (n == 3) {
		detect_key(kbd, k3);
	}

done:
	input_sync(kbd->idev);
err:
	return IRQ_HANDLED;
}

static int bk_i2c_probe(struct i2c_client *cli, const struct i2c_device_id *id)
{
	struct bk_i2c_data *kbd;
	int i, err, cells, len, offset;
	u32 brain_keycode, kernel_keycode;

	if (!i2c_check_functionality(cli->adapter, I2C_FUNC_SMBUS_BYTE)) {
		dev_err(&cli->dev, "the I2C bus is not compatible\n");
		return -EIO;
	}

	kbd = devm_kzalloc(&cli->dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd) {
		return -ENOMEM;
	}

	cells = 2;
	if (!of_get_property(cli->dev.of_node, "keymap", &len)) {
		dev_err(&cli->dev, "DT node has no keymap\n");
		return -EINVAL;
	}

	len /= sizeof(u32) * cells;
	kbd->kmlen = len;

	kbd->km = devm_kzalloc(&cli->dev, sizeof(struct keymap_def) * len,
			       GFP_KERNEL);
	if (!kbd->km) {
		return -ENOMEM;
	}

	for (i = 0; i < len; i++) {
		offset = i * cells;
		if (of_property_read_u32_index(cli->dev.of_node, "keymap",
					       offset, &brain_keycode)) {
			dev_err(&cli->dev,
				"could not read DT property (brain keycode)\n");
			return -EINVAL;
		}
		if (of_property_read_u32_index(cli->dev.of_node, "keymap",
					       offset + 1, &kernel_keycode)) {
			dev_err(&cli->dev,
				"could not read DT property (kernel keycode)\n");
			return -EINVAL;
		}
		kbd->km[i].brain_keycode = brain_keycode;
		kbd->km[i].kernel_keycode = kernel_keycode;
		dev_dbg(&cli->dev, "normal: brain: %02x, kernel: %02x",
			brain_keycode, kernel_keycode);
	}

	if (!of_get_property(cli->dev.of_node, "keymap-symbol", &len)) {
		dev_err(&cli->dev, "DT node has no keymap (symbol)\n");
		return -EINVAL;
	}

	len /= sizeof(u32) * cells;
	kbd->kmlen_symbol = len;

	kbd->km_symbol = devm_kzalloc(
		&cli->dev, sizeof(struct keymap_def) * len, GFP_KERNEL);
	if (!kbd->km_symbol) {
		return -ENOMEM;
	}

	for (i = 0; i < len; i++) {
		offset = i * cells;
		if (of_property_read_u32_index(cli->dev.of_node,
					       "keymap-symbol", offset,
					       &brain_keycode)) {
			dev_err(&cli->dev,
				"could not read DT property (brain keycode)\n");
			return -EINVAL;
		}
		if (of_property_read_u32_index(cli->dev.of_node,
					       "keymap-symbol", offset + 1,
					       &kernel_keycode)) {
			dev_err(&cli->dev,
				"could not read DT property (kernel keycode)\n");
			return -EINVAL;
		}
		kbd->km_symbol[i].brain_keycode = brain_keycode;
		kbd->km_symbol[i].kernel_keycode = kernel_keycode;
		dev_dbg(&cli->dev, "symbol: brain: %02x, kernel: %02x",
			brain_keycode, kernel_keycode);
	}

	kbd->cli = cli;
	i2c_set_clientdata(cli, kbd);

	kbd->idev = devm_input_allocate_device(&cli->dev);
	if (!kbd->idev) {
		dev_err(&cli->dev, "failed to allocate inpute device\n");
		return -ENOMEM;
	}

	kbd->idev->name = BRAIN_KBD_I2C_DEV_NAME;
	kbd->idev->id.bustype = BUS_I2C;

	__set_bit(EV_REP, kbd->idev->evbit); /* autorepeat */

	for (i = 0; i < kbd->kmlen; i++) {
		input_set_capability(kbd->idev, EV_KEY,
				     kbd->km[i].kernel_keycode);
	}

	for (i = 0; i < kbd->kmlen_symbol; i++) {
		input_set_capability(kbd->idev, EV_KEY,
				     kbd->km_symbol[i].kernel_keycode);
	}

	err = input_register_device(kbd->idev);
	if (err) {
		dev_err(&cli->dev, "failed to register input device: %d\n",
			err);
		return err;
	}

	i2c_smbus_read_byte_data(kbd->cli, 0x00);
	i2c_smbus_read_byte_data(kbd->cli, 0x0a);
	i2c_smbus_read_byte_data(kbd->cli, 0x01);

	i2c_smbus_read_byte_data(kbd->cli, 0x40);
	i2c_smbus_read_byte_data(kbd->cli, 0x41);
	i2c_smbus_read_byte_data(kbd->cli, 0x48);

	i2c_smbus_read_byte_data(kbd->cli, 0x00);
	i2c_smbus_read_word_data(kbd->cli, 0x04);
	i2c_smbus_read_byte_data(kbd->cli, 0x01);
	i2c_smbus_read_byte_data(kbd->cli, 0x0a);
	i2c_smbus_read_byte_data(kbd->cli, 0x00);
	i2c_smbus_read_byte_data(kbd->cli, 0x0a);
	i2c_smbus_read_byte_data(kbd->cli, 0x01);
	i2c_smbus_write_byte_data(kbd->cli, 0x88, 0x08);
	i2c_smbus_write_byte_data(kbd->cli, 0x82, 0x05);

	i2c_smbus_read_byte_data(kbd->cli, 0x00);
	i2c_smbus_read_byte_data(kbd->cli, 0x0a);

	i2c_smbus_write_byte_data(kbd->cli, 0x82, 0x02);
	i2c_smbus_write_byte_data(kbd->cli, 0x82, 0x03);

	msleep(500);

	i2c_smbus_read_word_data(kbd->cli, 0x04);

	err = devm_request_threaded_irq(&cli->dev, cli->irq, bk_i2c_irq_handler,
					NULL, IRQF_ONESHOT,
					BRAIN_KBD_I2C_DEV_NAME, kbd);
	if (err) {
		dev_err(&cli->dev, "failed to request irq: %d\n", err);
		return err;
	}
	return 0;
}

static const struct i2c_device_id bk_i2c_id_table[] = {
	{ BRAIN_KBD_I2C_DEV_NAME, 0 },
	{ /* sentinel */ },
};

static const struct of_device_id bk_i2c_of_match[] = {
	{
		.compatible = "sharp,brain-kbd-i2c",
	},
	{ /* sentinel */ },
};

static struct i2c_driver bk_i2c_driver = {
	.driver =  {
		.name = BRAIN_KBD_I2C_DEV_NAME,
		.of_match_table = of_match_ptr(bk_i2c_of_match),
	},
	.probe = bk_i2c_probe,
	.id_table = bk_i2c_id_table,
};
module_i2c_driver(bk_i2c_driver);

MODULE_AUTHOR("Takumi Sueda <puhitaku@gmail.com>");
MODULE_DESCRIPTION("SHARP Brain keyboard driver");
MODULE_LICENSE("GPL v2");
