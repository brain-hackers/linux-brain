// SPDX-License-Identifier: GPL-2.0-only
/*
 * Toshiba TMPA9xx timer support, ported from MuCross Linux 2.6.36:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Copyright (C) 2001 Deep Blue Solutions Ltd.
 * Copyright (C) 2010 Thomas Haase (Thomas.Haase@web.de)
 */
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

#define TIMER_LOAD 0x00
#define TIMER_VALUE 0x04
#define TIMER_CONTROL 0x08
#define TIMER_INTCLR 0x0c
#define TIMER_MODE 0x1c
#define TIMER_CAPEN 0x60
#define TIMER_CMPEN 0xe0
#define TIMER_ENABLE BIT(7)
#define TIMER_PERIODIC BIT(6)
#define TIMER_IRQ_ENABLE BIT(5)
#define TIMER_16BIT BIT(1)

static void __iomem *timer_base;
static struct clock_event_device tmpa9xx_clockevent;

static int tmpa9xx_set_periodic(struct clock_event_device *evt)
{
	writel_relaxed(DIV_ROUND_CLOSEST(32768, HZ) - 1,
		       timer_base + TIMER_LOAD);
	writel_relaxed(TIMER_ENABLE | TIMER_PERIODIC | TIMER_IRQ_ENABLE |
		       TIMER_16BIT, timer_base + TIMER_CONTROL);
	return 0;
}

static int tmpa9xx_shutdown(struct clock_event_device *evt)
{
	writel_relaxed(0, timer_base + TIMER_CONTROL);
	return 0;
}

static irqreturn_t tmpa9xx_timer_interrupt(int irq, void *data)
{
	writel_relaxed(1, timer_base + TIMER_INTCLR);
	tmpa9xx_clockevent.event_handler(&tmpa9xx_clockevent);
	return IRQ_HANDLED;
}

static int __init tmpa9xx_timer_init(struct device_node *np)
{
	int irq;

	timer_base = of_iomap(np, 0);
	if (!timer_base)
		return -ENXIO;
	irq = irq_of_parse_and_map(np, 0);
	if (!irq)
		return -EINVAL;
	writel_relaxed(0, timer_base + TIMER_CONTROL);
	writel_relaxed(0, timer_base + TIMER_MODE);
	writel_relaxed(0, timer_base + TIMER_CAPEN);
	writel_relaxed(0, timer_base + TIMER_CMPEN);
	writel_relaxed(1, timer_base + TIMER_INTCLR);
	if (request_irq(irq, tmpa9xx_timer_interrupt, IRQF_TIMER,
			"tmpa9xx-timer", NULL))
		return -EINVAL;
	tmpa9xx_clockevent.name = "tmpa9xx-timer";
	tmpa9xx_clockevent.features = CLOCK_EVT_FEAT_PERIODIC;
	tmpa9xx_clockevent.rating = 200;
	tmpa9xx_clockevent.set_state_periodic = tmpa9xx_set_periodic;
	tmpa9xx_clockevent.set_state_shutdown = tmpa9xx_shutdown;
	tmpa9xx_clockevent.cpumask = cpumask_of(0);
	clockevents_config_and_register(&tmpa9xx_clockevent, 32768, 1, 0xffff);
	return 0;
}
TIMER_OF_DECLARE(tmpa9xx, "toshiba,tmpa910-timer", tmpa9xx_timer_init);
