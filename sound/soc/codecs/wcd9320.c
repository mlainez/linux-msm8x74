// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define DEBUG 1
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/wait.h>
#include <linux/bitops.h>
#include <linux/regulator/consumer.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/of_gpio.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <sound/info.h>

#include "wcd9320-registers.h"
#include "wcd9320-clsh.h"
#include "wcd9320.h"
#include "wcd-slim.h"

#define WCD9XXX_SLIM_NUM_PORT_REG 3
#define WCD9320_RX_PORT_START_NUMBER  16
#define WCD9320_HPH_PA_SETTLE_COMP_ON 3000
#define WCD9320_HPH_PA_SETTLE_COMP_OFF 13000
#define NUM_INTERPOLATORS 7

enum {
	RX_MIX1_INP_SEL_ZERO = 0,
	RX_MIX1_INP_SEL_SRC1,
	RX_MIX1_INP_SEL_SRC2,
	RX_MIX1_INP_SEL_IIR1,
	RX_MIX1_INP_SEL_IIR2,
	RX_MIX1_INP_SEL_RX1,
	RX_MIX1_INP_SEL_RX2,
	RX_MIX1_INP_SEL_RX3,
	RX_MIX1_INP_SEL_RX4,
	RX_MIX1_INP_SEL_RX5,
	RX_MIX1_INP_SEL_RX6,
	RX_MIX1_INP_SEL_RX7,
	RX_MIX1_INP_SEL_AUXRX,
};

enum {
	COMPANDER_0,
	COMPANDER_1,
	COMPANDER_2,
	COMPANDER_MAX,
};

static unsigned short rx_digital_gain_reg[] = {
	WCD9320_A_CDC_RX1_VOL_CTL_B2_CTL,
	WCD9320_A_CDC_RX2_VOL_CTL_B2_CTL,
	WCD9320_A_CDC_RX3_VOL_CTL_B2_CTL,
	WCD9320_A_CDC_RX4_VOL_CTL_B2_CTL,
	WCD9320_A_CDC_RX5_VOL_CTL_B2_CTL,
	WCD9320_A_CDC_RX6_VOL_CTL_B2_CTL,
	WCD9320_A_CDC_RX7_VOL_CTL_B2_CTL,
};

/* Number of input and output Slimbus port */
enum {
	WCD9320_RX0 = 0,
	WCD9320_RX1,
	WCD9320_RX2,
	WCD9320_RX3,
	WCD9320_RX4,
	WCD9320_RX5,
	WCD9320_RX6,
	WCD9320_RX7,
	WCD9320_RX8,
	WCD9320_RX9,
	WCD9320_RX10,
	WCD9320_RX11,
	WCD9320_RX12,
	WCD9320_RX_MAX,
};

enum {
	COMPANDER_FS_8KHZ = 0,
	COMPANDER_FS_16KHZ,
	COMPANDER_FS_32KHZ,
	COMPANDER_FS_48KHZ,
	COMPANDER_FS_96KHZ,
	COMPANDER_FS_192KHZ,
	COMPANDER_FS_MAX,
};

/*
 * Rx path gain offsets
 */
enum {
	RX_GAIN_OFFSET_M1P5_DB,
	RX_GAIN_OFFSET_0_DB,
};

struct wcd9320_reg_mask_val {
	u16 reg;
	u8 mask;
	u8 val;
};

struct comp_sample_dependent_params {
	u8 peak_det_timeout;
	u8 rms_meter_div_fact;
	u8 rms_meter_resamp_fact;
};

static const struct comp_sample_dependent_params comp_samp_params[] = {
	{
		/* 8 Khz */
		.peak_det_timeout = 0x06,
		.rms_meter_div_fact = 0x09,
		.rms_meter_resamp_fact = 0x06,
	},
	{
		/* 16 Khz */
		.peak_det_timeout = 0x07,
		.rms_meter_div_fact = 0x0A,
		.rms_meter_resamp_fact = 0x0C,
	},
	{
		/* 32 Khz */
		.peak_det_timeout = 0x08,
		.rms_meter_div_fact = 0x0B,
		.rms_meter_resamp_fact = 0x1E,
	},
	{
		/* 48 Khz */
		.peak_det_timeout = 0x09,
		.rms_meter_div_fact = 0x0B,
		.rms_meter_resamp_fact = 0x28,
	},
	{
		/* 96 Khz */
		.peak_det_timeout = 0x0A,
		.rms_meter_div_fact = 0x0C,
		.rms_meter_resamp_fact = 0x50,
	},
	{
		/* 192 Khz */
		.peak_det_timeout = 0x0B,
		.rms_meter_div_fact = 0xC,
		.rms_meter_resamp_fact = 0x50,
	},
};

#define WCD9320_RATES (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |\
			SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000 |\
			SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)

#define WCD9320_MIX_RATES_MASK (SNDRV_PCM_RATE_48000 |\
				SNDRV_PCM_RATE_96000 | SNDRV_PCM_RATE_192000)

#define WCD9320_FORMATS_S16_S24_LE (SNDRV_PCM_FMTBIT_S16_LE | \
				   SNDRV_PCM_FMTBIT_S24_LE)

#define wcd9320_FORMATS (SNDRV_PCM_FMTBIT_S16_LE)

/*
 * Timeout in milli seconds and it is the wait time for
 * slim channel removal interrupt to receive.
 */
#define WCD9320_SLIM_CLOSE_TIMEOUT 1000
#define WCD9320_SLIM_IRQ_OVERFLOW (1 << 0)
#define WCD9320_SLIM_IRQ_UNDERFLOW (1 << 1)
#define WCD9320_SLIM_IRQ_PORT_CLOSED (1 << 2)
#define wcd9320_MCLK_CLK_12P288MHZ 12288000
#define wcd9320_MCLK_CLK_9P6MHZ 9600000

#define WCD9320_SLIM_NUM_PORT_REG 3
#define BYTE_BIT_MASK(nr) (1 << ((nr) % BITS_PER_BYTE))

#define SLIM_BW_CLK_GEAR_9 6200000
#define SLIM_BW_UNVOTE 0

#define wcd9320_DIG_CORE_REG_MIN  WCD9320_CDC_ANC0_CLK_RESET_CTL
#define wcd9320_DIG_CORE_REG_MAX  0xDFF

#define CALCULATE_VOUT_D(req_mv) (((req_mv - 650) * 10) / 25)

static int wcd9320_codec_enable_aux_pga(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event);

static const DECLARE_TLV_DB_SCALE(line_gain, 0, 7, 1);

enum {
	VI_SENSE_1,
	VI_SENSE_2,
	AIF4_SWITCH_VALUE,
	AUDIO_NOMINAL,
	CPE_NOMINAL,
	HPH_PA_DELAY,
	SB_CLK_GEAR,
	ANC_MIC_AMIC1,
	ANC_MIC_AMIC2,
	ANC_MIC_AMIC3,
	ANC_MIC_AMIC4,
	ANC_MIC_AMIC5,
	ANC_MIC_AMIC6,
};

enum {
	INTn_1_MIX_INP_SEL_ZERO = 0,
	INTn_1_MIX_INP_SEL_DEC0,
	INTn_1_MIX_INP_SEL_DEC1,
	INTn_1_MIX_INP_SEL_IIR0,
	INTn_1_MIX_INP_SEL_IIR1,
	INTn_1_MIX_INP_SEL_RX0,
	INTn_1_MIX_INP_SEL_RX1,
	INTn_1_MIX_INP_SEL_RX2,
	INTn_1_MIX_INP_SEL_RX3,
	INTn_1_MIX_INP_SEL_RX4,
	INTn_1_MIX_INP_SEL_RX5,
	INTn_1_MIX_INP_SEL_RX6,
	INTn_1_MIX_INP_SEL_RX7,

};

#define IS_VALID_NATIVE_FIFO_PORT(inp) \
	((inp >= INTn_1_MIX_INP_SEL_RX0) && \
	 (inp <= INTn_1_MIX_INP_SEL_RX3))

enum {
	INTn_2_INP_SEL_ZERO = 0,
	INTn_2_INP_SEL_RX0,
	INTn_2_INP_SEL_RX1,
	INTn_2_INP_SEL_RX2,
	INTn_2_INP_SEL_RX3,
	INTn_2_INP_SEL_RX4,
	INTn_2_INP_SEL_RX5,
	INTn_2_INP_SEL_RX6,
	INTn_2_INP_SEL_RX7,
	INTn_2_INP_SEL_PROXIMITY,
};

enum {
	INTERP_EAR = 0,
	INTERP_HPHL,
	INTERP_HPHR,
	INTERP_LO1,
	INTERP_LO2,
	INTERP_LO3,
	INTERP_LO4,
	INTERP_SPKR1,
	INTERP_SPKR2,
};

struct interp_sample_rate {
	int sample_rate;
	int rate_val;
};

static struct interp_sample_rate compander_sample_rate_val[] = {
	{8000, COMPANDER_FS_8KHZ},	/* 8K */
	{16000, COMPANDER_FS_16KHZ},	/* 16K */
	{32000, COMPANDER_FS_32KHZ},	/* 32K */
	{48000, COMPANDER_FS_48KHZ},	/* 48K */
	{96000, COMPANDER_FS_96KHZ},	/* 96K */
	{192000, COMPANDER_FS_192KHZ},	/* 192K */
};

/*
 * Diagnostic-only digital loopback TX channel: SLIM TX3 (0-indexed port 2),
 * wired in DAPM to tap RX7 MIX1's output directly (no decimator), so a
 * capture stream lets us check on real hardware whether data is actually
 * flowing through the RX path that the loudspeaker/headphone overflow
 * investigation could not otherwise observe.
 *
 * Sized to SLIM_MAX_TX_PORTS (16, see sound/soc/qcom/msm8974.c): the machine
 * driver's set_channel_map() always passes tx_num=16, and
 * mywcd_slim_init_slimslave() writes wcd->tx_chs[0..tx_num-1] unconditionally
 * -- a smaller allocation here would be a heap buffer overflow. Only index 2
 * (port 2 / "TX3") is ever added to an AIF's wcd_slim_ch_list, so the other
 * entries are inert placeholders.
 */
static const struct wcd_slim_ch wcd9320_tx_chs[16] = {
	WCD_SLIM_CH(0, 0),
	WCD_SLIM_CH(1, 0),
	WCD_SLIM_CH(2, 0),
	WCD_SLIM_CH(3, 0),
	WCD_SLIM_CH(4, 0),
	WCD_SLIM_CH(5, 0),
	WCD_SLIM_CH(6, 0),
	WCD_SLIM_CH(7, 0),
	WCD_SLIM_CH(8, 0),
	WCD_SLIM_CH(9, 0),
	WCD_SLIM_CH(10, 0),
	WCD_SLIM_CH(11, 0),
	WCD_SLIM_CH(12, 0),
	WCD_SLIM_CH(13, 0),
	WCD_SLIM_CH(14, 0),
	WCD_SLIM_CH(15, 0),
};

static const struct wcd_slim_ch wcd9320_rx_chs[WCD9320_RX_MAX] = {
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER, 0),	 /* 16 */
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 1, 1),	 /* 17 */
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 2, 2),   /* 18 */
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 3, 3),   /* 19 */
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 4, 4),   /* 20 */
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 5, 5),   /* 21 */
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 6, 6),
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 7, 7),
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 8, 8),
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 9, 9),
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 10, 10),
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 11, 11),
	WCD_SLIM_CH(WCD9320_RX_PORT_START_NUMBER + 12, 12),
};

static const u32 comp_shift[] = {
	4, /* Compander 0's clock source is on interpolator 7 */
	0,
	2,
};

enum {
	SRC_IN_HPHL,
	SRC_IN_LO1,
	SRC_IN_HPHR,
	SRC_IN_LO2,
	SRC_IN_SPKRL,
	SRC_IN_LO3,
	SRC_IN_SPKRR,
	SRC_IN_LO4,
};

static const DECLARE_TLV_DB_SCALE(digital_gain, 0, 1, 0);
static const DECLARE_TLV_DB_SCALE(analog_gain, 0, 25, 1);

static int wcd9320_config_compander(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event);
static int wcd9320_codec_vote_max_bw(struct snd_soc_component *comp,
				   bool vote);

static int wcd9320_enable_master_bias(struct wcd9320_priv *wcd)
{

	mutex_lock(&wcd->master_bias_lock);

	wcd->master_bias_users++;
	if (wcd->master_bias_users == 1) {
		snd_soc_component_update_bits(wcd->component, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x80, 0x80);
		snd_soc_component_update_bits(wcd->component, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x04, 0x04);
		snd_soc_component_update_bits(wcd->component, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x01, 0x01);
		usleep_range(1000, 1250);
		snd_soc_component_update_bits(wcd->component, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x80, 0x00);
	}

	mutex_unlock(&wcd->master_bias_lock);
	return 0;
}

