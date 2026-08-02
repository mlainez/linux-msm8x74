// SPDX-License-Identifier: GPL-2.0-only
/*
 * Battery-voltage-aware CPU load limiter for Qualcomm MSM8974.
 *
 * On a drained battery this platform silently resets (PS_HOLD) under
 * sustained multi-core load at *any* frequency - capping frequency alone
 * was measured insufficient (a 4-core spin at 729.6 MHz died as fast as
 * one at 2265.6 MHz), while idle-class demand (one core, low frequency)
 * is stable for hours on the same pack.  The stock firmware's BCL
 * mitigation likewise pairs a frequency cap with core hotplug.
 *
 * So, below a battery-voltage threshold this driver applies both:
 * a FREQ_QOS_MAX cap on the CPU policy and offlining of all secondary
 * CPUs, restoring them with hysteresis once the battery recovers.
 *
 * The battery voltage is polled through an IIO channel at a deliberately
 * slow rate: an earlier revision polling every second measurably degraded
 * load stability (continuous SPMI ADC traffic), so the default is 30 s -
 * this is a slowly-moving signal and the mitigation is preventive, not
 * reactive (a reactive cap cannot outrun a millisecond rail collapse).
 *
 * Thresholds, cap and poll rate come from DT with module-parameter
 * overrides so the policy can be tuned live while mapping a board's
 * load-vs-VBAT envelope.
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/workqueue.h>

static unsigned int vbat_low_uv;
module_param(vbat_low_uv, uint, 0644);
MODULE_PARM_DESC(vbat_low_uv, "mitigate below this VBAT (uV)");

static unsigned int vbat_clear_uv;
module_param(vbat_clear_uv, uint, 0644);
MODULE_PARM_DESC(vbat_clear_uv, "release the mitigation above this VBAT (uV)");

static unsigned int cap_freq_khz;
module_param(cap_freq_khz, uint, 0644);
MODULE_PARM_DESC(cap_freq_khz, "frequency cap while mitigated (kHz)");

static unsigned int poll_ms;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "battery voltage poll interval (ms)");

static bool hotplug;
module_param(hotplug, bool, 0644);
MODULE_PARM_DESC(hotplug, "also offline secondary CPUs while mitigated (the freq cap alone was measured sufficient; userspace hotplug managers may fight this)");

struct vbat_freqcap {
	struct device *dev;
	struct iio_channel *vbat;
	struct freq_qos_request qos;
	struct cpufreq_policy *policy;
	struct delayed_work work;
	bool capped;
	bool cpus_off;
};

static void vbat_freqcap_mitigate(struct vbat_freqcap *vfc, int uv)
{
	unsigned int cpu;
	int ret;

	if (!vfc->capped) {
		ret = freq_qos_update_request(&vfc->qos, cap_freq_khz);
		if (ret < 0)
			return;
		vfc->capped = true;
		dev_info(vfc->dev, "VBAT %d uV low: capping the CPU at %u kHz\n",
			 uv, cap_freq_khz);
	}

	if (!hotplug)
		return;
	/*
	 * Re-assert on every poll: userspace (udev coldplug, hotplug
	 * managers) can bring cores back behind our back.  CPU0 always stays.
	 */
	for_each_online_cpu(cpu) {
		if (cpu == 0)
			continue;
		ret = remove_cpu(cpu);
		if (ret)
			dev_warn(vfc->dev, "cpu%u offline failed: %d\n",
				 cpu, ret);
	}
	vfc->cpus_off = true;
	dev_info(vfc->dev, "secondary CPUs offlined\n");
}

static void vbat_freqcap_release(struct vbat_freqcap *vfc, int uv)
{
	unsigned int cpu;
	int ret;

	if (vfc->cpus_off) {
		for_each_possible_cpu(cpu) {
			if (cpu == 0 || cpu_online(cpu))
				continue;
			ret = add_cpu(cpu);
			if (ret)
				dev_warn(vfc->dev, "cpu%u online failed: %d\n",
					 cpu, ret);
		}
		vfc->cpus_off = false;
	}

	ret = freq_qos_update_request(&vfc->qos, FREQ_QOS_MAX_DEFAULT_VALUE);
	if (ret >= 0) {
		vfc->capped = false;
		dev_info(vfc->dev, "VBAT %d uV recovered: mitigation released\n",
			 uv);
	}
}

