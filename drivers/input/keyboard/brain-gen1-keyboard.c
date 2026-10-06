// SPDX-License-Identifier: GPL-2.0-only
/*
 * Sharp Brain first-generation 8x8 matrix keyboard.
 *
 * Matrix source (work in progress, retrieved 2026-08-27):
 * https://scrapbox.io/brain-hackers/PW-GC610
 * Modifier source (retrieved 2026-08-27):
 * https://wiki.brainux.org/beginners/get-started/
 * Printable symbol layers:
 * https://wiki.brainux.org/assets/images/keymap.png
 * Copyright Brainux Wiki contributors, licensed under CC BY-SA 4.0.
 *
 * GPIO use and scanning are based on the hardware-tested U-Boot Brain Gen1
 * driver.  Registers were checked against the TMPA910CRA manual and MuCross:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Original MuCross source:
 * linux-tmpa9xx-2.6.36-110310/arch/arm/mach-tmpa9xx/gpio.c
 * Copyright (C) 2009,2010 Kernel Concepts
 *
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define BRAIN_GEN1_ROWS		8
#define BRAIN_GEN1_COLS		8
#define BRAIN_GEN1_POLL_MS	10
#define BRAIN_GEN1_DEBOUNCE_MS	20
#define BRAIN_GEN1_SETTLE_US	10
#define BRAIN_GEN1_SHIFT	BIT_ULL(5 * BRAIN_GEN1_COLS)
#define BRAIN_GEN1_CTRL		BIT_ULL(5 * BRAIN_GEN1_COLS + 1)
#define BRAIN_GEN1_ALT		BIT_ULL(6 * BRAIN_GEN1_COLS + 1)
#define BRAIN_GEN1_SYMBOL	BIT_ULL(7 * BRAIN_GEN1_COLS + 1)
#define BRAIN_GEN1_MODIFIERS	(BRAIN_GEN1_SHIFT | BRAIN_GEN1_CTRL | \
				 BRAIN_GEN1_ALT)

struct brain_gen1_keyboard {
	struct input_dev *input;
	struct gpio_desc *rows[BRAIN_GEN1_ROWS];
	struct gpio_desc *cols[BRAIN_GEN1_COLS];
	struct delayed_work poll_work;
	u64 candidate;
	u64 stable;
	unsigned short active_codes[BRAIN_GEN1_ROWS * BRAIN_GEN1_COLS];
	unsigned long candidate_since;
	bool opened;
};

/* Indexed as KI row, then KO column.  The symbol key is handled internally. */
static const unsigned short
brain_gen1_keymap[BRAIN_GEN1_ROWS][BRAIN_GEN1_COLS] = {
	[0] = { KEY_POWER, KEY_SEARCH, KEY_F1, KEY_F2, KEY_F3, KEY_F4,
		KEY_HOME, KEY_MENU },
	[1] = { 0, 0, 0, 0, 0, KEY_CAPSLOCK, KEY_BOOKMARKS, 0 },
	[2] = { KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I },
	[3] = { KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_O, KEY_P },
	[4] = { KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_J, KEY_K, KEY_L },
	[5] = { KEY_LEFTSHIFT, KEY_LEFTCTRL, 0, 0, 0, KEY_N, KEY_M,
		KEY_MINUS },
	[6] = { 0, KEY_LEFTALT, KEY_VOLUMEUP, KEY_ZOOMIN, KEY_LEFT, KEY_UP,
		KEY_DOWN, KEY_RIGHT },
	[7] = { KEY_SPACE, 0, KEY_VOLUMEDOWN, KEY_ZOOMOUT,
		KEY_ESC, KEY_ENTER, KEY_BACKSPACE, KEY_DELETE },
};

static const unsigned short
brain_gen1_symbol_keymap[BRAIN_GEN1_ROWS][BRAIN_GEN1_COLS] = {
	[2] = { KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8 },
	[3] = { 0, 0, KEY_GRAVE, KEY_EQUAL, KEY_BACKSLASH, KEY_SEMICOLON,
		KEY_9, KEY_0 },
	[4] = { 0, 0, 0, 0, 0, KEY_APOSTROPHE, KEY_LEFTBRACE,
		KEY_RIGHTBRACE },
	[5] = { 0, 0, 0, 0, 0, KEY_COMMA, KEY_DOT, KEY_SLASH },
};

static bool brain_gen1_is_symbol_key(unsigned int row, unsigned int col)
{
	return row == 2 || row == 3 || row == 4 ||
		(row == 5 && col >= 5);
}