static int wcd9320_enable_rco(struct wcd9320_priv *wcd)
{
	/* Enable rco requires master bias to be enabled first */
	if (wcd->master_bias_users <= 0) {
		dev_err(wcd->dev, "Cannot turn on RCO, BG is not enabled\n");
		return -EINVAL;
	}

	if (wcd->clk_type == WCD_CLK_MCLK) {
		dev_err(wcd->dev, "Cannot turn on RCO, MCLK is not disbled\n");
		return -EINVAL;
	}

	if (((wcd->clk_rco_users == 0) &&
	     (wcd->clk_type == WCD_CLK_RCO)) ||
	    ((wcd->clk_rco_users > 0) &&
	    (wcd->clk_type != WCD_CLK_RCO))) {
		pr_err("%s: Error enabling RCO, clk_type: %d\n",
			__func__,
			wcd->clk_type);
		return -EINVAL;
	}

	/* config_mode 1 */
	if (++wcd->clk_rco_users == 1) {
		regmap_update_bits(wcd->regmap, WCD9XXX_A_RC_OSC_FREQ, 0x10, 0);
		/* bandgap mode to fast */
		regmap_write(wcd->regmap, WCD9XXX_A_BIAS_OSC_BG_CTL, 0x17);
		usleep_range(5, 55);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_RC_OSC_FREQ, 0x80, 0x80);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_RC_OSC_TEST, 0x80, 0x80);
		usleep_range(10, 60);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_RC_OSC_TEST, 0x80, 0);
		usleep_range(10000, 10250);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN1, 0x08, 0x08);
		regmap_write(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x02);
		usleep_range(1000, 1250);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN1, 0x01, 0x01);
		usleep_range(1000, 1200);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x02, 0x00);
		/* on MCLK */
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x04, 0x04);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CDC_CLK_MCLK_CTL, 0x01, 0x01);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_TX_COM_BIAS, 1 << 4, 1 << 4);

	}

	wcd->clk_type = WCD_CLK_RCO;

	return 0;
}

static int wcd9320_disable_rco(struct wcd9320_priv *wcd)
{
	if (wcd->clk_rco_users <= 0) {
		dev_err(wcd->dev, "No rco users, cannot disable rco\n");
		return -EINVAL;
	}

	if (--wcd->clk_rco_users == 0) {
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x04, 0x00);
		usleep_range(50, 100);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x02, 0x02);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN1, 0x05, 0x00);
		usleep_range(50, 100);

		wcd->clk_type = WCD_CLK_OFF;
	}

	return 0;
}

static int wcd9320_enable_mclk(struct wcd9320_priv *wcd)
{
	/* Enable mclk requires master bias to be enabled first */
	if (wcd->master_bias_users <= 0) {
		dev_err(wcd->dev, "Cannot turn on MCLK, BG is not enabled\n");
		return -EINVAL;
	}

	if (((wcd->clk_mclk_users == 0) &&
	     (wcd->clk_type == WCD_CLK_MCLK)) ||
	    ((wcd->clk_mclk_users > 0) &&
	    (wcd->clk_type != WCD_CLK_MCLK))) {
		pr_err("%s: Error enabling MCLK, clk_type: %d\n",
			__func__,
			wcd->clk_type);
		return -EINVAL;
	}

	if (wcd->clk_mclk_users == 0 && wcd->clk_type == WCD_CLK_RCO)
		wcd9320_disable_rco(wcd);

	if (++wcd->clk_mclk_users == 1) {
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN1,
				   0x08, 0x00);

		if (wcd->clk_type == WCD_CLK_RCO) {
			regmap_write(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x02);
			regmap_update_bits(wcd->regmap, WCD9XXX_A_BIAS_OSC_BG_CTL, 0x1, 0);
			regmap_update_bits(wcd->regmap, WCD9XXX_A_RC_OSC_FREQ, 0x80, 0);
		}

		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN1,
				   0x0c, 0x04);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN1,
				   0x01, 0x01);
		usleep_range(1000, 1200);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x02, 0x00);
		/* on MCLK */
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2, 0x04, 0x04);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CDC_CLK_MCLK_CTL,
				   0x01, 0x01);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_TX_COM_BIAS, 1 << 4,
				   1 << 4);

		usleep_range(50, 100);
	}

	/*
	 * Select the 9.6MHz MCLK mode (CHIP_CTL bits[2:1] = 0x2; 0x0 would be
	 * 12.288MHz). Our probe-time defaults-table write of this register
	 * does not stick because it happens with the core clock off; assert
	 * it here, after the MCLK buffers above are running, so it latches
	 * (mirrors downstream, which sets these bits with the clock on,
	 * gated on mclk_rate == 9.6MHz). Written unconditionally, not just
	 * on the users==1 transition, to survive any clock-type transitions.
	 * (CDC_CLK_POWER_CTL (0x314) is a read-only clock-domain power
	 * STATUS register -- writes are ignored; it reads 0x03 once MCLK is
	 * genuinely running, and was the key diagnostic for the dead-MCLK
	 * pinctrl bug fixed in the board DT.)
	 */
	regmap_update_bits(wcd->regmap, WCD9320_A_CHIP_CTL, 0x06, 0x02);

	wcd->clk_type = WCD_CLK_MCLK;

	return 0;
}

static int wcd9320_disable_mclk(struct wcd9320_priv *wcd)
{
	if (wcd->clk_mclk_users <= 0) {
		dev_err(wcd->dev, "No mclk users, cannot disable mclk\n");
		return -EINVAL;
	}

	if (--wcd->clk_mclk_users == 0) {
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2,
				   0x04, 0x00);

		usleep_range(50, 100);

		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN2,
				   0x02, 0x02);
		regmap_update_bits(wcd->regmap, WCD9XXX_A_CLK_BUFF_EN1,
				   0x05, 0x00);

		usleep_range(50, 100);

		regmap_update_bits(wcd->regmap, WCD9XXX_A_TX_COM_BIAS, 1 << 4,
				   1 << 4);

		wcd->clk_type = WCD_CLK_OFF;
	}

	return 0;
}

static int wcd9320_disable_master_bias(struct wcd9320_priv *wcd)
{
	mutex_lock(&wcd->master_bias_lock);
	if (wcd->master_bias_users <= 0) {
		mutex_unlock(&wcd->master_bias_lock);
		return -EINVAL;
	}

	wcd->master_bias_users--;
	if (wcd->master_bias_users == 0) {
		snd_soc_component_update_bits(wcd->component, WCD9XXX_A_BIAS_CENTRAL_BG_CTL,
				    0x03, 0x00);
		usleep_range(100, 150);
	}
	mutex_unlock(&wcd->master_bias_lock);
	return 0;
}

static int wcd9320_cdc_req_rco_enable(struct wcd9320_priv *wcd,
				     bool enable)
{
	int ret = 0;

	if (enable) {
		ret = clk_prepare_enable(wcd->codec_clk);
		if (ret) {
			dev_err(wcd->dev, "%s: ext clk enable failed\n",
				__func__);
			goto err;
		}
		/* get BG */
		wcd9320_enable_master_bias(wcd);
		/* get MCLK */
		wcd9320_enable_rco(wcd);

	} else {
		/* put MCLK */
		wcd9320_disable_rco(wcd);
		/* put BG */
		wcd9320_disable_master_bias(wcd);
		clk_disable_unprepare(wcd->codec_clk);
	}
err:
	return ret;
}

static int wcd9320_cdc_req_mclk_enable(struct wcd9320_priv *wcd,
				     bool enable)
{
	int ret = 0;

	if (enable) {
		ret = clk_prepare_enable(wcd->codec_clk);
		if (ret) {
			dev_err(wcd->dev, "%s: ext clk enable failed\n",
				__func__);
			goto err;
		}
		/* get BG */
		wcd9320_enable_master_bias(wcd);
		/* get MCLK */
		wcd9320_enable_mclk(wcd);

	} else {
		/* put MCLK */
		wcd9320_disable_mclk(wcd);
		/* put BG */
		wcd9320_disable_master_bias(wcd);
		clk_disable_unprepare(wcd->codec_clk);
	}
err:
	return ret;
}

static int wcd9320_codec_dsm_mux_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	u8 reg_val, zoh_mux_val = 0x00;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		reg_val = snd_soc_component_read(component, WCD9320_A_CDC_CONN_CLSH_CTL);

		if ((reg_val & 0x30) == 0x10)
			zoh_mux_val = 0x04;
		else if ((reg_val & 0x30) == 0x20)
			zoh_mux_val = 0x08;

		if (zoh_mux_val != 0x00)
			snd_soc_component_update_bits(component,
					WCD9320_A_CDC_CONN_CLSH_CTL,
					0x0C, zoh_mux_val);
		break;

	case SND_SOC_DAPM_POST_PMD:
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CONN_CLSH_CTL,
							0x0C, 0x00);
		break;
	}
	return 0;
}

static int slim_rx_mux_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{

	struct snd_soc_dapm_context *dapm = snd_soc_dapm_kcontrol_dapm(kcontrol);
	struct snd_soc_component *component = snd_soc_dapm_to_component(dapm);
	struct snd_soc_dapm_widget *widget = snd_soc_dapm_kcontrol_widget(kcontrol);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);

	ucontrol->value.enumerated.item[0] = wcd->rx_port_value[widget->shift];

	return 0;
}

static const char *const slim_rx_mux_text[] = {
	"ZERO", "AIF1_PB", "AIF2_PB", "AIF3_PB", "AIF4_PB", "AIF_MIX1_PB"
};

static int wcd_slim_rx_vport_validation(u32 port_id,
				struct list_head *codec_dai_list)
{
	struct wcd_slim_ch *ch;
	int ret = 0;

	list_for_each_entry(ch,
		codec_dai_list, list) {
		if (ch->port == port_id) {
			ret = -EINVAL;
			break;
		}
	}
	return ret;
}

static int slim_rx_mux_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_dapm_widget *widget = snd_soc_dapm_kcontrol_widget(kcontrol);
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_component(kcontrol);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	struct soc_enum *e = (struct soc_enum *)kcontrol->private_value;
	struct snd_soc_dapm_update *update = NULL;
	u32 port_id = widget->shift;

	wcd->rx_port_value[port_id] = ucontrol->value.enumerated.item[0];

	/* value need to match the Virtual port and AIF number */
	switch (wcd->rx_port_value[port_id]) {
	case 0:

		list_del_init(&wcd->slim_data->rx_chs[port_id].list);
		break;
	case 1:
		if (wcd_slim_rx_vport_validation(port_id +
			WCD9320_RX_PORT_START_NUMBER,
			&wcd->dai[AIF1_PB].wcd_slim_ch_list)) {
			goto rtn;
		}
		list_add_tail(&wcd->slim_data->rx_chs[port_id].list,
			      &wcd->dai[AIF1_PB].wcd_slim_ch_list);
		break;
	case 2:
		if (wcd_slim_rx_vport_validation(port_id +
			WCD9320_RX_PORT_START_NUMBER,
			&wcd->dai[AIF2_PB].wcd_slim_ch_list)) {
			goto rtn;
		}
		list_add_tail(&wcd->slim_data->rx_chs[port_id].list,
			      &wcd->dai[AIF2_PB].wcd_slim_ch_list);
		break;
	case 3:
		if (wcd_slim_rx_vport_validation(port_id +
			WCD9320_RX_PORT_START_NUMBER,
			&wcd->dai[AIF3_PB].wcd_slim_ch_list)) {
			goto rtn;
		}
		list_add_tail(&wcd->slim_data->rx_chs[port_id].list,
			      &wcd->dai[AIF3_PB].wcd_slim_ch_list);
		break;
	case 4:
		if (wcd_slim_rx_vport_validation(port_id +
			WCD9320_RX_PORT_START_NUMBER,
			&wcd->dai[AIF4_PB].wcd_slim_ch_list)) {
			goto rtn;
		}
		list_add_tail(&wcd->slim_data->rx_chs[port_id].list,
			      &wcd->dai[AIF4_PB].wcd_slim_ch_list);
		break;
	case 5:
		if (wcd_slim_rx_vport_validation(port_id +
			WCD9320_RX_PORT_START_NUMBER,
			&wcd->dai[AIF_MIX1_PB].wcd_slim_ch_list)) {
			goto rtn;
		}
		list_add_tail(&wcd->slim_data->rx_chs[port_id].list,
			      &wcd->dai[AIF_MIX1_PB].wcd_slim_ch_list);
		break;
	default:
		dev_err(wcd->dev, "Unknown AIF %d\n", wcd->rx_port_value[port_id]);
		goto err;
	}
rtn:
	snd_soc_dapm_mux_update_power(widget->dapm, kcontrol,
					wcd->rx_port_value[port_id], e, update);

	return 0;
err:
	return -EINVAL;
}

static const struct soc_enum slim_rx_mux_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(slim_rx_mux_text), slim_rx_mux_text);

