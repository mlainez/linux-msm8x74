// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2014, Sony Mobile Communications Inc.
 *
 * This driver is for the multi-block Switch-Mode Battery Charger and Boost
 * (SMBB) hardware, found in Qualcomm PM8941 PMICs.  The charger is an
 * integrated, single-cell lithium-ion battery charger.
 *
 * Sub-components:
 *  - Charger core
 *  - Buck
 *  - DC charge-path
 *  - USB charge-path
 *  - Battery interface
 *  - Boost (not implemented)
 *  - Misc
 *  - HF-Buck
 */

#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/extcon-provider.h>
#include <linux/iio/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>

/*
 * Real-time interrupt status registers, one per sub-block (offset 0x10 in
 * each peripheral).  These are the authoritative live state: the driver's
 * cached status word depends on edges being delivered, and on this hardware
 * the fast-charge and usbin-valid interrupts do not always fire (a cable
 * unplug/replug left both counters at zero while the cached bits still
 * claimed a running cycle).  Bit assignments are the ones the DT uses for
 * the named interrupts.
 */
#define SMBB_CHG_RT_STS		0x010
#define CHG_RT_TRKL_ON		BIT(4)
#define CHG_RT_FAST_ON		BIT(5)
#define CHG_RT_CHG_DONE		BIT(7)
#define SMBB_BAT_RT_STS		0x210
#define BAT_RT_PRESENT		BIT(0)
#define BAT_RT_TEMP_OK		BIT(1)
#define SMBB_USB_RT_STS		0x310
#define USB_RT_VALID		BIT(1)
#define USB_RT_CHG_GONE		BIT(2)
#define SMBB_DC_RT_STS		0x410
#define DC_RT_VALID		BIT(1)

#define SMBB_CHG_VMAX		0x040
#define SMBB_CHG_VSAFE		0x041
#define SMBB_CHG_CFG		0x043
#define SMBB_CHG_IMAX		0x044
#define SMBB_CHG_ISAFE		0x045
#define SMBB_CHG_VIN_MIN	0x047
#define SMBB_CHG_CTRL		0x049
#define CTRL_EN			BIT(7)
/*
 * Forces the charge path to run from the battery instead of a connected
 * charger.  Set by the vendor driver around reverse-boost recovery and by
 * host-mode transitions, and it survives a reset - so it must be cleared
 * explicitly at probe or charging never engages again.
 */
#define CTRL_ON_BAT_FORCE	BIT(0)
#define SMBB_CHG_FAILED		0x04a
#define CHG_FAILED_CLEAR	BIT(7)	/* write-1-to-clear latch */
#define SMBB_CHG_VBAT_WEAK	0x052
#define SMBB_CHG_IBAT_TERM_CHG	0x05b
#define IBAT_TERM_CHG_IEOC	BIT(7)
#define IBAT_TERM_CHG_IEOC_BMS	BIT(7)
#define IBAT_TERM_CHG_IEOC_CHG	0
#define SMBB_CHG_VBAT_DET	0x05d
#define SMBB_CHG_TCHG_MAX_EN	0x060
#define TCHG_MAX_EN		BIT(7)
#define SMBB_CHG_TCHG_MAX	0x061
/* register counts in 4-minute units, biased by one */
#define TCHG_MAX_MINUTES(m)	((m) / 4 - 1)
#define SMBB_CHG_WDOG_TIME	0x062
#define SMBB_CHG_WDOG_EN	0x065
#define WDOG_EN			BIT(7)
#define SMBB_CHG_SEC_ACCESS	0x0d0
/*
 * The vendor calls these two the "trickle stuck workaround": without them
 * a cycle can stay in trickle and never promote to fast charge, which
 * looks exactly like "the input is valid but nothing charges".
 */
#define SMBB_CHG_TRICKLE_CLAMP	0x0e3
#define SMBB_CHG_OVR0		0x0ed

#define SMBB_BUCK_REG_MODE	0x174
#define BUCK_REG_MODE		BIT(0)
#define BUCK_REG_MODE_VBAT	BIT(0)
#define BUCK_REG_MODE_VSYS	0

#define SMBB_BAT_PRES_STATUS	0x208
#define PRES_STATUS_BAT_PRES	BIT(7)
#define SMBB_BAT_TEMP_STATUS	0x209
#define TEMP_STATUS_OK		BIT(7)
#define TEMP_STATUS_HOT		BIT(6)
#define SMBB_BAT_BPD_CTRL	0x248
#define BPD_CTRL_BAT_THM_EN	BIT(1)
#define BPD_CTRL_BAT_ID_EN	BIT(0)
#define BPD_CTRL_SEL_MASK	(BPD_CTRL_BAT_THM_EN | BPD_CTRL_BAT_ID_EN)
#define SMBB_BAT_BTC_CTRL	0x249
#define BTC_CTRL_COMP_EN	BIT(7)
#define BTC_CTRL_COLD_EXT	BIT(1)
#define BTC_CTRL_HOT_EXT_N	BIT(0)
#define SMBB_BAT_VREF_THM_CTRL	0x24a
#define VREF_BAT_THM_FORCE_ON	(BIT(7) | BIT(6))

#define SMBB_USB_IMAX		0x344
#define SMBB_USB_SUSP		0x347
#define USB_SUSP_EN		BIT(0)	/* input suspend; survives a reset */
#define SMBB_USB_OTG_CTL	0x348
#define OTG_CTL_EN		BIT(0)
#define SMBB_USB_ENUM_TIMER_STOP 0x34e
#define ENUM_TIMER_STOP		BIT(0)
#define SMBB_USB_SEC_ACCESS	0x3d0
#define SEC_ACCESS_MAGIC	0xa5
#define SMBB_USB_REV_BST	0x3ed
#define REV_BST_CHG_GONE	BIT(7)

#define SMBB_DC_IMAX		0x444

#define SMBB_MISC_REV2		0x601
#define SMBB_MISC_BOOT_DONE	0x642
#define BOOT_DONE		BIT(7)

#define STATUS_USBIN_VALID	BIT(0) /* USB connection is valid */
#define STATUS_DCIN_VALID	BIT(1) /* DC connection is valid */
#define STATUS_BAT_HOT		BIT(2) /* Battery temp 1=Hot, 0=Cold */
#define STATUS_BAT_OK		BIT(3) /* Battery temp OK */
#define STATUS_BAT_PRESENT	BIT(4) /* Battery is present */
#define STATUS_CHG_DONE		BIT(5) /* Charge cycle is complete */
#define STATUS_CHG_TRKL		BIT(6) /* Trickle charging */
#define STATUS_CHG_FAST		BIT(7) /* Fast charging */
#define STATUS_CHG_GONE		BIT(8) /* No charger is connected */

enum smbb_attr {
	ATTR_BAT_ISAFE,
	ATTR_BAT_IMAX,
	ATTR_USBIN_IMAX,
	ATTR_DCIN_IMAX,
	ATTR_BAT_VSAFE,
	ATTR_BAT_VMAX,
	ATTR_BAT_VMIN,
	ATTR_CHG_VDET,
	ATTR_VIN_MIN,
	_ATTR_CNT,
};

struct smbb_charger {
	unsigned int revision;
	unsigned int addr;
	struct device *dev;
	struct extcon_dev *edev;

	bool dc_disabled;
	bool jeita_ext_temp;
	/*
	 * Set for boards that must never run a charge cycle - a carrier board
	 * feeding the battery terminals from a bench supply, for instance.
	 * The driver stays loaded for OTG and reporting, but the charge path
	 * is actively turned off rather than left as the boot chain had it.
	 */
	bool charging_disabled;
	unsigned long status;
	struct mutex statlock;

	/*
	 * Engagement supervisor.  The hardware needs software to keep the
	 * charge path armed: see smbb_engage_work().  rearm_stage carries a
	 * pending CTRL_EN off->on toggle across two work invocations so the
	 * settling delay does not block a workqueue.
	 */
	struct delayed_work engage_work;
	unsigned int rearm_stage;
	unsigned int rearm_count;
	unsigned int gate_clear_count;
	bool was_charging;
	bool no_batt_reported;		/* logged "running off the input" once */
	unsigned long last_ramp;	/* jiffies of the last input-limit step up */
	const char *supply_mode;	/* last mode announced (string literal) */

