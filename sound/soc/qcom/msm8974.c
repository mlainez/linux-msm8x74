// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, the FP2 mainline audio port.
 *
 * MSM8974 ASoC machine driver: binds the q6afe (qdsp6) SLIMbus DAIs to
 * the WCD9320 (Taiko) codec so a sound card can be created on devices
 * such as the Fairphone 2.
 */

#include <dt-bindings/sound/qcom,q6afe.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <uapi/linux/input-event-codes.h>
#include "common.h"
#include "qdsp6/q6afe.h"

#define DRIVER_NAME		"msm8974"
#define DEFAULT_SAMPLE_RATE_48K	48000
#define WCD9320_DEFAULT_MCLK_RATE	9600000
#define SLIM_MAX_TX_PORTS	16
#define SLIM_MAX_RX_PORTS	13

struct msm8974_snd_data {
	struct snd_soc_jack jack;
	bool jack_setup;
	bool slim_port_setup;
	struct snd_soc_card *card;
};

static struct snd_soc_jack_pin msm8974_jack_pins[] = {
	{
		.pin = "Headphone Jack",
		.mask = SND_JACK_HEADPHONE,
	},
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
};

static int msm8974_slim_snd_hw_params(struct snd_pcm_substream *substream,
				      struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct snd_soc_dai *codec_dai;
	u32 rx_ch[SLIM_MAX_RX_PORTS], tx_ch[SLIM_MAX_TX_PORTS];
	u32 rx_ch_cnt = 0, tx_ch_cnt = 0;
	int ret = 0, i;

	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_get_channel_map(codec_dai,
				&tx_ch_cnt, tx_ch, &rx_ch_cnt, rx_ch);
		if (ret != 0 && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "failed to get codec chan map, err:%d\n", ret);
			return ret;
		} else if (ret == -ENOTSUPP) {
			/* Ignore unsupported */
			continue;
		}

		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			ret = snd_soc_dai_set_channel_map(cpu_dai, 0, NULL,
							  rx_ch_cnt, rx_ch);
		else
			ret = snd_soc_dai_set_channel_map(cpu_dai, tx_ch_cnt,
							  tx_ch, 0, NULL);
		if (ret != 0 && ret != -ENOTSUPP) {
			dev_err(rtd->dev, "failed to set cpu chan map, err:%d\n", ret);
			return ret;
		}
	}

	return 0;
}

static int msm8974_snd_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX...SLIMBUS_6_TX:
		return msm8974_slim_snd_hw_params(substream, params);
	default:
		break;
	}

	return 0;
}

