// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slimbus.h>
#include <linux/ratelimit.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include "wcd9320.h"
#include "wcd9320-registers.h"


/*
 * Like the PGD, the interface device's value elements live at a flat
 * 0x800 offset (element = 0x800 + reg). Model it with reg_base, not a
 * regmap paging window: a selector_reg of 0x800 aliases IFC register
 * 0x000, which regmap's page-select read-modify-write would silently
 * clobber on every access (see wcd9320_regmap_config for the full
 * story -- on the PGD this zeroed CHIP_CTL and broke the core clock).
 */
static const struct regmap_config wcd9320_ifd_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.can_multi_write = true,
	/* no cache: every IFC port-config write must reach the codec */
	.cache_type = REGCACHE_NONE,
	.reg_base = 0x800,
	.max_register = WCD9320_MAX_REGISTER,
};


static int wcd9320_parse_dt(struct wcd9320 *wcd)
{
	struct device *dev = wcd->dev;
	struct device_node *np = dev->of_node;
	int ret;

	wcd->reset_gpio = of_get_named_gpio(np,	"qcom,reset-gpio", 0);
	if (wcd->reset_gpio < 0) {
		dev_err(dev, "Reset gpio missing in DT\n");
		return -EINVAL;
	}

	wcd->irq_gpio = of_get_named_gpio(np, "qcom,gpio-int2", 0);
	if (!gpio_is_valid(wcd->irq_gpio)) {
		dev_err(dev, "IRQ gpio missing in DT\n");
		return -EINVAL;
	}

	wcd->clk_gpio = of_get_named_gpio(np, "qcom,clk-gpio", 0);
	if (!gpio_is_valid(wcd->clk_gpio)) {
		dev_err(dev, "CLK gpio missing in DT\n");
		return -EINVAL;
	}

	gpio_request(wcd->clk_gpio, "WCD CLK");
	gpio_direction_output(wcd->clk_gpio, 1);

	ret = of_property_read_u32(np, "qcom,codec-clk-rate", &wcd->codec_clk_rate);
	if (ret) {
		dev_err(dev, "Reset mclk rate missing in DT\n");
		return -EINVAL;
	}

	if (wcd->codec_clk_rate != WCD9XXX_MCLK_CLK_9P6HZ) {
		dev_err(dev, "Invalid mclk_rate = %u\n", wcd->codec_clk_rate);
		return -EINVAL;
	}
	wcd->codec_clk = devm_clk_get(dev, "clk");
	if (IS_ERR(wcd->codec_clk)) {
		dev_err(dev, "Unable to find codec clk\n");
		return -EINVAL;
	}

	clk_set_rate(wcd->codec_clk, wcd->codec_clk_rate);

	return 0;
}

static void wcd9320_reset(struct wcd9320 *wcd)
{
	gpio_direction_output(wcd->reset_gpio, 0);
	msleep(20);
	gpio_direction_output(wcd->reset_gpio, 1);
	msleep(20);
}