static const struct snd_kcontrol_new slim_rx_mux[WCD9320_RX_MAX] = {
	SOC_DAPM_ENUM_EXT("SLIM RX1 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX2 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX3 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX4 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX5 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX6 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
	SOC_DAPM_ENUM_EXT("SLIM RX7 Mux", slim_rx_mux_enum,
			  slim_rx_mux_get, slim_rx_mux_put),
};

static const struct snd_kcontrol_new rx_int1_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("HPHL Switch", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE("HPHL Native Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_kcontrol_new rx_int2_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("HPHR Switch", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE("HPHR Native Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_kcontrol_new rx_int3_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("LO1 Switch", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE("LO1 Native Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_kcontrol_new rx_int4_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("LO2 Switch", SND_SOC_NOPM, 0, 1, 0),
	SOC_DAPM_SINGLE("LO2 Native Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_kcontrol_new rx_int5_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("LO3 Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_kcontrol_new rx_int6_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("LO4 Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_kcontrol_new rx_int7_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("SPKRL Switch", SND_SOC_NOPM, 0, 1, 0)
};

static const struct snd_kcontrol_new rx_int8_spline_mix_switch[] = {
	SOC_DAPM_SINGLE("SPKRR Switch", SND_SOC_NOPM, 0, 1, 0)
};

static void wcd9320_codec_enable_int_port(struct wcd_slim_codec_dai_data *dai,
					struct snd_soc_component *component)
{
	struct wcd_slim_ch *ch;
	int port_num = 0;
	unsigned short reg = 0;
	unsigned int val = 0;
	struct wcd9320_priv *wcd;

	wcd = dev_get_drvdata(component->dev);
	list_for_each_entry(ch, &dai->wcd_slim_ch_list, list) {
		if (ch->port >= WCD9320_RX_PORT_START_NUMBER) {
			port_num = ch->port - WCD9320_RX_PORT_START_NUMBER;
			reg = WCD9320_SLIM_PGD_PORT_INT_EN0 + (port_num / 8);
			regmap_read(wcd->if_regmap,
				reg, &val);

			if (!(val & BYTE_BIT_MASK(port_num))) {
				val |= BYTE_BIT_MASK(port_num);
				regmap_write(wcd->if_regmap, reg, val);
				regmap_read(
					wcd->if_regmap, reg, &val);
			}
		} else {
			port_num = ch->port;
			reg = WCD9320_SLIM_PGD_PORT_INT_TX_EN0 + (port_num / 8);
			regmap_read(wcd->if_regmap,
				reg, &val);
			if (!(val & BYTE_BIT_MASK(port_num))) {
				val |= BYTE_BIT_MASK(port_num);
				regmap_write(wcd->if_regmap,
					reg, val);
				regmap_read(
					wcd->if_regmap, reg, &val);
			}
		}
	}
}

static int wcd_slim_get_slave_port(unsigned int ch_num)
{
	int ret = 0;

	ret = (ch_num - BASE_CH_NUM);
	if (ret < 0) {
		pr_err("%s: Error:- Invalid slave port found = %d\n",
			__func__, ret);
		return -EINVAL;
	}
	return ret;
}

static int wcd9320_codec_enable_slim_chmask(struct wcd_slim_codec_dai_data *dai,
					  bool up)
{
	int ret = 0;
	struct wcd_slim_ch *ch;

	if (up) {
		list_for_each_entry(ch, &dai->wcd_slim_ch_list, list) {
			ret = wcd_slim_get_slave_port(ch->ch_num);
			if (ret < 0) {
				pr_err("%s: Invalid slave port ID: %d\n",
				       __func__, ret);
				ret = -EINVAL;
			} else {
				set_bit(ret, &dai->ch_mask);
			}
		}
	} else {
		ret = wait_event_timeout(dai->dai_wait, (dai->ch_mask == 0),
					 msecs_to_jiffies(
						WCD9320_SLIM_CLOSE_TIMEOUT));
		if (!ret) {
			pr_err("%s: Slim close tx/rx wait timeout, ch_mask:0x%lx\n",
				__func__, dai->ch_mask);
			ret = -ETIMEDOUT;
		} else {
			ret = 0;
		}
	}
	return ret;
}

static int wcd_slim_stream_prepare(struct wcd9320_priv *wcd,
	struct wcd_slim_codec_dai_data *dai_data);

static int wcd9320_codec_enable_slimrx(struct snd_soc_dapm_widget *w,
				     struct snd_kcontrol *kcontrol,
				     int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int ret = 0;
	struct wcd_slim_codec_dai_data *dai;

	/* Execute the callback only if interface type is slimbus */
	if (wcd->intf_type != WCD_INTERFACE_TYPE_SLIMBUS)
		return 0;

	dai = &wcd->dai[w->shift];

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		wcd9320_codec_enable_int_port(dai, component);
		(void) wcd9320_codec_enable_slim_chmask(dai, true);
		break;
	case SND_SOC_DAPM_POST_PMD:
		/*
		 * The SLIM stream is torn down from .trigger STOP (mirroring
		 * the activate-at-trigger ordering); only release the channel
		 * mask bookkeeping here.
		 */
		wcd9320_codec_enable_slim_chmask(dai, false);
		break;
	}
	return ret;
}

static int wcd9320_get_compander(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{

	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);

	ucontrol->value.integer.value[0] = wcd->comp_enabled;
	return 0;
}

static int wcd9320_set_compander(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int value = ucontrol->value.integer.value[0];

	wcd->comp_enabled = value;

	/* TODO other comp support */
	/* Wavegen to 5 msec */
	snd_soc_component_write(component, WCD9320_A_RX_HPH_CNP_WG_CTL, (value ? 0xda:0xdb));
	snd_soc_component_write(component, WCD9320_A_RX_HPH_CNP_WG_TIME, (value ? 0x15:0x58));
	snd_soc_component_write(component, WCD9320_A_RX_HPH_BIAS_WG_OCP, (value ? 0x2a:0x1a));
	/* Enable Chopper */
	snd_soc_component_update_bits(component, WCD9320_A_RX_HPH_CHOP_CTL, 0x80, (value ? 0x80:0x00));
	snd_soc_component_write(component, WCD9320_A_NCP_DTEST, (value ? 0x20:0x10));
	return 0;
}

static int wcd9320_codec_enable_rx_bias(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (++wcd->rx_bias_count == 1) {
			snd_soc_component_update_bits(component, WCD9XXX_A_RX_COM_BIAS,
						      0x80, 0x80);
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (--wcd->rx_bias_count)
			snd_soc_component_update_bits(component, WCD9XXX_A_RX_COM_BIAS,
						      0x80, 0x00);
		break;
	};

	return 0;
}

static int wcd9320_hph_pa_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	u32 pa_settle_time = WCD9320_HPH_PA_SETTLE_COMP_OFF;

	pr_debug("%s: %s event = %d\n", __func__, w->name, event);
	if (w->shift != 4 && w->shift != 5) {
		pr_err("%s: Invalid w->shift %d\n", __func__, w->shift);
		return -EINVAL;
	}

	if (wcd->comp_enabled)
		pa_settle_time = WCD9320_HPH_PA_SETTLE_COMP_ON;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Let MBHC module know PA is turning on */
		break;

	case SND_SOC_DAPM_POST_PMU:
		usleep_range(pa_settle_time, pa_settle_time + 1000);
		pr_debug("%s: sleep %d us after %s PA enable\n", __func__,
				pa_settle_time, w->name);
		/* supplies are managed by the HPH DAC PRE_DAC/POST_PA events */

		break;

	case SND_SOC_DAPM_PRE_PMD:
		/* Let MBHC know PA is turning off */
		break;

	case SND_SOC_DAPM_POST_PMD:
		usleep_range(pa_settle_time, pa_settle_time + 1000);
		pr_debug("%s: sleep %d us after %s PA disable\n", __func__,
				pa_settle_time, w->name);

		/* Let MBHC module know PA turned off */

		/* supplies are managed by the HPH DAC PRE_DAC/POST_PA events */

		break;
	}
	return 0;
}

static int wcd9320_codec_hphr_dac_event(struct snd_soc_dapm_widget *w,
				      struct snd_kcontrol *kcontrol,
				      int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RDAC_CLK_EN_CTL,
							0x04, 0x04);
		snd_soc_component_update_bits(component, w->reg, 0x40, 0x40);

		wcd_clsh_fsm(component, &wcd->clsh_d,
			     CLSH_REQ_ENABLE,
			     WCD_CLSH_STATE_HPHR,
			     WCD_CLSH_EVENT_PRE_DAC);

		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RDAC_CLK_EN_CTL,
					      0x04, 0x00);
		snd_soc_component_update_bits(component, w->reg, 0x40, 0x00);

		wcd_clsh_fsm(component, &wcd->clsh_d,
			     CLSH_REQ_DISABLE,
			     WCD_CLSH_STATE_HPHR,
			     WCD_CLSH_EVENT_POST_PA);
		break;
	};

	return ret;
}

static int wcd9320_codec_hphl_dac_event(struct snd_soc_dapm_widget *w,
				      struct snd_kcontrol *kcontrol,
				      int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:

		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RDAC_CLK_EN_CTL,
				    0x02, 0x02);
		wcd_clsh_fsm(component, &wcd->clsh_d,
			     CLSH_REQ_ENABLE,
			     WCD_CLSH_STATE_HPHL,
			     WCD_CLSH_EVENT_PRE_DAC);

		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RDAC_CLK_EN_CTL,
				    0x02, 0x00);
		wcd_clsh_fsm(component, &wcd->clsh_d,
			     CLSH_REQ_DISABLE,
			     WCD_CLSH_STATE_HPHL,
			     WCD_CLSH_EVENT_POST_PA);
		break;
	};

	return ret;
}

static const struct snd_kcontrol_new hphl_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_L Switch", WCD9320_A_RX_PA_AUX_IN_CONN,
					7, 1, 0),
};

static const struct snd_kcontrol_new hphr_pa_mix[] = {
	SOC_DAPM_SINGLE("AUX_PGA_R Switch", WCD9320_A_RX_PA_AUX_IN_CONN,
					6, 1, 0),
};


static int wcd9320_codec_enable_interpolator(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RX_RESET_CTL,
			1 << w->shift, 1 << w->shift);
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RX_RESET_CTL,
			1 << w->shift, 0x0);
		break;
	case SND_SOC_DAPM_POST_PMU:
		/* apply the digital gain after the interpolator is enabled*/
		if ((w->shift) < ARRAY_SIZE(rx_digital_gain_reg))
			snd_soc_component_write(component,
				  rx_digital_gain_reg[w->shift],
				  snd_soc_component_read(component,
				  rx_digital_gain_reg[w->shift])
				  );
	break;
	};

	return 0;
}

static int wcd9320_codec_enable_spk_pa(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_component_update_bits(component, WCD9320_A_SPKR_DRV_EN,
			0x80, 0x80);
		break;
	case SND_SOC_DAPM_POST_PMD:
		snd_soc_component_update_bits(component, WCD9320_A_SPKR_DRV_EN,
			0x80, 0x00);
		break;
	}

	return 0;
}

static int wcd9320_spk_dac_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	return 0;
}

static int wcd9320_codec_enable_vdd_spkr(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int ret = 0;

	WARN_ONCE(!wcd->spkdrv_reg, "SPKDRV supply isn't defined\n");

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (wcd->spkdrv_reg) {
			ret = regulator_enable(wcd->spkdrv_reg);
			if (ret)
				dev_err(component->dev,
					"%s: Failed to enable spkdrv_reg\n",
					__func__);
		}
		break;
	case SND_SOC_DAPM_POST_PMD:
		if (wcd->spkdrv_reg) {
			ret = regulator_disable(wcd->spkdrv_reg);
			if (ret)
				dev_err(component->dev,
					"%s: Failed to disable spkdrv_reg\n",
					__func__);
		}
		break;
	}

	return ret;
}

static const char * const rx_cf_text[] = {
	"CF_NEG_3DB_4HZ", "CF_NEG_3DB_75HZ", "CF_NEG_3DB_150HZ",
	"CF_NEG_3DB_0P48HZ"
};

static const char * const wcd9320_ear_pa_gain_text[] = {
	"G_6_DB", "G_4P5_DB", "G_3_DB", "G_1P5_DB",
	"G_0_DB", "G_M2P5_DB", "UNDEFINED", "G_M12_DB"
};

static const struct soc_enum wcd9320_ear_pa_gain_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(wcd9320_ear_pa_gain_text),
			wcd9320_ear_pa_gain_text);

static const char * const spl_src0_mux_text[] = {
	"ZERO", "SRC_IN_HPHL", "SRC_IN_LO1",
};

static const struct soc_enum cf_dec1_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX1_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec2_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX2_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec3_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX3_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec4_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX4_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec5_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX5_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec6_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX6_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec7_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX7_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec8_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX8_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec9_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX9_MUX_CTL, 4, 3, rx_cf_text);

static const struct soc_enum cf_dec10_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_TX10_MUX_CTL, 4, 3, rx_cf_text);


static const struct soc_enum cf_rxmix1_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_RX1_B4_CTL, 0, 3, rx_cf_text);

static const struct soc_enum cf_rxmix2_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_RX2_B4_CTL, 0, 3, rx_cf_text);

static const struct soc_enum cf_rxmix3_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_RX3_B4_CTL, 0, 3, rx_cf_text);

static const struct soc_enum cf_rxmix4_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_RX4_B4_CTL, 0, 3, rx_cf_text);

static const struct soc_enum cf_rxmix5_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_RX5_B4_CTL, 0, 3, rx_cf_text);

static const struct soc_enum cf_rxmix6_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_RX6_B4_CTL, 0, 3, rx_cf_text);

static const struct soc_enum cf_rxmix7_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_RX7_B4_CTL, 0, 3, rx_cf_text);


