// SPDX-License-Identifier: GPL-2.0-only
/*
 * DTPM hierarchy description for Qualcomm SoCs
 *
 * Puts the CPUs of the SoC under one package node of the powercap "dtpm"
 * zone, so that a single power limit caps them as a whole.
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/dtpm.h>
#include <linux/energy_model.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static struct dtpm_node msm8974_hierarchy[] = {
	[0] = { .name = "msm8974",
		.type = DTPM_NODE_VIRTUAL },
	[1] = { .name = "package",
		.type = DTPM_NODE_VIRTUAL,
		.parent = &msm8974_hierarchy[0] },
	[2] = { .name = "/cpus/cpu@0",
		.type = DTPM_NODE_DT,
		.parent = &msm8974_hierarchy[1] },
	[3] = { .name = "/cpus/cpu@1",
		.type = DTPM_NODE_DT,
		.parent = &msm8974_hierarchy[1] },
	[4] = { .name = "/cpus/cpu@2",
		.type = DTPM_NODE_DT,
		.parent = &msm8974_hierarchy[1] },
	[5] = { .name = "/cpus/cpu@3",
		.type = DTPM_NODE_DT,
		.parent = &msm8974_hierarchy[1] },
	[6] = { /* sentinel */ }
};

/* Not __initdata: the hierarchy is created from a probe that may be deferred. */
static struct of_device_id qcom_dtpm_match_table[] = {
	{ .compatible = "qcom,msm8974", .data = msm8974_hierarchy },
	{}
};

static const char *const qcom_dtpm_machines[] = {
	"qcom,msm8974",
	NULL
};

static struct platform_device *qcom_dtpm_pdev;

/*
 * The DTPM CPU nodes are only created for CPUs that already have a cpufreq
 * policy and an energy model when the hierarchy is built; a CPU without one
 * is skipped for good. Defer until every online CPU has both.
 */
static int qcom_dtpm_probe(struct platform_device *pdev)
{
	struct cpufreq_policy *policy;
	int cpu, ret = 0;

	cpus_read_lock();
	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			ret = dev_err_probe(&pdev->dev, -EPROBE_DEFER,
					    "no cpufreq policy for CPU%d yet\n",
					    cpu);
			break;
		}
		cpufreq_cpu_put(policy);

		if (!em_cpu_get(cpu)) {
			ret = dev_err_probe(&pdev->dev, -ENODEV,
					    "no energy model for CPU%d\n", cpu);
			break;
		}
	}
	cpus_read_unlock();
	if (ret)
		return ret;

	return dtpm_create_hierarchy(qcom_dtpm_match_table);
}

static void qcom_dtpm_remove(struct platform_device *pdev)
{
	dtpm_destroy_hierarchy();
}

static struct platform_driver qcom_dtpm_driver = {
	.probe = qcom_dtpm_probe,
	.remove = qcom_dtpm_remove,
	.driver = {
		.name = "qcom-dtpm",
	},
};

static int __init qcom_dtpm_init(void)
{
	int ret;

	if (!of_machine_compatible_match(qcom_dtpm_machines))
		return -ENODEV;

	ret = platform_driver_register(&qcom_dtpm_driver);
	if (ret)
		return ret;

	qcom_dtpm_pdev = platform_device_register_simple("qcom-dtpm", -1, NULL, 0);
	if (IS_ERR(qcom_dtpm_pdev)) {
		platform_driver_unregister(&qcom_dtpm_driver);
		return PTR_ERR(qcom_dtpm_pdev);
	}

	return 0;
}
module_init(qcom_dtpm_init);

static void __exit qcom_dtpm_exit(void)
{
	platform_device_unregister(qcom_dtpm_pdev);
	platform_driver_unregister(&qcom_dtpm_driver);
}
module_exit(qcom_dtpm_exit);

MODULE_DESCRIPTION("Qualcomm DTPM hierarchy driver");
MODULE_LICENSE("GPL");
