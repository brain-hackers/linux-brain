      // SPDX-License-Identifier: GPL-2.0-only
/*
 * SHARP Brain keyboard (GPIO matrix, interrupt-driven) driver
 * Changed by Jahg <jahg@outlook.com>
 * Version: V1.5 20261122
 *
 * Based on the original polling driver:
 *   Copyright 2021 Takumi Sueda <puhitaku@gmail.com>
 *
 * Interrupt conversion:
 *   - The 8 matrix-in (row) GPIOs are requested as edge-both wake sources.
 *   - Between scans all 7 matrix-out (column) GPIOs are driven low, so any
 *     key press pulls its row low and generates a falling edge; releasing
 *     the last key on a row generates a rising edge.
 *   - A hard IRQ only masks all row lines and arms a delayed work; the
 *     matrix scan itself runs in a SCHED_OTHER workqueue after a debounce.
 *     This follows the kernel's standard matrix-keypad design. Scanning in
 *     a threaded IRQ (SCHED_FIFO) let column-toggling false edges re-enter
 *     continuously and pin the thread at 100% CPU -> RT throttling.
 *   - scan_pending coalesces bursts of edges (key bounce + chord skew)
 *     into one debounced scan, so a held modifier key cannot snowball.
 *   - Releases are reported immediately: there is no per-key press lockout
 *     window. The workqueue debounce plus scan_pending coalescing already
 *     settle contact bounce and chord skew, so a lockout window is redundant
 *     and only risked swallowing a genuine fast tap's release (which made
 *     the input core autorepeat the key forever - the stuck-key bug).
 *   - No input polling timer is used any more.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#define DEV_NAME "brain-kbd-gpio"

#define BK_N_IN 8
#define BK_N_OUT 7

/*
 * Delay from the first key edge to the actual matrix scan. This is the
 * event-level debounce: by the time the work runs, contact bounce and the
 * skew between chord keys have settled, so one scan sees a stable matrix.
 */
#define BK_DEBOUNCE_MS 15

struct bk_gpio_data {
	struct input_dev *inputdev;
	struct gpio_desc *in[BK_N_IN];
	struct gpio_desc *out[BK_N_OUT];
	int irq[BK_N_IN];

	/*
	 * Scanning runs in a workqueue (SCHED_OTHER), NOT in a threaded IRQ.
	 * A threaded IRQ thread is SCHED_FIFO; the false edges that column
	 * toggling injects on held rows re-enter it continuously and pin it
	 * at 100% CPU, which is exactly what triggered RT throttling.
	 *
	 * scan_pending + irq_lock implement the standard matrix-keypad
	 * pattern: the hard IRQ disables all row IRQs and arms the delayed
	 * work once; the work scans, then re-enables the IRQs. Edges latched
	 * while scanning simply cause one more debounced scan (a no-op on
	 * state), never an RT runaway.
	 */
	spinlock_t irq_lock;
	struct delayed_work scan_work;
	bool scan_pending;

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

static ssize_t symbol_activated_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct input_dev *inputdev = container_of(dev, struct input_dev, dev);
	struct bk_gpio_data *kbd = input_get_drvdata(inputdev);

	return sprintf(buf, "%d\n", READ_ONCE(kbd->symbol));
}
static DEVICE_ATTR_RO(symbol_activated);

static struct attribute *brain_kbd_attrs[] = {
	&dev_attr_symbol_activated.attr,
	NULL,
};

static const struct attribute_group brain_kbd_group = {
	.attrs = brain_kbd_attrs,
};

static void bk_gpio_set_symbol(struct bk_gpio_data *kbd, bool pressed)
{
	if (kbd->symbol == pressed)
		return;

	WRITE_ONCE(kbd->symbol, pressed);
	sysfs_notify(&kbd->inputdev->dev.kobj, NULL,
		     dev_attr_symbol_activated.attr.name);
}

/*
 * Leave all columns driven low. This is the idle/wake state: a pressed key
 * connects a row to a low column and therefore changes the row IRQ level.
 */
static void bk_gpio_set_idle_columns(struct bk_gpio_data *kbd)
{
	int i;

	for (i = 0; i < BK_N_OUT; i++)
		gpiod_direction_output(kbd->out[i], 0);
}