static const struct snd_soc_dapm_route wcd9320_audio_map[] = {
	/* Headset (RX MIX1 and RX MIX2) */
	{"HEADPHONE", NULL, "HPHL"},
	{"HEADPHONE", NULL, "HPHR"},
	{"RX_BIAS", NULL, "MCLK"},


	{"HPHL_PA_MIXER", "AUX_PGA_L Switch", "AUX_PGA_Left"},
	{"HPHR_PA_MIXER", "AUX_PGA_R Switch", "AUX_PGA_Right"},

	{"HPHL DAC", NULL, "RX_BIAS"},
	{"HPHR DAC", NULL, "RX_BIAS"},

	{"HPHR", NULL, "HPHR_PA_MIXER"},
	{"HPHR_PA_MIXER", NULL, "HPHR DAC"},

	{"HPHL", NULL, "HPHL_PA_MIXER"},
	{"HPHL_PA_MIXER", NULL, "HPHL DAC"},

	{"HPHL DAC", "Switch", "CLASS_H_DSM MUX"},
	{"HPHR DAC", NULL, "RX2 CHAIN"},

	{"CLASS_H_DSM MUX", "DSM_HPHL_RX1", "RX1 CHAIN"},

	{"RX1 INTERP", NULL, "RX1 MIX2"},
	{"RX1 CHAIN", NULL, "RX1 INTERP"},
	{"RX2 INTERP", NULL, "RX2 MIX2"},
	{"RX2 CHAIN", NULL, "RX2 INTERP"},

	{"RX1 MIX2", NULL, "RX1 MIX1"},
	{"RX2 MIX2", NULL, "RX2 MIX1"},

	{"RX1 MIX1", NULL, "RX1 MIX1 INP1"},
	{"RX2 MIX1", NULL, "RX2 MIX1 INP1"},

	{"SLIM RX1 MUX", "AIF1_PB", "AIF1 PB"},
	{"SLIM RX2 MUX", "AIF1_PB", "AIF1 PB"},

	{"SLIM RX1", NULL, "SLIM RX1 MUX"},
	{"SLIM RX2", NULL, "SLIM RX2 MUX"},

	{"RX1 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX2 MIX1 INP1", "RX2", "SLIM RX2"},

	/* Speaker (RX7 / SPK) path */
	{"SPK_OUT", NULL, "SPK PA"},
	{"SPK PA", NULL, "SPK DAC"},
	{"SPK DAC", NULL, "RX7 MIX2"},
	{"SPK DAC", NULL, "VDD_SPKDRV"},
	{"SPK DAC", NULL, "RX_BIAS"},

	{"RX7 MIX2", NULL, "RX7 MIX1"},

	{"RX7 MIX1", NULL, "RX7 MIX1 INP1"},
	{"RX7 MIX1", NULL, "RX7 MIX1 INP2"},
	{"AIF1 CAP", NULL, "RX7 MIX1"},

	{"RX7 MIX1 INP1", "RX1", "SLIM RX1"},
	{"RX7 MIX1 INP1", "RX2", "SLIM RX2"},
	{"RX7 MIX1 INP1", "RX3", "SLIM RX3"},
	{"RX7 MIX1 INP1", "RX4", "SLIM RX4"},
	{"RX7 MIX1 INP1", "RX5", "SLIM RX5"},
	{"RX7 MIX1 INP1", "RX6", "SLIM RX6"},
	{"RX7 MIX1 INP1", "RX7", "SLIM RX7"},
	{"RX7 MIX1 INP2", "RX1", "SLIM RX1"},
	{"RX7 MIX1 INP2", "RX2", "SLIM RX2"},
	{"RX7 MIX1 INP2", "RX3", "SLIM RX3"},
	{"RX7 MIX1 INP2", "RX4", "SLIM RX4"},
	{"RX7 MIX1 INP2", "RX5", "SLIM RX5"},
	{"RX7 MIX1 INP2", "RX6", "SLIM RX6"},
	{"RX7 MIX1 INP2", "RX7", "SLIM RX7"},
};

static const char * const rx_hph_mode_mux_text[] = {
	"CLS_H_INVALID", "CLS_H_HIFI", "CLS_H_LP", "CLS_AB", "CLS_H_LOHIFI"
};

static const struct soc_enum rx_hph_mode_mux_enum =
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(rx_hph_mode_mux_text),
			    rx_hph_mode_mux_text);

static const struct snd_kcontrol_new wcd9320_snd_controls[] = {
	SOC_SINGLE_S8_TLV("RX1 Digital Volume", WCD9320_A_CDC_RX1_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),
	SOC_SINGLE_S8_TLV("RX2 Digital Volume", WCD9320_A_CDC_RX2_VOL_CTL_B2_CTL,
		-84, 40, digital_gain),

	SOC_ENUM("TX1 HPF cut off", cf_dec1_enum),
	SOC_ENUM("TX2 HPF cut off", cf_dec2_enum),
	SOC_ENUM("TX3 HPF cut off", cf_dec3_enum),
	SOC_ENUM("TX4 HPF cut off", cf_dec4_enum),
	SOC_ENUM("TX5 HPF cut off", cf_dec5_enum),
	SOC_ENUM("TX6 HPF cut off", cf_dec6_enum),
	SOC_ENUM("TX7 HPF cut off", cf_dec7_enum),
	SOC_ENUM("TX8 HPF cut off", cf_dec8_enum),
	SOC_ENUM("TX9 HPF cut off", cf_dec9_enum),
	SOC_ENUM("TX10 HPF cut off", cf_dec10_enum),

	SOC_SINGLE_TLV("HPHL Volume", WCD9320_A_RX_HPH_L_GAIN, 0, 20, 1,
		line_gain),
	SOC_SINGLE_TLV("HPHR Volume", WCD9320_A_RX_HPH_R_GAIN, 0, 20, 1,
		line_gain),


	SOC_SINGLE("TX1 HPF Switch", WCD9320_A_CDC_TX1_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX2 HPF Switch", WCD9320_A_CDC_TX2_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX3 HPF Switch", WCD9320_A_CDC_TX3_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX4 HPF Switch", WCD9320_A_CDC_TX4_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX5 HPF Switch", WCD9320_A_CDC_TX5_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX6 HPF Switch", WCD9320_A_CDC_TX6_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX7 HPF Switch", WCD9320_A_CDC_TX7_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX8 HPF Switch", WCD9320_A_CDC_TX8_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX9 HPF Switch", WCD9320_A_CDC_TX9_MUX_CTL, 3, 1, 0),
	SOC_SINGLE("TX10 HPF Switch", WCD9320_A_CDC_TX10_MUX_CTL, 3, 1, 0),

	SOC_SINGLE("RX1 HPF Switch", WCD9320_A_CDC_RX1_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX2 HPF Switch", WCD9320_A_CDC_RX2_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX3 HPF Switch", WCD9320_A_CDC_RX3_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX4 HPF Switch", WCD9320_A_CDC_RX4_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX5 HPF Switch", WCD9320_A_CDC_RX5_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX6 HPF Switch", WCD9320_A_CDC_RX6_B5_CTL, 2, 1, 0),
	SOC_SINGLE("RX7 HPF Switch", WCD9320_A_CDC_RX7_B5_CTL, 2, 1, 0),

	SOC_ENUM("RX1 HPF cut off", cf_rxmix1_enum),
	SOC_ENUM("RX2 HPF cut off", cf_rxmix2_enum),
	SOC_ENUM("RX3 HPF cut off", cf_rxmix3_enum),
	SOC_ENUM("RX4 HPF cut off", cf_rxmix4_enum),
	SOC_ENUM("RX5 HPF cut off", cf_rxmix5_enum),
	SOC_ENUM("RX6 HPF cut off", cf_rxmix6_enum),
	SOC_ENUM("RX7 HPF cut off", cf_rxmix7_enum),


	SOC_SINGLE_EXT("COMP1 Switch", SND_SOC_NOPM, COMPANDER_1, 1, 0,
		       wcd9320_get_compander, wcd9320_set_compander),
};


static void wcd9320_discharge_comp(struct snd_soc_component *component, int comp)
{
	/* Level meter DIV Factor to 5*/
	snd_soc_component_update_bits(component, WCD9320_A_CDC_COMP0_B2_CTL + (comp * 8), 0xF0,
			    0x05 << 4);
	/* RMS meter Sampling to 0x01 */
	snd_soc_component_write(component, WCD9320_A_CDC_COMP0_B3_CTL + (comp * 8), 0x01);

	/* Worst case timeout for compander CnP sleep timeout */
	usleep_range(3000, 3250);
}

static int wcd9320_config_gain_compander(struct snd_soc_component *component,
					 int comp, bool enable)
{
	int ret = 0;

	switch (comp) {
	case COMPANDER_0:
		snd_soc_component_update_bits(component, WCD9320_A_SPKR_DRV_GAIN,
				    1 << 2, !enable << 2);
		break;
	case COMPANDER_1:
		snd_soc_component_update_bits(component, WCD9320_A_RX_HPH_L_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_component_update_bits(component, WCD9320_A_RX_HPH_R_GAIN,
				    1 << 5, !enable << 5);
		break;
	case COMPANDER_2:
		snd_soc_component_update_bits(component, WCD9320_A_RX_LINE_1_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_component_update_bits(component, WCD9320_A_RX_LINE_3_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_component_update_bits(component, WCD9320_A_RX_LINE_2_GAIN,
				    1 << 5, !enable << 5);
		snd_soc_component_update_bits(component, WCD9320_A_RX_LINE_4_GAIN,
				    1 << 5, !enable << 5);
		break;
	default:
		WARN_ON(1);
		ret = -EINVAL;
	}

	return ret;
}

static int wcd9320_config_compander(struct snd_soc_dapm_widget *w, struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_component(kcontrol);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	const int comp = w->shift;
	const u32 rate = wcd->comp_fs;
	const struct comp_sample_dependent_params *comp_params =
				&comp_samp_params[rate];
	int mask = 0x03; /* !COMP0, TODO: support other companders */
	int enable_mask = 0x03; /* !COMP0, TODO: support other companders */

	if (!wcd->comp_enabled)
		return 0;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/* Set compander Sample rate */
		snd_soc_component_update_bits(component,
				    WCD9320_A_CDC_COMP0_FS_CFG + (comp * 8),
				    0x07, rate);
		/* Set the static gain offset */
		/*
		 * TODO: buck checking
		 * if (comp == COMPANDER_1
		 *	&& buck_mv == WCD9XXX_CDC_BUCK_MV_1P8) {
		 *	snd_soc_component_update_bits(component,
		 *			WCD9320_A_CDC_COMP0_B4_CTL + (comp * 8),
		 *			0x80, 0x80);
		 * } else {
		 */
		snd_soc_component_update_bits(component,
				WCD9320_A_CDC_COMP0_B4_CTL + (comp * 8),
				0x80, 0x00);

		/* Enable RX interpolation path compander clocks */
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RX_B2_CTL,
				    mask << comp_shift[comp],
				    mask << comp_shift[comp]);
		/* Toggle compander reset bits */
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp],
				    mask << comp_shift[comp]);
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Set gain source to compander */
		wcd9320_config_gain_compander(component, comp, true);

		/* Compander enable */
		snd_soc_component_update_bits(component, WCD9320_A_CDC_COMP0_B1_CTL +
				    (comp * 8), enable_mask, enable_mask);

		wcd9320_discharge_comp(component, comp);

		/* Set sample rate dependent paramater */
		snd_soc_component_write(component, WCD9320_A_CDC_COMP0_B3_CTL + (comp * 8),
			      comp_params->rms_meter_resamp_fact);
		snd_soc_component_update_bits(component,
				    WCD9320_A_CDC_COMP0_B2_CTL + (comp * 8),
				    0xF0, comp_params->rms_meter_div_fact << 4);
		snd_soc_component_update_bits(component,
					WCD9320_A_CDC_COMP0_B2_CTL + (comp * 8),
					0x0F, comp_params->peak_det_timeout);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* Disable compander */
		snd_soc_component_update_bits(component,
				    WCD9320_A_CDC_COMP0_B1_CTL + (comp * 8),
				    enable_mask, 0x00);

		/* Toggle compander reset bits */
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp],
				    mask << comp_shift[comp]);
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_OTHR_RESET_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Turn off the clock for compander in pair */
		snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_RX_B2_CTL,
				    mask << comp_shift[comp], 0);

		/* Set gain source to register */
		wcd9320_config_gain_compander(component, comp, false);
		break;
	}

	return 0;
}

static int wcd9320_codec_aif4_mixer_switch_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_soc_dapm_kcontrol_component(kcontrol);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);

	if (test_bit(AIF4_SWITCH_VALUE, &wcd->status_mask))
		ucontrol->value.integer.value[0] = 1;
	else
		ucontrol->value.integer.value[0] = 0;

	return 0;
}



static int wcd9320_codec_aif4_mixer_switch_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{

	struct snd_soc_component *component = snd_soc_kcontrol_component(kcontrol);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	struct snd_soc_dapm_context *dapm = snd_soc_component_get_dapm(component);
	struct snd_soc_dapm_update *update = NULL;

	if (ucontrol->value.integer.value[0]) {
		snd_soc_dapm_mixer_update_power(dapm, kcontrol, 1, update);
		set_bit(AIF4_SWITCH_VALUE, &wcd->status_mask);
	} else {
		snd_soc_dapm_mixer_update_power(dapm, kcontrol, 0, update);
		clear_bit(AIF4_SWITCH_VALUE, &wcd->status_mask);
	}

	return 1;
}

