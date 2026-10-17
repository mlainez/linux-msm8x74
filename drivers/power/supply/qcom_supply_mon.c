// SPDX-License-Identifier: GPL-2.0-only
/*
 * Supply monitor for Qualcomm PM8941 based boards
 *
 * Samples the system rail (VPH_PWR), the USB input and, when the board is fed
 * at the battery terminals, the current it draws there through the PMIC ADCs,
 * the real time status of the SMBB charger, the CPU frequencies and the
 * hottest CPU zone at a fixed rate, into a ring in reserved RAM. A warm reset
 * (PS_HOLD, watchdog) keeps that RAM: the next boot reports the last samples
 * of the previous one, which tell a supply collapse from a software failure.
 * A real power loss (UVLO, SMPL) clears it; the PMIC power-off reason, logged
 * by the PON driver, is the evidence for those.
 *
 * Optionally, the CPU frequencies are capped while VPH_PWR is below a
 * threshold, for boards fed by a supply that cannot carry full DVFS load.
 */

#include <linux/cpufreq.h>
#include <linux/debugfs.h>
#include <linux/iio/consumer.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#define SMBB_CHG_RT_STS		0x010
#define SMBB_BAT_RT_STS		0x210
#define SMBB_USB_RT_STS		0x310
#define SMBB_DC_RT_STS		0x410

#define LOG_MAGIC		0x534d4f4e	/* "SMON" */
#define LOG_VERSION		1
#define NR_CPUS_LOGGED		4
#define NR_ZONES		4

#define FLAG_CAPPED		BIT(0)
#define FLAG_VPH_ERR		BIT(1)

static unsigned int period_ms = 200;
module_param(period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "Sampling period in ms, 0 to stop sampling");

static unsigned int vmin_mv;
module_param(vmin_mv, uint, 0644);
MODULE_PARM_DESC(vmin_mv, "Cap the CPU frequencies below this VPH_PWR (mV), 0 to never cap");

static unsigned int vrel_mv;
module_param(vrel_mv, uint, 0644);
MODULE_PARM_DESC(vrel_mv, "Release the cap above this VPH_PWR (mV), default vmin_mv + 150");

static unsigned int cap_khz = 960000;
module_param(cap_khz, uint, 0644);
MODULE_PARM_DESC(cap_khz, "Frequency cap while VPH_PWR is low (kHz)");

static unsigned int hold_ms = 2000;
module_param(hold_ms, uint, 0644);
MODULE_PARM_DESC(hold_ms, "Time VPH_PWR must stay above vrel_mv before the cap is released (ms)");

static unsigned int warn_mv;
module_param(warn_mv, uint, 0644);
MODULE_PARM_DESC(warn_mv, "Log a warning when VPH_PWR drops below this (mV), 0 to never warn");

struct supply_sample {
	u32 seq;
	u32 ms;			/* since boot */
	u16 vph_mv;
	u16 usbin_mv;
	u8 chg_rt;
	u8 bat_rt;
	u8 usb_rt;
	u8 dc_rt;
	u16 mhz[NR_CPUS_LOGGED];
	s8 max_temp;		/* hottest CPU zone, degC */
	u8 flags;
	s16 ibat_ma;		/* drawn from the battery terminals, 0 if unknown */
};

struct supply_log {
	u32 magic;
	u32 version;
	u32 boot;		/* boots logged into this region */
	u32 sample_size;
	u32 nr_samples;		/* capacity */
	u32 head;		/* next slot */
	u32 count;		/* samples written this boot */
	u32 reserved;
	struct supply_sample samples[];
};

struct supply_mon {
	struct device *dev;
	struct regmap *regmap;
	u32 chg_base;
	struct iio_channel *vph;
	struct iio_channel *usbin;
	struct iio_channel *ibat;
	struct thermal_zone_device *zones[NR_ZONES];
	struct supply_log *log;
	struct supply_log *prev;	/* copy of the previous boot's log */
	size_t log_size;
	struct delayed_work work;
	struct freq_qos_request qos[NR_CPUS_LOGGED];
	bool qos_added[NR_CPUS_LOGGED];
	bool capped;
	unsigned long above_since;
	u16 vph_min_mv;
	u32 caps;
	u32 seq;
	struct dentry *debugfs;
	struct supply_sample last;
};

