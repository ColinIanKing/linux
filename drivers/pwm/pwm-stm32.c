// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics 2016
 *
 * Author: Gerald Baeza <gerald.baeza@st.com>
 *
 * Inspired by timer-stm32.c from Maxime Coquelin
 *             pwm-atmel.c from Bo Shen
 */

#include <linux/bitfield.h>
#include <linux/mfd/stm32-timers.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define CCMR_CHANNEL_SHIFT 8
#define CCMR_CHANNEL_MASK  0xFF
#define MAX_BREAKINPUT 2
#define STM32_MAX_PWM_OUTPUT 4

struct stm32_breakinput {
	u32 index;
	u32 level;
	u32 filter;
};

struct stm32_pwm {
	struct mutex lock; /* protect pwm config/enable */
	struct clk *clk;
	struct regmap *regmap;
	u32 max_arr;
	bool have_complementary_output;
	struct stm32_breakinput breakinputs[MAX_BREAKINPUT];
	unsigned int num_breakinputs;
	u32 capture[4] ____cacheline_aligned; /* DMA'able buffer */
};

static inline struct stm32_pwm *to_stm32_pwm_dev(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static u32 active_channels(struct stm32_pwm *dev)
{
	u32 ccer;

	regmap_read(dev->regmap, TIM_CCER, &ccer);

	return ccer & TIM_CCER_CCXE;
}

struct stm32_pwm_waveform {
	u32 ccer;
	u32 psc;
	u32 arr;
	u32 ccr;
};

static int stm32_pwm_round_waveform_tohw(struct pwm_chip *chip,
					 struct pwm_device *pwm,
					 const struct pwm_waveform *wf,
					 void *_wfhw)
{
	struct stm32_pwm_waveform *wfhw = _wfhw;
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	unsigned int ch = pwm->hwpwm;
	unsigned long rate;
	u64 ccr, duty;
	int ret;

	if (wf->period_length_ns == 0) {
		*wfhw = (struct stm32_pwm_waveform){
			.ccer = 0,
		};

		return 0;
	}

	ret = clk_enable(priv->clk);
	if (ret)
		return ret;

	wfhw->ccer = TIM_CCER_CCxE(ch + 1);
	if (priv->have_complementary_output)
		wfhw->ccer |= TIM_CCER_CCxNE(ch + 1);

	rate = clk_get_rate(priv->clk);

	if (active_channels(priv) & ~TIM_CCER_CCxE(ch + 1)) {
		u64 arr;

		/*
		 * Other channels are already enabled, so the configured PSC and
		 * ARR must be used for this channel, too.
		 */
		ret = regmap_read(priv->regmap, TIM_PSC, &wfhw->psc);
		if (ret)
			goto out;

		ret = regmap_read(priv->regmap, TIM_ARR, &wfhw->arr);
		if (ret)
			goto out;

		arr = mul_u64_u64_div_u64(wf->period_length_ns, rate,
					  (u64)NSEC_PER_SEC * (wfhw->psc + 1));
		if (arr <= wfhw->arr) {
			/*
			 * requested period is smaller than the currently
			 * configured and unchangable period, report back the smallest
			 * possible period, i.e. the current state and return 1
			 * to indicate the wrong rounding direction.
			 */
			ret = 1;
		}

	} else {
		/*
		 * .probe() asserted that clk_get_rate() is not bigger than 1 GHz, so
		 * the calculations here won't overflow.
		 * First we need to find the minimal value for prescaler such that
		 *
		 *        period_ns * clkrate
		 *   ------------------------------ < max_arr + 1
		 *   NSEC_PER_SEC * (prescaler + 1)
		 *
		 * This equation is equivalent to
		 *
		 *        period_ns * clkrate
		 *   ---------------------------- < prescaler + 1
		 *   NSEC_PER_SEC * (max_arr + 1)
		 *
		 * Using integer division and knowing that the right hand side is
		 * integer, this is further equivalent to
		 *
		 *   (period_ns * clkrate) // (NSEC_PER_SEC * (max_arr + 1)) ≤ prescaler
		 */
		u64 psc = mul_u64_u64_div_u64(wf->period_length_ns, rate,
					      (u64)NSEC_PER_SEC * ((u64)priv->max_arr + 1));
		u64 arr;

		wfhw->psc = min_t(u64, psc, MAX_TIM_PSC);

		arr = mul_u64_u64_div_u64(wf->period_length_ns, rate,
					  (u64)NSEC_PER_SEC * (wfhw->psc + 1));
		if (!arr) {
			/*
			 * requested period is too small, report back the smallest
			 * possible period, i.e. ARR = 0. The only valid CCR
			 * value is then zero, too.
			 */
			wfhw->arr = 0;
			wfhw->ccr = 0;
			ret = 1;
			goto out;
		}

		/*
		 * ARR is limited intentionally to values less than
		 * priv->max_arr to allow 100% duty cycle.
		 */
		wfhw->arr = min_t(u64, arr, priv->max_arr) - 1;
	}

	duty = mul_u64_u64_div_u64(wf->duty_length_ns, rate,
				   (u64)NSEC_PER_SEC * (wfhw->psc + 1));
	duty = min_t(u64, duty, wfhw->arr + 1);

	if (wf->duty_length_ns && wf->duty_offset_ns &&
	    wf->duty_length_ns + wf->duty_offset_ns >= wf->period_length_ns) {
		wfhw->ccer |= TIM_CCER_CCxP(ch + 1);
		if (priv->have_complementary_output)
			wfhw->ccer |= TIM_CCER_CCxNP(ch + 1);

		ccr = wfhw->arr + 1 - duty;
	} else {
		ccr = duty;
	}

	wfhw->ccr = min_t(u64, ccr, wfhw->arr + 1);

out:
	dev_dbg(&chip->dev, "pwm#%u: %lld/%lld [+%lld] @%lu -> CCER: %08x, PSC: %08x, ARR: %08x, CCR: %08x\n",
		pwm->hwpwm, wf->duty_length_ns, wf->period_length_ns, wf->duty_offset_ns,
		rate, wfhw->ccer, wfhw->psc, wfhw->arr, wfhw->ccr);

	clk_disable(priv->clk);

	return ret;
}

/*
 * This should be moved to lib/math/div64.c. Currently there are some changes
 * pending to mul_u64_u64_div_u64. Uwe will care for that when the dust settles.
 */
static u64 stm32_pwm_mul_u64_u64_div_u64_roundup(u64 a, u64 b, u64 c)
{
	u64 res = mul_u64_u64_div_u64(a, b, c);
	/* Those multiplications might overflow but it doesn't matter */
	u64 rem = a * b - c * res;

	if (rem)
		res += 1;

	return res;
}

static int stm32_pwm_round_waveform_fromhw(struct pwm_chip *chip,
					   struct pwm_device *pwm,
					   const void *_wfhw,
					   struct pwm_waveform *wf)
{
	const struct stm32_pwm_waveform *wfhw = _wfhw;
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	unsigned long rate = clk_get_rate(priv->clk);
	unsigned int ch = pwm->hwpwm;

	if (wfhw->ccer & TIM_CCER_CCxE(ch + 1)) {
		u64 ccr_ns;

		/* The result doesn't overflow for rate >= 15259 */
		wf->period_length_ns = stm32_pwm_mul_u64_u64_div_u64_roundup(((u64)wfhw->psc + 1) * (wfhw->arr + 1),
									     NSEC_PER_SEC, rate);

		ccr_ns = stm32_pwm_mul_u64_u64_div_u64_roundup(((u64)wfhw->psc + 1) * wfhw->ccr,
							       NSEC_PER_SEC, rate);

		if (wfhw->ccer & TIM_CCER_CCxP(ch + 1)) {
			wf->duty_length_ns =
				stm32_pwm_mul_u64_u64_div_u64_roundup(((u64)wfhw->psc + 1) * (wfhw->arr + 1 - wfhw->ccr),
								      NSEC_PER_SEC, rate);

			wf->duty_offset_ns = ccr_ns;
		} else {
			wf->duty_length_ns = ccr_ns;
			wf->duty_offset_ns = 0;
		}
	} else {
		*wf = (struct pwm_waveform){
			.period_length_ns = 0,
		};
	}

	dev_dbg(&chip->dev, "pwm#%u: CCER: %08x, PSC: %08x, ARR: %08x, CCR: %08x @%lu -> %lld/%lld [+%lld]\n",
		pwm->hwpwm, wfhw->ccer, wfhw->psc, wfhw->arr, wfhw->ccr, rate,
		wf->duty_length_ns, wf->period_length_ns, wf->duty_offset_ns);

	return 0;
}

static int stm32_pwm_read_waveform(struct pwm_chip *chip,
				     struct pwm_device *pwm,
				     void *_wfhw)
{
	struct stm32_pwm_waveform *wfhw = _wfhw;
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	unsigned int ch = pwm->hwpwm;
	int ret;

	ret = clk_enable(priv->clk);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, TIM_CCER, &wfhw->ccer);
	if (ret)
		goto out;

	if (wfhw->ccer & TIM_CCER_CCxE(ch + 1)) {
		ret = regmap_read(priv->regmap, TIM_PSC, &wfhw->psc);
		if (ret)
			goto out;

		ret = regmap_read(priv->regmap, TIM_ARR, &wfhw->arr);
		if (ret)
			goto out;

		if (wfhw->arr == U32_MAX)
			wfhw->arr -= 1;

		ret = regmap_read(priv->regmap, TIM_CCRx(ch + 1), &wfhw->ccr);
		if (ret)
			goto out;

		if (wfhw->ccr > wfhw->arr + 1)
			wfhw->ccr = wfhw->arr + 1;
	}

out:
	clk_disable(priv->clk);

	return ret;
}