static int _wcd9320_codec_enable_mclk(struct snd_soc_component *component, int enable)
{
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int ret = 0;

	if (!wcd->codec_clk) {
		dev_err(wcd->dev, "%s: wcd clock is NULL\n", __func__);
		return -EINVAL;
	}

	if (enable) {
		ret = wcd9320_cdc_req_mclk_enable(wcd, true);
		if (ret)
			return ret;

		set_bit(AUDIO_NOMINAL, &wcd->status_mask);
	} else {
		clear_bit(AUDIO_NOMINAL, &wcd->status_mask);
		wcd9320_cdc_req_mclk_enable(wcd, false);
	}

	return ret;
}

static int wcd9320_codec_enable_mclk(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return _wcd9320_codec_enable_mclk(component, true);
	case SND_SOC_DAPM_POST_PMD:
		return _wcd9320_codec_enable_mclk(component, false);
	}

	return 0;
}
static const char * const spl_src1_mux_text[] = {
	"ZERO", "SRC_IN_HPHR", "SRC_IN_LO2",
};

static const char * const spl_src2_mux_text[] = {
	"ZERO", "SRC_IN_LO3", "SRC_IN_SPKRL",
};

static const char * const spl_src3_mux_text[] = {
	"ZERO", "SRC_IN_LO4", "SRC_IN_SPKRR",
};

static const char * const rx_int0_7_mix_mux_text[] = {
	"ZERO", "RX0", "RX1", "RX2", "RX3", "RX4", "RX5",
	"RX6", "RX7", "PROXIMITY"
};

static const char * const rx_int_mix_mux_text[] = {
	"ZERO", "RX0", "RX1", "RX2", "RX3", "RX4", "RX5",
	"RX6", "RX7"
};

static const char * const rx_prim_mix_text[] = {
	"ZERO", "DEC0", "DEC1", "IIR0", "IIR1", "RX0", "RX1", "RX2",
	"RX3", "RX4", "RX5", "RX6", "RX7"
};

static const char * const rx_int_dem_inp_mux_text[] = {
	"NORMAL_DSM_OUT", "CLSH_DSM_OUT",
};

static const char * const rx_echo_mux_text[] = {
	"ZERO", "RX_MIX0", "RX_MIX1", "RX_MIX2", "RX_MIX3", "RX_MIX4",
	"RX_MIX5", "RX_MIX6", "RX_MIX7", "RX_MIX8",
};


static const char * const class_h_dsm_text[] = {
	"ZERO", "DSM_HPHL_RX1", "DSM_SPKR_RX7"
};
static const char * const rx_mix1_text[] = {
	"ZERO", "SRC1", "SRC2", "IIR1", "IIR2", "RX1", "RX2", "RX3", "RX4", "RX5", "RX6", "RX7"
};

static const struct snd_kcontrol_new hphl_switch[] = {
	SOC_DAPM_SINGLE("Switch", WCD9320_A_RX_HPH_L_DAC_CTL, 6, 1, 0)
};

static const char * const rx1_interpolator_text[] = {
	"ZERO", "RX1 MIX2"
};

static const struct soc_enum rx1_interpolator_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_CLK_RX_B1_CTL, 0, 2, rx1_interpolator_text);

static const struct snd_kcontrol_new rx1_interpolator =
	SOC_DAPM_ENUM("RX1 INTERP Mux", rx1_interpolator_enum);

static const char * const rx2_interpolator_text[] = {
	"ZERO", "RX2 MIX2"
};
static const struct soc_enum rx2_interpolator_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_CLK_RX_B1_CTL, 1, 2, rx2_interpolator_text);

static const struct snd_kcontrol_new rx2_interpolator =
	SOC_DAPM_ENUM("RX2 INTERP Mux", rx2_interpolator_enum);


static const struct soc_enum rx_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_CONN_RX1_B1_CTL, 0, 12, rx_mix1_text);
static const struct soc_enum rx2_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_CONN_RX2_B1_CTL, 0, 12, rx_mix1_text);

static const struct soc_enum rx7_mix1_inp1_chain_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_CONN_RX7_B1_CTL, 0, 12, rx_mix1_text);
static const struct soc_enum rx7_mix1_inp2_chain_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_CONN_RX7_B1_CTL, 4, 12, rx_mix1_text);

static const struct snd_kcontrol_new rx_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX1 MIX1 INP1 Mux", rx_mix1_inp1_chain_enum);
static const struct snd_kcontrol_new rx2_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX2 MIX1 INP1 Mux", rx2_mix1_inp1_chain_enum);
static const struct snd_kcontrol_new rx7_mix1_inp1_mux =
	SOC_DAPM_ENUM("RX7 MIX1 INP1 Mux", rx7_mix1_inp1_chain_enum);
static const struct snd_kcontrol_new rx7_mix1_inp2_mux =
	SOC_DAPM_ENUM("RX7 MIX1 INP2 Mux", rx7_mix1_inp2_chain_enum);

static const struct soc_enum class_h_dsm_enum =
	SOC_ENUM_SINGLE(WCD9320_A_CDC_CONN_CLSH_CTL, 4, 3, class_h_dsm_text);

static const struct snd_kcontrol_new class_h_dsm_mux =
	SOC_DAPM_ENUM("CLASS_H_DSM MUX Mux", class_h_dsm_enum);

static const struct snd_kcontrol_new aif4_switch_mixer_controls =
	SOC_SINGLE_EXT("Switch", SND_SOC_NOPM,
			0, 1, 0, wcd9320_codec_aif4_mixer_switch_get,
			wcd9320_codec_aif4_mixer_switch_put);