	/*
	 * Adaptive input current limit.  The vendor driver never programs the
	 * configured input limit outright: it starts at 100/500 mA and lets
	 * the limit settle upward, so a marginal supply or cable is not asked
	 * for more than it can hold.  Mirror that with a ramp that backs off
	 * whenever the input actually collapses.
	 */
	unsigned int iusb_idx;		/* limit currently programmed */
	unsigned int iusb_floor_idx;	/* ramp starts here after insertion */
	unsigned int iusb_max_idx;	/* configured ceiling (from DT) */
	unsigned int iusb_learn_idx;	/* ceiling learned from a collapse */
	unsigned int collapse_count;
	bool collapse_pending;
	unsigned int rearm_delay_ms;	/* backoff between re-arm attempts */

	/*
	 * Battery voltage, when the board provides it.  Without it the
	 * supervisor cannot tell a charge path that needs prodding from a
	 * battery that is simply full, because the hardware reports neither
	 * a running cycle nor end-of-charge in that state.
	 */
	struct iio_channel *vbat_chan;
	struct dentry *debugfs;

	unsigned int attr[_ATTR_CNT];

	struct power_supply *usb_psy;
	struct power_supply *dc_psy;
	struct power_supply *bat_psy;
	struct regmap *regmap;

	struct regulator_desc otg_rdesc;
	struct regulator_dev *otg_reg;
};

static const unsigned int smbb_usb_extcon_cable[] = {
	EXTCON_USB,
	EXTCON_NONE,
};

static int smbb_vbat_weak_fn(unsigned int index)
{
	return 2100000 + index * 100000;
}

static int smbb_vin_fn(unsigned int index)
{
	if (index > 42)
		return 5600000 + (index - 43) * 200000;
	return 3400000 + index * 50000;
}

static int smbb_vmax_fn(unsigned int index)
{
	return 3240000 + index * 10000;
}

static int smbb_vbat_det_fn(unsigned int index)
{
	return 3240000 + index * 20000;
}

static int smbb_imax_fn(unsigned int index)
{
	if (index < 2)
		return 100000 + index * 50000;
	return index * 100000;
}

static int smbb_bat_imax_fn(unsigned int index)
{
	return index * 50000;
}

static unsigned int smbb_hw_lookup(unsigned int val, int (*fn)(unsigned int))
{
	unsigned int widx;
	unsigned int sel;

	for (widx = sel = 0; (*fn)(widx) <= val; ++widx)
		sel = widx;

	return sel;
}

static const struct smbb_charger_attr {
	const char *name;
	unsigned int reg;
	unsigned int safe_reg;
	unsigned int max;
	unsigned int min;
	unsigned int fail_ok;
	int (*hw_fn)(unsigned int);
} smbb_charger_attrs[] = {
	[ATTR_BAT_ISAFE] = {
		.name = "qcom,fast-charge-safe-current",
		.reg = SMBB_CHG_ISAFE,
		.max = 3000000,
		.min = 200000,
		.hw_fn = smbb_bat_imax_fn,
		.fail_ok = 1,
	},
	[ATTR_BAT_IMAX] = {
		.name = "qcom,fast-charge-current-limit",
		.reg = SMBB_CHG_IMAX,
		.safe_reg = SMBB_CHG_ISAFE,
		.max = 3000000,
		.min = 200000,
		.hw_fn = smbb_bat_imax_fn,
	},
	[ATTR_DCIN_IMAX] = {
		.name = "qcom,dc-current-limit",
		.reg = SMBB_DC_IMAX,
		.max = 2500000,
		.min = 100000,
		.hw_fn = smbb_imax_fn,
	},
	[ATTR_BAT_VSAFE] = {
		.name = "qcom,fast-charge-safe-voltage",
		.reg = SMBB_CHG_VSAFE,
		.max = 5000000,
		.min = 3240000,
		.hw_fn = smbb_vmax_fn,
		.fail_ok = 1,
	},
	[ATTR_BAT_VMAX] = {
		.name = "qcom,fast-charge-high-threshold-voltage",
		.reg = SMBB_CHG_VMAX,
		.safe_reg = SMBB_CHG_VSAFE,
		.max = 5000000,
		.min = 3240000,
		.hw_fn = smbb_vmax_fn,
	},
	[ATTR_BAT_VMIN] = {
		.name = "qcom,fast-charge-low-threshold-voltage",
		.reg = SMBB_CHG_VBAT_WEAK,
		.max = 3600000,
		.min = 2100000,
		.hw_fn = smbb_vbat_weak_fn,
	},
	[ATTR_CHG_VDET] = {
		.name = "qcom,auto-recharge-threshold-voltage",
		.reg = SMBB_CHG_VBAT_DET,
		.max = 5000000,
		.min = 3240000,
		.hw_fn = smbb_vbat_det_fn,
	},
	[ATTR_VIN_MIN] = {
		.name = "qcom,minimum-input-voltage",
		.reg = SMBB_CHG_VIN_MIN,
		.max = 9600000,
		.min = 4200000,
		.hw_fn = smbb_vin_fn,
	},
	[ATTR_USBIN_IMAX] = {
		.name = "usb-charge-current-limit",
		.reg = SMBB_USB_IMAX,
		.max = 2500000,
		.min = 100000,
		.hw_fn = smbb_imax_fn,
	},
};

static int smbb_charger_attr_write(struct smbb_charger *chg,
		enum smbb_attr which, unsigned int val)
{
	const struct smbb_charger_attr *prop;
	unsigned int wval;
	unsigned int out;
	int rc;

	prop = &smbb_charger_attrs[which];

	if (val > prop->max || val < prop->min) {
		dev_err(chg->dev, "value out of range for %s [%u:%u]\n",
			prop->name, prop->min, prop->max);
		return -EINVAL;
	}

	if (prop->safe_reg) {
		rc = regmap_read(chg->regmap,
				chg->addr + prop->safe_reg, &wval);
		if (rc) {
			dev_err(chg->dev,
				"unable to read safe value for '%s'\n",
				prop->name);
			return rc;
		}

		wval = prop->hw_fn(wval);

		if (val > wval) {
			dev_warn(chg->dev,
				"%s above safe value, clamping at %u\n",
				prop->name, wval);
			val = wval;
		}
	}

	wval = smbb_hw_lookup(val, prop->hw_fn);

	rc = regmap_write(chg->regmap, chg->addr + prop->reg, wval);
	if (rc) {
		dev_err(chg->dev, "unable to update %s", prop->name);
		return rc;
	}
	out = prop->hw_fn(wval);
	if (out != val) {
		dev_warn(chg->dev,
			"%s inaccurate, rounded to %u\n",
			prop->name, out);
	}

	dev_dbg(chg->dev, "%s <= %d\n", prop->name, out);

	chg->attr[which] = out;

	return 0;
}

static int smbb_charger_attr_read(struct smbb_charger *chg,
		enum smbb_attr which)
{
	const struct smbb_charger_attr *prop;
	unsigned int val;
	int rc;

	prop = &smbb_charger_attrs[which];

	rc = regmap_read(chg->regmap, chg->addr + prop->reg, &val);
	if (rc) {
		dev_err(chg->dev, "failed to read %s\n", prop->name);
		return rc;
	}
	val = prop->hw_fn(val);
	dev_dbg(chg->dev, "%s => %d\n", prop->name, val);

	chg->attr[which] = val;

	return 0;
}

static int smbb_charger_attr_parse(struct smbb_charger *chg,
		enum smbb_attr which)
{
	const struct smbb_charger_attr *prop;
	unsigned int val;
	int rc;

	prop = &smbb_charger_attrs[which];

	rc = of_property_read_u32(chg->dev->of_node, prop->name, &val);
	if (rc == 0) {
		rc = smbb_charger_attr_write(chg, which, val);
		if (!rc || !prop->fail_ok)
			return rc;
	}
	return smbb_charger_attr_read(chg, which);
}

static void smbb_set_line_flag(struct smbb_charger *chg, int irq, int flag)
{
	bool state;
	int ret;

	ret = irq_get_irqchip_state(irq, IRQCHIP_STATE_LINE_LEVEL, &state);
	if (ret < 0) {
		dev_err(chg->dev, "failed to read irq line\n");
		return;
	}

	mutex_lock(&chg->statlock);
	if (state)
		chg->status |= flag;
	else
		chg->status &= ~flag;
	mutex_unlock(&chg->statlock);

	dev_dbg(chg->dev, "status = %03lx\n", chg->status);
}