static int stm32_pwm_write_waveform(struct pwm_chip *chip,
				      struct pwm_device *pwm,
				      const void *_wfhw)
{
	const struct stm32_pwm_waveform *wfhw = _wfhw;
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	unsigned int ch = pwm->hwpwm;
	int ret;

	ret = clk_enable(priv->clk);
	if (ret)
		return ret;

	if (wfhw->ccer & TIM_CCER_CCxE(ch + 1)) {
		u32 ccer, mask;
		unsigned int shift;
		u32 ccmr;

		ret = regmap_read(priv->regmap, TIM_CCER, &ccer);
		if (ret)
			goto out;

		/* If there are other channels enabled, don't update PSC and ARR */
		if (ccer & ~TIM_CCER_CCxE(ch + 1) & TIM_CCER_CCXE) {
			u32 psc, arr;

			ret = regmap_read(priv->regmap, TIM_PSC, &psc);
			if (ret)
				goto out;

			if (psc != wfhw->psc) {
				ret = -EBUSY;
				goto out;
			}

			ret = regmap_read(priv->regmap, TIM_ARR, &arr);
			if (ret)
				goto out;

			if (arr != wfhw->arr) {
				ret = -EBUSY;
				goto out;
			}
		} else {
			ret = regmap_write(priv->regmap, TIM_PSC, wfhw->psc);
			if (ret)
				goto out;

			ret = regmap_write(priv->regmap, TIM_ARR, wfhw->arr);
			if (ret)
				goto out;

			ret = regmap_set_bits(priv->regmap, TIM_CR1, TIM_CR1_ARPE);
			if (ret)
				goto out;

		}

		/* set polarity */
		mask = TIM_CCER_CCxP(ch + 1) | TIM_CCER_CCxNP(ch + 1);
		ret = regmap_update_bits(priv->regmap, TIM_CCER, mask, wfhw->ccer);
		if (ret)
			goto out;

		ret = regmap_write(priv->regmap, TIM_CCRx(ch + 1), wfhw->ccr);
		if (ret)
			goto out;

		/* Configure output mode */
		shift = (ch & 0x1) * CCMR_CHANNEL_SHIFT;
		ccmr = (TIM_CCMR_PE | TIM_CCMR_M1) << shift;
		mask = CCMR_CHANNEL_MASK << shift;

		if (ch < 2)
			ret = regmap_update_bits(priv->regmap, TIM_CCMR1, mask, ccmr);
		else
			ret = regmap_update_bits(priv->regmap, TIM_CCMR2, mask, ccmr);
		if (ret)
			goto out;

		ret = regmap_set_bits(priv->regmap, TIM_BDTR, TIM_BDTR_MOE);
		if (ret)
			goto out;

		if (!(ccer & TIM_CCER_CCxE(ch + 1))) {
			mask = TIM_CCER_CCxE(ch + 1) | TIM_CCER_CCxNE(ch + 1);

			ret = clk_enable(priv->clk);
			if (ret)
				goto out;

			ccer = (ccer & ~mask) | (wfhw->ccer & mask);
			regmap_write(priv->regmap, TIM_CCER, ccer);

			/* Make sure that registers are updated */
			regmap_set_bits(priv->regmap, TIM_EGR, TIM_EGR_UG);

			/* Enable controller */
			regmap_set_bits(priv->regmap, TIM_CR1, TIM_CR1_CEN);
		}

	} else {
		/* disable channel */
		u32 mask, ccer;

		mask = TIM_CCER_CCxE(ch + 1);
		if (priv->have_complementary_output)
			mask |= TIM_CCER_CCxNE(ch + 1);

		ret = regmap_read(priv->regmap, TIM_CCER, &ccer);
		if (ret)
			goto out;

		if (ccer & mask) {
			ccer = ccer & ~mask;

			ret = regmap_write(priv->regmap, TIM_CCER, ccer);
			if (ret)
				goto out;

			if (!(ccer & TIM_CCER_CCXE)) {
				/* When all channels are disabled, we can disable the controller */
				ret = regmap_clear_bits(priv->regmap, TIM_CR1, TIM_CR1_CEN);
				if (ret)
					goto out;
			}

			clk_disable(priv->clk);
		}
	}

out:
	clk_disable(priv->clk);

	return ret;
}