static void supply_mon_set_cap(struct supply_mon *sm, bool cap)
{
	s32 limit = cap ? cap_khz : FREQ_QOS_MAX_DEFAULT_VALUE;
	int cpu;

	for (cpu = 0; cpu < NR_CPUS_LOGGED; cpu++) {
		struct cpufreq_policy *policy;

		if (!sm->qos_added[cpu]) {
			policy = cpufreq_cpu_get(cpu);
			if (!policy)
				continue;
			if (freq_qos_add_request(&policy->constraints,
						 &sm->qos[cpu], FREQ_QOS_MAX,
						 FREQ_QOS_MAX_DEFAULT_VALUE) >= 0)
				sm->qos_added[cpu] = true;
			cpufreq_cpu_put(policy);
			if (!sm->qos_added[cpu])
				continue;
		}
		freq_qos_update_request(&sm->qos[cpu], limit);
	}
}

static void supply_mon_regulate(struct supply_mon *sm, struct supply_sample *s)
{
	unsigned int release = vrel_mv ? vrel_mv : vmin_mv + 150;

	if (!vmin_mv || (s->flags & FLAG_VPH_ERR)) {
		if (sm->capped) {
			supply_mon_set_cap(sm, false);
			sm->capped = false;
		}
		return;
	}

	if (s->vph_mv < vmin_mv) {
		sm->above_since = 0;
		if (!sm->capped) {
			supply_mon_set_cap(sm, true);
			sm->capped = true;
			sm->caps++;
			dev_warn(sm->dev, "VPH_PWR %u mV < %u mV: CPUs capped at %u kHz\n",
				 s->vph_mv, vmin_mv, cap_khz);
		}
	} else if (sm->capped && s->vph_mv > release) {
		if (!sm->above_since) {
			sm->above_since = jiffies ? jiffies : 1;
		} else if (time_after(jiffies, sm->above_since + msecs_to_jiffies(hold_ms))) {
			supply_mon_set_cap(sm, false);
			sm->capped = false;
			dev_info(sm->dev, "VPH_PWR %u mV: CPU cap released\n", s->vph_mv);
		}
	}

	if (sm->capped)
		s->flags |= FLAG_CAPPED;
}

static int supply_mon_read_mv(struct iio_channel *chan, u16 *mv)
{
	int uv, ret;

	if (!chan)
		return -ENODEV;
	ret = iio_read_channel_processed(chan, &uv);
	if (ret < 0)
		return ret;
	*mv = clamp(uv / 1000, 0, U16_MAX);

	return 0;
}

static void supply_mon_sample(struct supply_mon *sm, struct supply_sample *s)
{
	unsigned int val;
	int i, temp, val_ma;

	memset(s, 0, sizeof(*s));
	s->seq = ++sm->seq;
	s->ms = jiffies_to_msecs(get_jiffies_64() - INITIAL_JIFFIES);

	if (supply_mon_read_mv(sm->vph, &s->vph_mv))
		s->flags |= FLAG_VPH_ERR;
	supply_mon_read_mv(sm->usbin, &s->usbin_mv);
	/* The IADC reads current drawn from the terminals as negative */
	if (sm->ibat && !iio_read_channel_processed(sm->ibat, &val_ma))
		s->ibat_ma = clamp(-val_ma, S16_MIN, S16_MAX);

	if (sm->regmap) {
		if (!regmap_read(sm->regmap, sm->chg_base + SMBB_CHG_RT_STS, &val))
			s->chg_rt = val;
		if (!regmap_read(sm->regmap, sm->chg_base + SMBB_BAT_RT_STS, &val))
			s->bat_rt = val;
		if (!regmap_read(sm->regmap, sm->chg_base + SMBB_USB_RT_STS, &val))
			s->usb_rt = val;
		if (!regmap_read(sm->regmap, sm->chg_base + SMBB_DC_RT_STS, &val))
			s->dc_rt = val;
	}

	for (i = 0; i < NR_CPUS_LOGGED; i++)
		s->mhz[i] = cpufreq_quick_get(i) / 1000;

	s->max_temp = S8_MIN;
	for (i = 0; i < NR_ZONES; i++) {
		/* TSENS registers its zones after this driver probes */
		if (IS_ERR_OR_NULL(sm->zones[i])) {
			char name[THERMAL_NAME_LENGTH];

			snprintf(name, sizeof(name), "cpu%d-thermal", i);
			sm->zones[i] = thermal_zone_get_zone_by_name(name);
		}
		if (!IS_ERR_OR_NULL(sm->zones[i]) &&
		    !thermal_zone_get_temp(sm->zones[i], &temp))
			s->max_temp = max_t(int, s->max_temp, clamp(temp / 1000, -127, 127));
	}
}