static unsigned short brain_gen1_code(unsigned int bit, bool symbol)
{
	unsigned int row = bit / BRAIN_GEN1_COLS;
	unsigned int col = bit % BRAIN_GEN1_COLS;

	if (symbol && brain_gen1_is_symbol_key(row, col))
		return brain_gen1_symbol_keymap[row][col];
	return brain_gen1_keymap[row][col];
}

static u64 brain_gen1_scan(struct brain_gen1_keyboard *kbd)
{
	u64 keys = 0;
	unsigned int col;

	for (col = 0; col < BRAIN_GEN1_COLS; col++) {
		u32 rows;
		unsigned int row, i;

		/*
		 * Break before make.  Otherwise the previous open-drain KO can
		 * remain low during the next KI sample, reporting one key in
		 * two adjacent matrix columns.
		 */
		for (i = 0; i < BRAIN_GEN1_COLS; i++)
			gpiod_set_value(kbd->cols[i], 1);
		udelay(BRAIN_GEN1_SETTLE_US);
		gpiod_set_value(kbd->cols[col], 0);
		udelay(BRAIN_GEN1_SETTLE_US);
		rows = 0;
		for (row = 0; row < BRAIN_GEN1_ROWS; row++)
			if (!gpiod_get_value(kbd->rows[row]))
				rows |= BIT(row);
		for (row = 0; row < BRAIN_GEN1_ROWS; row++)
			if (rows & BIT(row))
				keys |= BIT_ULL(row * BRAIN_GEN1_COLS + col);
	}
	for (col = 0; col < BRAIN_GEN1_COLS; col++)
		gpiod_set_value(kbd->cols[col], 1);
	return keys;
}

static void brain_gen1_report_pressed(struct brain_gen1_keyboard *kbd,
				      u64 mask, bool symbol)
{
	unsigned int bit;

	for (bit = 0; bit < BRAIN_GEN1_ROWS * BRAIN_GEN1_COLS; bit++) {
		unsigned short code;

		if (!(mask & BIT_ULL(bit)))
			continue;
		code = brain_gen1_code(bit, symbol);
		kbd->active_codes[bit] = code;
		if (code)
			input_report_key(kbd->input, code, true);
	}
}

static void brain_gen1_report_released(struct brain_gen1_keyboard *kbd,
				       u64 mask)
{
	unsigned int bit;

	for (bit = 0; bit < BRAIN_GEN1_ROWS * BRAIN_GEN1_COLS; bit++) {
		unsigned short code;

		if (!(mask & BIT_ULL(bit)))
			continue;
		code = kbd->active_codes[bit];
		if (code)
			input_report_key(kbd->input, code, false);
		kbd->active_codes[bit] = 0;
	}
}

static void brain_gen1_report(struct brain_gen1_keyboard *kbd, u64 keys)
{
	u64 changed = keys ^ kbd->stable;
	u64 pressed = changed & keys;
	u64 released = changed & ~keys;
	bool symbol = keys & BRAIN_GEN1_SYMBOL;

	/* Chords must reach the VT layer with modifiers already asserted. */
	brain_gen1_report_pressed(kbd, pressed & BRAIN_GEN1_MODIFIERS, false);
	brain_gen1_report_pressed(kbd, pressed & ~BRAIN_GEN1_MODIFIERS &
				   ~BRAIN_GEN1_SYMBOL, symbol);
	brain_gen1_report_released(kbd, released & ~BRAIN_GEN1_MODIFIERS &
				    ~BRAIN_GEN1_SYMBOL);
	brain_gen1_report_released(kbd, released & BRAIN_GEN1_MODIFIERS);
	kbd->stable = keys;
	input_sync(kbd->input);
}

static void brain_gen1_poll(struct work_struct *work)
{
	struct brain_gen1_keyboard *kbd = container_of(to_delayed_work(work),
						struct brain_gen1_keyboard,
						poll_work);
	u64 keys = brain_gen1_scan(kbd);

	if (keys != kbd->candidate) {
		kbd->candidate = keys;
		kbd->candidate_since = jiffies;
	} else if (keys != kbd->stable &&
		   time_after_eq(jiffies, kbd->candidate_since +
				 msecs_to_jiffies(BRAIN_GEN1_DEBOUNCE_MS))) {
		brain_gen1_report(kbd, keys);
	}
	if (kbd->opened)
		schedule_delayed_work(&kbd->poll_work,
				      msecs_to_jiffies(BRAIN_GEN1_POLL_MS));
}