#define TIM_CCER_CC12P (TIM_CCER_CC1P | TIM_CCER_CC2P)
#define TIM_CCER_CC12E (TIM_CCER_CC1E | TIM_CCER_CC2E)
#define TIM_CCER_CC34P (TIM_CCER_CC3P | TIM_CCER_CC4P)
#define TIM_CCER_CC34E (TIM_CCER_CC3E | TIM_CCER_CC4E)

/*
 * Capture using PWM input mode:
 *                              ___          ___
 * TI[1, 2, 3 or 4]: ........._|   |________|
 *                             ^0  ^1       ^2
 *                              .   .        .
 *                              .   .        XXXXX
 *                              .   .   XXXXX     |
 *                              .  XXXXX     .    |
 *                            XXXXX .        .    |
 * COUNTER:        ______XXXXX  .   .        .    |_XXX
 *                 start^       .   .        .        ^stop
 *                      .       .   .        .
 *                      v       v   .        v
 *                                  v
 * CCR1/CCR3:       tx..........t0...........t2
 * CCR2/CCR4:       tx..............t1.........
 *
 * DMA burst transfer:          |            |
 *                              v            v
 * DMA buffer:                  { t0, tx }   { t2, t1 }
 * DMA done:                                 ^
 *
 * 0: IC1/3 snapchot on rising edge: counter value -> CCR1/CCR3
 *    + DMA transfer CCR[1/3] & CCR[2/4] values (t0, tx: doesn't care)
 * 1: IC2/4 snapchot on falling edge: counter value -> CCR2/CCR4
 * 2: IC1/3 snapchot on rising edge: counter value -> CCR1/CCR3
 *    + DMA transfer CCR[1/3] & CCR[2/4] values (t2, t1)
 *
 * DMA done, compute:
 * - Period     = t2 - t0
 * - Duty cycle = t1 - t0
 */
