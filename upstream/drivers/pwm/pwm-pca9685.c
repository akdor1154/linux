// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for PCA9685 16-channel 12-bit PWM LED controller
 *
 * Copyright (C) 2013 Steffen Trumtrar <s.trumtrar@pengutronix.de>
 * Copyright (C) 2015 Clemens Gruber <clemens.gruber@pqgruber.com>
 *
 * based on the pwm-twl-led.c driver
 */

#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/bitmap.h>

/*
 * Because the PCA9685 has only one prescaler per chip, only the first channel
 * that is enabled is allowed to change the prescale register.
 * PWM channels requested afterwards must use a period that results in the same
 * prescale setting as the one set by the first requested channel.
 * GPIOs do not count as enabled PWMs as they are not using the prescaler.
 */

#define PCA9685_MODE1		0x00
#define PCA9685_MODE2		0x01
#define PCA9685_SUBADDR1	0x02
#define PCA9685_SUBADDR2	0x03
#define PCA9685_SUBADDR3	0x04
#define PCA9685_ALLCALLADDR	0x05
#define PCA9685_LEDX_ON_L	0x06
#define PCA9685_LEDX_ON_H	0x07
#define PCA9685_LEDX_OFF_L	0x08
#define PCA9685_LEDX_OFF_H	0x09

#define PCA9685_ALL_LED_ON_L	0xFA
#define PCA9685_ALL_LED_ON_H	0xFB
#define PCA9685_ALL_LED_OFF_L	0xFC
#define PCA9685_ALL_LED_OFF_H	0xFD
#define PCA9685_PRESCALE	0xFE

#define PCA9685_PRESCALE_MIN	0x03	/* => max. frequency of 1526 Hz */
#define PCA9685_PRESCALE_MAX	0xFF	/* => min. frequency of 24 Hz */

#define PCA9685_COUNTER_RANGE	4096
#define PCA9685_OSC_CLOCK_MHZ	25	/* Internal oscillator with 25 MHz */

#define PCA9685_NUMREGS		0xFF
#define PCA9685_MAXCHAN		0x10

#define LED_FULL		BIT(4)
#define MODE1_ALLCALL		BIT(0)
#define MODE1_SUB3		BIT(1)
#define MODE1_SUB2		BIT(2)
#define MODE1_SUB1		BIT(3)
#define MODE1_SLEEP		BIT(4)
#define MODE2_INVRT		BIT(4)
#define MODE2_OUTDRV		BIT(2)

#define LED_N_ON_H(N)	(PCA9685_LEDX_ON_H + (4 * (N)))
#define LED_N_ON_L(N)	(PCA9685_LEDX_ON_L + (4 * (N)))
#define LED_N_OFF_H(N)	(PCA9685_LEDX_OFF_H + (4 * (N)))
#define LED_N_OFF_L(N)	(PCA9685_LEDX_OFF_L + (4 * (N)))

#define REG_ON_H(C)	((C) >= PCA9685_MAXCHAN ? PCA9685_ALL_LED_ON_H : LED_N_ON_H((C)))
#define REG_ON_L(C)	((C) >= PCA9685_MAXCHAN ? PCA9685_ALL_LED_ON_L : LED_N_ON_L((C)))
#define REG_OFF_H(C)	((C) >= PCA9685_MAXCHAN ? PCA9685_ALL_LED_OFF_H : LED_N_OFF_H((C)))
#define REG_OFF_L(C)	((C) >= PCA9685_MAXCHAN ? PCA9685_ALL_LED_OFF_L : LED_N_OFF_L((C)))

struct pca9685 {
	struct regmap *regmap;
	struct mutex lock;
	DECLARE_BITMAP(pwms_enabled, PCA9685_MAXCHAN + 1);
#if IS_ENABLED(CONFIG_GPIOLIB)
	struct gpio_chip gpio;
	DECLARE_BITMAP(pwms_inuse, PCA9685_MAXCHAN + 1);
#endif
};

