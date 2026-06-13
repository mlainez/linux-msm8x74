/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Class-H control for the WCD9320 (Taiko) codec.
 *
 * TODO: Taiko class-H is not yet implemented. The shared wcd-clsh-v2
 * helper targets the WCD9335+ register layout and cannot drive Taiko, so
 * for now these are no-ops. This only affects the headphone/earpiece
 * power path; the loudspeaker (SPKDRV) and line-out paths do not use it.
 */
#ifndef __WCD9320_CLSH_H__
#define __WCD9320_CLSH_H__

#include <sound/soc.h>

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
	u8 state;
};

static inline void wcd_clsh_fsm(struct snd_soc_component *component,
				struct wcd_clsh_cdc_data *cdc_clsh_d,
				u8 req_state, u8 clsh_state, u8 clsh_event)
{
}

static inline void wcd_clsh_init(struct wcd_clsh_cdc_data *clsh)
{
}

#endif /* __WCD9320_CLSH_H__ */
