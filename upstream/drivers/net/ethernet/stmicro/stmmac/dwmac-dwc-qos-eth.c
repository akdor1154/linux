// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synopsys DWC Ethernet Quality-of-Service v4.10a linux driver
 *
 * Copyright (C) 2016 Joao Pinto <jpinto@synopsys.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/ethtool.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/stmmac.h>

#include "stmmac_platform.h"
#include "dwmac4.h"

struct tegra_eqos {
	struct device *dev;
	void __iomem *regs;

	struct reset_control *rst;

	struct gpio_desc *reset;
};

static int dwc_eth_dwmac_config_dt(struct platform_device *pdev,
				   struct plat_stmmacenet_data *plat_dat)
{
	struct device *dev = &pdev->dev;
	u32 burst_map = 0;
	u32 bit_index = 0;
	u32 a_index = 0;

	if (!plat_dat->axi) {
		plat_dat->axi = devm_kzalloc(&pdev->dev,
					     sizeof(struct stmmac_axi),
					     GFP_KERNEL);

		if (!plat_dat->axi)
			return -ENOMEM;
	}

	plat_dat->axi->axi_lpi_en = device_property_read_bool(dev,
							      "snps,en-lpi");
	if (device_property_read_u32(dev, "snps,write-requests",
				     &plat_dat->axi->axi_wr_osr_lmt)) {
		/**
		 * Since the register has a reset value of 1, if property
		 * is missing, default to 1.
		 */
		plat_dat->axi->axi_wr_osr_lmt = 1;
	} else {
		/**
		 * If property exists, to keep the behavior from dwc_eth_qos,
		 * subtract one after parsing.
		 */
		plat_dat->axi->axi_wr_osr_lmt--;
	}

	if (device_property_read_u32(dev, "snps,read-requests",
				     &plat_dat->axi->axi_rd_osr_lmt)) {
		/**
		 * Since the register has a reset value of 1, if property
		 * is missing, default to 1.
		 */
		plat_dat->axi->axi_rd_osr_lmt = 1;
	} else {
		/**
		 * If property exists, to keep the behavior from dwc_eth_qos,
		 * subtract one after parsing.
		 */
		plat_dat->axi->axi_rd_osr_lmt--;
	}
	device_property_read_u32(dev, "snps,burst-map", &burst_map);

	/* converts burst-map bitmask to burst array */
	for (bit_index = 0; bit_index < 7; bit_index++) {
		if (burst_map & (1 << bit_index)) {
			switch (bit_index) {
			case 0:
			plat_dat->axi->axi_blen[a_index] = 4; break;
			case 1:
			plat_dat->axi->axi_blen[a_index] = 8; break;
			case 2:
			plat_dat->axi->axi_blen[a_index] = 16; break;
			case 3:
			plat_dat->axi->axi_blen[a_index] = 32; break;
			case 4:
			plat_dat->axi->axi_blen[a_index] = 64; break;
			case 5:
			plat_dat->axi->axi_blen[a_index] = 128; break;
			case 6:
			plat_dat->axi->axi_blen[a_index] = 256; break;
			default:
			break;
			}
			a_index++;
		}
	}

	/* dwc-qos needs GMAC4, AAL, TSO and PMT */
	plat_dat->has_gmac4 = 1;
	plat_dat->dma_cfg->aal = 1;
	plat_dat->flags |= STMMAC_FLAG_TSO_EN;
	plat_dat->pmt = 1;

	return 0;
}

static int dwc_qos_probe(struct platform_device *pdev,
			 struct plat_stmmacenet_data *plat_dat,
			 struct stmmac_resources *stmmac_res)
{
	plat_dat->pclk = stmmac_pltfr_find_clk(plat_dat, "phy_ref_clk");

	return 0;
}

#define SDMEMCOMPPADCTRL 0x8800
#define  SDMEMCOMPPADCTRL_PAD_E_INPUT_OR_E_PWRD BIT(31)

#define AUTO_CAL_CONFIG 0x8804
#define  AUTO_CAL_CONFIG_START BIT(31)
#define  AUTO_CAL_CONFIG_ENABLE BIT(29)

#define AUTO_CAL_STATUS 0x880c
#define  AUTO_CAL_STATUS_ACTIVE BIT(31)