static int brain_gen1_open(struct input_dev *input)
{
	struct brain_gen1_keyboard *kbd = input_get_drvdata(input);

	unsigned int col;

	for (col = 0; col < BRAIN_GEN1_COLS; col++)
		gpiod_set_value(kbd->cols[col], 1);
	kbd->candidate = 0;
	kbd->stable = 0;
	kbd->candidate_since = jiffies;
	kbd->opened = true;
	schedule_delayed_work(&kbd->poll_work, 0);
	return 0;
}

static void brain_gen1_close(struct input_dev *input)
{
	struct brain_gen1_keyboard *kbd = input_get_drvdata(input);
	unsigned int col;

	kbd->opened = false;
	cancel_delayed_work_sync(&kbd->poll_work);
	for (col = 0; col < BRAIN_GEN1_COLS; col++)
		gpiod_set_value(kbd->cols[col], 1);
}

static int brain_gen1_keyboard_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct brain_gen1_keyboard *kbd;
	struct input_dev *input;
	int error;
	unsigned int row, col, count;

	kbd = devm_kzalloc(dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;
	count = gpiod_count(dev, "matrix-in");
	if (count != BRAIN_GEN1_ROWS) {
		dev_err(dev, "expected %u matrix-in-gpios, got %u\n",
			BRAIN_GEN1_ROWS, count);
		return -EINVAL;
	}
	for (row = 0; row < BRAIN_GEN1_ROWS; row++) {
		kbd->rows[row] = devm_gpiod_get_index(dev, "matrix-in", row,
						      GPIOD_IN);
		if (IS_ERR(kbd->rows[row]))
			return PTR_ERR(kbd->rows[row]);
	}

	count = gpiod_count(dev, "matrix-out");
	if (count != BRAIN_GEN1_COLS) {
		dev_err(dev, "expected %u matrix-out-gpios, got %u\n",
			BRAIN_GEN1_COLS, count);
		return -EINVAL;
	}
	for (col = 0; col < BRAIN_GEN1_COLS; col++) {
		kbd->cols[col] = devm_gpiod_get_index(dev, "matrix-out", col,
						      GPIOD_OUT_HIGH);
		if (IS_ERR(kbd->cols[col]))
			return PTR_ERR(kbd->cols[col]);
	}

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;
	kbd->input = input;
	INIT_DELAYED_WORK(&kbd->poll_work, brain_gen1_poll);
	input->name = "Sharp Brain Gen1 keyboard";
	input->phys = "brain-gen1/input0";
	input->id.bustype = BUS_HOST;
	input->open = brain_gen1_open;
	input->close = brain_gen1_close;
	input_set_drvdata(input, kbd);
	__set_bit(EV_REP, input->evbit);
	for (row = 0; row < BRAIN_GEN1_ROWS; row++)
		for (col = 0; col < BRAIN_GEN1_COLS; col++) {
			unsigned short code = brain_gen1_keymap[row][col];

			if (code)
				input_set_capability(input, EV_KEY, code);
			code = brain_gen1_symbol_keymap[row][col];
			if (code)
				input_set_capability(input, EV_KEY, code);
		}

	platform_set_drvdata(pdev, kbd);
	error = input_register_device(input);
	if (error)
		input_free_device(input);
	return error;
}

static int brain_gen1_keyboard_remove(struct platform_device *pdev)
{
	struct brain_gen1_keyboard *kbd = platform_get_drvdata(pdev);
	unsigned int col;

	kbd->opened = false;
	cancel_delayed_work_sync(&kbd->poll_work);
	for (col = 0; col < BRAIN_GEN1_COLS; col++)
		gpiod_set_value(kbd->cols[col], 1);
	input_unregister_device(kbd->input);
	return 0;
}

static const struct of_device_id brain_gen1_keyboard_of_match[] = {
	{ .compatible = "sharp,brain-gen1-keyboard" },
	{ }
};
MODULE_DEVICE_TABLE(of, brain_gen1_keyboard_of_match);

static struct platform_driver brain_gen1_keyboard_driver = {
	.probe = brain_gen1_keyboard_probe,
	.remove = brain_gen1_keyboard_remove,
	.driver = {
		.name = "brain-gen1-keyboard",
		.of_match_table = brain_gen1_keyboard_of_match,
	},
};
module_platform_driver(brain_gen1_keyboard_driver);

MODULE_AUTHOR("Sharp Brain hackers");
MODULE_DESCRIPTION("Sharp Brain first-generation matrix keyboard");
MODULE_LICENSE("GPL v2");