static void vbat_freqcap_work(struct work_struct *work)
{
	struct vbat_freqcap *vfc =
		container_of(work, struct vbat_freqcap, work.work);
	unsigned int clear_uv;
	int uv, ret;

	ret = iio_read_channel_processed(vfc->vbat, &uv);
	if (ret < 0) {
		dev_warn_ratelimited(vfc->dev, "vbat read failed: %d\n", ret);
		goto resched;
	}

	/* A clear threshold at or below the low one would oscillate. */
	clear_uv = max(vbat_clear_uv, vbat_low_uv + 50000);

	if (uv < vbat_low_uv)
		vbat_freqcap_mitigate(vfc, uv);
	else if (vfc->capped && uv > clear_uv)
		vbat_freqcap_release(vfc, uv);

resched:
	schedule_delayed_work(&vfc->work, msecs_to_jiffies(poll_ms));
}

static int vbat_freqcap_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct vbat_freqcap *vfc;
	u32 val;
	int ret;

	vfc = devm_kzalloc(dev, sizeof(*vfc), GFP_KERNEL);
	if (!vfc)
		return -ENOMEM;
	vfc->dev = dev;

	/*
	 * The CPU policy and the ADC both come up late; defer until they
	 * exist rather than running without either.
	 */
	vfc->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(vfc->vbat))
		return dev_err_probe(dev, PTR_ERR(vfc->vbat),
				     "no vbat channel\n");

	vfc->policy = cpufreq_cpu_get(0);
	if (!vfc->policy)
		return -EPROBE_DEFER;

	/* DT defaults; module params (when set) win so tuning needs no DT edit. */
	if (!vbat_low_uv)
		vbat_low_uv = !of_property_read_u32(dev->of_node,
				"qcom,vbat-low-microvolt", &val) ? val : 4050000;
	if (!vbat_clear_uv)
		vbat_clear_uv = !of_property_read_u32(dev->of_node,
				"qcom,vbat-clear-microvolt", &val) ? val : 4200000;
	if (!cap_freq_khz)
		cap_freq_khz = !of_property_read_u32(dev->of_node,
				"qcom,cap-freq-khz", &val) ? val : 300000;
	if (!poll_ms)
		poll_ms = !of_property_read_u32(dev->of_node,
				"qcom,poll-ms", &val) ? val : 30000;

	ret = freq_qos_add_request(&vfc->policy->constraints, &vfc->qos,
				   FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
	if (ret < 0) {
		cpufreq_cpu_put(vfc->policy);
		return dev_err_probe(dev, ret, "freq qos request failed\n");
	}

	INIT_DELAYED_WORK(&vfc->work, vbat_freqcap_work);
	platform_set_drvdata(pdev, vfc);
	/* First check soon after boot: the mitigation is preventive. */
	schedule_delayed_work(&vfc->work, msecs_to_jiffies(2000));

	dev_info(dev, "active: cap %u kHz%s below %u uV, clear above %u uV, poll %u ms\n",
		 cap_freq_khz, hotplug ? " + hotplug" : "", vbat_low_uv,
		 vbat_clear_uv, poll_ms);
	return 0;
}

static void vbat_freqcap_remove(struct platform_device *pdev)
{
	struct vbat_freqcap *vfc = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&vfc->work);
	if (vfc->capped)
		vbat_freqcap_release(vfc, 0);
	freq_qos_remove_request(&vfc->qos);
	cpufreq_cpu_put(vfc->policy);
}

static const struct of_device_id vbat_freqcap_of_match[] = {
	{ .compatible = "qcom,vbat-freqcap" },
	{ }
};
MODULE_DEVICE_TABLE(of, vbat_freqcap_of_match);

static struct platform_driver vbat_freqcap_driver = {
	.probe = vbat_freqcap_probe,
	.remove = vbat_freqcap_remove,
	.driver = {
		.name = "qcom-vbat-freqcap",
		.of_match_table = vbat_freqcap_of_match,
	},
};
module_platform_driver(vbat_freqcap_driver);

MODULE_DESCRIPTION("Battery-voltage-aware CPU load limiter");
MODULE_LICENSE("GPL");