static int wcd9320_power_up(struct wcd9320 *wcd)
{
	struct device *dev = wcd->dev;
	int ret;

	wcd->num_of_supplies = 7;
	wcd->supplies[0].supply = "vdd-buck";
	wcd->supplies[1].supply = "vdd-tx-h";
	wcd->supplies[2].supply = "vdd-rx-h";
	wcd->supplies[3].supply = "vddpx-1";
	wcd->supplies[4].supply = "vdd-a-1p2v";
	wcd->supplies[5].supply = "vddcx-1";
	wcd->supplies[6].supply = "vddcx-2";

	ret = regulator_bulk_get(dev, wcd->num_of_supplies, wcd->supplies);
	if (ret != 0) {
		dev_err(dev, "Failed to get supplies: err = %d\n", ret);
		return ret;
	}

	ret = regulator_bulk_enable(wcd->num_of_supplies, wcd->supplies);
	if (ret != 0) {
		dev_err(dev, "Failed to get supplies: err = %d\n", ret);
		return ret;
	}

	/*
	 * For WCD9335, it takes about 600us for the Vout_A and
	 * Vout_D to be ready after BUCK_SIDO is powered up.
	 * SYS_RST_N shouldn't be pulled high during this time
	 */
	usleep_range(600, 650);


	return 0;
}

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	struct wcd9320 *wcd = dev_id;

	unsigned int status[3] = {0}, istatus[4] = {0}, tmp = 0;
	unsigned int i;

	regmap_read(wcd->regmap, WCD9XXX_A_INTR_STATUS0 + 0, &status[0]);
	regmap_read(wcd->regmap, WCD9XXX_A_INTR_STATUS0 + 1, &status[1]);
	regmap_read(wcd->regmap, WCD9XXX_A_INTR_STATUS0 + 2, &status[2]);

	regmap_write(wcd->regmap, WCD9XXX_A_INTR_CLEAR0 + 0, status[0]);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_CLEAR0 + 1, status[1]);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_CLEAR0 + 2, status[2]);

	if (status[0] & 1) {
		regmap_read(wcd->ifd_regmap,
			    WCD9320_SLIM_PGD_PORT_INT_STATUS_RX_0 + 0, &istatus[0]);
		regmap_read(wcd->ifd_regmap,
			    WCD9320_SLIM_PGD_PORT_INT_STATUS_RX_0 + 1, &istatus[1]);
		regmap_read(wcd->ifd_regmap,
			    WCD9320_SLIM_PGD_PORT_INT_STATUS_RX_0 + 2, &istatus[2]);
		regmap_read(wcd->ifd_regmap,
			    WCD9320_SLIM_PGD_PORT_INT_STATUS_RX_0 + 3, &istatus[3]);
		dev_dbg(wcd->dev, "irq2 %X %X %X %X\n",
			istatus[0], istatus[1], istatus[2], istatus[3]);
		regmap_write(wcd->ifd_regmap,
			     WCD9320_SLIM_PGD_PORT_INT_CLR_RX_0 + 0, istatus[0]);
		regmap_write(wcd->ifd_regmap,
			     WCD9320_SLIM_PGD_PORT_INT_CLR_RX_0 + 1, istatus[1]);
		regmap_write(wcd->ifd_regmap,
			     WCD9320_SLIM_PGD_PORT_INT_CLR_RX_0 + 2, istatus[2]);
		regmap_write(wcd->ifd_regmap,
			     WCD9320_SLIM_PGD_PORT_INT_CLR_RX_0 + 3, istatus[3]);

		for (i = 0; i < 32; i++) {
			if (istatus[i / 8] & (1 << (i % 8))) {
				regmap_read(wcd->ifd_regmap,
					    WCD9320_SLIM_PGD_PORT_INT_RX_SOURCE0 + i,
					    &tmp);
				dev_dbg(wcd->dev, "%u %u\n", i, tmp);

				/*
				 * Overflow/underflow on RX/TX ports is normal,
				 * benign, and expected (matches the downstream
				 * taiko_slimbus_irq() handling exactly): just
				 * mask the port's own interrupt-enable bit so
				 * it stops reasserting, and leave the port's
				 * data-path config register alone. Toggling
				 * PGD_PORT_CFG here (as a previous version of
				 * this driver did) repeatedly disabled and
				 * re-armed the live data port on every single
				 * overflow -- which fires every ~8.6ms during
				 * normal playback -- and silently prevented
				 * any sustained data flow to the interpolator.
				 */
				if (tmp & 0x3) {
					bool tx = i >= 16;
					unsigned int port_id = tx ? i - 16 : i;
					u16 reg = (tx ? WCD9320_SLIM_PGD_PORT_INT_TX_EN0
						      : WCD9320_SLIM_PGD_PORT_INT_EN0)
						  + port_id / 8;
					unsigned int en = 0;

					dev_dbg_ratelimited(wcd->dev,
						"%s on %s port %u (src=0x%x), masking port IRQ\n",
						(tmp & 0x1) ? "overflow" : "underflow",
						tx ? "TX" : "RX", port_id, tmp);
					regmap_read(wcd->ifd_regmap, reg, &en);
					if (en & (1 << (port_id % 8)))
						regmap_write(wcd->ifd_regmap, reg,
							     en ^ (1 << (port_id % 8)));
				}
			}
		}
	}

	dev_dbg(wcd->dev, "irq %i %X %X %X\n", irq, status[0], status[1], status[2]);

	return IRQ_HANDLED;
}