static void supply_mon_work(struct work_struct *work)
{
	struct supply_mon *sm = container_of(work, struct supply_mon, work.work);
	struct supply_sample s;

	if (!period_ms)
		goto again;

	supply_mon_sample(sm, &s);
	supply_mon_regulate(sm, &s);

	if (!(s.flags & FLAG_VPH_ERR)) {
		if (!sm->vph_min_mv || s.vph_mv < sm->vph_min_mv)
			sm->vph_min_mv = s.vph_mv;
		if (warn_mv && s.vph_mv < warn_mv)
			dev_warn_ratelimited(sm->dev, "VPH_PWR dip: %u mV\n", s.vph_mv);
	}

	sm->last = s;
	if (sm->log) {
		struct supply_log *log = sm->log;

		memcpy_toio((void __iomem *)&log->samples[log->head], &s, sizeof(s));
		log->head = (log->head + 1) % log->nr_samples;
		log->count++;
	}

again:
	schedule_delayed_work(&sm->work, msecs_to_jiffies(period_ms ? period_ms : 1000));
}

static void supply_mon_print(struct seq_file *m, const struct supply_sample *s)
{
	seq_printf(m, "%10u %8u.%03u %5u %5u %5d %02x %02x %02x %02x %4u/%4u/%4u/%4u %4d %s%s\n",
		   s->seq, s->ms / 1000, s->ms % 1000, s->vph_mv, s->usbin_mv, s->ibat_ma,
		   s->chg_rt, s->bat_rt, s->usb_rt, s->dc_rt,
		   s->mhz[0], s->mhz[1], s->mhz[2], s->mhz[3], s->max_temp,
		   s->flags & FLAG_CAPPED ? "capped " : "",
		   s->flags & FLAG_VPH_ERR ? "vph-error" : "");
}

static const char supply_mon_header[] =
	"       seq        time   vph usbin  ibat chg bat usb dc  cpu0-3 MHz          temp flags\n";