static inline struct pca9685 *to_pca(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

/* This function is supposed to be called with the lock mutex held */
static bool pca9685_prescaler_can_change(struct pca9685 *pca, int channel)
{
	/* No PWM enabled: Change allowed */
	if (bitmap_empty(pca->pwms_enabled, PCA9685_MAXCHAN + 1))
		return true;
	/* More than one PWM enabled: Change not allowed */
	if (bitmap_weight(pca->pwms_enabled, PCA9685_MAXCHAN + 1) > 1)
		return false;
	/*
	 * Only one PWM enabled: Change allowed if the PWM about to
	 * be changed is the one that is already enabled
	 */
	return test_bit(channel, pca->pwms_enabled);
}

static int pca9685_read_reg(struct pwm_chip *chip, unsigned int reg, unsigned int *val)
{
	struct pca9685 *pca = to_pca(chip);
	struct device *dev = pwmchip_parent(chip);
	int err;

	err = regmap_read(pca->regmap, reg, val);
	if (err)
		dev_err(dev, "regmap_read of register 0x%x failed: %pe\n", reg, ERR_PTR(err));

	return err;
}

static int pca9685_write_reg(struct pwm_chip *chip, unsigned int reg, unsigned int val)
{
	struct pca9685 *pca = to_pca(chip);
	struct device *dev = pwmchip_parent(chip);
	int err;

	err = regmap_write(pca->regmap, reg, val);
	if (err)
		dev_err(dev, "regmap_write to register 0x%x failed: %pe\n", reg, ERR_PTR(err));

	return err;
}

/* Helper function to set the duty cycle ratio to duty/4096 (e.g. duty=2048 -> 50%) */
static void pca9685_pwm_set_duty(struct pwm_chip *chip, int channel, unsigned int duty)
{
	struct pwm_device *pwm = &chip->pwms[channel];
	unsigned int on, off;

	if (duty == 0) {
		/* Set the full OFF bit, which has the highest precedence */
		pca9685_write_reg(chip, REG_OFF_H(channel), LED_FULL);
		return;
	} else if (duty >= PCA9685_COUNTER_RANGE) {
		/* Set the full ON bit and clear the full OFF bit */
		pca9685_write_reg(chip, REG_ON_H(channel), LED_FULL);
		pca9685_write_reg(chip, REG_OFF_H(channel), 0);
		return;
	}


	if (pwm->state.usage_power && channel < PCA9685_MAXCHAN) {
		/*
		 * If usage_power is set, the pca9685 driver will phase shift
		 * the individual channels relative to their channel number.
		 * This improves EMI because the enabled channels no longer
		 * turn on at the same time, while still maintaining the
		 * configured duty cycle / power output.
		 */
		on = channel * PCA9685_COUNTER_RANGE / PCA9685_MAXCHAN;
	} else
		on = 0;

	off = (on + duty) % PCA9685_COUNTER_RANGE;

	/* Set ON time (clears full ON bit) */
	pca9685_write_reg(chip, REG_ON_L(channel), on & 0xff);
	pca9685_write_reg(chip, REG_ON_H(channel), (on >> 8) & 0xf);
	/* Set OFF time (clears full OFF bit) */
	pca9685_write_reg(chip, REG_OFF_L(channel), off & 0xff);
	pca9685_write_reg(chip, REG_OFF_H(channel), (off >> 8) & 0xf);
}

static unsigned int pca9685_pwm_get_duty(struct pwm_chip *chip, int channel)
{
	struct pwm_device *pwm = &chip->pwms[channel];
	unsigned int off = 0, on = 0, val = 0;

	if (WARN_ON(channel >= PCA9685_MAXCHAN)) {
		/* HW does not support reading state of "all LEDs" channel */
		return 0;
	}

	pca9685_read_reg(chip, LED_N_OFF_H(channel), &off);
	if (off & LED_FULL) {
		/* Full OFF bit is set */
		return 0;
	}

	pca9685_read_reg(chip, LED_N_ON_H(channel), &on);
	if (on & LED_FULL) {
		/* Full ON bit is set */
		return PCA9685_COUNTER_RANGE;
	}

	pca9685_read_reg(chip, LED_N_OFF_L(channel), &val);
	off = ((off & 0xf) << 8) | (val & 0xff);
	if (!pwm->state.usage_power)
		return off;

	/* Read ON register to calculate duty cycle of staggered output */
	if (pca9685_read_reg(chip, LED_N_ON_L(channel), &val)) {
		/* Reset val to 0 in case reading LED_N_ON_L failed */
		val = 0;
	}
	on = ((on & 0xf) << 8) | (val & 0xff);
	return (off - on) & (PCA9685_COUNTER_RANGE - 1);
}

#if IS_ENABLED(CONFIG_GPIOLIB)
static bool pca9685_pwm_test_and_set_inuse(struct pca9685 *pca, int pwm_idx)
{
	bool is_inuse;

	mutex_lock(&pca->lock);
	if (pwm_idx >= PCA9685_MAXCHAN) {
		/*
		 * "All LEDs" channel:
		 * pretend already in use if any of the PWMs are requested
		 */
		if (!bitmap_empty(pca->pwms_inuse, PCA9685_MAXCHAN)) {
			is_inuse = true;
			goto out;
		}
	} else {
		/*
		 * Regular channel:
		 * pretend already in use if the "all LEDs" channel is requested
		 */
		if (test_bit(PCA9685_MAXCHAN, pca->pwms_inuse)) {
			is_inuse = true;
			goto out;
		}
	}
	is_inuse = test_and_set_bit(pwm_idx, pca->pwms_inuse);
out:
	mutex_unlock(&pca->lock);
	return is_inuse;
}

static void pca9685_pwm_clear_inuse(struct pca9685 *pca, int pwm_idx)
{
	mutex_lock(&pca->lock);
	clear_bit(pwm_idx, pca->pwms_inuse);
	mutex_unlock(&pca->lock);
}

static int pca9685_pwm_gpio_request(struct gpio_chip *gpio, unsigned int offset)
{
	struct pwm_chip *chip = gpiochip_get_data(gpio);
	struct pca9685 *pca = to_pca(chip);

	if (pca9685_pwm_test_and_set_inuse(pca, offset))
		return -EBUSY;
	pm_runtime_get_sync(pwmchip_parent(chip));
	return 0;
}

static int pca9685_pwm_gpio_get(struct gpio_chip *gpio, unsigned int offset)
{
	struct pwm_chip *chip = gpiochip_get_data(gpio);

	return pca9685_pwm_get_duty(chip, offset) != 0;
}

static int pca9685_pwm_gpio_set(struct gpio_chip *gpio, unsigned int offset,
				int value)
{
	struct pwm_chip *chip = gpiochip_get_data(gpio);

	pca9685_pwm_set_duty(chip, offset, value ? PCA9685_COUNTER_RANGE : 0);

	return 0;
}

static void pca9685_pwm_gpio_free(struct gpio_chip *gpio, unsigned int offset)
{
	struct pwm_chip *chip = gpiochip_get_data(gpio);
	struct pca9685 *pca = to_pca(chip);

	pca9685_pwm_set_duty(chip, offset, 0);
	pm_runtime_put(pwmchip_parent(chip));
	pca9685_pwm_clear_inuse(pca, offset);
}

static int pca9685_pwm_gpio_get_direction(struct gpio_chip *chip,
					  unsigned int offset)
{
	/* Always out */
	return GPIO_LINE_DIRECTION_OUT;
}

static int pca9685_pwm_gpio_direction_input(struct gpio_chip *gpio,
					    unsigned int offset)
{
	return -EINVAL;
}

static int pca9685_pwm_gpio_direction_output(struct gpio_chip *gpio,
					     unsigned int offset, int value)
{
	pca9685_pwm_gpio_set(gpio, offset, value);

	return 0;
}

/*
 * The PCA9685 has a bit for turning the PWM output full off or on. Some
 * boards like Intel Galileo actually uses these as normal GPIOs so we
 * expose a GPIO chip here which can exclusively take over the underlying
 * PWM channel.
 */
static int pca9685_pwm_gpio_probe(struct pwm_chip *chip)
{
	struct pca9685 *pca = to_pca(chip);
	struct device *dev = pwmchip_parent(chip);

	pca->gpio.label = dev_name(dev);
	pca->gpio.parent = dev;
	pca->gpio.request = pca9685_pwm_gpio_request;
	pca->gpio.free = pca9685_pwm_gpio_free;
	pca->gpio.get_direction = pca9685_pwm_gpio_get_direction;
	pca->gpio.direction_input = pca9685_pwm_gpio_direction_input;
	pca->gpio.direction_output = pca9685_pwm_gpio_direction_output;
	pca->gpio.get = pca9685_pwm_gpio_get;
	pca->gpio.set_rv = pca9685_pwm_gpio_set;
	pca->gpio.base = -1;
	pca->gpio.ngpio = PCA9685_MAXCHAN;
	pca->gpio.can_sleep = true;

	return devm_gpiochip_add_data(dev, &pca->gpio, chip);
}
#else
static inline bool pca9685_pwm_test_and_set_inuse(struct pca9685 *pca,
						  int pwm_idx)
{
	return false;
}

static inline void
pca9685_pwm_clear_inuse(struct pca9685 *pca, int pwm_idx)
{
}

static inline int pca9685_pwm_gpio_probe(struct pwm_chip *chip)
{
	return 0;
}
#endif

static void pca9685_set_sleep_mode(struct pwm_chip *chip, bool enable)
{
	struct device *dev = pwmchip_parent(chip);
	struct pca9685 *pca = to_pca(chip);
	int err = regmap_update_bits(pca->regmap, PCA9685_MODE1,
				     MODE1_SLEEP, enable ? MODE1_SLEEP : 0);
	if (err) {
		dev_err(dev, "regmap_update_bits of register 0x%x failed: %pe\n",
			PCA9685_MODE1, ERR_PTR(err));
		return;
	}

	if (!enable) {
		/* Wait 500us for the oscillator to be back up */
		udelay(500);
	}
}

static int __pca9685_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			       const struct pwm_state *state)
{
	struct pca9685 *pca = to_pca(chip);
	unsigned long long duty, prescale;
	unsigned int val = 0;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	prescale = DIV_ROUND_CLOSEST_ULL(PCA9685_OSC_CLOCK_MHZ * state->period,
					 PCA9685_COUNTER_RANGE * 1000) - 1;
	if (prescale < PCA9685_PRESCALE_MIN || prescale > PCA9685_PRESCALE_MAX) {
		dev_err(pwmchip_parent(chip), "pwm not changed: period out of bounds!\n");
		return -EINVAL;
	}

	if (!state->enabled) {
		pca9685_pwm_set_duty(chip, pwm->hwpwm, 0);
		return 0;
	}

	pca9685_read_reg(chip, PCA9685_PRESCALE, &val);
	if (prescale != val) {
		if (!pca9685_prescaler_can_change(pca, pwm->hwpwm)) {
			dev_err(pwmchip_parent(chip),
				"pwm not changed: periods of enabled pwms must match!\n");
			return -EBUSY;
		}

		/*
		 * Putting the chip briefly into SLEEP mode
		 * at this point won't interfere with the
		 * pm_runtime framework, because the pm_runtime
		 * state is guaranteed active here.
		 */
		/* Put chip into sleep mode */
		pca9685_set_sleep_mode(chip, true);

		/* Change the chip-wide output frequency */
		pca9685_write_reg(chip, PCA9685_PRESCALE, prescale);

		/* Wake the chip up */
		pca9685_set_sleep_mode(chip, false);
	}

	duty = PCA9685_COUNTER_RANGE * state->duty_cycle;
	duty = DIV_ROUND_UP_ULL(duty, state->period);
	pca9685_pwm_set_duty(chip, pwm->hwpwm, duty);
	return 0;
}