/*
 * Engagement supervisor.
 *
 * This charger is not self-arming.  Three classes of state stop a charge
 * cycle from ever starting, all of them sticky across a reset and none of
 * them observable from the power-supply properties:
 *
 *  - the gates: CTRL_ON_BAT_FORCE, USB_SUSP and the CHG_FAILED latch;
 *  - the hardware end-of-charge latch, which is only cleared by a
 *    CTRL_EN off->on toggle: without it a full battery that later drops
 *    below VBAT_DET never resumes charging;
 *  - transients that clear CTRL_EN behind our back.
 *
 * The vendor driver handles all three by re-asserting CTRL_EN every ten
 * seconds while an input is present and by toggling it at end of charge.
 * Do the same: clear the gates and re-assert CTRL_EN on every pass, and
 * when an input is present but no cycle is running, re-arm with a
 * two-stage toggle (settling delay taken as a work reschedule, so nothing
 * blocks the workqueue).  If the battery is genuinely full the hardware
 * comparator simply keeps the buck off and the toggle is a no-op.
 */
#define SMBB_ENGAGE_POLL_MS	10000
#define SMBB_REARM_SETTLE_MS	2000
#define SMBB_REARM_MAX_MS	300000

static void smbb_clear_gates(struct smbb_charger *chg)
{
	static const struct {
		unsigned int off, mask, val;
	} gates[] = {
		{ SMBB_CHG_CTRL, CTRL_ON_BAT_FORCE, 0 },
		{ SMBB_USB_SUSP, USB_SUSP_EN, 0 },
	};
	unsigned int reg;
	int i, rc;

	for (i = 0; i < ARRAY_SIZE(gates); ++i) {
		rc = regmap_read(chg->regmap, chg->addr + gates[i].off, &reg);
		if (rc || !(reg & gates[i].mask))
			continue;
		regmap_update_bits(chg->regmap, chg->addr + gates[i].off,
				   gates[i].mask, gates[i].val);
		chg->gate_clear_count++;
		dev_info(chg->dev, "cleared charge gate %03x (was %02x)\n",
			 gates[i].off, reg);
	}

	/* Write-1-to-clear, so it needs a plain write, not update_bits. */
	rc = regmap_read(chg->regmap, chg->addr + SMBB_CHG_FAILED, &reg);
	if (!rc && (reg & CHG_FAILED_CLEAR)) {
		regmap_write(chg->regmap, chg->addr + SMBB_CHG_FAILED,
			     CHG_FAILED_CLEAR);
		chg->gate_clear_count++;
		dev_info(chg->dev, "cleared the charge-failed latch\n");
	}
}

/*
 * Read the charge path straight out of the hardware.  Decisions here must
 * not depend on the cached status word: see the RT_STS comment above.
 */
static void smbb_hw_state(struct smbb_charger *chg, bool *input,
			  bool *charging, bool *bat_ready, bool *done)
{
	unsigned int chgr = 0, usb = 0, dc = 0, bat = 0;

	regmap_read(chg->regmap, chg->addr + SMBB_CHG_RT_STS, &chgr);
	regmap_read(chg->regmap, chg->addr + SMBB_USB_RT_STS, &usb);
	regmap_read(chg->regmap, chg->addr + SMBB_BAT_RT_STS, &bat);
	if (!chg->dc_disabled)
		regmap_read(chg->regmap, chg->addr + SMBB_DC_RT_STS, &dc);

	/*
	 * CHG_GONE is the signal that matters when a cable is pulled: this
	 * hardware keeps USB_RT_VALID asserted and even leaves the
	 * fast-charge bit set after the input is physically gone, and only
	 * raises CHG_GONE.  Measured on a Fairphone 2: across an unplug the
	 * only register change was USB_RT_STS 0x03 -> 0x07 while the battery
	 * visibly took over the load.  Treating the stale bits as truth would
	 * make the supervisor believe a cycle is running and skip the re-arm
	 * it exists to perform.
	 */
	if (usb & USB_RT_CHG_GONE)
		usb &= ~USB_RT_VALID;

	*input = (usb & USB_RT_VALID) || (dc & DC_RT_VALID);
	*charging = *input && (chgr & (CHG_RT_FAST_ON | CHG_RT_TRKL_ON));
	*bat_ready = (bat & BAT_RT_PRESENT) && (bat & BAT_RT_TEMP_OK);
	*done = chgr & CHG_RT_CHG_DONE;
}

/*
 * Is the OTG boost driving VBUS?  That bit is the kernel's own record of being
 * in host mode: the USB controller asks for vbus-supply (chg_otg) when the ID
 * pin says a peripheral is attached, and the boost then makes 5 V out of VPH -
 * a LOAD on the supply, not a source.
 */
static bool smbb_otg_enabled(struct smbb_charger *chg)
{
	unsigned int val = 0;

	regmap_read(chg->regmap, chg->addr + SMBB_USB_OTG_CTL, &val);

	return val & OTG_CTL_EN;
}

/*
 * How this board is being powered right now.  It is worth naming explicitly
 * because the answer decides which voltage is meaningful, and a battery-less
 * fixture can be wired either way:
 *
 *   INPUT   a charger/host supplies USB_IN and the system draws through the
 *           charge path.  The battery terminals are unfed, so VBAT_SNS is
 *           meaningless (~0.83 V measured) and USBIN is the node that sags.
 *   EXTERN  no USB input; VPH is fed directly at the battery terminals from an
 *           external supply.  Now VBAT_SNS *is* the feed voltage.  If the OTG
 *           boost is also on, the board is hosting and paying for VBUS too.
 *   BATTERY a cell is fitted: the normal phone case.
 */
static const char *smbb_supply_mode(struct smbb_charger *chg, bool input,
				    bool present)
{
	if (present)
		return "battery";
	if (input)
		return "input (no battery: system drawn through the charge path)";
	if (smbb_otg_enabled(chg))
		return "extern + otg (no input; VPH fed at the terminals, hosting VBUS)";
	return "extern (no input, no battery: VPH fed at the terminals)";
}

/*
 * Presence alone, without the temperature qualifier that bat_ready carries: a
 * board with no cell at all has to be powered from the input, whatever the
 * (meaningless) battery thermistor reading says.
 */
static bool smbb_battery_present(struct smbb_charger *chg)
{
	unsigned int bat = 0;

	regmap_read(chg->regmap, chg->addr + SMBB_BAT_RT_STS, &bat);

	return bat & BAT_RT_PRESENT;
}

/* 500 mA: what a USB host port is required to supply, and the vendor's
 * fallback limit before its input-current limit has settled.
 */
#define SMBB_IUSB_FLOOR_UA	500000
/*
 * Where to start the input limit when there is no battery and the input is
 * therefore powering the SoC.  500 mA starves bring-up; 1.5 A collapses a plain
 * host port (measured on the carrier rig 2026-08-04, "input collapsed at
 * 1500000 uA" followed by UVLO).  900 mA is what USB 3 guarantees, so start
 * there and let the ramp below discover whether this supply gives more.
 */
#define SMBB_IUSB_NOBATT_UA	900000
/*
 * Minimum time between upward input-limit steps.  The supervisor is kicked
 * immediately by every charger interrupt, so an unstable input produces a burst
 * of passes - and an event-driven ramp then walks from 900 mA to the ceiling in
 * milliseconds and collapses there (measured on the carrier rig 2026-08-04:
 * "input collapsed at 1500000 uA" moments after probe started at 900 mA).  An
 * adaptive limit has to settle at each step before believing it holds.
 */
#define SMBB_AICL_STEP_MS	3000

/*
 * The input limit as the HARDWARE has it, not as we believe it to be.  Worth
 * having separately: a stale or uninitialised cache made this driver report
 * "input collapsed at 100000 uA" during a run where the register held something
 * else entirely, which cost a debugging cycle on 2026-08-04.
 */
static unsigned int smbb_iusb_hw_ua(struct smbb_charger *chg)
{
	unsigned int idx = 0;

	regmap_read(chg->regmap, chg->addr + SMBB_USB_IMAX, &idx);

	return smbb_imax_fn(idx);
}

static void smbb_set_iusb(struct smbb_charger *chg, unsigned int idx)
{
	if (idx > chg->iusb_learn_idx)
		idx = chg->iusb_learn_idx;
	if (idx == chg->iusb_idx)
		return;
	if (regmap_write(chg->regmap, chg->addr + SMBB_USB_IMAX, idx))
		return;
	chg->iusb_idx = idx;
	dev_dbg(chg->dev, "input limit %u uA\n", smbb_imax_fn(idx));
}

