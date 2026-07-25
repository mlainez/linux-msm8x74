// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017-18 Linaro Limited

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/reboot-mode.h>
#include <linux/regmap.h>

#define PON_REASON1			0x08
#define PON_WARM_RESET_REASON1		0x0a
#define PON_WARM_RESET_REASON2		0x0b
#define PON_POFF_REASON1		0x0c
#define PON_POFF_REASON2		0x0d
#define PON_SOFT_RB_SPARE		0x8f

#define GEN1_REASON_SHIFT		2
#define GEN2_REASON_SHIFT		1

#define NO_REASON_SHIFT			0

/* Why the PMIC turned the system on. */
static const char * const pon_reasons[] = {
	"hard reset", "SMPL (momentary power loss)", "RTC alarm",
	"DC charger", "USB charger", "PON1 (secondary PMIC)",
	"CBL (external supply)", "KPD (power key)",
};

/*
 * Why it turned the system off, i.e. how the *previous* boot ended. The
 * interesting ones for post-mortems are UVLO (the PMIC dropped the SoC
 * because a rail collapsed), PMIC watchdog and STAGE3 (a hang the hardware
 * had to break), and the thermal entries.
 */
static const char * const poff_reasons[] = {
	"SOFT (software)", "PS_HOLD (MSM-controlled shutdown)",
	"PMIC watchdog", "GP1 (keypad reset 1)", "GP2 (keypad reset 2)",
	"KPDPWR_AND_RESIN", "RESIN_N", "KPDPWR_N (long power key)",
	NULL, NULL, NULL, "charger (ENUM_TIMER/BOOT_DONE)",
	"TFT (thermal fault tolerance)", "UVLO (undervoltage lockout)",
	"OTST3 (overtemp)", "STAGE3 reset",
};

struct qcom_pon {
	struct device *dev;
	struct regmap *regmap;
	u32 baseaddr;
	struct reboot_mode_driver reboot_mode;
	long reason_shift;
};

static int qcom_pon_reboot_mode_write(struct reboot_mode_driver *reboot,
				    unsigned int magic)
{
	struct qcom_pon *pon = container_of
			(reboot, struct qcom_pon, reboot_mode);
	int ret;

	ret = regmap_update_bits(pon->regmap,
				 pon->baseaddr + PON_SOFT_RB_SPARE,
				 GENMASK(7, pon->reason_shift),
				 magic << pon->reason_shift);
	if (ret < 0)
		dev_err(pon->dev, "update reboot mode bits failed\n");

	return ret;
}

static void qcom_pon_log_bits(struct qcom_pon *pon, const char *what,
			      unsigned int bits, const char * const *names,
			      unsigned int num_names)
{
	unsigned int i;

	if (!bits) {
		dev_info(pon->dev, "%s: none (0x%02x)\n", what, bits);
		return;
	}

	for (i = 0; i < num_names; i++) {
		if (!(bits & BIT(i)) || !names[i])
			continue;
		dev_info(pon->dev, "%s: %s\n", what, names[i]);
	}
}

/*
 * Report why the system powered on and how the previous boot ended. On this
 * hardware a failing rail or a hardware-broken hang leaves no trace in the
 * kernel log at all - the PMIC just drops the SoC - so these registers are
 * the only post-mortem evidence available after a silent reset.
 */
static void qcom_pon_report_reasons(struct qcom_pon *pon)
{
	unsigned int pon_reason, warm1, warm2, poff1, poff2;
	int ret;

	ret = regmap_read(pon->regmap, pon->baseaddr + PON_REASON1,
			  &pon_reason);
	if (ret) {
		dev_warn(pon->dev, "cannot read the power-on reason: %d\n",
			 ret);
		return;
	}
	if (regmap_read(pon->regmap, pon->baseaddr + PON_WARM_RESET_REASON1,
			&warm1) ||
	    regmap_read(pon->regmap, pon->baseaddr + PON_WARM_RESET_REASON2,
			&warm2) ||
	    regmap_read(pon->regmap, pon->baseaddr + PON_POFF_REASON1,
			&poff1) ||
	    regmap_read(pon->regmap, pon->baseaddr + PON_POFF_REASON2,
			&poff2))
		return;

	dev_info(pon->dev,
		 "pon=0x%02x warm_reset=0x%02x%02x poff=0x%02x%02x\n",
		 pon_reason, warm2, warm1, poff2, poff1);

	qcom_pon_log_bits(pon, "power-on", pon_reason, pon_reasons,
			  ARRAY_SIZE(pon_reasons));
	qcom_pon_log_bits(pon, "previous power-off",
			  poff1 | (poff2 << 8), poff_reasons,
			  ARRAY_SIZE(poff_reasons));
}

static int qcom_pon_probe(struct platform_device *pdev)
{
	struct qcom_pon *pon;
	long reason_shift;
	int error;

	pon = devm_kzalloc(&pdev->dev, sizeof(*pon), GFP_KERNEL);
	if (!pon)
		return -ENOMEM;

	pon->dev = &pdev->dev;

	pon->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!pon->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	error = of_property_read_u32(pdev->dev.of_node, "reg",
				     &pon->baseaddr);
	if (error)
		return error;

	qcom_pon_report_reasons(pon);

	reason_shift = (long)of_device_get_match_data(&pdev->dev);

	if (reason_shift != NO_REASON_SHIFT) {
		pon->reboot_mode.dev = &pdev->dev;
		pon->reason_shift = reason_shift;
		pon->reboot_mode.write = qcom_pon_reboot_mode_write;
		error = devm_reboot_mode_register(&pdev->dev, &pon->reboot_mode);
		if (error) {
			dev_err(&pdev->dev, "can't register reboot mode\n");
			return error;
		}
	}

	platform_set_drvdata(pdev, pon);

	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id qcom_pon_id_table[] = {
	{ .compatible = "qcom,pm8916-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8941-pon", .data = (void *)NO_REASON_SHIFT },
	{ .compatible = "qcom,pms405-pon", .data = (void *)GEN1_REASON_SHIFT },
	{ .compatible = "qcom,pm8998-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ .compatible = "qcom,pmk8350-pon", .data = (void *)GEN2_REASON_SHIFT },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_pon_id_table);

static struct platform_driver qcom_pon_driver = {
	.probe = qcom_pon_probe,
	.driver = {
		.name = "qcom-pon",
		.of_match_table = qcom_pon_id_table,
	},
};
module_platform_driver(qcom_pon_driver);

MODULE_DESCRIPTION("Qualcomm Power On driver");
MODULE_LICENSE("GPL v2");