static const struct snd_soc_dapm_widget wcd9320_dapm_widgets[] = {

	SND_SOC_DAPM_AIF_IN_E("AIF1 PB", "AIF1 Playback", 0, SND_SOC_NOPM,
				AIF1_PB, 0, wcd9320_codec_enable_slimrx,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN_E("AIF2 PB", "AIF2 Playback", 0, SND_SOC_NOPM,
				AIF2_PB, 0, wcd9320_codec_enable_slimrx,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN_E("AIF3 PB", "AIF3 Playback", 0, SND_SOC_NOPM,
				AIF3_PB, 0, wcd9320_codec_enable_slimrx,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	/* Diagnostic-only RX7-MIX1 digital loopback capture (see wcd9320_tx_chs) */
	SND_SOC_DAPM_AIF_OUT_E("AIF1 CAP", "AIF1 Capture", 0, SND_SOC_NOPM,
				AIF1_CAP, 0, wcd9320_codec_enable_slimrx,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	/*
	 * The SLIM RX port index (widget->shift) must start at WCD9320_RX0 so
	 * "SLIM RX1 MUX" maps to rx_chs[0] = SLIM port 16 = hardware RX-mixer
	 * input "RX1". The previous WCD9320_RX1.. base left port 16 unreachable
	 * and put "SLIM RX1" on port 17 (hw input "RX2"), so the data arrived
	 * on a port whose hw input never matched the RX7 MIX route/rate setup
	 * -> the interpolator never drained it (port overflow, silence).
	 */
	SND_SOC_DAPM_MUX("SLIM RX1 MUX", SND_SOC_NOPM, WCD9320_RX0, 0,
				&slim_rx_mux[WCD9320_RX0]),
	SND_SOC_DAPM_MUX("SLIM RX2 MUX", SND_SOC_NOPM, WCD9320_RX1, 0,
				&slim_rx_mux[WCD9320_RX1]),
	SND_SOC_DAPM_MUX("SLIM RX3 MUX", SND_SOC_NOPM, WCD9320_RX2, 0,
				&slim_rx_mux[WCD9320_RX2]),
	SND_SOC_DAPM_MUX("SLIM RX4 MUX", SND_SOC_NOPM, WCD9320_RX3, 0,
				&slim_rx_mux[WCD9320_RX3]),
	SND_SOC_DAPM_MUX("SLIM RX5 MUX", SND_SOC_NOPM, WCD9320_RX4, 0,
				&slim_rx_mux[WCD9320_RX4]),
	SND_SOC_DAPM_MUX("SLIM RX6 MUX", SND_SOC_NOPM, WCD9320_RX5, 0,
				&slim_rx_mux[WCD9320_RX5]),
	SND_SOC_DAPM_MUX("SLIM RX7 MUX", SND_SOC_NOPM, WCD9320_RX6, 0,
				&slim_rx_mux[WCD9320_RX6]),

	SND_SOC_DAPM_MIXER("SLIM RX1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX3", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX4", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX5", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX6", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("SLIM RX7", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX1 MIX2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX2 MIX2", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_ADC_E("AUX_PGA_Left", "AIF1 Playback", WCD9320_A_RX_AUX_SW_CTL, 7, 0,
		wcd9320_codec_enable_aux_pga, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_ADC_E("AUX_PGA_Right", "AIF1 Playback", WCD9320_A_RX_AUX_SW_CTL, 6, 0,
		wcd9320_codec_enable_aux_pga, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIXER("HPHL_PA_MIXER", SND_SOC_NOPM, 0, 0,
		hphl_pa_mix, ARRAY_SIZE(hphl_pa_mix)),

	SND_SOC_DAPM_MIXER("HPHR_PA_MIXER", SND_SOC_NOPM, 0, 0,
		hphr_pa_mix, ARRAY_SIZE(hphr_pa_mix)),

	SND_SOC_DAPM_MUX_E("RX1 INTERP", WCD9320_A_CDC_CLK_RX_B1_CTL, 0, 0,
		&rx1_interpolator, wcd9320_codec_enable_interpolator,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MUX_E("RX2 INTERP", WCD9320_A_CDC_CLK_RX_B1_CTL, 1, 0,
		&rx2_interpolator, wcd9320_codec_enable_interpolator,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),
	/* Headphone */
	SND_SOC_DAPM_OUTPUT("HEADPHONE"),

	SND_SOC_DAPM_SUPPLY("COMP1_CLK", SND_SOC_NOPM, 1, 0,
		wcd9320_config_compander, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("HPHL", WCD9320_A_RX_HPH_CNP_EN, 5, 0, NULL, 0,
		wcd9320_hph_pa_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MIXER_E("HPHL DAC", WCD9320_A_RX_HPH_L_DAC_CTL, 7, 0,
		hphl_switch, ARRAY_SIZE(hphl_switch), wcd9320_codec_hphl_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_PGA_E("HPHR", WCD9320_A_RX_HPH_CNP_EN, 4, 0, NULL, 0,
		wcd9320_hph_pa_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU |	SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_DAC_E("HPHR DAC", NULL, WCD9320_A_RX_HPH_R_DAC_CTL, 7, 0,
		wcd9320_codec_hphr_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_MIXER("RX1 MIX1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX2 MIX1", SND_SOC_NOPM, 0, 0, NULL, 0),

	SND_SOC_DAPM_MIXER("RX1 CHAIN", WCD9320_A_CDC_RX1_B6_CTL, 5, 0, NULL, 0),
	SND_SOC_DAPM_MIXER("RX2 CHAIN", WCD9320_A_CDC_RX2_B6_CTL, 5, 0, NULL, 0),

	SND_SOC_DAPM_MUX("RX1 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX2 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx2_mix1_inp1_mux),

	SND_SOC_DAPM_MUX_E("CLASS_H_DSM MUX", SND_SOC_NOPM, 0, 0,
		&class_h_dsm_mux, wcd9320_codec_dsm_mux_event,
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("RX_BIAS", SND_SOC_NOPM, 0, 0,
		wcd9320_codec_enable_rx_bias, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("CDC_I2S_RX_CONN", WCD9XXX_A_CDC_CLK_OTHR_CTL, 5, 0,
			    NULL, 0),

	SND_SOC_DAPM_SUPPLY("MCLK",  SND_SOC_NOPM, 0, 0,
		wcd9320_codec_enable_mclk, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	/* Speaker (RX7 / SPK) */
	SND_SOC_DAPM_OUTPUT("SPK_OUT"),

	SND_SOC_DAPM_PGA_E("SPK PA", SND_SOC_NOPM, 0, 0, NULL, 0,
		wcd9320_codec_enable_spk_pa,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_DAC_E("SPK DAC", NULL, SND_SOC_NOPM, 0, 0,
		wcd9320_spk_dac_event,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SUPPLY("VDD_SPKDRV", SND_SOC_NOPM, 0, 0,
		wcd9320_codec_enable_vdd_spkr,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_MIXER("RX7 MIX1", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_MIXER_E("RX7 MIX2", WCD9320_A_CDC_CLK_RX_B1_CTL, 6, 0, NULL,
		0, wcd9320_codec_enable_interpolator,
		SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_MUX("RX7 MIX1 INP1", SND_SOC_NOPM, 0, 0,
		&rx7_mix1_inp1_mux),
	SND_SOC_DAPM_MUX("RX7 MIX1 INP2", SND_SOC_NOPM, 0, 0,
		&rx7_mix1_inp2_mux),

};

static int wcd9320_get_channel_map(const struct snd_soc_dai *dai,
				 unsigned int *tx_num, unsigned int *tx_slot,
				 unsigned int *rx_num, unsigned int *rx_slot)
{
	struct wcd9320_priv *wcd = snd_soc_component_get_drvdata(dai->component);
	u32 i = 0;
	struct wcd_slim_ch *ch;

	switch (dai->id) {
	case AIF1_PB:
	case AIF2_PB:
	case AIF3_PB:
	case AIF4_PB:
	case AIF_MIX1_PB:
		if (!rx_slot || !rx_num) {
			dev_err(wcd->dev, "Invalid rx_slot %p or rx_num %p\n",
				rx_slot, rx_num);
			return -EINVAL;
		}
		list_for_each_entry(ch, &wcd->dai[dai->id].wcd_slim_ch_list,
				    list) {
			dev_err(wcd->dev, "slot_num %u ch->ch_num %d\n",
				i, ch->ch_num);
			rx_slot[i++] = ch->ch_num;
		}
		*rx_num = i;
		break;
	case AIF1_CAP:
	case AIF2_CAP:
	case AIF3_CAP:
	case AIF4_MAD_TX:
	case AIF4_VIFEED:
		if (!tx_slot || !tx_num) {
			dev_err(wcd->dev, "Invalid tx_slot %p or tx_num %p\n",
				tx_slot, tx_num);
			return -EINVAL;
		}
		list_for_each_entry(ch, &wcd->dai[dai->id].wcd_slim_ch_list,
				    list) {
			tx_slot[i++] = ch->ch_num;
		}
		*tx_num = i;
		break;

	default:
		dev_err(wcd->dev, "Invalid DAI ID %x\n", dai->id);
		break;
	}

	return 0;
}

static int mywcd_slim_init_slimslave(struct wcd_slim_data *wcd, u8 wcd_slim_pgd_la,
			   unsigned int tx_num, const unsigned int *tx_slot,
			   unsigned int rx_num, const unsigned int *rx_slot)
{
	int ret = 0;
	int i;

	if (wcd->rx_chs) {
		wcd->num_rx_port = rx_num;
		for (i = 0; i < rx_num; i++) {
			/*
			 * Only refresh the channel number; the list node is
			 * initialised once at probe and owned by the SLIM RX
			 * mux, so re-initialising it here would tear the
			 * channel back out of its AIF dai list.
			 */
			wcd->rx_chs[i].ch_num = rx_slot[i];
		}
	} else {
		pr_err("Not able to allocate memory for %d slimbus rx ports\n",
			wcd->num_rx_port);
	}

	if (wcd->tx_chs) {
		wcd->num_tx_port = tx_num;
		for (i = 0; i < tx_num; i++) {
			wcd->tx_chs[i].ch_num = tx_slot[i];
		}
	} else {
		pr_err("Not able to allocate memory for %d slimbus tx ports\n",
			wcd->num_tx_port);
	}

	return ret;
}

static int wcd9320_set_channel_map(struct snd_soc_dai *dai,
				 unsigned int tx_num, const unsigned int *tx_slot,
				 unsigned int rx_num, const unsigned int *rx_slot)
{
	struct wcd9320_priv *wcd;

	wcd = snd_soc_component_get_drvdata(dai->component);

	if (!tx_slot || !rx_slot) {
		dev_err(wcd->dev, "Invalid tx_slot=%p, rx_slot=%p\n",
			tx_slot, rx_slot);
		return -EINVAL;
	}

	if (wcd->intf_type == WCD_INTERFACE_TYPE_SLIMBUS) {
		mywcd_slim_init_slimslave(wcd->slim_data, wcd->slim->laddr,
					   tx_num, tx_slot, rx_num, rx_slot);
	}
	return 0;
}

static int wcd9320_set_interpolator_rate(struct snd_soc_dai *dai,
					   u32 sample_rate)
{
	u32 j;
	int i, comp_fs;
	u8 rx_mix1_inp;
	u16 rx_mix_1_reg_1, rx_mix_1_reg_2;
	u16 rx_fs_reg;
	u8 rx_mix_1_reg_1_val, rx_mix_1_reg_2_val;
	struct snd_soc_component *component = dai->component;
	struct wcd_slim_ch *ch;
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);

	for (i = 0; i < COMPANDER_FS_MAX; i++) {
		if (sample_rate ==
				compander_sample_rate_val[i].sample_rate) {
			comp_fs =
				compander_sample_rate_val[i].rate_val;
			break;
		}
	}

	list_for_each_entry(ch, &wcd->dai[dai->id].wcd_slim_ch_list, list) {
		rx_mix1_inp = ch->port + RX_MIX1_INP_SEL_RX1 - 16; //WCD9320_TX_PORT_NUMBER;
		WARN_ON(rx_mix1_inp < RX_MIX1_INP_SEL_RX1 || rx_mix1_inp > RX_MIX1_INP_SEL_RX7);

		rx_mix_1_reg_1 = WCD9320_A_CDC_CONN_RX1_B1_CTL;

		for (j = 0; j < NUM_INTERPOLATORS; j++) {
			rx_mix_1_reg_2 = rx_mix_1_reg_1 + 1;

			rx_mix_1_reg_1_val = snd_soc_component_read(component, rx_mix_1_reg_1);
			rx_mix_1_reg_2_val = snd_soc_component_read(component, rx_mix_1_reg_2);

			if (((rx_mix_1_reg_1_val & 0x0F) == rx_mix1_inp) ||
				(((rx_mix_1_reg_1_val >> 4) & 0x0F) == rx_mix1_inp) ||
				((rx_mix_1_reg_2_val & 0x0F) == rx_mix1_inp)) {

				rx_fs_reg = WCD9320_A_CDC_RX1_B5_CTL + 8 * j;

				dev_dbg(component->dev, "%s: AIF_PB DAI(%d) connected to RX%u\n", __func__, dai->id, j + 1);
				dev_dbg(component->dev, "%s: set RX%u sample rate to %u\n", __func__, j + 1, sample_rate);

				snd_soc_component_update_bits(component, rx_fs_reg, 0xE0, comp_fs << 5);

				if (j < 3)
					wcd->comp_fs = comp_fs;
			}
			if (j < 2)
				rx_mix_1_reg_1 += 3;
			else
				rx_mix_1_reg_1 += 2;
		}
	}
	return 0;
}


static int wcd_slim_stream_prepare(struct wcd9320_priv *wcd,
	struct wcd_slim_codec_dai_data *dai_data)

{
	struct slim_stream_config *cfg = &dai_data->sconfig;
	struct list_head *wcd_slim_ch_list = &dai_data->wcd_slim_ch_list;
	unsigned int rate = dai_data->rate;
	unsigned int bit_width = dai_data->bit_width;
	struct wcd_slim_data *wcd_sd = wcd->slim_data;
	u8  payload = 0;
	u16 codec_port = 0;
	int ret;
	struct wcd_slim_ch *rx;
	int i = 0;

	cfg->ch_count = 0;
	cfg->port_mask = 0;
	/*
	 * Direction is per-channel-list: RX channels (port >= 16) belong to
	 * a playback AIF, TX channels (port < 16) to a capture AIF -- a
	 * dai_data's wcd_slim_ch_list is never mixed. Peek the first entry.
	 */
	cfg->direction = SNDRV_PCM_STREAM_PLAYBACK;
	if (!list_empty(wcd_slim_ch_list)) {
		rx = list_first_entry(wcd_slim_ch_list, struct wcd_slim_ch, list);
		if (rx->port < WCD9320_RX_PORT_START_NUMBER)
			cfg->direction = SNDRV_PCM_STREAM_CAPTURE;
	}
	/* Configure slave interface device */
	list_for_each_entry(rx, wcd_slim_ch_list, list) {
		payload |= 1 << rx->shift;
		cfg->port_mask |= 1 << (rx->shift + 0x10);
		cfg->ch_count++;

	}
	kfree(cfg->chs);
	cfg->chs = kcalloc(cfg->ch_count, sizeof(unsigned int), GFP_KERNEL);
	if (!cfg->chs)
		return -ENOMEM;

	cfg->rate = rate;
	cfg->bps = bit_width;
	i = 0;
	list_for_each_entry(rx, wcd_slim_ch_list, list) {
		u16 ch_reg, cfg_reg;

		codec_port = rx->port;
		cfg->chs[i++] = rx->ch_num;

		if (cfg->direction == SNDRV_PCM_STREAM_CAPTURE) {
			ch_reg = (codec_port < 8) ?
				SB_PGD_TX_PORT_MULTI_CHANNEL_0(codec_port) :
				SB_PGD_TX_PORT_MULTI_CHANNEL_1(codec_port);
			cfg_reg = SB_PGD_PORT_CFG_BYTE_ADDR(
				wcd_sd->port_tx_cfg_reg_base, codec_port);
		} else {
			ch_reg = SB_PGD_RX_PORT_MULTI_CHANNEL_0(
				wcd_sd->rx_port_ch_reg_base, codec_port);
			cfg_reg = SB_PGD_PORT_CFG_BYTE_ADDR(
				wcd_sd->port_rx_cfg_reg_base, codec_port);
		}

		/* write to interface device */
		ret = regmap_write(wcd_sd->if_regmap, ch_reg, payload);
		if (ret < 0) {
			pr_err("%s:Intf-dev fail reg[%d] payload[%d] ret[%d]\n",
				__func__, ch_reg, payload, ret);
			return ret;
		}
		/* configure the slave port for water mark and enable*/
		ret = regmap_write(wcd_sd->if_regmap, cfg_reg, WATER_MARK_VAL);
		if (ret < 0) {
			pr_err("%s:watermark set failure for port[%d] ret[%d]",
				__func__, codec_port, ret);
		}
	}
	if (!dai_data->sruntime)
		dai_data->sruntime = slim_stream_allocate(wcd->slim,
							  "WCD9320-SLIM");

	return 0;

}

static int wcd9320_prepare(struct snd_pcm_substream *substream,
			 struct snd_soc_dai *dai)
{

	struct wcd9320_priv *wcd = snd_soc_component_get_drvdata(dai->component);
	struct wcd_slim_codec_dai_data *dai_data = &wcd->dai[dai->id];

	if ((substream->stream == SNDRV_PCM_STREAM_PLAYBACK) &&
	    test_bit(SB_CLK_GEAR, &wcd->status_mask)) {
		wcd9320_codec_vote_max_bw(dai->component, false);
		clear_bit(SB_CLK_GEAR, &wcd->status_mask);
	}

	/*
	 * SLIM channel setup intentionally does NOT happen here: .prepare
	 * fires before DAPM has powered the interpolator clock (verified on
	 * hardware: CDC_CLK_RX_B1_CTL reads 0x0 at this point). Channel
	 * setup lives in .trigger START (see wcd9320_trigger), where the
	 * clock is reliably on and the q6afe producer has been started.
	 */
	(void)dai_data;

	return 0;
}

/*
 * Activate the SLIM data channels from .trigger START, i.e. AFTER the q6afe
 * RX producer has been started by the backend trigger. Activating earlier
 * (in .prepare / DAPM POST_PMU) makes the ADSP framer see an active channel
 * with no bound source ("empty reconfiguration") so no audio is deposited and
 * the codec RX port underflows -> silence. This mirrors the wcd9335 ordering.
 */
static int wcd9320_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct wcd9320_priv *wcd = snd_soc_component_get_drvdata(dai->component);
	struct wcd_slim_codec_dai_data *dai_data = &wcd->dai[dai->id];
	unsigned int en = 0;
	int i;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			regmap_read(wcd->regmap, WCD9320_A_CDC_CLK_RX_B1_CTL, &en);
			en &= 0x7f;
			/*
			 * Pull the active interpolator(s)' digital volume to
			 * the -84dB floor across stream start: the samples
			 * that reach the RX path between slim_stream_enable()
			 * and the interpolator reset pulse below otherwise
			 * play as a short burst of garbage (audible crackle
			 * right before the stream). Restored below after the
			 * reset has taken and the garbage flushed.
			 */
			for (i = 0; i < WCD9320_NUM_INTERPOLATORS; i++)
				if (en & BIT(i))
					regmap_write(wcd->regmap,
						rx_digital_gain_reg[i],
						(u8)-84);
			/*
			 * Take the speaker PA off the line for the reset
			 * pulse below: resetting the interpolator steps the
			 * DAC output, which the digital volume floor cannot
			 * attenuate (it is upstream of the DAC). Measured on
			 * hardware (mic capture): a single ~8ms impulse ~4x
			 * louder than the tone, exactly at the reset, ~18ms
			 * before audio. Re-enabled below once everything has
			 * settled with the gain still floored.
			 */
			regmap_update_bits(wcd->regmap, WCD9320_A_SPKR_DRV_EN,
					   0x80, 0x00);
		}
		/*
		 * Re-write the codec-side SLIM port config (channel map +
		 * watermark/enable) on EVERY stream start, not just the
		 * first: the port state does not survive a stream stop, and
		 * the working downstream stack re-issues these writes on
		 * every playback (mirrors upstream wcd9335, whose
		 * slim_set_hw_params runs on every hw_params call).
		 */
		wcd_slim_stream_prepare(wcd, dai_data);
		slim_stream_prepare(dai_data->sruntime, &dai_data->sconfig);
		slim_stream_enable(dai_data->sruntime);
		/*
		 * Kick the active RX interpolator(s) with a reset pulse now
		 * that the SLIM channel is live. DAPM already pulsed
		 * CDC_CLK_RX_RESET when the interpolator widget powered up,
		 * but that happens before the ADSP starts depositing data on
		 * the bus; the interpolator needs a reset while clock AND
		 * data are both active to start draining the port FIFO.
		 * Removing this re-pulse regressed audible output to silence.
		 */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			if (en) {
				regmap_update_bits(wcd->regmap,
					WCD9320_A_CDC_CLK_RX_RESET_CTL, en, en);
				regmap_update_bits(wcd->regmap,
					WCD9320_A_CDC_CLK_RX_RESET_CTL, en, 0);
			}
			/* let the start-of-stream garbage flush */
			msleep(20);
			/* PA back on while the input is settled and floored */
			regmap_update_bits(wcd->regmap, WCD9320_A_SPKR_DRV_EN,
					   0x80, 0x80);
			usleep_range(2000, 2500);
			for (i = 0; i < WCD9320_NUM_INTERPOLATORS; i++)
				if (en & BIT(i))
					regmap_write(wcd->regmap,
						rx_digital_gain_reg[i], 0);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/*
		 * Pull the digital volume to the floor before tearing the
		 * SLIM stream down: this trigger runs while the analog PA is
		 * still powered (DAPM powers down afterwards), so the
		 * momentary underflow when the channel dies otherwise pops
		 * out of the speaker. Downstream avoids this by closing the
		 * SLIM channel from the AIF widget's POST_PMD, after the PA
		 * is already off. The next stream start restores the gain.
		 */
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			regmap_read(wcd->regmap, WCD9320_A_CDC_CLK_RX_B1_CTL, &en);
			en &= 0x7f;
			for (i = 0; i < WCD9320_NUM_INTERPOLATORS; i++)
				if (en & BIT(i))
					regmap_write(wcd->regmap,
						rx_digital_gain_reg[i],
						(u8)-84);
		}
		slim_stream_disable(dai_data->sruntime);
		slim_stream_unprepare(dai_data->sruntime);
		break;
	}

	return 0;
}

static int wcd9320_hw_params(struct snd_pcm_substream *substream,
			   struct snd_pcm_hw_params *params,
			   struct snd_soc_dai *dai)
{
	struct wcd9320_priv *wcd = snd_soc_component_get_drvdata(dai->component);
	struct snd_soc_component *component = dai->component;
	int ret;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		ret = wcd9320_set_interpolator_rate(dai, params_rate(params));
		if (ret) {
			dev_err(wcd->dev, "cannot set sample rate: %u\n",
				params_rate(params));
			return ret;
		}
		switch (params_width(params)) {
		case 16:
			wcd->dai[dai->id].bit_width = 16;
			break;
		case 24:
			wcd->dai[dai->id].bit_width = 24;
			break;
		}
		wcd->dai[dai->id].rate = params_rate(params);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		/*
		 * Diagnostic-only TX3/RX7-loopback DAI: no interpolator rate
		 * to set (it taps a fixed digital point, not a decimator).
		 */
		switch (params_width(params)) {
		case 16:
			wcd->dai[dai->id].bit_width = 16;
			break;
		case 24:
			wcd->dai[dai->id].bit_width = 24;
			break;
		}
		wcd->dai[dai->id].rate = params_rate(params);
		break;
	default:
		dev_err(wcd->dev, "Invalid stream type %d\n",
			substream->stream);
		return -EINVAL;
	};
	/*
	 * Select the sample format on the SLIM-port -> RX sideband bus.
	 * Each RX SB port has a 2-bit field in CONN_RX_SB_B1/B2_CTL
	 * (field_shift = port * 2); 0x2 = 16-bit, 0x0 = 24-bit. We carry a
	 * single channel on codec port 16 = RX SB port 0 (shift 0), matching
	 * downstream taiko_set_rxsb_port_format() and the working reference's
	 * live value (0x3AE = 0x02 for 16-bit playback).
	 */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		snd_soc_component_update_bits(component,
			WCD9320_A_CDC_CONN_RX_SB_B1_CTL, 0x03,
			wcd->dai[dai->id].bit_width == 16 ? 0x02 : 0x00);
	return 0;
}

static int wcd9320_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	return 0;
}

static const struct snd_soc_dai_ops wcd9320_dai_ops = {
	.hw_params = wcd9320_hw_params,
	.prepare = wcd9320_prepare,
	.trigger = wcd9320_trigger,
	.set_fmt = wcd9320_set_dai_fmt,
	.set_channel_map = wcd9320_set_channel_map,
	.get_channel_map = wcd9320_get_channel_map,
};

static struct snd_soc_dai_driver wcd9320_slim_dai[] = {
	[0] = {
		.name = "wcd9320_rx1",
		.id = AIF1_PB,
		.playback = {
			.stream_name = "AIF1 Playback",
			.rates = WCD9320_RATES,
			.formats = WCD9320_FORMATS_S16_S24_LE,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &wcd9320_dai_ops,
	},
	[1] = {
		/*
		 * Diagnostic-only capture DAI: digital loopback tapping
		 * RX7 MIX1's output via the SLIM TX3 sideband mux, to check
		 * whether RX playback data physically reaches the codec's
		 * digital core (see the loudspeaker-silence investigation).
		 */
		.name = "wcd9320_tx3_loopback",
		.id = AIF1_CAP,
		.capture = {
			.stream_name = "AIF1 Capture",
			.rates = WCD9320_RATES,
			.formats = WCD9320_FORMATS_S16_S24_LE,
			.rate_max = 192000,
			.rate_min = 8000,
			.channels_min = 1,
			.channels_max = 1,
		},
		.ops = &wcd9320_dai_ops,
	},
};

static const struct wcd9320_reg_mask_val wcd9320_codec_reg_common_defaults[] = {
	/* set MCLk to 9.6 */
	WCD9320_REG_VAL(WCD9320_A_CHIP_CTL, 0x02),
	WCD9320_REG_VAL(WCD9320_A_CDC_CLK_POWER_CTL, 0x03),

	/* EAR PA deafults  */
	WCD9320_REG_VAL(WCD9320_A_RX_EAR_CMBUFF, 0x05),

	/* RX deafults */
	WCD9320_REG_VAL(WCD9320_A_CDC_RX1_B5_CTL, 0x78),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX2_B5_CTL, 0x78),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX3_B5_CTL, 0x78),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX4_B5_CTL, 0x78),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX5_B5_CTL, 0x78),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX6_B5_CTL, 0x78),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX7_B5_CTL, 0x78),

	/* RX1 and RX2 defaults */
	WCD9320_REG_VAL(WCD9320_A_CDC_RX1_B6_CTL, 0xA0),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX2_B6_CTL, 0xA0),

	/* RX3 to RX7 defaults */
	WCD9320_REG_VAL(WCD9320_A_CDC_RX3_B6_CTL, 0x80),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX4_B6_CTL, 0x80),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX5_B6_CTL, 0x80),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX6_B6_CTL, 0x80),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX7_B6_CTL, 0x80),

	/* MAD registers */
	WCD9320_REG_VAL(WCD9320_A_MAD_ANA_CTRL, 0xF1),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_MAIN_CTL_1, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_MAIN_CTL_2, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_1, 0x00),
	/* Set SAMPLE_TX_EN bit */
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_2, 0x03),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_3, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_4, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_5, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_6, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_7, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_CTL_8, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_IIR_CTL_PTR, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_MAD_AUDIO_IIR_CTL_VAL, 0x40),
	WCD9320_REG_VAL(WCD9320_A_CDC_DEBUG_B7_CTL, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_CLK_OTHR_RESET_B1_CTL, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_CLK_OTHR_CTL, 0x00),
	WCD9320_REG_VAL(WCD9320_A_CDC_CONN_MAD, 0x01),

	/* Set HPH Path to low power mode */
	WCD9320_REG_VAL(WCD9320_A_RX_HPH_BIAS_PA, 0x55),

	/* BUCK default */
	WCD9320_REG_VAL(WCD9XXX_A_BUCK_CTRL_CCL_4, 0x51),
	WCD9320_REG_VAL(WCD9XXX_A_BUCK_CTRL_CCL_1, 0x5B),
};

static const struct wcd9320_reg_mask_val wcd9320_codec_reg_2_0_defaults[] = {
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_1_GAIN, 0x2),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_2_GAIN, 0x2),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_1_2_ADC_IB, 0x44),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_3_GAIN, 0x2),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_4_GAIN, 0x2),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_3_4_ADC_IB, 0x44),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_5_GAIN, 0x2),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_6_GAIN, 0x2),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX_5_6_ADC_IB, 0x44),
	WCD9320_REG_VAL(WCD9XXX_A_BUCK_MODE_3, 0xCE),
	WCD9320_REG_VAL(WCD9XXX_A_BUCK_CTRL_VCL_1, 0x8),
	WCD9320_REG_VAL(WCD9320_A_BUCK_CTRL_CCL_4, 0x51),
	WCD9320_REG_VAL(WCD9320_A_NCP_DTEST, 0x10),
	WCD9320_REG_VAL(WCD9320_A_RX_HPH_CHOP_CTL, 0xA4),
	WCD9320_REG_VAL(WCD9320_A_RX_HPH_OCP_CTL, 0x69),
	WCD9320_REG_VAL(WCD9320_A_RX_HPH_CNP_WG_CTL, 0xDA),
	WCD9320_REG_VAL(WCD9320_A_RX_HPH_CNP_WG_TIME, 0x15),
	WCD9320_REG_VAL(WCD9320_A_RX_EAR_BIAS_PA, 0x76),
	WCD9320_REG_VAL(WCD9320_A_RX_EAR_CNP, 0xC0),
	WCD9320_REG_VAL(WCD9320_A_RX_LINE_BIAS_PA, 0x78),
	WCD9320_REG_VAL(WCD9320_A_RX_LINE_1_TEST, 0x2),
	WCD9320_REG_VAL(WCD9320_A_RX_LINE_2_TEST, 0x2),
	WCD9320_REG_VAL(WCD9320_A_RX_LINE_3_TEST, 0x2),
	WCD9320_REG_VAL(WCD9320_A_RX_LINE_4_TEST, 0x2),
	WCD9320_REG_VAL(WCD9320_A_SPKR_DRV_OCP_CTL, 0x97),
	WCD9320_REG_VAL(WCD9320_A_SPKR_DRV_CLIP_DET, 0x1),
	WCD9320_REG_VAL(WCD9320_A_SPKR_DRV_IEC, 0x0),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX1_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX2_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX3_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX4_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX5_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX6_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX7_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX8_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX9_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_TX10_MUX_CTL, 0x48),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX1_B4_CTL, 0x8),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX2_B4_CTL, 0x8),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX3_B4_CTL, 0x8),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX4_B4_CTL, 0x8),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX5_B4_CTL, 0x8),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX6_B4_CTL, 0x8),
	WCD9320_REG_VAL(WCD9320_A_CDC_RX7_B4_CTL, 0x8),
	WCD9320_REG_VAL(WCD9320_A_CDC_VBAT_GAIN_UPD_MON, 0x0),
	WCD9320_REG_VAL(WCD9320_A_CDC_PA_RAMP_B1_CTL, 0x0),
	WCD9320_REG_VAL(WCD9320_A_CDC_PA_RAMP_B2_CTL, 0x0),
	WCD9320_REG_VAL(WCD9320_A_CDC_PA_RAMP_B3_CTL, 0x0),
	WCD9320_REG_VAL(WCD9320_A_CDC_PA_RAMP_B4_CTL, 0x0),
	WCD9320_REG_VAL(WCD9320_A_CDC_SPKR_CLIPDET_B1_CTL, 0x0),
	WCD9320_REG_VAL(WCD9320_A_CDC_COMP0_B4_CTL, 0x37),
	WCD9320_REG_VAL(WCD9320_A_CDC_COMP0_B5_CTL, 0x7f),
	WCD9320_REG_VAL(WCD9320_A_CDC_COMP0_B5_CTL, 0x7f),
};