static int wcd9320_init(struct wcd9320 *wcd)
{
	int ret;

	wcd->irq = gpio_to_irq(wcd->irq_gpio);
	if (wcd->irq < 0) {
		pr_err("Unable to configure irq\n");
		return wcd->irq;
	}

	ret = devm_request_threaded_irq(wcd->dev, wcd->irq, NULL, irq_handler,
					IRQF_TRIGGER_HIGH | IRQF_ONESHOT, "wcd",
					wcd);

	return 0;
}

static int wcd9320_bring_up(struct wcd9320 *wcd)
{
	int i;
	int ret = 0;

	ret = regmap_write(wcd->regmap, WCD9XXX_A_LEAKAGE_CTL, 0x4);
	dev_dbg(wcd->dev, "regmap ret: %d", ret);
	regmap_write(wcd->regmap, WCD9XXX_A_CDC_CTL, 0);
	usleep_range(5000, 5250);
	regmap_write(wcd->regmap, WCD9XXX_A_CDC_CTL, 3);
	regmap_write(wcd->regmap, WCD9XXX_A_LEAKAGE_CTL, 3);

	for (i = 0; i < 3; i++) {
		regmap_write(wcd->regmap, WCD9XXX_A_INTR_LEVEL0 + i, i == 0 ? 1 : 0);
		regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0 + i, 0xff);
	}

	regmap_write(wcd->regmap, WCD9XXX_A_INTR_LEVEL0 + 3, 0);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0 + 3, 0x7f);

	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0, 0xbf);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0, 0xff);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0, 0xdd);
	/*
	 * Unmask bit0 (WCD9320_IRQ_SLIMBUS) too: it was left masked here
	 * (0xcd keeps bit0 set), so the codec's SLIM RX-port overflow/
	 * underflow events (which roll up through this top-level bit)
	 * never assert the external "wcd" GPIO IRQ, and irq_handler() --
	 * which reads+clears PORT_INT_STATUS/SOURCE for the active port --
	 * never runs to service them.
	 */
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0, 0xcc);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0 + 2, 0xfe);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0 + 2, 0xff);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0 + 2, 0xfd);
	regmap_write(wcd->regmap, WCD9XXX_A_INTR_MASK0 + 2, 0xff);

	return ret;
}

static int wcd9320_slim_probe(struct slim_device *slim)
{
	struct device *dev = &slim->dev;
	struct wcd9320 *wcd;
	int ret = 0;

	pr_err("DEBUG:: %x: %x: %x: %x\n",
			slim->e_addr.instance,
			slim->e_addr.dev_index,
			slim->e_addr.prod_code,
			slim->e_addr.manf_id);
	// Interface device
	if (slim->e_addr.dev_index == 0)
		return 0;

	wcd = devm_kzalloc(dev, sizeof(*wcd), GFP_KERNEL);
	if (!wcd)
		return	-ENOMEM;

	wcd->slim = slim;
	wcd->dev = dev;


	ret = wcd9320_power_up(wcd);
	if (ret) {
		dev_err(dev, "Error Powering\n");
		return ret;
	}

	ret = wcd9320_parse_dt(wcd);
	if (ret) {
		dev_err(dev, "Error parsing DT\n");
		return ret;
	}

	wcd->regmap = regmap_init_slimbus(slim, &wcd9320_regmap_config);

	if (IS_ERR(wcd->regmap)) {
		ret = PTR_ERR(wcd->regmap);
		dev_err(&slim->dev, "%s: Failed to allocate register map: %d\n",
			__func__, ret);
		return ret;
	}


	dev_set_drvdata(dev, wcd);

	usleep_range(600, 650);
	wcd9320_reset(wcd);
	wcd->version = 2;
	wcd->dev = dev;
	wcd->intf_type = WCD_INTERFACE_TYPE_SLIMBUS;

	wcd->slim_data.slim = slim;
	wcd->slim_data.regmap = wcd->regmap;
	wcd->slim_data.rx_port_ch_reg_base = 0x180 - (WCD93XX_RX_SLAVE_PORTS * 4);
	wcd->slim_data.port_rx_cfg_reg_base = 0x040 - WCD93XX_RX_SLAVE_PORTS;
	wcd->slim_data.port_tx_cfg_reg_base = 0x050;



	return 0;
}