/*
 * True when the battery sits at or above the auto-recharge threshold, i.e.
 * the hardware is right to keep the buck off and nothing needs re-arming.
 * Errs towards "not full" when no channel is available, preserving the
 * previous behaviour on boards that do not wire one up.
 */
static bool smbb_battery_full(struct smbb_charger *chg)
{
	int uv, rc;

	if (!chg->vbat_chan)
		return false;

	rc = iio_read_channel_processed(chg->vbat_chan, &uv);
	if (rc < 0)
		return false;

	return uv >= chg->attr[ATTR_CHG_VDET];
}

static void smbb_engage_work(struct work_struct *work)
{
	struct smbb_charger *chg = container_of(work, struct smbb_charger,
						engage_work.work);
	bool input, charging, bat_ready, done;
	unsigned int delay = SMBB_ENGAGE_POLL_MS;
	const char *mode;

	smbb_hw_state(chg, &input, &charging, &bat_ready, &done);

	/*
	 * Announce how the board is powered whenever that changes.  On a
	 * fixture this is the difference between "USB_IN feeds everything" and
	 * "VPH is fed at the terminals while we host a hub", which decides
	 * which voltage is worth watching - and it is invisible otherwise.
	 */
	mode = smbb_supply_mode(chg, input, smbb_battery_present(chg));
	if (mode != chg->supply_mode) {
		chg->supply_mode = mode;
		/*
		 * Ratelimited: on a phone a marginal battery contact can toggle
		 * presence repeatedly, and this is diagnostics, not an event
		 * anyone needs every instance of.
		 */
		dev_info_ratelimited(chg->dev, "supply mode: %s\n", mode);
	}

	if (chg->charging_disabled) {
		/* keep the path off; nothing here should ever enable it */
		regmap_update_bits(chg->regmap, chg->addr + SMBB_CHG_CTRL,
				   CTRL_EN, 0);
		return;
	}

	/*
	 * Clear the sticky gates BEFORE deciding whether an input is present.
	 * smbb_hw_state() deliberately treats a latched CHG_GONE as cancelling
	 * USB_RT_VALID, because this hardware leaves stale valid/fast bits set
	 * across an unplug - but the latch outlives the condition, so a cable
	 * that is physically present can read as absent until something clears
	 * it.  Clearing after the "input gone" branch (as this did until
	 * 2026-08-04) meant that misreading persisted for a full poll interval
	 * while the branch dropped the input limit to its floor.  On a board with
	 * no battery that is fatal: measured on the carrier rig, boot reset with
	 * "input collapsed at 100000 uA" moments after g_ether enumerated.
	 */
	smbb_clear_gates(chg);

	if (!input) {
		/*
		 * Cable gone: forget what was learned about this supply and
		 * start the next insertion from the floor again.
		 */
		chg->iusb_learn_idx = chg->iusb_max_idx;
		chg->collapse_pending = false;
		chg->rearm_delay_ms = SMBB_ENGAGE_POLL_MS;
		chg->rearm_stage = 0;
		/*
		 * ... but only when there is a battery to fall back on.  With no
		 * cell, this input is what powers the SoC: if it really is gone
		 * the board is already dead, and if the read was wrong (see the
		 * CHG_GONE latch above) then lowering the limit is what kills
		 * it.  Leave the limit alone and let the next pass re-evaluate.
		 */
		if (smbb_battery_present(chg))
			smbb_set_iusb(chg, chg->iusb_floor_idx);
		if (chg->was_charging) {
			dev_dbg(chg->dev, "input gone, supervisor idle\n");
			chg->was_charging = false;
		}
		return;	/* an insertion IRQ reschedules us */
	}

	if (charging != chg->was_charging) {
		dev_info(chg->dev, "charge cycle %s\n",
			 charging ? "running" : "stopped");
		chg->was_charging = charging;
		/* a state change is a fresh opportunity: retry promptly again */
		chg->rearm_delay_ms = SMBB_ENGAGE_POLL_MS;
	}

	/*
	 * The input collapsed while it was still nominally present: the
	 * supply cannot hold what we are drawing.  Learn a lower ceiling and
	 * stay below it for as long as this cable remains plugged in.
	 */
	if (chg->collapse_pending) {
		chg->collapse_pending = false;
		chg->collapse_count++;
		chg->last_ramp = jiffies;
		/*
		 * Learn a lower ceiling, with or without a battery.
		 *
		 * A previous version of this branch held the limit when no cell
		 * was fitted, reasoning that asking for less current starves the
		 * very load that caused the sag.  Measured on the carrier rig
		 * 2026-08-04, that is wrong in the way that matters: the input
		 * does not sag gracefully, it COLLAPSES, and a collapsed input
		 * delivers nothing at all.  Asking for a level the supply can
		 * actually hold keeps it regulated, which is the entire purpose
		 * of an adaptive input limit - the board reset with
		 * "input collapsed at 1500000 uA" on a host port that cannot
		 * source 1.5 A.  Shedding load is userspace's job (see
		 * rig-envelope, which throttles on this collapse count); getting
		 * the input to stay up is this driver's.
		 */
		if (chg->iusb_idx > chg->iusb_floor_idx) {
			chg->iusb_learn_idx = chg->iusb_idx - 1;
			dev_info(chg->dev,
				 "input collapsed at %u uA (hw %u uA), limiting to %u uA\n",
				 smbb_imax_fn(chg->iusb_idx),
				 smbb_iusb_hw_ua(chg),
				 smbb_imax_fn(chg->iusb_learn_idx));
			smbb_set_iusb(chg, chg->iusb_learn_idx);
		}
	} else if ((charging || !smbb_battery_present(chg)) &&
		   chg->iusb_idx < chg->iusb_learn_idx &&
		   time_after(jiffies, chg->last_ramp +
					msecs_to_jiffies(SMBB_AICL_STEP_MS))) {
		/*
		 * Holding up under the current draw: ask for one step more.
		 * With no battery there is no charge cycle to gate this on, and
		 * the ramp still matters - it is the SoC's budget - so the test
		 * is "not collapsing" rather than "charging".
		 */
		chg->last_ramp = jiffies;
		smbb_set_iusb(chg, chg->iusb_idx + 1);
	}

	switch (chg->rearm_stage) {
	case 1:
		/* settling delay elapsed: bring CTRL_EN back up */
		regmap_update_bits(chg->regmap, chg->addr + SMBB_CHG_CTRL,
				   CTRL_EN, CTRL_EN);
		chg->rearm_stage = 0;
		chg->rearm_count++;
		dev_dbg(chg->dev, "re-arm complete (%u total)\n",
			chg->rearm_count);
		/*
		 * If that did not produce a cycle, back off before trying
		 * again: a charge path that needs prodding recovers within
		 * minutes, and nothing else deserves a CTRL_EN toggle every
		 * ten seconds indefinitely.
		 */
		chg->rearm_delay_ms = min(chg->rearm_delay_ms * 3,
					  SMBB_REARM_MAX_MS);
		delay = chg->rearm_delay_ms;
		break;
	default:
		/*
		 * Do not re-arm when the hardware reports the cycle complete:
		 * the battery is full and the auto-recharge comparator will
		 * start the next cycle by itself once it drains.  Re-arming
		 * anyway would toggle CTRL_EN every pass for as long as a full
		 * battery stays plugged in, which is neither useful nor what
		 * the vendor does (it re-arms once at end of charge and then
		 * waits on the comparator).
		 */
		if (!smbb_battery_present(chg)) {
			/*
			 * No battery at all, but an input is present.  Keep the
			 * path ARMED: on this PMIC the input reaches VPH through
			 * the charge path, so switching it off here does not
			 * merely stop charging, it removes the system's only
			 * supply on a board without a cell.  This is how a
			 * phone boots on a charger with a dead or missing
			 * pack, and it is what mainline does.
			 *
			 * An earlier version of this branch forced CTRL_EN off
			 * on the theory that enabling it would drive the
			 * charger against a bench supply feeding the battery
			 * terminals.  Measured on the carrier rig 2026-08-04,
			 * that is backwards: with the path forced off the
			 * board reset at 7.6-8.3 s on every boot, PON
			 * reporting UVLO, while mainline - which leaves it
			 * armed at 1.5 A - booted the same fixture with all
			 * three remoteprocs and WLAN up.  The charger cannot
			 * push current into a node already held above its
			 * regulation target, so an external supply is not
			 * fought; it is supplemented exactly when it sags.
			 *
			 * NOTE the test is presence, not bat_ready: a battery
			 * that is merely out of its temperature window must
			 * still not be charged, and falls through to the
			 * normal path below.
			 *
			 * Do NOT touch the input limit here.  An earlier
			 * version jumped straight to the learned ceiling on the
			 * theory that a battery-less board must not crawl - but
			 * that bypassed the rate-limited ramp below and slammed
			 * 900 mA to 1.5 A on the first pass, collapsing the host
			 * port (measured 2026-08-04: "input collapsed at
			 * 1500000 uA (hw 1500000 uA)" seconds after probe had
			 * programmed 900 mA).  Probe picks a starting point the
			 * supply can hold and the ramp walks up from there, one
			 * settled step at a time.  This branch's job is only to
			 * keep the path armed.
			 */
			regmap_update_bits(chg->regmap,
					   chg->addr + SMBB_CHG_CTRL,
					   CTRL_EN, CTRL_EN);
			if (!chg->no_batt_reported) {
				dev_info(chg->dev,
					 "no battery present: powering the system from the input\n");
				chg->no_batt_reported = true;
			}
			break;
		}
		if (!bat_ready) {
			/*
			 * A battery IS fitted but is not ready - on this
			 * hardware that means outside its temperature window.
			 * Never arm the path for it, and never re-arm below
			 * either: charging a cell out of range is exactly what
			 * the temperature qualifier exists to prevent.  (Until
			 * 2026-08-04 this case shared the branch above, which
			 * now deliberately keys on presence alone so that a
			 * battery-less board can be powered; without this guard
			 * that change would have armed charging for a hot pack.)
			 */
			regmap_update_bits(chg->regmap,
					   chg->addr + SMBB_CHG_CTRL,
					   CTRL_EN, 0);
			break;
		}
		if (charging || done || smbb_battery_full(chg)) {
			regmap_update_bits(chg->regmap,
					   chg->addr + SMBB_CHG_CTRL,
					   CTRL_EN, CTRL_EN);
			break;
		}
		/*
		 * Input present, battery present and in range, yet no cycle:
		 * drop CTRL_EN so the next pass raises it, clearing any
		 * latched done/failed state in the hardware.
		 */
		regmap_update_bits(chg->regmap, chg->addr + SMBB_CHG_CTRL,
				   CTRL_EN, 0);
		chg->rearm_stage = 1;
		delay = SMBB_REARM_SETTLE_MS;
		break;
	}

	schedule_delayed_work(&chg->engage_work, msecs_to_jiffies(delay));
}