static const struct wcd9320_reg_mask_val wcd9320_codec_reg_init_val[] = {
	/* Initialize current threshold to 350MA
	 * number of wait and run cycles to 4096
	 */
	{WCD9320_A_RX_HPH_OCP_CTL, 0xE1, 0x61},
	{WCD9320_A_RX_COM_OCP_COUNT, 0xFF, 0xFF},
	{WCD9320_A_RX_HPH_L_TEST, 0x01, 0x01},
	{WCD9320_A_RX_HPH_R_TEST, 0x01, 0x01},

	/* Initialize gain registers to use register gain */
	{WCD9320_A_RX_HPH_L_GAIN, 0x20, 0x20},
	{WCD9320_A_RX_HPH_R_GAIN, 0x20, 0x20},
	{WCD9320_A_RX_LINE_1_GAIN, 0x20, 0x20},
	{WCD9320_A_RX_LINE_2_GAIN, 0x20, 0x20},
	{WCD9320_A_RX_LINE_3_GAIN, 0x20, 0x20},
	{WCD9320_A_RX_LINE_4_GAIN, 0x20, 0x20},
	{WCD9320_A_SPKR_DRV_GAIN, 0x04, 0x04},

	/* Use 16 bit sample size for TX1 to TX6 */
	{WCD9320_A_CDC_CONN_TX_SB_B1_CTL, 0x30, 0x20},
	{WCD9320_A_CDC_CONN_TX_SB_B2_CTL, 0x30, 0x20},
	{WCD9320_A_CDC_CONN_TX_SB_B3_CTL, 0x30, 0x20},
	{WCD9320_A_CDC_CONN_TX_SB_B4_CTL, 0x30, 0x20},
	{WCD9320_A_CDC_CONN_TX_SB_B5_CTL, 0x30, 0x20},
	{WCD9320_A_CDC_CONN_TX_SB_B6_CTL, 0x30, 0x20},

	/* Use 16 bit sample size for TX7 to TX10 */
	{WCD9320_A_CDC_CONN_TX_SB_B7_CTL, 0x60, 0x40},
	{WCD9320_A_CDC_CONN_TX_SB_B8_CTL, 0x60, 0x40},
	{WCD9320_A_CDC_CONN_TX_SB_B9_CTL, 0x60, 0x40},
	{WCD9320_A_CDC_CONN_TX_SB_B10_CTL, 0x60, 0x40},

	/*enable HPF filter for TX paths */
	{WCD9320_A_CDC_TX1_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX2_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX3_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX4_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX5_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX6_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX7_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX8_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX9_MUX_CTL, 0x8, 0x0},
	{WCD9320_A_CDC_TX10_MUX_CTL, 0x8, 0x0},

	/* Compander zone selection */
	{WCD9320_A_CDC_COMP0_B4_CTL, 0x3F, 0x37},
	{WCD9320_A_CDC_COMP1_B4_CTL, 0x3F, 0x37},
	{WCD9320_A_CDC_COMP2_B4_CTL, 0x3F, 0x37},
	{WCD9320_A_CDC_COMP0_B5_CTL, 0x7F, 0x7F},
	{WCD9320_A_CDC_COMP1_B5_CTL, 0x7F, 0x7F},
	{WCD9320_A_CDC_COMP2_B5_CTL, 0x7F, 0x7F},

	/*
	 * Setup wavegen timer to 20msec and disable chopper
	 * as default. This corresponds to Compander OFF
	 */
	{WCD9320_A_RX_HPH_CNP_WG_CTL, 0xFF, 0xDB},
	{WCD9320_A_RX_HPH_CNP_WG_TIME, 0xFF, 0x58},
	{WCD9320_A_RX_HPH_BIAS_WG_OCP, 0xFF, 0x1A},
	{WCD9320_A_RX_HPH_CHOP_CTL, 0xFF, 0x24},

	/* Choose max non-overlap time for NCP */
	{WCD9320_A_NCP_CLK, 0xFF, 0xFC},

	/* Program the 0.85 volt VBG_REFERENCE */
	{WCD9320_A_BIAS_CURR_CTL_2, 0xFF, 0x04},

	/* set MAD input MIC to DMIC1 */
	{WCD9320_A_CDC_CONN_MAD, 0x0F, 0x08},

	/* set DMIC CLK drive strength to 4mA */
	{WCD9320_A_HDRIVE_OVERRIDE, 0x07, 0x01},
};
static void wcd9320_codec_init(struct snd_soc_component *component)
{
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(wcd9320_codec_reg_init_val); i++)
		snd_soc_component_update_bits(component,
				wcd9320_codec_reg_init_val[i].reg,
				wcd9320_codec_reg_init_val[i].mask,
				wcd9320_codec_reg_init_val[i].val);

	dev_dbg(component->dev, "WCD OSC Freq: %d",
		snd_soc_component_read(component, WCD9XXX_A_RC_OSC_FREQ));
	/* Set voltage level and always use LDO */
	snd_soc_component_update_bits(component, WCD9320_A_LDO_H_MODE_1, 0x0C,
				(0x03 << 2));


	snd_soc_component_update_bits(component, WCD9320_A_MICB_CFILT_1_VAL, 0xFC, (0x18 << 2));
	snd_soc_component_update_bits(component, WCD9320_A_MICB_CFILT_2_VAL, 0xFC, (0x26 << 2));
	snd_soc_component_update_bits(component, WCD9320_A_MICB_CFILT_3_VAL, 0xFC, (0x26 << 2));


	snd_soc_component_update_bits(component, WCD9320_A_MICB_1_CTL, 0x60,
			    (0x00 << 5));
	snd_soc_component_update_bits(component, WCD9320_A_MICB_2_CTL, 0x60,
			    (0x01 << 5));
	snd_soc_component_update_bits(component, WCD9320_A_MICB_3_CTL, 0x60,
			    (0x02 << 5));
	snd_soc_component_update_bits(component, WCD9320_A_MICB_4_CTL, 0x60,
			    (0x03 << 5));


	snd_soc_component_write(component, WCD9320_A_BIAS_REF_CTL,
				     0x1C);

	/* Set micbias capless mode with tail current */
	snd_soc_component_update_bits(component, WCD9320_A_MICB_1_CTL, 0x1E, 0x16);
	snd_soc_component_update_bits(component, WCD9320_A_MICB_2_CTL, 0x1E, 0x16);
	snd_soc_component_update_bits(component, WCD9320_A_MICB_3_CTL, 0x1E, 0x16);
	snd_soc_component_update_bits(component, WCD9320_A_MICB_4_CTL, 0x1E, 0x16);

	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX1_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX2_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX3_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX4_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX5_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX6_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX7_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX8_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX9_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_TX10_DMIC_CTL,
		0x7, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_DMIC_B1_CTL,
		0xEE, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_CLK_DMIC_B2_CTL,
		0xE, 0x00);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_ANC1_B2_CTL,
		0x1, 0x01);
	snd_soc_component_update_bits(component, WCD9320_A_CDC_ANC2_B2_CTL,
		0x01,  0x01);


	for (i = 0; i < WCD9XXX_SLIM_NUM_PORT_REG; i++)
		regmap_write(wcd->if_regmap, WCD9320_SLIM_PGD_PORT_INT_EN0 + i,
			     0xFF);

	 snd_soc_component_update_bits(component, WCD9XXX_A_MICB_CFILT_2_CTL, 0x80, 0);

}