static void bk_gpio_read_keys(struct bk_gpio_data *kbd, ulong *result)
{
	struct device *dev = &kbd->inputdev->dev;
	int try, i, err;
	ulong in[10][7];

	for (try = 0; try < ARRAY_SIZE(in); try++) {
		for (i = 0; i < ARRAY_SIZE(in[0]); i++) {
			/*
			 * Drive only the scanned column low and leave every
			 * other column in high-impedance (input) mode.
			 */
			gpiod_direction_output(kbd->out[i], 0);
			udelay(100);
			in[try][i] = 0;
			err = gpiod_get_array_value(BK_N_IN, kbd->in, NULL,
						    &in[try][i]);
			if (err) {
				dev_err(dev, "failed to get array value: %d\n",
					err);
			} else {
				/*
				 * Decode the raw read into a 7-bit per-column
				 * row mask where a pressed key is 1.
				 * Raw bit 5 is unused; raw bits 6/7 map to
				 * rows 5/6.
				 */
				in[try][i] = (((in[try][i] ^ (in[try][i] >> 1)) & 0x1f) ^
					      (in[try][i] >> 1)) & 0x7f;
			}
			gpiod_direction_input(kbd->out[i]);
		}

		if (try < 3)
			continue;

		if (memcmp(in[try - 2], in[try - 1], sizeof(in[0])) == 0 &&
		    memcmp(in[try - 1], in[try], sizeof(in[0])) == 0) {
			for (i = 0; i < ARRAY_SIZE(in[0]); i++)
				result[i] = in[try][i];
			goto out;
		}

		/* not to exceed 10ms in total; sleep instead of busy-waiting
		 * so the threaded handler does not burn RT CPU time.
		 */
		usleep_range(1000, 1200);
	}

	dev_dbg(dev, "failed to debounce: exceeded %d tries\n", ARRAY_SIZE(in));
	/* Keep the last known state instead of synthesizing a mass release. */
	memcpy(result, kbd->last_in, ARRAY_SIZE(kbd->last_in) * sizeof(result[0]));

out:
	bk_gpio_set_idle_columns(kbd);
}

/*
 * Read the (already debounced) matrix and report key state changes.
 * Every release is reported immediately - the workqueue debounce and
 * scan_pending coalescing already settle contact bounce and chord skew.
 */
static void bk_gpio_report_keys(struct bk_gpio_data *kbd)
{
	struct input_dev *inputdev = kbd->inputdev;
	struct device *dev = &inputdev->dev;
	int i, j, bank;
	ulong in[7], agg;

	bk_gpio_read_keys(kbd, in);

	if (memcmp(in, kbd->last_in, sizeof(in)) == 0)
		goto skip;

	dev_dbg(dev, "%02lx %02lx %02lx %02lx %02lx %02lx %02lx",
		in[0], in[1], in[2], in[3], in[4], in[5], in[6]);

	agg = 0;
	for (i = 0; i < 7; i++)
		agg |= in[i];

	for (i = 0; i < 7; i++) {
		bank = 1 << i;
		if (agg & bank) {
			for (j = 0; j < 7; j++) {
				/* skip invalid keymap */
				if (kbd->km[i][j] == UINT_MAX)
					continue;

			if ((in[j] & bank) == 0) {
				if (!kbd->pressed[i][j]) {
					if (i == kbd->sym_key_bank &&
					    j == kbd->sym_key_num) {
						dev_dbg(dev, "symbol\n");
						bk_gpio_set_symbol(kbd, true);
					} else {
						dev_dbg(dev, "P: %04x\n",
							kbd->km[i][j]);
						input_report_key(
							inputdev,
							kbd->symbol ? kbd->km_symbol[i][j]
									    : kbd->km[i][j],
							1);
					}
				}
				kbd->pressed[i][j] = true;
			} else if (kbd->pressed[i][j]) {
				if (i == kbd->sym_key_bank &&
				    j == kbd->sym_key_num) {
					bk_gpio_set_symbol(kbd, false);
				} else {
					dev_dbg(dev, "R: %04x\n",
						kbd->km[i][j]);
					input_report_key(inputdev,
							 kbd->km[i][j], 0);
					input_report_key(inputdev,
							 kbd->km_symbol[i][j], 0);
				}
				kbd->pressed[i][j] = false;
			}
			}
		} else {
			/* flush deasserted bank */
			for (j = 0; j < 7; j++) {
				if (!kbd->pressed[i][j])
					continue;
				kbd->pressed[i][j] = false;

				if (i == kbd->sym_key_bank &&
				    j == kbd->sym_key_num) {
					bk_gpio_set_symbol(kbd, false);
				} else {
					dev_dbg(dev, "R: %04x\n", kbd->km[i][j]);
					input_report_key(inputdev, kbd->km[i][j], 0);
					input_report_key(inputdev, kbd->km_symbol[i][j], 0);
				}
			}
		}
	}

	input_sync(inputdev);
skip:
	memcpy(kbd->last_in, in, sizeof(in));
}