static void tegra_eqos_fix_speed(void *priv, int speed, unsigned int mode)
{
	struct tegra_eqos *eqos = priv;
	bool needs_calibration = false;
	u32 value;
	int err;

	switch (speed) {
	case SPEED_1000:
	case SPEED_100:
		needs_calibration = true;
		fallthrough;

	case SPEED_10:
		break;

	default:
		dev_err(eqos->dev, "invalid speed %d\n", speed);
		break;
	}

	if (needs_calibration) {
		/* calibrate */
		value = readl(eqos->regs + SDMEMCOMPPADCTRL);
		value |= SDMEMCOMPPADCTRL_PAD_E_INPUT_OR_E_PWRD;
		writel(value, eqos->regs + SDMEMCOMPPADCTRL);

		udelay(1);

		value = readl(eqos->regs + AUTO_CAL_CONFIG);
		value |= AUTO_CAL_CONFIG_START | AUTO_CAL_CONFIG_ENABLE;
		writel(value, eqos->regs + AUTO_CAL_CONFIG);

		err = readl_poll_timeout_atomic(eqos->regs + AUTO_CAL_STATUS,
						value,
						value & AUTO_CAL_STATUS_ACTIVE,
						1, 10);
		if (err < 0) {
			dev_err(eqos->dev, "calibration did not start\n");
			goto failed;
		}

		err = readl_poll_timeout_atomic(eqos->regs + AUTO_CAL_STATUS,
						value,
						(value & AUTO_CAL_STATUS_ACTIVE) == 0,
						20, 200);
		if (err < 0) {
			dev_err(eqos->dev, "calibration didn't finish\n");
			goto failed;
		}

	failed:
		value = readl(eqos->regs + SDMEMCOMPPADCTRL);
		value &= ~SDMEMCOMPPADCTRL_PAD_E_INPUT_OR_E_PWRD;
		writel(value, eqos->regs + SDMEMCOMPPADCTRL);
	} else {
		value = readl(eqos->regs + AUTO_CAL_CONFIG);
		value &= ~AUTO_CAL_CONFIG_ENABLE;
		writel(value, eqos->regs + AUTO_CAL_CONFIG);
	}
}

static int tegra_eqos_probe(struct platform_device *pdev,
			    struct plat_stmmacenet_data *plat_dat,
			    struct stmmac_resources *res)
{
	struct device *dev = &pdev->dev;
	struct tegra_eqos *eqos;
	int err;

	eqos = devm_kzalloc(&pdev->dev, sizeof(*eqos), GFP_KERNEL);
	if (!eqos)
		return -ENOMEM;

	eqos->dev = &pdev->dev;
	eqos->regs = res->addr;

	if (!is_of_node(dev->fwnode))
		goto bypass_clk_reset_gpio;

	plat_dat->clk_tx_i = stmmac_pltfr_find_clk(plat_dat, "tx");

	eqos->reset = devm_gpiod_get(&pdev->dev, "phy-reset", GPIOD_OUT_HIGH);
	if (IS_ERR(eqos->reset)) {
		err = PTR_ERR(eqos->reset);
		return err;
	}

	usleep_range(2000, 4000);
	gpiod_set_value(eqos->reset, 0);

	/* MDIO bus was already reset just above */
	plat_dat->mdio_bus_data->needs_reset = false;

	eqos->rst = devm_reset_control_get(&pdev->dev, "eqos");
	if (IS_ERR(eqos->rst)) {
		err = PTR_ERR(eqos->rst);
		goto reset_phy;
	}

	err = reset_control_assert(eqos->rst);
	if (err < 0)
		goto reset_phy;

	usleep_range(2000, 4000);

	err = reset_control_deassert(eqos->rst);
	if (err < 0)
		goto reset_phy;

	usleep_range(2000, 4000);

bypass_clk_reset_gpio:
	plat_dat->fix_mac_speed = tegra_eqos_fix_speed;
	plat_dat->set_clk_tx_rate = stmmac_set_clk_tx_rate;
	plat_dat->bsp_priv = eqos;
	plat_dat->flags |= STMMAC_FLAG_SPH_DISABLE;

	return 0;

reset_phy:
	gpiod_set_value(eqos->reset, 1);

	return err;
}

static void tegra_eqos_remove(struct platform_device *pdev)
{
	struct tegra_eqos *eqos = get_stmmac_bsp_priv(&pdev->dev);

	reset_control_assert(eqos->rst);
	gpiod_set_value(eqos->reset, 1);
}

