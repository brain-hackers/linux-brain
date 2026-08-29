// SPDX-License-Identifier: GPL-2.0
/*
 * Toshiba TMPA9xx SD host controller glue
 *
 * Hardware setup is based on the MuCross TMPA9xx Linux 2.6.36 release:
 * https://mucross.com/downloads/tx09-linux/Release-20110309/src/
 * Copyright (C) 2010 Thomas Haase (Thomas.Haase@web.de)
 *
 * The controller register interface is handled by the upstream TMIO core.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/tmio.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "tmio_mmc.h"

#define TMPA9XX_SDHI_CLK_GATE	BIT(2)
#define TMPA9XX_GPIO_DIR		0x400
#define TMPA9XX_GPIO_FR1		0x424
#define TMPA9XX_SDHI_PINS	0xff
#define TMPA9XX_SDHI_RESET_RELEASE	(BIT(0) | BIT(1))

struct tmpa9xx_sdhi {
	struct tmio_mmc_data pdata;
	void __iomem *clkcr3;
	void __iomem *gpio;
};

static struct tmpa9xx_sdhi *tmpa9xx_sdhi_priv(struct tmio_mmc_host *host)
{
	return container_of(host->pdata, struct tmpa9xx_sdhi, pdata);
}

static int tmpa9xx_sdhi_clk_enable(struct tmio_mmc_host *host)
{
	struct tmpa9xx_sdhi *priv = tmpa9xx_sdhi_priv(host);

	writel(readl(priv->clkcr3) | TMPA9XX_SDHI_CLK_GATE, priv->clkcr3);
	return 0;
}

static void tmpa9xx_sdhi_clk_start(struct tmio_mmc_host *host)
{
	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL,
			CLK_CTL_SCLKEN |
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
	usleep_range(10000, 11000);
}

static void tmpa9xx_sdhi_clk_stop(struct tmio_mmc_host *host)
{
	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL,
			~CLK_CTL_SCLKEN &
			sd_ctrl_read16(host, CTL_SD_CARD_CLK_CTL));
	usleep_range(10000, 11000);
}

static void tmpa9xx_sdhi_set_clock(struct tmio_mmc_host *host,
				   unsigned int new_clock)
{
	unsigned int divisor;
	u16 clk;

	if (!new_clock) {
		tmpa9xx_sdhi_clk_stop(host);
		return;
	}

	/* Zero selects /2; bits 0..7 select /4 through /512. */
	divisor = host->pdata->hclk / new_clock;
	clk = divisor <= 2 ? 0 : roundup_pow_of_two(divisor) >> 2;

	tmpa9xx_sdhi_clk_stop(host);
	sd_ctrl_write16(host, CTL_SD_CARD_CLK_CTL, clk & CLK_CTL_DIV_MASK);
	usleep_range(10000, 11000);
	tmpa9xx_sdhi_clk_start(host);
}

static void tmpa9xx_sdhi_reset(struct tmio_mmc_host *host)
{
	sd_ctrl_write16(host, CTL_RESET_SD, 0);
	usleep_range(10000, 11000);
	/*
	 * TMPA910 requires bit 1 as well as bit 0 on reset release.
	 * With 0x0001, CMD24 succeeds but transmits zero-filled sectors.
	 * 0x0003 matches WinCE and passes 16-bit PIO write/readback tests.
	 * The internal function of bit 1 is not publicly documented.
	 */
	sd_ctrl_write16(host, CTL_RESET_SD, TMPA9XX_SDHI_RESET_RELEASE);
	usleep_range(10000, 11000);
}

static int tmpa9xx_sdhi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tmpa9xx_sdhi *priv;
	struct tmio_mmc_host *host;
	struct resource *res;
	int irq;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	priv->clkcr3 = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->clkcr3))
		return PTR_ERR(priv->clkcr3);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	priv->gpio = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->gpio))
		return PTR_ERR(priv->gpio);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	/* Match the SDHC0 clock and Port G setup validated by U-Boot. */
	writel(readl(priv->clkcr3) | TMPA9XX_SDHI_CLK_GATE, priv->clkcr3);
	writel(0, priv->gpio + TMPA9XX_GPIO_DIR);
	writel(TMPA9XX_SDHI_PINS, priv->gpio + TMPA9XX_GPIO_FR1);

	priv->pdata.hclk = 96000000;
	priv->pdata.ocr_mask = MMC_VDD_32_33 | MMC_VDD_33_34;
	priv->pdata.flags = TMIO_MMC_BLKSZ_2BYTES;

	host = tmio_mmc_host_alloc(pdev, &priv->pdata);
	if (IS_ERR(host))
		return PTR_ERR(host);

	/* TMPA910 spaces the TMIO register numbers by two bytes. */
	host->bus_shift = 1;
	host->clk_enable = tmpa9xx_sdhi_clk_enable;
	host->set_clock = tmpa9xx_sdhi_set_clock;
	host->reset = tmpa9xx_sdhi_reset;
	host->mmc->f_max = priv->pdata.hclk / 2;
	host->mmc->f_min = priv->pdata.hclk / 512;

	/*
	 * Brain Gen1 does not provide a usable native card-detect signal:
	 * CTL_STATUS.SIGSTATE remains clear with a card inserted.  U-Boot's
	 * hardware-validated driver likewise operates without consulting CD.
	 * For a board marked broken-cd, let the MMC core probe by polling
	 * instead of rejecting the card through tmio_mmc_get_cd().
	 */
	if (host->mmc->caps & MMC_CAP_NEEDS_POLL)
		host->ops.get_cd = NULL;

	ret = tmio_mmc_host_probe(host);
	if (ret)
		goto free_host;

	ret = devm_request_irq(dev, irq, tmio_mmc_irq, IRQF_TRIGGER_HIGH,
			       dev_name(dev), host);
	if (ret)
		goto remove_host;

	dev_info(dev, "TMPA9xx SDHI at %pR, irq %d\n",
		 platform_get_resource(pdev, IORESOURCE_MEM, 0), irq);
	return 0;

remove_host:
	tmio_mmc_host_remove(host);
free_host:
	tmio_mmc_host_free(host);
	return ret;
}

static int tmpa9xx_sdhi_remove(struct platform_device *pdev)
{
	struct tmio_mmc_host *host = platform_get_drvdata(pdev);

	tmio_mmc_host_remove(host);
	tmio_mmc_host_free(host);
	return 0;
}

static const struct of_device_id tmpa9xx_sdhi_of_match[] = {
	{ .compatible = "toshiba,tmpa910-sdhi" },
	{ }
};
MODULE_DEVICE_TABLE(of, tmpa9xx_sdhi_of_match);

static struct platform_driver tmpa9xx_sdhi_driver = {
	.driver = {
		.name = "tmpa9xx-sdhi",
		.of_match_table = tmpa9xx_sdhi_of_match,
	},
	.probe = tmpa9xx_sdhi_probe,
	.remove = tmpa9xx_sdhi_remove,
};
module_platform_driver(tmpa9xx_sdhi_driver);

MODULE_DESCRIPTION("Toshiba TMPA9xx SD host controller driver");
MODULE_LICENSE("GPL v2");