static int pca9685_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct pca9685 *pca = to_pca(chip);
	int ret;

	mutex_lock(&pca->lock);
	ret = __pca9685_pwm_apply(chip, pwm, state);
	if (ret == 0) {
		if (state->enabled)
			set_bit(pwm->hwpwm, pca->pwms_enabled);
		else
			clear_bit(pwm->hwpwm, pca->pwms_enabled);
	}
	mutex_unlock(&pca->lock);

	return ret;
}

static int pca9685_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				 struct pwm_state *state)
{
	unsigned long long duty;
	unsigned int val = 0;

	/* Calculate (chip-wide) period from prescale value */
	pca9685_read_reg(chip, PCA9685_PRESCALE, &val);
	/*
	 * PCA9685_OSC_CLOCK_MHZ is 25, i.e. an integer divider of 1000.
	 * The following calculation is therefore only a multiplication
	 * and we are not losing precision.
	 */
	state->period = (PCA9685_COUNTER_RANGE * 1000 / PCA9685_OSC_CLOCK_MHZ) *
			(val + 1);

	/* The (per-channel) polarity is fixed */
	state->polarity = PWM_POLARITY_NORMAL;

	if (pwm->hwpwm >= PCA9685_MAXCHAN) {
		/*
		 * The "all LEDs" channel does not support HW readout
		 * Return 0 and disabled for backwards compatibility
		 */
		state->duty_cycle = 0;
		state->enabled = false;
		return 0;
	}

	state->enabled = true;
	duty = pca9685_pwm_get_duty(chip, pwm->hwpwm);
	state->duty_cycle = DIV_ROUND_DOWN_ULL(duty * state->period, PCA9685_COUNTER_RANGE);

	return 0;
}