struct dwc_eth_dwmac_data {
	int (*probe)(struct platform_device *pdev,
		     struct plat_stmmacenet_data *plat_dat,
		     struct stmmac_resources *res);
	void (*remove)(struct platform_device *pdev);
	const char *stmmac_clk_name;
};

static const struct dwc_eth_dwmac_data dwc_qos_data = {
	.probe = dwc_qos_probe,
	.stmmac_clk_name = "apb_pclk",
};

static const struct dwc_eth_dwmac_data tegra_eqos_data = {
	.probe = tegra_eqos_probe,
	.remove = tegra_eqos_remove,
	.stmmac_clk_name = "slave_bus",
};

static const struct dwc_eth_dwmac_data fsd_eqos_data = {
	.stmmac_clk_name = "slave_bus",
};

static int dwc_eth_dwmac_probe(struct platform_device *pdev)
{
	const struct dwc_eth_dwmac_data *data;
	struct plat_stmmacenet_data *plat_dat;
	struct stmmac_resources stmmac_res;
	int ret;

	data = device_get_match_data(&pdev->dev);

	memset(&stmmac_res, 0, sizeof(struct stmmac_resources));

	/**
	 * Since stmmac_platform supports name IRQ only, basic platform
	 * resource initialization is done in the glue logic.
	 */
	stmmac_res.irq = platform_get_irq(pdev, 0);
	if (stmmac_res.irq < 0)
		return stmmac_res.irq;
	stmmac_res.wol_irq = stmmac_res.irq;

	stmmac_res.addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(stmmac_res.addr))
		return PTR_ERR(stmmac_res.addr);

	plat_dat = devm_stmmac_probe_config_dt(pdev, stmmac_res.mac);
	if (IS_ERR(plat_dat))
		return PTR_ERR(plat_dat);

	ret = devm_clk_bulk_get_all(&pdev->dev, &plat_dat->clks);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "Failed to retrieve all required clocks\n");
	plat_dat->num_clks = ret;

	ret = clk_bulk_prepare_enable(plat_dat->num_clks, plat_dat->clks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "Failed to enable clocks\n");

	plat_dat->stmmac_clk = stmmac_pltfr_find_clk(plat_dat,
						     data->stmmac_clk_name);

	if (data->probe)
		ret = data->probe(pdev, plat_dat, &stmmac_res);
	if (ret < 0) {
		dev_err_probe(&pdev->dev, ret, "failed to probe subdriver\n");
		clk_bulk_disable_unprepare(plat_dat->num_clks, plat_dat->clks);
		return ret;
	}

	ret = dwc_eth_dwmac_config_dt(pdev, plat_dat);
	if (ret)
		goto remove;

	ret = stmmac_dvr_probe(&pdev->dev, plat_dat, &stmmac_res);
	if (ret)
		goto remove;

	return ret;

remove:
	if (data->remove)
		data->remove(pdev);

	return ret;
}

static void dwc_eth_dwmac_remove(struct platform_device *pdev)
{
	const struct dwc_eth_dwmac_data *data = device_get_match_data(&pdev->dev);
	struct plat_stmmacenet_data *plat_dat = dev_get_platdata(&pdev->dev);

	stmmac_dvr_remove(&pdev->dev);

	if (data->remove)
		data->remove(pdev);

	if (plat_dat)
		clk_bulk_disable_unprepare(plat_dat->num_clks, plat_dat->clks);
}

static const struct of_device_id dwc_eth_dwmac_match[] = {
	{ .compatible = "snps,dwc-qos-ethernet-4.10", .data = &dwc_qos_data },
	{ .compatible = "nvidia,tegra186-eqos", .data = &tegra_eqos_data },
	{ .compatible = "tesla,fsd-ethqos", .data = &fsd_eqos_data },
	{ }
};
MODULE_DEVICE_TABLE(of, dwc_eth_dwmac_match);

static struct platform_driver dwc_eth_dwmac_driver = {
	.probe  = dwc_eth_dwmac_probe,
	.remove = dwc_eth_dwmac_remove,
	.driver = {
		.name           = "dwc-eth-dwmac",
		.pm             = &stmmac_pltfr_pm_ops,
		.of_match_table = dwc_eth_dwmac_match,
	},
};
module_platform_driver(dwc_eth_dwmac_driver);

MODULE_AUTHOR("Joao Pinto <jpinto@synopsys.com>");
MODULE_DESCRIPTION("Synopsys DWC Ethernet Quality-of-Service v4.10a driver");
MODULE_LICENSE("GPL v2");