static void smbb_engage_kick(struct smbb_charger *chg)
{
	if (chg->dev)
		mod_delayed_work(system_wq, &chg->engage_work, 0);
}

static irqreturn_t smbb_usb_valid_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_USBIN_VALID);
	extcon_set_state_sync(chg->edev, EXTCON_USB,
				chg->status & STATUS_USBIN_VALID);
	power_supply_changed(chg->usb_psy);
	smbb_engage_kick(chg);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_dc_valid_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_DCIN_VALID);
	if (!chg->dc_disabled)
		power_supply_changed(chg->dc_psy);
	smbb_engage_kick(chg);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_bat_temp_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;
	unsigned int val;
	int rc;

	rc = regmap_read(chg->regmap, chg->addr + SMBB_BAT_TEMP_STATUS, &val);
	if (rc)
		return IRQ_HANDLED;

	mutex_lock(&chg->statlock);
	if (val & TEMP_STATUS_OK) {
		chg->status |= STATUS_BAT_OK;
	} else {
		chg->status &= ~STATUS_BAT_OK;
		if (val & TEMP_STATUS_HOT)
			chg->status |= STATUS_BAT_HOT;
	}
	mutex_unlock(&chg->statlock);

	power_supply_changed(chg->bat_psy);
	return IRQ_HANDLED;
}

static irqreturn_t smbb_bat_present_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_BAT_PRESENT);
	power_supply_changed(chg->bat_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_done_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_DONE);
	power_supply_changed(chg->bat_psy);
	/*
	 * End of charge: the hardware latches "done" and will not restart a
	 * cycle when the battery later falls below VBAT_DET.  Let the
	 * supervisor re-arm with a CTRL_EN toggle.
	 */
	smbb_engage_kick(chg);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_gone_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_GONE);
	/*
	 * Let the supervisor decide what this was: with the input still
	 * valid it is a collapse (back off the input limit), without it it is
	 * just a cable being pulled.
	 */
	chg->collapse_pending = true;
	power_supply_changed(chg->bat_psy);
	power_supply_changed(chg->usb_psy);
	if (!chg->dc_disabled)
		power_supply_changed(chg->dc_psy);
	smbb_engage_kick(chg);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_fast_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_FAST);
	power_supply_changed(chg->bat_psy);

	return IRQ_HANDLED;
}

static irqreturn_t smbb_chg_trkl_handler(int irq, void *_data)
{
	struct smbb_charger *chg = _data;

	smbb_set_line_flag(chg, irq, STATUS_CHG_TRKL);
	power_supply_changed(chg->bat_psy);

	return IRQ_HANDLED;
}

static const struct smbb_irq {
	const char *name;
	irqreturn_t (*handler)(int, void *);
} smbb_charger_irqs[] = {
	{ "chg-done", smbb_chg_done_handler },
	{ "chg-fast", smbb_chg_fast_handler },
	{ "chg-trkl", smbb_chg_trkl_handler },
	{ "bat-temp-ok", smbb_bat_temp_handler },
	{ "bat-present", smbb_bat_present_handler },
	{ "chg-gone", smbb_chg_gone_handler },
	{ "usb-valid", smbb_usb_valid_handler },
	{ "dc-valid", smbb_dc_valid_handler },
};

static int smbb_usbin_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		mutex_lock(&chg->statlock);
		val->intval = !(chg->status & STATUS_CHG_GONE) &&
				(chg->status & STATUS_USBIN_VALID);
		mutex_unlock(&chg->statlock);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		val->intval = chg->attr[ATTR_USBIN_IMAX];
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		val->intval = 2500000;
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_usbin_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		rc = smbb_charger_attr_write(chg, ATTR_USBIN_IMAX,
				val->intval);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_dcin_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		mutex_lock(&chg->statlock);
		val->intval = !(chg->status & STATUS_CHG_GONE) &&
				(chg->status & STATUS_DCIN_VALID);
		mutex_unlock(&chg->statlock);
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		val->intval = chg->attr[ATTR_DCIN_IMAX];
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX:
		val->intval = 2500000;
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_dcin_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT:
		rc = smbb_charger_attr_write(chg, ATTR_DCIN_IMAX,
				val->intval);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_charger_writable_property(struct power_supply *psy,
		enum power_supply_property psp)
{
	return psp == POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT;
}

static int smbb_battery_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	unsigned long status;
	int rc = 0;

	mutex_lock(&chg->statlock);
	status = chg->status;
	mutex_unlock(&chg->statlock);

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		if (status & STATUS_CHG_GONE)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (!(status & (STATUS_DCIN_VALID | STATUS_USBIN_VALID)))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (status & STATUS_CHG_DONE)
			val->intval = POWER_SUPPLY_STATUS_FULL;
		else if (!(status & STATUS_BAT_OK))
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (status & (STATUS_CHG_FAST | STATUS_CHG_TRKL))
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else /* everything is ok for charging, but we are not... */
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		if (status & STATUS_BAT_OK)
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		else if (status & STATUS_BAT_HOT)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else
			val->intval = POWER_SUPPLY_HEALTH_COLD;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		if (status & STATUS_CHG_FAST)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		else if (status & STATUS_CHG_TRKL)
			val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		else
			val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = !!(status & STATUS_BAT_PRESENT);
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = chg->attr[ATTR_BAT_IMAX];
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = chg->attr[ATTR_BAT_VMAX];
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		/* this charger is a single-cell lithium-ion battery charger
		* only.  If you hook up some other technology, there will be
		* fireworks.
		*/
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = 3000000; /* single-cell li-ion low end */
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_battery_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct smbb_charger *chg = power_supply_get_drvdata(psy);
	int rc;

	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		rc = smbb_charger_attr_write(chg, ATTR_BAT_IMAX, val->intval);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		rc = smbb_charger_attr_write(chg, ATTR_BAT_VMAX, val->intval);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	return rc;
}