static int pca9685_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct pca9685 *pca = to_pca(chip);

	if (pca9685_pwm_test_and_set_inuse(pca, pwm->hwpwm))
		return -EBUSY;

	if (pwm->hwpwm < PCA9685_MAXCHAN) {
		/* PWMs - except the "all LEDs" channel - default to enabled */
		mutex_lock(&pca->lock);
		set_bit(pwm->hwpwm, pca->pwms_enabled);
		mutex_unlock(&pca->lock);
	}

	pm_runtime_get_sync(pwmchip_parent(chip));

	return 0;
}

static void pca9685_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct pca9685 *pca = to_pca(chip);

	mutex_lock(&pca->lock);
	pca9685_pwm_set_duty(chip, pwm->hwpwm, 0);
	clear_bit(pwm->hwpwm, pca->pwms_enabled);
	mutex_unlock(&pca->lock);

	pm_runtime_put(pwmchip_parent(chip));
	pca9685_pwm_clear_inuse(pca, pwm->hwpwm);
}

static const struct pwm_ops pca9685_pwm_ops = {
	.apply = pca9685_pwm_apply,
	.get_state = pca9685_pwm_get_state,
	.request = pca9685_pwm_request,
	.free = pca9685_pwm_free,
};

static const struct regmap_config pca9685_regmap_i2c_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = PCA9685_NUMREGS,
	.cache_type = REGCACHE_NONE,
};