static int msm8974_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct snd_soc_dai *cpu_dai = snd_soc_rtd_to_cpu(rtd, 0);
	struct msm8974_snd_data *pdata = snd_soc_card_get_drvdata(card);
	struct snd_soc_dai_link *link = rtd->dai_link;
	/*
	 * Codec SLIMBUS configuration
	 * RX1, RX2, RX3, RX4, RX5, RX6, RX7, RX8, RX9, RX10, RX11, RX12, RX13
	 * TX1, TX2, TX3, TX4, TX5, TX6, TX7, TX8, TX9, TX10, TX11, TX12, TX13
	 * TX14, TX15, TX16
	 */
	unsigned int rx_ch[SLIM_MAX_RX_PORTS] = {144, 145, 146, 147, 148, 149,
					150, 151, 152, 153, 154, 155, 156};
	unsigned int tx_ch[SLIM_MAX_TX_PORTS] = {128, 129, 130, 131, 132, 133,
					    134, 135, 136, 137, 138, 139,
					    140, 141, 142, 143};
	struct snd_jack *jack;
	int rval, i;

	if (!pdata->jack_setup) {
		rval = snd_soc_card_jack_new_pins(card, "Headset Jack",
						  SND_JACK_HEADSET |
						  SND_JACK_HEADPHONE |
						  SND_JACK_BTN_0 | SND_JACK_BTN_1 |
						  SND_JACK_BTN_2 | SND_JACK_BTN_3,
						  &pdata->jack,
						  msm8974_jack_pins,
						  ARRAY_SIZE(msm8974_jack_pins));
		if (rval < 0) {
			dev_err(card->dev, "Unable to add Headset Jack\n");
			return rval;
		}

		jack = pdata->jack.jack;

		snd_jack_set_key(jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
		snd_jack_set_key(jack, SND_JACK_BTN_1, KEY_VOICECOMMAND);
		snd_jack_set_key(jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
		snd_jack_set_key(jack, SND_JACK_BTN_3, KEY_VOLUMEDOWN);
		pdata->jack_setup = true;
	}

	switch (cpu_dai->id) {
	case SLIMBUS_0_RX...SLIMBUS_6_TX:
		/* setting up wcd multiple times for slim port is redundant */
		if (pdata->slim_port_setup || !link->no_pcm)
			return 0;

		for_each_rtd_codec_dais(rtd, i, codec_dai) {
			rval = snd_soc_dai_set_channel_map(codec_dai,
							   ARRAY_SIZE(tx_ch),
							   tx_ch,
							   ARRAY_SIZE(rx_ch),
							   rx_ch);
			if (rval != 0 && rval != -ENOTSUPP)
				return rval;

			snd_soc_dai_set_sysclk(codec_dai, 0,
					       WCD9320_DEFAULT_MCLK_RATE,
					       SNDRV_PCM_STREAM_PLAYBACK);

			rval = snd_soc_component_set_jack(codec_dai->component,
							  &pdata->jack, NULL);
			if (rval != 0 && rval != -ENOTSUPP) {
				dev_warn(card->dev, "Failed to set jack: %d\n", rval);
				return rval;
			}
		}

		pdata->slim_port_setup = true;
		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_ops msm8974_be_ops = {
	.hw_params = msm8974_snd_hw_params,
};

static int msm8974_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	struct snd_mask *fmt = hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT);

	rate->min = rate->max = DEFAULT_SAMPLE_RATE_48K;
	channels->min = channels->max = 2;
	snd_mask_set_format(fmt, SNDRV_PCM_FORMAT_S16_LE);

	return 0;
}

static const struct snd_soc_dapm_widget msm8974_snd_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_MIC("Handset Mic", NULL),
};

static const struct snd_kcontrol_new msm8974_snd_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone Jack"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static void msm8974_add_ops(struct snd_soc_card *card)
{
	struct snd_soc_dai_link *link;
	int i;

	for_each_card_prelinks(card, i, link) {
		if (link->no_pcm == 1) {
			link->ops = &msm8974_be_ops;
			link->be_hw_params_fixup = msm8974_be_hw_params_fixup;
		}
		link->init = msm8974_dai_init;
	}
}

static int msm8974_snd_platform_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card;
	struct msm8974_snd_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	card = devm_kzalloc(dev, sizeof(*card), GFP_KERNEL);
	if (!card)
		return -ENOMEM;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	card->driver_name = DRIVER_NAME;
	card->dapm_widgets = msm8974_snd_widgets;
	card->num_dapm_widgets = ARRAY_SIZE(msm8974_snd_widgets);
	card->controls = msm8974_snd_controls;
	card->num_controls = ARRAY_SIZE(msm8974_snd_controls);
	card->dev = dev;
	card->owner = THIS_MODULE;
	dev_set_drvdata(dev, card);

	ret = qcom_snd_parse_of(card);
	if (ret)
		return ret;

	data->card = card;
	snd_soc_card_set_drvdata(card, data);

	msm8974_add_ops(card);

	return devm_snd_soc_register_card(dev, card);
}

static const struct of_device_id msm8974_snd_device_id[] = {
	{ .compatible = "qcom,msm8974-sndcard" },
	{ }
};
MODULE_DEVICE_TABLE(of, msm8974_snd_device_id);

static struct platform_driver msm8974_snd_driver = {
	.probe = msm8974_snd_platform_probe,
	.driver = {
		.name = "msm8974-sndcard",
		.of_match_table = msm8974_snd_device_id,
	},
};
module_platform_driver(msm8974_snd_driver);

MODULE_DESCRIPTION("MSM8974 ASoC Machine Driver");
MODULE_LICENSE("GPL");
