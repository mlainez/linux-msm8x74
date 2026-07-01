/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Minimal class-H supply control for the WCD9320 (Taiko) codec.
 *
 * The headphone/earpiece PAs need their supply rails (the internal buck,
 * the negative charge pump / NCP and the class-H block) brought up before
 * the DAC drives output. The full downstream class-H state machine is large;
 * this is a reduced version that just reference-counts the shared supplies
 * and turns them on for the first active path and off for the last, which is
 * enough to drive the HPH outputs.
 */
#ifndef __WCD9320_CLSH_H__
#define __WCD9320_CLSH_H__

#include <sound/soc.h>

/* Supply registers (raw addresses to avoid header ordering dependencies). */
#define WCD9320_CLSH_REG_CLSH_B1_CTL	0x320
#define WCD9320_CLSH_REG_BUCK_MODE_1	0x181
#define WCD9320_CLSH_REG_NCP_STATIC	0x194
#define WCD9320_CLSH_REG_NCP_EN		0x192
#define WCD9320_CLSH_REG_CLK_OTHR_CTL	0x30c

#define WCD_CLSH_STATE_IDLE	0x00
#define WCD_CLSH_STATE_EAR	(0x01 << 0)
#define WCD_CLSH_STATE_HPHL	(0x01 << 1)
#define WCD_CLSH_STATE_HPHR	(0x01 << 2)
#define WCD_CLSH_STATE_LO	(0x01 << 3)

enum {
	CLS_H_NORMAL = 0,	/* Class-H Default */
	CLS_H_HIFI,		/* Class-H HiFi */
	CLS_H_LP,		/* Class-H Low Power */
	CLS_AB,			/* Class-AB */
	CLS_H_LOHIFI,		/* LoHIFI */
	CLS_NONE,		/* None of the above modes */
};

enum {
	CLSH_REQ_DISABLE,
	CLSH_REQ_ENABLE,
};

enum {
	WCD_CLSH_EVENT_PRE_DAC,
	WCD_CLSH_EVENT_POST_PA,
};

struct wcd_clsh_cdc_data {
	u8 state;	/* reference count of active class-H users */
};

static inline void wcd_clsh_supplies(struct snd_soc_component *component,
				     bool enable)
{
	if (enable) {
		/* class-H block */
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_CLSH_B1_CTL, 0x01, 0x01);
		/* internal buck */
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_BUCK_MODE_1, 0x80, 0x80);
		/* NCP: fclk level 8, enable static, then enable NCP */
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_NCP_STATIC, 0x10, 0x00);
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_NCP_STATIC, 0x0f, 0x08);
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_NCP_STATIC, 0x20, 0x20);
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_NCP_EN, 0x01, 0x01);
		usleep_range(1000, 1200);
		/* charge pump */
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_CLK_OTHR_CTL, 0x01, 0x01);
	} else {
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_CLK_OTHR_CTL, 0x01, 0x00);
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_NCP_EN, 0x01, 0x00);
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_BUCK_MODE_1, 0x80, 0x00);
		snd_soc_component_update_bits(component,
			WCD9320_CLSH_REG_CLSH_B1_CTL, 0x01, 0x00);
	}
}

static inline void wcd_clsh_fsm(struct snd_soc_component *component,
				struct wcd_clsh_cdc_data *cdc_clsh_d,
				u8 req, u8 clsh_state, u8 clsh_event)
{
	/* Bring the shared supplies up before the DAC, down after the PA. */
	if (req == CLSH_REQ_ENABLE && clsh_event == WCD_CLSH_EVENT_PRE_DAC) {
		if (cdc_clsh_d->state++ == 0)
			wcd_clsh_supplies(component, true);
	} else if (req == CLSH_REQ_DISABLE &&
		   clsh_event == WCD_CLSH_EVENT_POST_PA) {
		if (cdc_clsh_d->state && --cdc_clsh_d->state == 0)
			wcd_clsh_supplies(component, false);
	}
}

static inline void wcd_clsh_init(struct wcd_clsh_cdc_data *clsh)
{
	clsh->state = 0;
}

#endif /* __WCD9320_CLSH_H__ */