/*
 * Hard IRQ context: do the absolute minimum. Mask every row line so the
 * column toggling of the upcoming scan cannot re-enter us, then arm one
 * delayed work. The scan itself runs later in process context.
 */
static irqreturn_t bk_gpio_irq_handler(int irq, void *dev_id)
{
	struct bk_gpio_data *kbd = dev_id;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&kbd->irq_lock, flags);

	/* A scan is already armed (or running) - do not disable twice. */
	if (kbd->scan_pending)
		goto out;

	for (i = 0; i < BK_N_IN; i++)
		disable_irq_nosync(kbd->irq[i]);

	kbd->scan_pending = true;
	schedule_delayed_work(&kbd->scan_work, msecs_to_jiffies(BK_DEBOUNCE_MS));

out:
	spin_unlock_irqrestore(&kbd->irq_lock, flags);
	return IRQ_HANDLED;
}

/*
 * Process context (workqueue, SCHED_OTHER). Scan the debounced matrix,
 * report key changes, then restore the idle state and re-arm the IRQs.
 * Any false edges injected while scanning are latched and coalesced into
 * at most one more debounced scan - no SCHED_FIFO busy loop, no throttling.
 */
static void bk_gpio_scan_work(struct work_struct *work)
{
	struct bk_gpio_data *kbd =
		container_of(work, struct bk_gpio_data, scan_work.work);
	unsigned long flags;
	int i;

	bk_gpio_report_keys(kbd);

	/* Leave columns low so the next key edge can wake us again. */
	bk_gpio_set_idle_columns(kbd);

	spin_lock_irqsave(&kbd->irq_lock, flags);
	kbd->scan_pending = false;
	for (i = 0; i < BK_N_IN; i++)
		enable_irq(kbd->irq[i]);
	spin_unlock_irqrestore(&kbd->irq_lock, flags);
}

