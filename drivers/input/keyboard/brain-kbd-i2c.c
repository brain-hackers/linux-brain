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

#define BK_KEY(val) ((val) & 0x3f)
#define BK_IS_PRESSED(val) ((~val & 0x40) >> 6)

#define BK_IS_SWITCH(val) (((val) & 0x80) != 0)
#define BK_SW_CODE(val) (((val) >> 1) & 0x1F)
#define BK_SWITCH_ON(val) (((val) & 1) == 0)

#define BK_KEYCODE_MAX (64)
#define BK_N_ROLL_MAX (3)

enum {
	BK_SW_LCD_TRANSFORMING_TO_TABLET = 3,
	BK_SW_UNKNOWN4,
	BK_SW_USB_VBUS,
	BK_SW_LCD_FULLY_TRANSFORMED,
	BK_SW_LCD_TRANSFORMING_TO_CLOSED
};

struct keymap_def {
	unsigned int normal_event_code;
	unsigned int symbol_event_code;
};

struct bk_i2c_data {
	struct i2c_client *cli;
	struct input_dev *idev;
	struct keymap_def keymaps[BK_KEYCODE_MAX];

	bool symbol_states[BK_KEYCODE_MAX];
	bool symbol_mode;
	u32 symbol_keycode;
	bool closing;
};

static bool handle_switch(struct bk_i2c_data *kbd, u8 keycode)
{
	bool sw_on = BK_SWITCH_ON(keycode);
	unsigned int sw_code;

	switch (BK_SW_CODE(keycode)) {
	case BK_SW_LCD_TRANSFORMING_TO_TABLET:
		if (!sw_on)
			kbd->closing = false;
		return true;

	case BK_SW_LCD_TRANSFORMING_TO_CLOSED:
		if (sw_on)
			kbd->closing = true;
		return true;

	case BK_SW_LCD_FULLY_TRANSFORMED:
		sw_code = (kbd->closing) ? SW_LID : SW_TABLET_MODE;
		input_report_switch(kbd->idev, sw_code, sw_on);
		return true;

	case BK_SW_USB_VBUS:
		input_report_switch(kbd->idev, SW_DOCK, sw_on);
		return true;

	default:
		dev_dbg(&kbd->cli->dev, "Unknown switch event %0x02X", keycode);
	}
	return false;
}

static bool handle_symbol_key(struct bk_i2c_data *kbd, u8 keycode)
{
	struct keymap_def *keymap = &kbd->keymaps[BK_KEY(keycode)];

	if (keymap->symbol_event_code == KEY_RESERVED)
		return false;

	if (kbd->symbol_states[BK_KEY(keycode)] == false) {
		input_report_key(kbd->idev, keymap->normal_event_code, 0);

		dev_dbg(&kbd->cli->dev,
			"mode changed, normal key %02x(%02x) released\n",
			BK_KEY(keycode), keymap->normal_event_code);
	}

	input_report_key(kbd->idev, keymap->symbol_event_code,
			 BK_IS_PRESSED(keycode));

	kbd->symbol_states[BK_KEY(keycode)] = BK_IS_PRESSED(keycode);

	dev_dbg(&kbd->cli->dev, "symbol key %02x(%02x) %s\n", BK_KEY(keycode),
		keymap->symbol_event_code,
		BK_IS_PRESSED(keycode) ? "pressed" : "released");

	return true;
}

static bool handle_normal_key(struct bk_i2c_data *kbd, u8 keycode)
{
	struct keymap_def *keymap = &kbd->keymaps[BK_KEY(keycode)];

	if (keymap->normal_event_code == KEY_RESERVED)
		return false;

	if (kbd->symbol_states[BK_KEY(keycode)] == true) {
		input_report_key(kbd->idev, keymap->symbol_event_code, 0);

		dev_dbg(&kbd->cli->dev,
			"mode changed, symbol key %02x(%02x) released\n",
			BK_KEY(keycode), keymap->symbol_event_code);
	}

	input_report_key(kbd->idev, keymap->normal_event_code,
			 BK_IS_PRESSED(keycode));

	kbd->symbol_states[BK_KEY(keycode)] = false;

	dev_dbg(&kbd->cli->dev, "normal key %02x(%02x) %s\n", BK_KEY(keycode),
		keymap->normal_event_code,
		BK_IS_PRESSED(keycode) ? "pressed" : "released");

	return true;
}