static int wcd9320_slim_status(struct slim_device *sdev,
				 enum slim_device_status s)
{
	int ret, i;
	struct wcd9320 *wcd;
	struct device_node *np;
	struct device *dev;
	struct device_node *ifd_np = NULL;

	/* Interface device */
	if (sdev->e_addr.dev_index == 0)
		return 0;

	wcd = dev_get_drvdata(&sdev->dev);
	np = wcd->dev->of_node;
	dev = wcd->dev;

	wcd9320_init(wcd);

	ifd_np = of_parse_phandle(np, "ifd", 0);
	if (!ifd_np) {
		dev_err(wcd->dev, "No Interface device found\n");
		return 0;
	}

	wcd->slim_ifd = of_slim_get_device(sdev->ctrl, ifd_np);
	if (!wcd->slim_ifd) {
		dev_err(wcd->dev, "UNable to get SLIM Interface device\n");
		return 0;
	}

	/* Assign the interface device a logical address so the NGD can
	 * schedule SLIM data channels to it (otherwise it stays at LA 0
	 * and data-channel setup times out). The IFD may report present
	 * slightly after the PGD, so retry until it enumerates.
	 */
	for (i = 0; i < 20; i++) {
		if (!slim_get_logical_addr(wcd->slim_ifd))
			break;
		msleep(20);
	}
	if (!wcd->slim_ifd->is_laddr_valid)
		dev_err(wcd->dev, "IFD did not get a logical address (laddr=0x%x)\n",
			wcd->slim_ifd->laddr);

	wcd->ifd_regmap = regmap_init_slimbus(wcd->slim_ifd, &wcd9320_ifd_regmap_config);
	if (IS_ERR(wcd->ifd_regmap)) {
		ret = PTR_ERR(wcd->ifd_regmap);
		dev_err(dev, "%s: Failed to allocate register map: %d\n",
			__func__, ret);
		return ret;
	}

	ret = wcd9320_bring_up(wcd);
	if (ret)
		pr_err("DEBUG:: %s: %d\n", __func__, __LINE__);

	wcd->slim_slave = wcd->slim_ifd;
	wcd->slim_data.slim_slave = wcd->slim_ifd;
	wcd->slim_data.if_regmap = wcd->ifd_regmap;

	return of_platform_populate(wcd->dev->of_node, NULL, NULL, wcd->dev);
}

static void wcd9320_slim_remove(struct slim_device *sdev)
{
}

static const struct slim_device_id wcd9320_slim_id[] = {
	/*
	 * Only bind the PGD (generic device, dev_index 1). The interface
	 * device (dev_index 0) must NOT be bound here: it is fetched via
	 * of_slim_get_device() from the PGD's slim_status() and assigned a
	 * logical address there. Binding it to this driver too makes it
	 * probe-defer on "Failed to get logical address" and leaves its
	 * regmap non-functional, so SLIM port programming never reaches
	 * the hardware and no audio data flows.
	 */
	{0x217, 0xa0, 0x1, 0x0},
	{}
};
MODULE_DEVICE_TABLE(slim, wcd9320_slim_id);

static struct slim_driver wcd9320_slim_driver = {
	.driver = {
		.name = "wcd9320-slim",
		.owner = THIS_MODULE,
	},
	.probe = wcd9320_slim_probe,
	.remove = wcd9320_slim_remove,
	.device_status = wcd9320_slim_status,
	.id_table = wcd9320_slim_id,
};

/*
 * The codec is split across two drivers: this SLIMbus driver enumerates the
 * device and sets up its regmaps, and a platform driver (in wcd9320.c, bound
 * to the of_platform_populate()d child) provides the ASoC component. Register
 * both from a single module init so the combined module has one entry point.
 */
static int __init wcd9320_module_init(void)
{
	int ret;

	ret = platform_driver_register(&wcd9320_codec_driver);
	if (ret)
		return ret;

	ret = slim_driver_register(&wcd9320_slim_driver);
	if (ret)
		platform_driver_unregister(&wcd9320_codec_driver);

	return ret;
}
module_init(wcd9320_module_init);

static void __exit wcd9320_module_exit(void)
{
	slim_driver_unregister(&wcd9320_slim_driver);
	platform_driver_unregister(&wcd9320_codec_driver);
}
module_exit(wcd9320_module_exit);

MODULE_DESCRIPTION("Codec core driver");
MODULE_LICENSE("GPL v2");