static int smbb_battery_writable_property(struct power_supply *psy,
		enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CURRENT_MAX:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return 1;
	default:
		return 0;
	}
}

static enum power_supply_property smbb_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT_MAX,
};

static enum power_supply_property smbb_battery_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_TECHNOLOGY,
};

static const struct reg_off_mask_default {
	unsigned int offset;
	unsigned int mask;
	unsigned int value;
	unsigned int rev_mask;
	/*
	 * Write the byte unconditionally instead of read-modify-write.
	 * Required for the SEC_ACCESS unlock (whose effect is the write
	 * itself, not the resulting value) and for write-1-to-clear latches,
	 * both of which regmap_update_bits() would skip when the register
	 * already reads the target value.
	 */
	bool force;
} smbb_charger_setup[] = {
	/* The bootloader is supposed to set this... make sure anyway. */
	{ SMBB_MISC_BOOT_DONE, BOOT_DONE, BOOT_DONE },

	/*
	 * Bound a charge cycle, as the vendor does (its DT asks for 150
	 * minutes on this board).  A cycle that runs longer than that is a
	 * fault, not a slow charge; the hardware latches CHG_FAILED and the
	 * supervisor clears it and re-arms, which is also what the vendor
	 * does from its chg-failed handler.
	 */
	{ SMBB_CHG_TCHG_MAX, 0xff, TCHG_MAX_MINUTES(150), 0, true },
	{ SMBB_CHG_TCHG_MAX_EN, TCHG_MAX_EN, TCHG_MAX_EN },

	/* Clear and disable watchdog */
	{ SMBB_CHG_WDOG_TIME, 0xff, 160 },
	{ SMBB_CHG_WDOG_EN, WDOG_EN, 0 },

	/* Use charger based EoC detection */
	{ SMBB_CHG_IBAT_TERM_CHG, IBAT_TERM_CHG_IEOC, IBAT_TERM_CHG_IEOC_CHG },

	/* Disable GSM PA load adjustment.
	* The PA signal is incorrectly connected on v2.
	*/
	{ SMBB_CHG_CFG, 0xff, 0x00, BIT(3) },

	/* Use VBAT (not VSYS) to compensate for IR drop during fast charging */
	{ SMBB_BUCK_REG_MODE, BUCK_REG_MODE, BUCK_REG_MODE_VBAT },

	/*
	 * Battery-presence detection and its reference.  The vendor driver
	 * selects the thermistor as the presence source and forces the
	 * thermistor bias reference on before anything consumes it; the
	 * boot chain leaves both at hardware defaults.  Sensing presence or
	 * temperature against an unbiased thermistor line lets the PMIC
	 * transiently read the battery as absent, and its response to that
	 * is a system power cut identical to a physical battery removal
	 * (PON poff=0x2000).  The vendor does NOT arm the BTC comparators
	 * on this board (it leaves BAT_BTC_CTRL exactly as the bootloader
	 * left it), so no COMP_EN is set here either.
	 */
	{ SMBB_BAT_BPD_CTRL, BPD_CTRL_SEL_MASK, BPD_CTRL_BAT_THM_EN },
	{ SMBB_BAT_VREF_THM_CTRL, VREF_BAT_THM_FORCE_ON, VREF_BAT_THM_FORCE_ON },

	/* Stop USB enumeration timer */
	{ SMBB_USB_ENUM_TIMER_STOP, ENUM_TIMER_STOP, ENUM_TIMER_STOP },

	/*
	 * Vendor "trickle stuck" workaround: without it a cycle can stay in
	 * trickle and never promote to fast charge.  SEC_ACCESS unlocks
	 * exactly the write that follows it, so each protected register needs
	 * its own unlock.
	 */
	{ SMBB_CHG_SEC_ACCESS, 0xff, SEC_ACCESS_MAGIC, 0, true },
	{ SMBB_CHG_OVR0, 0xff, 0x00, 0, true },
	{ SMBB_CHG_SEC_ACCESS, 0xff, SEC_ACCESS_MAGIC, 0, true },
	{ SMBB_CHG_TRICKLE_CLAMP, 0xff, 0x00, 0, true },

	/*
	 * Take hardware reverse-boost/ARB termination out of the charge
	 * path, as the vendor does at init.  This block used to be #if 0'd
	 * out here, and had its mask and value swapped, so enabling it as
	 * written would have cleared SEC_ACCESS instead of unlocking it.
	 */
	{ SMBB_USB_SEC_ACCESS, 0xff, SEC_ACCESS_MAGIC, 0, true },
	{ SMBB_USB_REV_BST, 0xff, REV_BST_CHG_GONE, 0, true },

	/* Stop USB enumeration timer, again */
	{ SMBB_USB_ENUM_TIMER_STOP, ENUM_TIMER_STOP, ENUM_TIMER_STOP },

	/*
	 * Enable charging with a guaranteed 0->1 edge.  Dropping CTRL_EN
	 * first matters: a single read-modify-write is skipped entirely when
	 * the boot chain already left CTRL_EN set, so any end-of-charge state
	 * latched in the hardware stays latched - which is why charging
	 * engaged on some boots and not others.
	 */
	{ SMBB_CHG_CTRL, CTRL_EN, 0 },
	{ SMBB_CHG_CTRL, CTRL_EN, CTRL_EN },
};


/*
 * A driver-owned diagnostic file: /sys/kernel/debug/qcom_smbb/state.
 *
 * It reads only the handful of registers this driver understands.  That
 * matters on this PMIC: the generic regmap debugfs dump walks the whole
 * address range, and reads of addresses the SPMI arbiter refuses produce a
 * storm of pmic_arb warnings, so it must not be used here.
 */
static const struct {
	const char *name;
	unsigned int off;
} smbb_debug_regs[] = {
	{ "chg_rt_sts",   SMBB_CHG_RT_STS },
	{ "usb_rt_sts",   SMBB_USB_RT_STS },
	{ "bat_rt_sts",   SMBB_BAT_RT_STS },
	{ "dc_rt_sts",    SMBB_DC_RT_STS },
	{ "chg_ctrl",     SMBB_CHG_CTRL },
	{ "chg_failed",   SMBB_CHG_FAILED },
	{ "vmax",         SMBB_CHG_VMAX },
	{ "vbat_det",     SMBB_CHG_VBAT_DET },
	{ "ibat_max",     SMBB_CHG_IMAX },
	{ "vin_min",      SMBB_CHG_VIN_MIN },
	{ "ibat_term",    SMBB_CHG_IBAT_TERM_CHG },
	{ "bat_pres_sts", SMBB_BAT_PRES_STATUS },
	{ "bat_temp_sts", SMBB_BAT_TEMP_STATUS },
	{ "bpd_ctrl",     SMBB_BAT_BPD_CTRL },
	{ "btc_ctrl",     SMBB_BAT_BTC_CTRL },
	{ "vref_thm",     SMBB_BAT_VREF_THM_CTRL },
	{ "usb_imax",     SMBB_USB_IMAX },
	{ "tchg_max",     SMBB_CHG_TCHG_MAX },
	{ "tchg_max_en",  SMBB_CHG_TCHG_MAX_EN },
	{ "usb_susp",     SMBB_USB_SUSP },
	{ "usb_otg_ctl",  SMBB_USB_OTG_CTL },
};