static bool detect_key(struct bk_i2c_data *kbd, u8 keycode)
{
	if (BK_IS_SWITCH(keycode))
		return handle_switch(kbd, keycode);

	if (BK_KEY(keycode) == (u8)kbd->symbol_keycode) {
		if (BK_IS_PRESSED(keycode)) {
			dev_dbg(&kbd->cli->dev, "symbol pressed!\n");
			kbd->symbol_mode = true;
		} else {
			dev_dbg(&kbd->cli->dev, "symbol released!\n");
			kbd->symbol_mode = false;
		}
		return true;
	}

	if (kbd->symbol_mode)
		return handle_symbol_key(kbd, keycode);
	else
		return handle_normal_key(kbd, keycode);
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

	dev_dbg(&kbd->cli->dev, "N=%d, k1=%02X\n", n, k1);
	if (k1 == 0x00) {
		dev_dbg(&kbd->cli->dev,
			"interrupted but no key press was found\n");
		goto err;
	}

	if (n < 1) {
		goto done;
	} else if (n > BK_N_ROLL_MAX) {
		dev_dbg(&kbd->cli->dev, "invalid sequence\n");
		n = BK_N_ROLL_MAX;
	}
	if (!detect_key(kbd, k1)) {
		dev_dbg(&kbd->cli->dev, "unknown key was pressed: k1=%02x\n",
			BK_KEY(k1));
	}
	if (n < 2) {
		goto done;
	}

	raw = i2c_smbus_read_word_swapped(kbd->cli, BK_CMD_KEYCODE);
	if (raw < 0) {
		dev_err(&kbd->cli->dev, "failed to read 2nd/3rd:%x\n", raw);
		goto err;
	}

	k2 = (raw & 0xff00) >> 8;
	k3 = raw & 0xff;

	dev_dbg(&kbd->cli->dev, "k2=%02x, k3=%02x\n", k2, k3);
	if (!detect_key(kbd, k2)) {
		dev_dbg(&kbd->cli->dev, "unknown key was pressed: k2=%02x\n",
			BK_KEY(k2));
	}

	if (n < 3) {
		goto done;
	}
	if (!detect_key(kbd, k3)) {
		dev_dbg(&kbd->cli->dev, "unknown key was pressed: k3=%02x\n",
			BK_KEY(k3));
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

	if (of_property_read_u32(cli->dev.of_node, "symbol-keycode",
				 &kbd->symbol_keycode)) {
		kbd->symbol_keycode = 0x19;
	}

	cells = 2;
	if (!of_get_property(cli->dev.of_node, "keymap", &len)) {
		dev_err(&cli->dev, "DT node has no keymap\n");
		return -EINVAL;
	}

	for (i = 0; i < BK_KEYCODE_MAX; i++) {
		kbd->keymaps[i].normal_event_code = KEY_RESERVED;
		kbd->keymaps[i].symbol_event_code = KEY_RESERVED;
		kbd->symbol_states[i] = false;
	}

	len /= sizeof(u32) * cells;
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

		if (brain_keycode <= BK_KEYCODE_MAX) {
			kbd->keymaps[brain_keycode].normal_event_code =
				kernel_keycode;

			dev_dbg(&cli->dev, "normal: brain: %02x, kernel: %02x",
				brain_keycode, kernel_keycode);
		} else {
			dev_err(&cli->dev, "invalid keycode: %02x\n",
				brain_keycode);
		}
	}

	if (!of_get_property(cli->dev.of_node, "keymap-symbol", &len)) {
		dev_err(&cli->dev, "DT node has no keymap (symbol)\n");
		return -EINVAL;
	}

	len /= sizeof(u32) * cells;
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
		if (brain_keycode <= BK_KEYCODE_MAX) {
			kbd->keymaps[brain_keycode].symbol_event_code =
				kernel_keycode;

			dev_dbg(&cli->dev, "symbol: brain: %02x, kernel: %02x",
				brain_keycode, kernel_keycode);
		} else {
			dev_err(&cli->dev, "invalid keycode: %02x\n",
				brain_keycode);
		}
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

	for (i = 0; i < BK_KEYCODE_MAX; i++) {
		if (kbd->keymaps[i].normal_event_code != KEY_RESERVED)
			input_set_capability(kbd->idev, EV_KEY,
					     kbd->keymaps[i].normal_event_code);

		if (kbd->keymaps[i].symbol_event_code != KEY_RESERVED)
			input_set_capability(kbd->idev, EV_KEY,
					     kbd->keymaps[i].symbol_event_code);
	}
	kbd->closing = false;
	input_set_capability(kbd->idev, EV_SW, SW_LID);
	input_set_capability(kbd->idev, EV_SW, SW_TABLET_MODE);
	input_set_capability(kbd->idev, EV_SW, SW_DOCK);

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