static int pca9685_pwm_probe(struct i2c_client *client)
{
	struct pwm_chip *chip;
	struct pca9685 *pca;
	unsigned int reg;
	int ret;

	/* Add an extra channel for ALL_LED */
	chip = devm_pwmchip_alloc(&client->dev, PCA9685_MAXCHAN + 1, sizeof(*pca));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	pca = to_pca(chip);

	pca->regmap = devm_regmap_init_i2c(client, &pca9685_regmap_i2c_config);
	if (IS_ERR(pca->regmap)) {
		ret = PTR_ERR(pca->regmap);
		dev_err(&client->dev, "Failed to initialize register map: %d\n",
			ret);
		return ret;
	}

	i2c_set_clientdata(client, chip);

	mutex_init(&pca->lock);

	ret = pca9685_read_reg(chip, PCA9685_MODE2, &reg);
	if (ret)
		return ret;

	if (device_property_read_bool(&client->dev, "invert"))
		reg |= MODE2_INVRT;
	else
		reg &= ~MODE2_INVRT;

	if (device_property_read_bool(&client->dev, "open-drain"))
		reg &= ~MODE2_OUTDRV;
	else
		reg |= MODE2_OUTDRV;

	ret = pca9685_write_reg(chip, PCA9685_MODE2, reg);
	if (ret)
		return ret;

	/* Disable all LED ALLCALL and SUBx addresses to avoid bus collisions */
	pca9685_read_reg(chip, PCA9685_MODE1, &reg);
	reg &= ~(MODE1_ALLCALL | MODE1_SUB1 | MODE1_SUB2 | MODE1_SUB3);
	pca9685_write_reg(chip, PCA9685_MODE1, reg);

	/* Reset OFF/ON registers to POR default */
	pca9685_write_reg(chip, PCA9685_ALL_LED_OFF_L, 0);
	pca9685_write_reg(chip, PCA9685_ALL_LED_OFF_H, LED_FULL);
	pca9685_write_reg(chip, PCA9685_ALL_LED_ON_L, 0);
	pca9685_write_reg(chip, PCA9685_ALL_LED_ON_H, LED_FULL);

	chip->ops = &pca9685_pwm_ops;

	ret = pwmchip_add(chip);
	if (ret < 0)
		return ret;

	ret = pca9685_pwm_gpio_probe(chip);
	if (ret < 0) {
		pwmchip_remove(chip);
		return ret;
	}

	pm_runtime_enable(&client->dev);

	if (pm_runtime_enabled(&client->dev)) {
		/*
		 * Although the chip comes out of power-up in the sleep state,
		 * we force it to sleep in case it was woken up before
		 */
		pca9685_set_sleep_mode(chip, true);
		pm_runtime_set_suspended(&client->dev);
	} else {
		/* Wake the chip up if runtime PM is disabled */
		pca9685_set_sleep_mode(chip, false);
	}

	return 0;
}