static int supply_mon_previous_show(struct seq_file *m, void *data)
{
	struct supply_mon *sm = m->private;
	struct supply_log *p = sm->prev;
	u32 n, i, first;

	if (!p) {
		seq_puts(m, "no log from the previous boot\n");
		return 0;
	}

	n = min(p->count, p->nr_samples);
	first = (p->head + p->nr_samples - n) % p->nr_samples;
	seq_printf(m, "boot %u, %u samples\n", p->boot, n);
	seq_puts(m, supply_mon_header);
	for (i = 0; i < n; i++)
		supply_mon_print(m, &p->samples[(first + i) % p->nr_samples]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(supply_mon_previous);

static int supply_mon_current_show(struct seq_file *m, void *data)
{
	struct supply_mon *sm = m->private;

	seq_printf(m, "vph_min_mv %u\ncaps %u\ncapped %u\nperiod_ms %u\n",
		   sm->vph_min_mv, sm->caps, sm->capped, period_ms);
	seq_puts(m, supply_mon_header);
	supply_mon_print(m, &sm->last);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(supply_mon_current);

/* Keep the previous boot's samples, then start this boot's log */
static void supply_mon_init_log(struct supply_mon *sm)
{
	struct supply_log *log = sm->log;
	u32 capacity = (sm->log_size - sizeof(*log)) / sizeof(struct supply_sample);
	u32 boot = 0;

	if (log->magic == LOG_MAGIC && log->version == LOG_VERSION &&
	    log->sample_size == sizeof(struct supply_sample) &&
	    log->nr_samples == capacity && log->head < capacity) {
		sm->prev = kmalloc(sm->log_size, GFP_KERNEL);
		if (sm->prev)
			memcpy_fromio(sm->prev, (void __iomem *)log, sm->log_size);
		boot = log->boot;
	}

	memset_io((void __iomem *)log, 0, sizeof(*log));
	log->version = LOG_VERSION;
	log->boot = boot + 1;
	log->sample_size = sizeof(struct supply_sample);
	log->nr_samples = capacity;
	log->magic = LOG_MAGIC;

	if (sm->prev) {
		struct supply_log *p = sm->prev;
		u32 n = min(p->count, p->nr_samples);
		u32 i, first = (p->head + p->nr_samples - min(n, 5u)) % p->nr_samples;

		dev_info(sm->dev, "previous boot: %u samples, last ones:\n", n);
		for (i = 0; i < min(n, 5u); i++) {
			struct supply_sample *s = &p->samples[(first + i) % p->nr_samples];

			dev_info(sm->dev, "  t=%u.%03u s vph=%u mV usbin=%u mV ibat=%d mA rt=%02x/%02x/%02x/%02x %u/%u/%u/%u MHz %d C%s\n",
				 s->ms / 1000, s->ms % 1000, s->vph_mv, s->usbin_mv, s->ibat_ma,
				 s->chg_rt, s->bat_rt, s->usb_rt, s->dc_rt,
				 s->mhz[0], s->mhz[1], s->mhz[2], s->mhz[3],
				 s->max_temp, s->flags & FLAG_CAPPED ? " capped" : "");
		}
	}
}

static int supply_mon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct reserved_mem *rmem;
	struct supply_mon *sm;

	sm = devm_kzalloc(dev, sizeof(*sm), GFP_KERNEL);
	if (!sm)
		return -ENOMEM;
	sm->dev = dev;

	sm->vph = devm_iio_channel_get(dev, "vph");
	if (IS_ERR(sm->vph))
		return dev_err_probe(dev, PTR_ERR(sm->vph), "no VPH_PWR channel\n");
	sm->usbin = devm_iio_channel_get(dev, "usbin");
	if (IS_ERR(sm->usbin)) {
		if (PTR_ERR(sm->usbin) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		sm->usbin = NULL;
	}
	sm->ibat = devm_iio_channel_get(dev, "ibat");
	if (IS_ERR(sm->ibat)) {
		if (PTR_ERR(sm->ibat) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		sm->ibat = NULL;
	}

	/* The charger status registers, through the PMIC regmap */
	np = of_parse_phandle(dev->of_node, "qcom,charger", 0);
	if (np) {
		struct platform_device *chg = of_find_device_by_node(np);

		if (!of_property_read_u32(np, "reg", &sm->chg_base) && chg)
			sm->regmap = dev_get_regmap(chg->dev.parent, NULL);
		if (chg)
			put_device(&chg->dev);
		of_node_put(np);
	}

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (np) {
		rmem = of_reserved_mem_lookup(np);
		of_node_put(np);
		if (rmem && rmem->size > sizeof(struct supply_log) + sizeof(struct supply_sample)) {
			sm->log = devm_memremap(dev, rmem->base, rmem->size, MEMREMAP_WC);
			if (IS_ERR(sm->log))
				sm->log = NULL;
			else
				sm->log_size = rmem->size;
		}
	}
	if (sm->log)
		supply_mon_init_log(sm);
	else
		dev_warn(dev, "no log region, samples are not kept across resets\n");

	sm->debugfs = debugfs_create_dir("supply_mon", NULL);
	debugfs_create_file("previous", 0444, sm->debugfs, sm, &supply_mon_previous_fops);
	debugfs_create_file("current", 0444, sm->debugfs, sm, &supply_mon_current_fops);

	platform_set_drvdata(pdev, sm);
	INIT_DELAYED_WORK(&sm->work, supply_mon_work);
	schedule_delayed_work(&sm->work, 0);

	dev_info(dev, "sampling every %u ms%s\n", period_ms,
		 vmin_mv ? ", CPU cap below vmin_mv" : "");

	return 0;
}

static void supply_mon_remove(struct platform_device *pdev)
{
	struct supply_mon *sm = platform_get_drvdata(pdev);
	int cpu;

	cancel_delayed_work_sync(&sm->work);
	for (cpu = 0; cpu < NR_CPUS_LOGGED; cpu++)
		if (sm->qos_added[cpu])
			freq_qos_remove_request(&sm->qos[cpu]);
	debugfs_remove_recursive(sm->debugfs);
	kfree(sm->prev);
}

static const struct of_device_id supply_mon_of_match[] = {
	{ .compatible = "qcom,pm8941-supply-monitor" },
	{ }
};
MODULE_DEVICE_TABLE(of, supply_mon_of_match);

static struct platform_driver supply_mon_driver = {
	.probe = supply_mon_probe,
	.remove = supply_mon_remove,
	.driver = {
		.name = "qcom-supply-mon",
		.of_match_table = supply_mon_of_match,
	},
};
module_platform_driver(supply_mon_driver);

MODULE_DESCRIPTION("Qualcomm PM8941 supply monitor");
MODULE_LICENSE("GPL");