static int bk_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct bk_gpio_data *kbd;
	int i, j, count, err, cells, len, offset;
	struct gpio_desc *gpio;
	u32 keydef[3];

	kbd = devm_kzalloc(dev, sizeof(*kbd), GFP_KERNEL);
	if (!kbd)
		return -ENOMEM;

	spin_lock_init(&kbd->irq_lock);
	INIT_DELAYED_WORK(&kbd->scan_work, bk_gpio_scan_work);

	count = gpiod_count(dev, "matrix-in");
	if (count != BK_N_IN) {
		dev_err(dev, "expected length of matrix-in-gpios is %d, not %d\n",
			BK_N_IN, count);
		return -EINVAL;
	}

	for (i = 0; i < gpiod_count(dev, "matrix-in"); i++) {
		gpio = devm_gpiod_get_index(dev, "matrix-in", i, GPIOD_IN);
		if (IS_ERR(gpio)) {
			dev_err(dev, "failed to get input gpio %d\n", i);
			return PTR_ERR(gpio);
		}
		kbd->in[i] = gpio;
	}

	count = gpiod_count(dev, "matrix-out");
	if (count != BK_N_OUT) {
		dev_err(dev, "expected length of matrix-out-gpios is %d, not %d\n",
			BK_N_OUT, count);
		return -EINVAL;
	}

	for (i = 0; i < gpiod_count(dev, "matrix-out"); i++) {
		gpio = devm_gpiod_get_index(dev, "matrix-out", i, GPIOD_OUT_LOW);
		if (IS_ERR(gpio)) {
			dev_err(dev, "failed to get output gpio %d\n", i);
			return PTR_ERR(gpio);
		}
		gpiod_set_value(gpio, 0);
		kbd->out[i] = gpio;
	}

	/* Idle/wake state: all columns low, so any key press pulls a row low. */
	bk_gpio_set_idle_columns(kbd);

	/* Init input device */
	kbd->inputdev = devm_input_allocate_device(dev);
	if (!kbd->inputdev) {
		dev_err(dev, "failed to allocate input device\n");
		return -ENOMEM;
	}

	input_set_drvdata(kbd->inputdev, kbd);

	kbd->inputdev->name = DEV_NAME;
	kbd->inputdev->id.bustype = BUS_HOST;

	__set_bit(EV_KEY, kbd->inputdev->evbit);
	__set_bit(EV_REP, kbd->inputdev->evbit); /* autorepeat */

	/* Parse keymap */
	cells = 3;
	if (!of_get_property(dev->of_node, "keymap", &len)) {
		dev_err(dev, "DT node has no keymap\n");
		return -EINVAL;
	}

	len /= sizeof(u32) * cells;
	kbd->kmlen = len;

	for (i = 0; i < 7; i++) {
		for (j = 0; j < 7; j++)
			kbd->km[i][j] = UINT_MAX;
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

		input_set_capability(kbd->inputdev, EV_KEY, keydef[2]);
	}

	if (!of_get_property(dev->of_node, "keymap-symbol", &len)) {
		dev_err(dev, "DT node has no keymap (symbol)\n");
		return -EINVAL;
	}

	len /= sizeof(u32) * cells;
	kbd->kmlen_symbol = len;

	for (i = 0; i < 7; i++) {
		for (j = 0; j < 7; j++)
			kbd->km_symbol[i][j] = UINT_MAX;
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

		input_set_capability(kbd->inputdev, EV_KEY, keydef[2]);
	}

	if (of_property_read_u32_index(dev->of_node, "symbol-key", 0,
				       &kbd->sym_key_bank)) {
		dev_err(dev, "could not read symbol key bank\n");
		return -EINVAL;
	}
	dev_dbg(dev, "sym_key_bank = %d\n", kbd->sym_key_bank);

	if (of_property_read_u32_index(dev->of_node, "symbol-key", 1,
				       &kbd->sym_key_num)) {
		dev_err(dev, "could not read symbol key num\n");
		return -EINVAL;
	}
	dev_dbg(dev, "sym_key_num = %d\n", kbd->sym_key_num);

	for (i = 0; i < 7; i++) {
		for (j = 0; j < 7; j++)
			kbd->pressed[i][j] = false;
	}

	err = input_register_device(kbd->inputdev);
	if (err) {
		dev_err(dev, "failed to register input device: %d\n", err);
		return err;
	}

	err = sysfs_create_group(&kbd->inputdev->dev.kobj, &brain_kbd_group);
	if (err) {
		dev_err(dev, "failed to create sysfs group: %d\n", err);
		return err;
	}

	/* Synchronize a key that was already held during boot. */
	bk_gpio_report_keys(kbd);

	/* Map each matrix-in row GPIO to its IRQ. */
	for (i = 0; i < BK_N_IN; i++) {
		kbd->irq[i] = gpiod_to_irq(kbd->in[i]);
		if (kbd->irq[i] < 0) {
			dev_err(dev, "failed to map matrix-in %d to IRQ: %d\n",
				i, kbd->irq[i]);
			return kbd->irq[i];
		}
	}

	/*
	 * One edge-both hard IRQ per row. No threaded handler: the matrix
	 * scan runs in the workqueue. IRQF_ONESHOT is intentionally dropped -
	 * the hard IRQ masks/unmasks ALL rows explicitly around the scan,
	 * which is the standard kernel matrix-keypad scheme.
	 */
	for (i = 0; i < BK_N_IN; i++) {
		err = devm_request_irq(
			dev, kbd->irq[i], bk_gpio_irq_handler,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
				IRQF_NO_AUTOEN,
			DEV_NAME, kbd);
		if (err) {
			dev_err(dev, "failed to request irq %d: %d\n",
				kbd->irq[i], err);
			return err;
		}
	}

	/* All handlers are installed; start listening for key edges. */
	for (i = 0; i < BK_N_IN; i++)
		enable_irq(kbd->irq[i]);

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
MODULE_DESCRIPTION("SHARP Brain GPIO matrix keyboard driver (irq)");
MODULE_LICENSE("GPL v2");