static void pca9685_pwm_remove(struct i2c_client *client)
{
	struct pwm_chip *chip = i2c_get_clientdata(client);

	pwmchip_remove(chip);

	if (!pm_runtime_enabled(&client->dev)) {
		/* Put chip in sleep state if runtime PM is disabled */
		pca9685_set_sleep_mode(chip, true);
	}

	pm_runtime_disable(&client->dev);
}

static int __maybe_unused pca9685_pwm_runtime_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pwm_chip *chip = i2c_get_clientdata(client);

	pca9685_set_sleep_mode(chip, true);
	return 0;
}

static int __maybe_unused pca9685_pwm_runtime_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct pwm_chip *chip = i2c_get_clientdata(client);

	pca9685_set_sleep_mode(chip, false);
	return 0;
}

static const struct i2c_device_id pca9685_id[] = {
	{ "pca9685" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, pca9685_id);

static const struct acpi_device_id pca9685_acpi_ids[] = {
	{ "INT3492", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(acpi, pca9685_acpi_ids);

static const struct of_device_id pca9685_dt_ids[] = {
	{ .compatible = "nxp,pca9685-pwm", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, pca9685_dt_ids);

static const struct dev_pm_ops pca9685_pwm_pm = {
	SET_RUNTIME_PM_OPS(pca9685_pwm_runtime_suspend,
			   pca9685_pwm_runtime_resume, NULL)
};

static struct i2c_driver pca9685_i2c_driver = {
	.driver = {
		.name = "pca9685-pwm",
		.acpi_match_table = pca9685_acpi_ids,
		.of_match_table = pca9685_dt_ids,
		.pm = &pca9685_pwm_pm,
	},
	.probe = pca9685_pwm_probe,
	.remove = pca9685_pwm_remove,
	.id_table = pca9685_id,
};

module_i2c_driver(pca9685_i2c_driver);

MODULE_AUTHOR("Steffen Trumtrar <s.trumtrar@pengutronix.de>");
MODULE_DESCRIPTION("PWM driver for PCA9685");
MODULE_LICENSE("GPL");