static int stm32_pwm_raw_capture(struct pwm_chip *chip, struct pwm_device *pwm,
				 unsigned long tmo_ms, u32 *raw_prd,
				 u32 *raw_dty)
{
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	struct device *parent = pwmchip_parent(chip)->parent;
	enum stm32_timers_dmas dma_id;
	u32 ccen, ccr;
	int ret;

	/* Ensure registers have been updated, enable counter and capture */
	regmap_set_bits(priv->regmap, TIM_EGR, TIM_EGR_UG);
	regmap_set_bits(priv->regmap, TIM_CR1, TIM_CR1_CEN);

	/* Use cc1 or cc3 DMA resp for PWM input channels 1 & 2 or 3 & 4 */
	dma_id = pwm->hwpwm < 2 ? STM32_TIMERS_DMA_CH1 : STM32_TIMERS_DMA_CH3;
	ccen = pwm->hwpwm < 2 ? TIM_CCER_CC12E : TIM_CCER_CC34E;
	ccr = pwm->hwpwm < 2 ? TIM_CCR1 : TIM_CCR3;
	regmap_set_bits(priv->regmap, TIM_CCER, ccen);

	/*
	 * Timer DMA burst mode. Request 2 registers, 2 bursts, to get both
	 * CCR1 & CCR2 (or CCR3 & CCR4) on each capture event.
	 * We'll get two capture snapchots: { CCR1, CCR2 }, { CCR1, CCR2 }
	 * or { CCR3, CCR4 }, { CCR3, CCR4 }
	 */
	ret = stm32_timers_dma_burst_read(parent, priv->capture, dma_id, ccr, 2,
					  2, tmo_ms);
	if (ret)
		goto stop;

	/* Period: t2 - t0 (take care of counter overflow) */
	if (priv->capture[0] <= priv->capture[2])
		*raw_prd = priv->capture[2] - priv->capture[0];
	else
		*raw_prd = priv->max_arr - priv->capture[0] + priv->capture[2];

	/* Duty cycle capture requires at least two capture units */
	if (pwm->chip->npwm < 2)
		*raw_dty = 0;
	else if (priv->capture[0] <= priv->capture[3])
		*raw_dty = priv->capture[3] - priv->capture[0];
	else
		*raw_dty = priv->max_arr - priv->capture[0] + priv->capture[3];

	if (*raw_dty > *raw_prd) {
		/*
		 * Race beetween PWM input and DMA: it may happen
		 * falling edge triggers new capture on TI2/4 before DMA
		 * had a chance to read CCR2/4. It means capture[1]
		 * contains period + duty_cycle. So, subtract period.
		 */
		*raw_dty -= *raw_prd;
	}

stop:
	regmap_clear_bits(priv->regmap, TIM_CCER, ccen);
	regmap_clear_bits(priv->regmap, TIM_CR1, TIM_CR1_CEN);

	return ret;
}