static void wcd9320_update_reg_defaults(struct wcd9320_priv *wcd)
{
	u32 i;

	for (i = 0; i < ARRAY_SIZE(wcd9320_codec_reg_common_defaults); i++)
		regmap_write(wcd->regmap,
			     wcd9320_codec_reg_common_defaults[i].reg,
			     wcd9320_codec_reg_common_defaults[i].val);

	for (i = 0; i < ARRAY_SIZE(wcd9320_codec_reg_2_0_defaults); i++)
		regmap_write(wcd->regmap,
			     wcd9320_codec_reg_2_0_defaults[i].reg,
			     wcd9320_codec_reg_2_0_defaults[i].val);
}

static int wcd9320_codec_slim_reserve_bw(struct snd_soc_component *component,
		u32 bw_ops, bool commit)
{
	return 0;
}

static int wcd9320_codec_vote_max_bw(struct snd_soc_component *component,
			bool vote)
{
	return wcd9320_codec_slim_reserve_bw(component,
			vote ? SLIM_BW_CLK_GEAR_9 : SLIM_BW_UNVOTE, true);
}

static int wcd9320_codec_set_sysclk(struct snd_soc_component *comp,
				    int clk_id, int source,
				    unsigned int freq, int dir)
{
	return 0;
}

static int wcd9320_codec_probe(struct snd_soc_component *component)
{
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);
	int i;

	snd_soc_component_init_regmap(component, wcd->regmap);
	/* Class-H Init*/
	wcd_clsh_init(&wcd->clsh_d);
	/* Default HPH Mode to Class-H HiFi */
	wcd->hph_mode = CLS_H_HIFI;
	wcd->component = component;

	snd_soc_component_update_bits(component, WCD9320_A_CHIP_CTL,
				      0x06, 0x02);

	wcd9320_codec_init(component);

	for (i = 0; i < NUM_CODEC_DAIS; i++) {
		dev_dbg(component->dev, "WCD dai %d", i);
		INIT_LIST_HEAD(&wcd->dai[i].wcd_slim_ch_list);
		init_waitqueue_head(&wcd->dai[i].dai_wait);
	}

	/*
	 * Diagnostic-only digital loopback: always carry the single TX3
	 * channel on AIF1 CAP (no selectable mux/kcontrol -- this capture
	 * path exists purely to check whether RX7 MIX1's data is reaching
	 * the codec's digital core, for the loudspeaker-silence investigation).
	 */
	list_add_tail(&wcd->slim_data->tx_chs[2].list,
		      &wcd->dai[AIF1_CAP].wcd_slim_ch_list);
	/* Tap RX1 MIX1 (RMIX1) onto the loopback TX channel. */
	snd_soc_component_write(component, WCD9320_A_CDC_CONN_TX_SB_B3_CTL, 0x01);

	return 0;
}

static void wcd9320_codec_remove(struct snd_soc_component *comp)
{
}

static const struct snd_soc_component_driver wcd9320_component_drv = {
	.probe = wcd9320_codec_probe,
	.remove = wcd9320_codec_remove,
	.set_sysclk = wcd9320_codec_set_sysclk,
	.controls = wcd9320_snd_controls,
	.num_controls = ARRAY_SIZE(wcd9320_snd_controls),
	.dapm_widgets = wcd9320_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(wcd9320_dapm_widgets),
	.dapm_routes = wcd9320_audio_map,
	.num_dapm_routes = ARRAY_SIZE(wcd9320_audio_map),
};

static int wcd9320_probe(struct platform_device *pdev)
{
	struct wcd9320 *control = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	int ret = 0, i;
	struct wcd9320_priv *wcd;

	wcd = devm_kzalloc(dev, sizeof(*wcd), GFP_KERNEL);
	if (!wcd)
		return -ENOMEM;

	wcd->slim_data = &control->slim_data;

	wcd->slim_data->rx_chs = devm_kzalloc(dev, sizeof(wcd9320_rx_chs), GFP_KERNEL);
	if (!wcd->slim_data->rx_chs)
		return -ENOMEM;

	memcpy(wcd->slim_data->rx_chs, wcd9320_rx_chs, sizeof(wcd9320_rx_chs));
	for (i = 0; i < ARRAY_SIZE(wcd9320_rx_chs); i++)
		INIT_LIST_HEAD(&wcd->slim_data->rx_chs[i].list);

	wcd->slim_data->tx_chs = devm_kzalloc(dev, sizeof(wcd9320_tx_chs), GFP_KERNEL);
	if (!wcd->slim_data->tx_chs)
		return -ENOMEM;

	memcpy(wcd->slim_data->tx_chs, wcd9320_tx_chs, sizeof(wcd9320_tx_chs));
	for (i = 0; i < ARRAY_SIZE(wcd9320_tx_chs); i++)
		INIT_LIST_HEAD(&wcd->slim_data->tx_chs[i].list);

	dev_set_drvdata(dev, wcd);

	wcd->regmap = control->regmap;
	wcd->if_regmap = control->ifd_regmap;
	wcd->slim = control->slim;
	wcd->slim_slave = control->slim_slave;
	wcd->version = control->version;
	wcd->intf_type = control->intf_type;
	wcd->dev = dev;
	wcd->codec_clk = control->codec_clk;

	mutex_init(&wcd->codec_bg_clk_lock);
	mutex_init(&wcd->master_bias_lock);
	set_bit(AUDIO_NOMINAL, &wcd->status_mask);

	/* Speaker driver supply is optional */
	wcd->spkdrv_reg = devm_regulator_get_optional(dev, "cdc-vdd-spkdrv");
	if (IS_ERR(wcd->spkdrv_reg))
		wcd->spkdrv_reg = NULL;

	ret = snd_soc_register_component(dev, &wcd9320_component_drv,
					 wcd9320_slim_dai,
					 ARRAY_SIZE(wcd9320_slim_dai));

	if (ret) {
		dev_err(dev, "Codec registration failed\n");
		return ret;
	}

	/* Update codec register default values */
	wcd9320_update_reg_defaults(wcd);

	return ret;

}

static int wcd9320_codec_enable_aux_pga(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct wcd9320_priv *wcd = dev_get_drvdata(component->dev);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		_wcd9320_codec_enable_mclk(component, true);
		break;

	case SND_SOC_DAPM_POST_PMD:
		wcd9320_cdc_req_rco_enable(wcd, true);
		break;
	}
	return 0;
}

static void wcd9320_remove(struct platform_device *pdev)
{
	struct wcd9320_priv *wcd;

	wcd = platform_get_drvdata(pdev);

	clk_put(wcd->codec_clk);
	devm_kfree(&pdev->dev, wcd);
	snd_soc_unregister_component(&pdev->dev);
}

static const struct of_device_id wcd9320_of_match[] = {
	{ .compatible = "qcom,wcd9320", },
	{}
};
struct platform_driver wcd9320_codec_driver = {
	.probe = wcd9320_probe,
	.remove = wcd9320_remove,
	.driver = {
		.name = "wcd9320_codec",
		.owner = THIS_MODULE,
		.of_match_table = wcd9320_of_match,
	},
};

MODULE_DESCRIPTION("WCD9320 Codec driver");
MODULE_LICENSE("GPL v2");