static int smbb_state_show(struct seq_file *s, void *data)
{
	struct smbb_charger *chg = s->private;
	bool input, charging, bat_ready, done;
	unsigned long status;
	unsigned int val;
	int i, rc;

	mutex_lock(&chg->statlock);
	status = chg->status;
	mutex_unlock(&chg->statlock);

	seq_printf(s, "status        0x%03lx\n", status);
	seq_printf(s, "  usbin_valid %d\n", !!(status & STATUS_USBIN_VALID));
	seq_printf(s, "  dcin_valid  %d\n", !!(status & STATUS_DCIN_VALID));
	seq_printf(s, "  bat_present %d\n", !!(status & STATUS_BAT_PRESENT));
	seq_printf(s, "  bat_temp_ok %d\n", !!(status & STATUS_BAT_OK));
	seq_printf(s, "  chg_fast    %d\n", !!(status & STATUS_CHG_FAST));
	seq_printf(s, "  chg_trkl    %d\n", !!(status & STATUS_CHG_TRKL));
	seq_printf(s, "  chg_done    %d\n", !!(status & STATUS_CHG_DONE));
	seq_printf(s, "  chg_gone    %d\n", !!(status & STATUS_CHG_GONE));
	seq_printf(s, "  otg_boost   %d\n", smbb_otg_enabled(chg));
	seq_printf(s, "supply mode   %s\n",
		   smbb_supply_mode(chg, !!(status & STATUS_USBIN_VALID) ||
					 !!(status & STATUS_DCIN_VALID),
				    !!(status & STATUS_BAT_PRESENT)));
	if (chg->charging_disabled)
		seq_puts(s, "charging      DISABLED by device tree\n");
	seq_printf(s, "supervisor    rearms=%u gate_clears=%u stage=%u backoff=%ums\n",
		   chg->rearm_count, chg->gate_clear_count, chg->rearm_stage,
		   chg->rearm_delay_ms);

	/*
	 * Hardware truth, for comparison with the cached bits above: a
	 * mismatch means interrupt edges were missed or coalesced, which is
	 * expected on this hardware and is why the supervisor reads these.
	 */
	smbb_hw_state(chg, &input, &charging, &bat_ready, &done);
	seq_printf(s, "hardware      input=%d charging=%d bat_ready=%d done=%d\n",
		   input, charging, bat_ready, done);
	if (chg->vbat_chan) {
		int uv;

		if (iio_read_channel_processed(chg->vbat_chan, &uv) >= 0)
			seq_printf(s, "battery       %d uV (recharge below %u uV, full=%d)\n",
				   uv, chg->attr[ATTR_CHG_VDET],
				   uv >= chg->attr[ATTR_CHG_VDET]);
	} else {
		seq_puts(s, "battery       no voltage channel\n");
	}
	seq_printf(s, "input limit   %u uA (floor %u, ceiling %u, learned %u, collapses %u)\n",
		   smbb_imax_fn(chg->iusb_idx),
		   smbb_imax_fn(chg->iusb_floor_idx),
		   smbb_imax_fn(chg->iusb_max_idx),
		   smbb_imax_fn(chg->iusb_learn_idx), chg->collapse_count);

	for (i = 0; i < ARRAY_SIZE(smbb_debug_regs); ++i) {
		rc = regmap_read(chg->regmap,
				 chg->addr + smbb_debug_regs[i].off, &val);
		if (rc)
			seq_printf(s, "%-13s (read failed: %d)\n",
				   smbb_debug_regs[i].name, rc);
		else
			seq_printf(s, "%-13s 0x%02x\n",
				   smbb_debug_regs[i].name, val);
	}

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(smbb_state);

static void smbb_debugfs_init(struct smbb_charger *chg)
{
	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		return;

	chg->debugfs = debugfs_create_dir(dev_name(chg->dev), NULL);
	debugfs_create_file("state", 0444, chg->debugfs, chg, &smbb_state_fops);
}

static char *smbb_bif[] = { "smbb-bif" };

static const struct power_supply_desc bat_psy_desc = {
	.name = "smbb-bif",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = smbb_battery_properties,
	.num_properties = ARRAY_SIZE(smbb_battery_properties),
	.get_property = smbb_battery_get_property,
	.set_property = smbb_battery_set_property,
	.property_is_writeable = smbb_battery_writable_property,
};

static const struct power_supply_desc usb_psy_desc = {
	.name = "smbb-usbin",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = smbb_charger_properties,
	.num_properties = ARRAY_SIZE(smbb_charger_properties),
	.get_property = smbb_usbin_get_property,
	.set_property = smbb_usbin_set_property,
	.property_is_writeable = smbb_charger_writable_property,
};

static const struct power_supply_desc dc_psy_desc = {
	.name = "smbb-dcin",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = smbb_charger_properties,
	.num_properties = ARRAY_SIZE(smbb_charger_properties),
	.get_property = smbb_dcin_get_property,
	.set_property = smbb_dcin_set_property,
	.property_is_writeable = smbb_charger_writable_property,
};

static int smbb_chg_otg_enable(struct regulator_dev *rdev)
{
	struct smbb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	dev_info(chg->dev, "OTG boost on: hosting VBUS from VPH\n");
	rc = regmap_update_bits(chg->regmap, chg->addr + SMBB_USB_OTG_CTL,
				OTG_CTL_EN, OTG_CTL_EN);
	if (rc)
		dev_err(chg->dev, "failed to update OTG_CTL\n");
	return rc;
}

static int smbb_chg_otg_disable(struct regulator_dev *rdev)
{
	struct smbb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	rc = regmap_update_bits(chg->regmap, chg->addr + SMBB_USB_OTG_CTL,
				OTG_CTL_EN, 0);
	if (rc)
		dev_err(chg->dev, "failed to update OTG_CTL\n");
	return rc;
}

static int smbb_chg_otg_is_enabled(struct regulator_dev *rdev)
{
	struct smbb_charger *chg = rdev_get_drvdata(rdev);
	unsigned int value = 0;
	int rc;

	rc = regmap_read(chg->regmap, chg->addr + SMBB_USB_OTG_CTL, &value);
	if (rc)
		dev_err(chg->dev, "failed to read OTG_CTL\n");

	return !!(value & OTG_CTL_EN);
}

static const struct regulator_ops smbb_chg_otg_ops = {
	.enable = smbb_chg_otg_enable,
	.disable = smbb_chg_otg_disable,
	.is_enabled = smbb_chg_otg_is_enabled,
};

static int smbb_charger_probe(struct platform_device *pdev)
{
	struct power_supply_config bat_cfg = {};
	struct power_supply_config usb_cfg = {};
	struct power_supply_config dc_cfg = {};
	struct smbb_charger *chg;
	struct regulator_config config = { };
	int rc, i;

	chg = devm_kzalloc(&pdev->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;

	chg->dev = &pdev->dev;
	mutex_init(&chg->statlock);
	INIT_DELAYED_WORK(&chg->engage_work, smbb_engage_work);

	chg->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!chg->regmap) {
		dev_err(&pdev->dev, "failed to locate regmap\n");
		return -ENODEV;
	}

	rc = of_property_read_u32(pdev->dev.of_node, "reg", &chg->addr);
	if (rc) {
		dev_err(&pdev->dev, "missing or invalid 'reg' property\n");
		return rc;
	}

	rc = regmap_read(chg->regmap, chg->addr + SMBB_MISC_REV2, &chg->revision);
	if (rc) {
		dev_err(&pdev->dev, "unable to read revision\n");
		return rc;
	}

	chg->revision += 1;
	if (chg->revision != 1 && chg->revision != 2 && chg->revision != 3) {
		dev_err(&pdev->dev, "v%d hardware not supported\n", chg->revision);
		return -ENODEV;
	}
	dev_info(&pdev->dev, "Initializing SMBB rev %u", chg->revision);

	chg->dc_disabled = of_property_read_bool(pdev->dev.of_node, "qcom,disable-dc");

	for (i = 0; i < _ATTR_CNT; ++i) {
		rc = smbb_charger_attr_parse(chg, i);
		if (rc) {
			dev_err(&pdev->dev, "failed to parse/apply settings\n");
			return rc;
		}
	}

	bat_cfg.drv_data = chg;
	bat_cfg.of_node = pdev->dev.of_node;
	chg->bat_psy = devm_power_supply_register(&pdev->dev,
						  &bat_psy_desc,
						  &bat_cfg);
	if (IS_ERR(chg->bat_psy)) {
		dev_err(&pdev->dev, "failed to register battery\n");
		return PTR_ERR(chg->bat_psy);
	}

	usb_cfg.drv_data = chg;
	usb_cfg.supplied_to = smbb_bif;
	usb_cfg.num_supplicants = ARRAY_SIZE(smbb_bif);
	chg->usb_psy = devm_power_supply_register(&pdev->dev,
						  &usb_psy_desc,
						  &usb_cfg);
	if (IS_ERR(chg->usb_psy)) {
		dev_err(&pdev->dev, "failed to register USB power supply\n");
		return PTR_ERR(chg->usb_psy);
	}

	chg->edev = devm_extcon_dev_allocate(&pdev->dev, smbb_usb_extcon_cable);
	if (IS_ERR(chg->edev)) {
		dev_err(&pdev->dev, "failed to allocate extcon device\n");
		return -ENOMEM;
	}

	rc = devm_extcon_dev_register(&pdev->dev, chg->edev);
	if (rc < 0) {
		dev_err(&pdev->dev, "failed to register extcon device\n");
		return rc;
	}

	if (!chg->dc_disabled) {
		dc_cfg.drv_data = chg;
		dc_cfg.supplied_to = smbb_bif;
		dc_cfg.num_supplicants = ARRAY_SIZE(smbb_bif);
		chg->dc_psy = devm_power_supply_register(&pdev->dev,
							 &dc_psy_desc,
							 &dc_cfg);
		if (IS_ERR(chg->dc_psy)) {
			dev_err(&pdev->dev, "failed to register DC power supply\n");
			return PTR_ERR(chg->dc_psy);
		}
	}

	/*
	 * Input current limit, programmed BEFORE the interrupts are hooked up.
	 *
	 * Two reasons, both measured on the carrier rig 2026-08-04.  First, the
	 * charger IRQ handlers kick the engagement supervisor, so registering
	 * them earlier let it run on zeroed indices - it reported an "input
	 * collapse at 100000 uA" that was simply iusb_idx == 0 naming the first
	 * table entry.  Second, and worse on a board with no cell: until this
	 * runs the input limit is whatever the boot chain left, and everything
	 * probe is about to start - the remoteprocs, the USB gadget - draws
	 * against that.  The board reset with UVLO at ~7.2 s, right after
	 * g_ether enumerated, with this programming still a few milliseconds
	 * away.
	 *
	 * chg->attr[] holds the configured limit, which becomes the ceiling
	 * rather than the value programmed at probe.
	 */
	chg->iusb_max_idx = smbb_hw_lookup(chg->attr[ATTR_USBIN_IMAX],
					   smbb_imax_fn);
	chg->iusb_floor_idx = min(smbb_hw_lookup(SMBB_IUSB_FLOOR_UA,
						 smbb_imax_fn),
				  chg->iusb_max_idx);
	chg->iusb_learn_idx = chg->iusb_max_idx;
	chg->iusb_idx = chg->iusb_max_idx + 1;	/* force the first write */
	chg->rearm_delay_ms = SMBB_ENGAGE_POLL_MS;
	chg->last_ramp = jiffies;

	/*
	 * ... unless there is no battery, in which case the input is not
	 * topping up a cell, it is powering the SoC.  Ramping from 500 mA at
	 * one step per 10 s supervisor pass then starves the board through the
	 * whole of driver probing and userspace bring-up: measured on the
	 * carrier rig 2026-08-04, the board reset at ~7 s with the ramp still
	 * near the floor, where mainline - which programs the configured limit
	 * outright - boots the same fixture.  Ask for the ceiling immediately
	 * and let the collapse detection in the supervisor learn a lower one if
	 * this supply cannot hold it.
	 */
	if (!smbb_battery_present(chg)) {
		unsigned int start = min(smbb_hw_lookup(SMBB_IUSB_NOBATT_UA,
							smbb_imax_fn),
					 chg->iusb_max_idx);

		dev_info(&pdev->dev,
			 "no battery at probe: input powers the system, starting at %u uA (ceiling %u uA)\n",
			 smbb_imax_fn(start), smbb_imax_fn(chg->iusb_max_idx));
		smbb_set_iusb(chg, start);
		dev_info(&pdev->dev, "input limit register now %u uA\n",
			 smbb_iusb_hw_ua(chg));
	} else {
		smbb_set_iusb(chg, chg->iusb_floor_idx);
	}


	for (i = 0; i < ARRAY_SIZE(smbb_charger_irqs); ++i) {
		int irq;

		irq = platform_get_irq_byname(pdev, smbb_charger_irqs[i].name);
		if (irq < 0)
			return irq;

		smbb_charger_irqs[i].handler(irq, chg);

		rc = devm_request_threaded_irq(&pdev->dev, irq, NULL,
				smbb_charger_irqs[i].handler, IRQF_ONESHOT,
				smbb_charger_irqs[i].name, chg);
		if (rc) {
			dev_err(&pdev->dev, "failed to request irq '%s'\n",
				smbb_charger_irqs[i].name);
			return rc;
		}
	}

	/*
	 * otg regulator is used to control VBUS voltage direction
	 * when USB switches between host and gadget mode
	 */
	chg->otg_rdesc.id = -1;
	chg->otg_rdesc.name = "otg-vbus";
	chg->otg_rdesc.ops = &smbb_chg_otg_ops;
	chg->otg_rdesc.owner = THIS_MODULE;
	chg->otg_rdesc.type = REGULATOR_VOLTAGE;
	chg->otg_rdesc.supply_name = "usb-otg-in";
	chg->otg_rdesc.of_match = "otg-vbus";

	config.dev = &pdev->dev;
	config.driver_data = chg;

	chg->otg_reg = devm_regulator_register(&pdev->dev, &chg->otg_rdesc,
					       &config);
	if (IS_ERR(chg->otg_reg))
		return PTR_ERR(chg->otg_reg);

	chg->jeita_ext_temp = of_property_read_bool(pdev->dev.of_node,
			"qcom,jeita-extended-temp-range");

	chg->charging_disabled = of_property_read_bool(pdev->dev.of_node,
			"qcom,charging-disabled");
	if (chg->charging_disabled)
		dev_info(&pdev->dev,
			 "charging disabled by device tree; keeping the charge path off\n");

	/*
	 * The BTC temperature-range select (and the comparator enable,
	 * formerly set unconditionally from smbb_charger_setup[]) is left
	 * exactly as the boot chain configured it, matching the vendor
	 * driver on this board.  See the BPD/VREF entries in the setup
	 * table for why arming comparators here is unsafe.
	 */

	/*
	 * Optional: a battery-voltage channel lets the supervisor recognise a
	 * full battery instead of retrying against the recharge comparator.
	 */
	chg->vbat_chan = devm_iio_channel_get(&pdev->dev, "vbat");
	if (IS_ERR(chg->vbat_chan)) {
		if (PTR_ERR(chg->vbat_chan) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_dbg(&pdev->dev, "no vbat channel: %pe\n", chg->vbat_chan);
		chg->vbat_chan = NULL;
	}

	/*
	 * Clear the sticky gates before the setup table enables charging:
	 * whatever the boot chain (or a previous kernel) left behind must not
	 * outlive our probe.
	 */
	smbb_clear_gates(chg);

	for (i = 0; i < ARRAY_SIZE(smbb_charger_setup); ++i) {
		const struct reg_off_mask_default *r = &smbb_charger_setup[i];

		if (r->rev_mask & BIT(chg->revision))
			continue;

		if (r->force) {
			rc = regmap_write(chg->regmap, chg->addr + r->offset,
					  r->value);
			if (rc) {
				dev_err(&pdev->dev,
					"unable to initializing charging, bailing\n");
				return rc;
			}
			continue;
		}

		rc = regmap_update_bits(chg->regmap, chg->addr + r->offset,
				r->mask, r->value);
		if (rc) {
			dev_err(&pdev->dev,
				"unable to initializing charging, bailing\n");
			return rc;
		}
	}

	platform_set_drvdata(pdev, chg);

	smbb_debugfs_init(chg);
	/* First supervisor pass right away: a cable may already be present. */
	schedule_delayed_work(&chg->engage_work, msecs_to_jiffies(500));

	return 0;
}

static void smbb_charger_remove(struct platform_device *pdev)
{
	struct smbb_charger *chg;

	chg = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&chg->engage_work);
	debugfs_remove_recursive(chg->debugfs);
	regmap_update_bits(chg->regmap, chg->addr + SMBB_CHG_CTRL, CTRL_EN, 0);
}

static const struct of_device_id smbb_charger_id_table[] = {
	{ .compatible = "qcom,pm8226-charger" },
	{ .compatible = "qcom,pm8941-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, smbb_charger_id_table);

static struct platform_driver smbb_charger_driver = {
	.probe	  = smbb_charger_probe,
	.remove_new	 = smbb_charger_remove,
	.driver	 = {
		.name   = "qcom-smbb",
		.of_match_table = smbb_charger_id_table,
	},
};
module_platform_driver(smbb_charger_driver);

MODULE_DESCRIPTION("Qualcomm Switch-Mode Battery Charger and Boost driver");
MODULE_LICENSE("GPL v2");