static int stm32_pwm_capture(struct pwm_chip *chip, struct pwm_device *pwm,
			     struct pwm_capture *result, unsigned long tmo_ms)
{
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	unsigned long long prd, div, dty;
	unsigned long rate;
	unsigned int psc = 0, icpsc, scale;
	u32 raw_prd = 0, raw_dty = 0;
	int ret = 0;

	mutex_lock(&priv->lock);

	if (active_channels(priv)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = clk_enable(priv->clk);
	if (ret) {
		dev_err(pwmchip_parent(chip), "failed to enable counter clock\n");
		goto unlock;
	}

	rate = clk_get_rate(priv->clk);
	if (!rate) {
		ret = -EINVAL;
		goto clk_dis;
	}

	/* prescaler: fit timeout window provided by upper layer */
	div = (unsigned long long)rate * (unsigned long long)tmo_ms;
	do_div(div, MSEC_PER_SEC);
	prd = div;
	while ((div > priv->max_arr) && (psc < MAX_TIM_PSC)) {
		psc++;
		div = prd;
		do_div(div, psc + 1);
	}
	regmap_write(priv->regmap, TIM_ARR, priv->max_arr);
	regmap_write(priv->regmap, TIM_PSC, psc);

	/* Reset input selector to its default input and disable slave mode */
	regmap_write(priv->regmap, TIM_TISEL, 0x0);
	regmap_write(priv->regmap, TIM_SMCR, 0x0);

	/* Map TI1 or TI2 PWM input to IC1 & IC2 (or TI3/4 to IC3 & IC4) */
	regmap_update_bits(priv->regmap,
			   pwm->hwpwm < 2 ? TIM_CCMR1 : TIM_CCMR2,
			   TIM_CCMR_CC1S | TIM_CCMR_CC2S, pwm->hwpwm & 0x1 ?
			   TIM_CCMR_CC1S_TI2 | TIM_CCMR_CC2S_TI2 :
			   TIM_CCMR_CC1S_TI1 | TIM_CCMR_CC2S_TI1);

	/* Capture period on IC1/3 rising edge, duty cycle on IC2/4 falling. */
	regmap_update_bits(priv->regmap, TIM_CCER, pwm->hwpwm < 2 ?
			   TIM_CCER_CC12P : TIM_CCER_CC34P, pwm->hwpwm < 2 ?
			   TIM_CCER_CC2P : TIM_CCER_CC4P);

	ret = stm32_pwm_raw_capture(chip, pwm, tmo_ms, &raw_prd, &raw_dty);
	if (ret)
		goto stop;

	/*
	 * Got a capture. Try to improve accuracy at high rates:
	 * - decrease counter clock prescaler, scale up to max rate.
	 * - use input prescaler, capture once every /2 /4 or /8 edges.
	 */
	if (raw_prd) {
		u32 max_arr = priv->max_arr - 0x1000; /* arbitrary margin */

		scale = max_arr / min(max_arr, raw_prd);
	} else {
		scale = priv->max_arr; /* below resolution, use max scale */
	}

	if (psc && scale > 1) {
		/* 2nd measure with new scale */
		psc /= scale;
		regmap_write(priv->regmap, TIM_PSC, psc);
		ret = stm32_pwm_raw_capture(chip, pwm, tmo_ms, &raw_prd,
					    &raw_dty);
		if (ret)
			goto stop;
	}

	/* Compute intermediate period not to exceed timeout at low rates */
	prd = (unsigned long long)raw_prd * (psc + 1) * NSEC_PER_SEC;
	do_div(prd, rate);

	for (icpsc = 0; icpsc < MAX_TIM_ICPSC ; icpsc++) {
		/* input prescaler: also keep arbitrary margin */
		if (raw_prd >= (priv->max_arr - 0x1000) >> (icpsc + 1))
			break;
		if (prd >= (tmo_ms * NSEC_PER_MSEC) >> (icpsc + 2))
			break;
	}

	if (!icpsc)
		goto done;

	/* Last chance to improve period accuracy, using input prescaler */
	regmap_update_bits(priv->regmap,
			   pwm->hwpwm < 2 ? TIM_CCMR1 : TIM_CCMR2,
			   TIM_CCMR_IC1PSC | TIM_CCMR_IC2PSC,
			   FIELD_PREP(TIM_CCMR_IC1PSC, icpsc) |
			   FIELD_PREP(TIM_CCMR_IC2PSC, icpsc));

	ret = stm32_pwm_raw_capture(chip, pwm, tmo_ms, &raw_prd, &raw_dty);
	if (ret)
		goto stop;

	if (raw_dty >= (raw_prd >> icpsc)) {
		/*
		 * We may fall here using input prescaler, when input
		 * capture starts on high side (before falling edge).
		 * Example with icpsc to capture on each 4 events:
		 *
		 *       start   1st capture                     2nd capture
		 *         v     v                               v
		 *         ___   _____   _____   _____   _____   ____
		 * TI1..4     |__|    |__|    |__|    |__|    |__|
		 *            v  v    .  .    .  .    .       v  v
		 * icpsc1/3:  .  0    .  1    .  2    .  3    .  0
		 * icpsc2/4:  0       1       2       3       0
		 *            v  v                            v  v
		 * CCR1/3  ......t0..............................t2
		 * CCR2/4  ..t1..............................t1'...
		 *               .                            .  .
		 * Capture0:     .<----------------------------->.
		 * Capture1:     .<-------------------------->.  .
		 *               .                            .  .
		 * Period:       .<------>                    .  .
		 * Low side:                                  .<>.
		 *
		 * Result:
		 * - Period = Capture0 / icpsc
		 * - Duty = Period - Low side = Period - (Capture0 - Capture1)
		 */
		raw_dty = (raw_prd >> icpsc) - (raw_prd - raw_dty);
	}

done:
	prd = (unsigned long long)raw_prd * (psc + 1) * NSEC_PER_SEC;
	result->period = DIV_ROUND_UP_ULL(prd, rate << icpsc);
	dty = (unsigned long long)raw_dty * (psc + 1) * NSEC_PER_SEC;
	result->duty_cycle = DIV_ROUND_UP_ULL(dty, rate);
stop:
	regmap_write(priv->regmap, TIM_CCER, 0);
	regmap_write(priv->regmap, pwm->hwpwm < 2 ? TIM_CCMR1 : TIM_CCMR2, 0);
	regmap_write(priv->regmap, TIM_PSC, 0);
clk_dis:
	clk_disable(priv->clk);
unlock:
	mutex_unlock(&priv->lock);

	return ret;
}

static const struct pwm_ops stm32pwm_ops = {
	.sizeof_wfhw = sizeof(struct stm32_pwm_waveform),
	.round_waveform_tohw = stm32_pwm_round_waveform_tohw,
	.round_waveform_fromhw = stm32_pwm_round_waveform_fromhw,
	.read_waveform = stm32_pwm_read_waveform,
	.write_waveform = stm32_pwm_write_waveform,

	.capture = IS_ENABLED(CONFIG_DMA_ENGINE) ? stm32_pwm_capture : NULL,
};

static int stm32_pwm_set_breakinput(struct stm32_pwm *priv,
				    const struct stm32_breakinput *bi)
{
	u32 shift = TIM_BDTR_BKF_SHIFT(bi->index);
	u32 bke = TIM_BDTR_BKE(bi->index);
	u32 bkp = TIM_BDTR_BKP(bi->index);
	u32 bkf = TIM_BDTR_BKF(bi->index);
	u32 mask = bkf | bkp | bke;
	u32 bdtr;

	bdtr = (bi->filter & TIM_BDTR_BKF_MASK) << shift | bke;

	if (bi->level)
		bdtr |= bkp;

	regmap_update_bits(priv->regmap, TIM_BDTR, mask, bdtr);

	regmap_read(priv->regmap, TIM_BDTR, &bdtr);

	return (bdtr & bke) ? 0 : -EINVAL;
}

static int stm32_pwm_apply_breakinputs(struct stm32_pwm *priv)
{
	unsigned int i;
	int ret;

	for (i = 0; i < priv->num_breakinputs; i++) {
		ret = stm32_pwm_set_breakinput(priv, &priv->breakinputs[i]);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int stm32_pwm_probe_breakinputs(struct stm32_pwm *priv,
				       struct device_node *np)
{
	int nb, ret, array_size;
	unsigned int i;

	nb = of_property_count_elems_of_size(np, "st,breakinput",
					     sizeof(struct stm32_breakinput));

	/*
	 * Because "st,breakinput" parameter is optional do not make probe
	 * failed if it doesn't exist.
	 */
	if (nb <= 0)
		return 0;

	if (nb > MAX_BREAKINPUT)
		return -EINVAL;

	priv->num_breakinputs = nb;
	array_size = nb * sizeof(struct stm32_breakinput) / sizeof(u32);
	ret = of_property_read_u32_array(np, "st,breakinput",
					 (u32 *)priv->breakinputs, array_size);
	if (ret)
		return ret;

	for (i = 0; i < priv->num_breakinputs; i++) {
		if (priv->breakinputs[i].index > 1 ||
		    priv->breakinputs[i].level > 1 ||
		    priv->breakinputs[i].filter > 15)
			return -EINVAL;
	}

	return stm32_pwm_apply_breakinputs(priv);
}

static void stm32_pwm_detect_complementary(struct stm32_pwm *priv, struct stm32_timers *ddata)
{
	u32 ccer;

	if (ddata->ipidr) {
		u32 val;

		/* Simply read from HWCFGR the number of complementary outputs (MP25). */
		regmap_read(priv->regmap, TIM_HWCFGR1, &val);
		priv->have_complementary_output = !!FIELD_GET(TIM_HWCFGR1_NB_OF_DT, val);
		return;
	}

	/*
	 * If complementary bit doesn't exist writing 1 will have no
	 * effect so we can detect it.
	 */
	regmap_set_bits(priv->regmap, TIM_CCER, TIM_CCER_CC1NE);
	regmap_read(priv->regmap, TIM_CCER, &ccer);
	regmap_clear_bits(priv->regmap, TIM_CCER, TIM_CCER_CC1NE);

	priv->have_complementary_output = (ccer != 0);
}

static unsigned int stm32_pwm_detect_channels(struct stm32_timers *ddata,
					      unsigned int *num_enabled)
{
	struct regmap *regmap = ddata->regmap;
	u32 ccer, ccer_backup;

	regmap_read(regmap, TIM_CCER, &ccer_backup);
	*num_enabled = hweight32(ccer_backup & TIM_CCER_CCXE);

	if (ddata->ipidr) {
		u32 hwcfgr;
		unsigned int npwm;

		/* Deduce from HWCFGR the number of outputs (MP25). */
		regmap_read(regmap, TIM_HWCFGR1, &hwcfgr);

		/*
		 * Timers may have more capture/compare channels than the
		 * actual number of PWM channel outputs (e.g. TIM_CH[1..4]).
		 */
		npwm = FIELD_GET(TIM_HWCFGR1_NB_OF_CC, hwcfgr);

		return npwm < STM32_MAX_PWM_OUTPUT ? npwm : STM32_MAX_PWM_OUTPUT;
	}

	/*
	 * If channels enable bits don't exist writing 1 will have no
	 * effect so we can detect and count them.
	 */
	regmap_set_bits(regmap, TIM_CCER, TIM_CCER_CCXE);
	regmap_read(regmap, TIM_CCER, &ccer);
	regmap_write(regmap, TIM_CCER, ccer_backup);

	return hweight32(ccer & TIM_CCER_CCXE);
}

static int stm32_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct stm32_timers *ddata = dev_get_drvdata(pdev->dev.parent);
	struct pwm_chip *chip;
	struct stm32_pwm *priv;
	unsigned int npwm, num_enabled;
	unsigned int i;
	int ret;

	npwm = stm32_pwm_detect_channels(ddata, &num_enabled);

	chip = devm_pwmchip_alloc(dev, npwm, sizeof(*priv));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	priv = to_stm32_pwm_dev(chip);

	mutex_init(&priv->lock);
	priv->regmap = ddata->regmap;
	priv->clk = ddata->clk;
	priv->max_arr = ddata->max_arr;

	if (!priv->regmap || !priv->clk)
		return dev_err_probe(dev, -EINVAL, "Failed to get %s\n",
				     priv->regmap ? "clk" : "regmap");

	ret = stm32_pwm_probe_breakinputs(priv, np);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to configure breakinputs\n");

	stm32_pwm_detect_complementary(priv, ddata);

	ret = devm_clk_rate_exclusive_get(dev, priv->clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to lock clock\n");

	/*
	 * With the clk running with not more than 1 GHz the calculations in
	 * .apply() won't overflow.
	 */
	if (clk_get_rate(priv->clk) > 1000000000)
		return dev_err_probe(dev, -EINVAL, "Clock freq too high (%lu)\n",
				     clk_get_rate(priv->clk));

	chip->ops = &stm32pwm_ops;

	/* Initialize clock refcount to number of enabled PWM channels. */
	for (i = 0; i < num_enabled; i++) {
		ret = clk_enable(priv->clk);
		if (ret)
			return ret;
	}

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Failed to register pwmchip\n");

	platform_set_drvdata(pdev, chip);

	return 0;
}

static int stm32_pwm_suspend(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	unsigned int i;
	u32 ccer, mask;

	/* Look for active channels */
	ccer = active_channels(priv);

	for (i = 0; i < chip->npwm; i++) {
		mask = TIM_CCER_CCxE(i + 1);
		if (ccer & mask) {
			dev_err(dev, "PWM %u still in use by consumer %s\n",
				i, chip->pwms[i].label);
			return -EBUSY;
		}
	}

	return pinctrl_pm_select_sleep_state(dev);
}

static int stm32_pwm_resume(struct device *dev)
{
	struct pwm_chip *chip = dev_get_drvdata(dev);
	struct stm32_pwm *priv = to_stm32_pwm_dev(chip);
	int ret;

	ret = pinctrl_pm_select_default_state(dev);
	if (ret)
		return ret;

	/* restore breakinput registers that may have been lost in low power */
	return stm32_pwm_apply_breakinputs(priv);
}

static DEFINE_SIMPLE_DEV_PM_OPS(stm32_pwm_pm_ops, stm32_pwm_suspend, stm32_pwm_resume);

static const struct of_device_id stm32_pwm_of_match[] = {
	{ .compatible = "st,stm32-pwm",	},
	{ .compatible = "st,stm32mp25-pwm", },
	{ /* end node */ },
};
MODULE_DEVICE_TABLE(of, stm32_pwm_of_match);

static struct platform_driver stm32_pwm_driver = {
	.probe	= stm32_pwm_probe,
	.driver	= {
		.name = "stm32-pwm",
		.of_match_table = stm32_pwm_of_match,
		.pm = pm_ptr(&stm32_pwm_pm_ops),
	},
};
module_platform_driver(stm32_pwm_driver);

MODULE_ALIAS("platform:stm32-pwm");
MODULE_DESCRIPTION("STMicroelectronics STM32 PWM driver");
MODULE_LICENSE("GPL v2");
