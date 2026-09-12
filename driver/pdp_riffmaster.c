// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 * Copyright (C) 2023 Scott K Logan <logans@cottsay.net>
 * Copyright (C) 2026 Dan Leehr <leehro@gmail.com>
 *
 * Split out from pdp_jaguar.c: a personal fork targeting only the PDP
 * Riffmaster, tuned to match RPCS3's default evdev bindings. pdp_jaguar.c
 * is kept in this tree unmodified for the real Jaguar/Stratocaster, but
 * isn't built by this fork - see Kbuild/dkms.conf.
 */

#include <linux/module.h>

#include "common.h"
#include "../auth/auth.h"

#define GIP_RM_NAME "PDP Riffmaster"

/* Tilt is reported as a digital press past this value, matching RPCS3's
 * default (BTN_TR, not an axis) and how RB actually uses tilt in-game.
 */
#define GIP_RM_TILT_THRESHOLD 128

/* No pickup switch on this Riffmaster. The real switch has 5 discrete
 * detents that hold their position; the neck stick springs back to center,
 * so it's read as a stepper instead: a flick past the threshold advances
 * the position by one detent, held until the next flick.
 */
#define GIP_RM_PICKUP_POSITIONS 5
#define GIP_RM_PICKUP_STICK_THRESHOLD 16384

enum gip_riffmaster_button {
	GIP_RM_BTN_MENU = BIT(2),
	GIP_RM_BTN_VIEW = BIT(3),
	GIP_RM_BTN_DPAD_U = BIT(8),
	GIP_RM_BTN_DPAD_D = BIT(9),
	GIP_RM_BTN_DPAD_L = BIT(10),
	GIP_RM_BTN_DPAD_R = BIT(11),
	/* stick-click is solo modifier so we can bind it separately if needed */
	GIP_RM_BTN_SOLO = BIT(14),
};

enum gip_riffmaster_fret_mask {
	GIP_RM_FRET_GREEN = BIT(0),
	GIP_RM_FRET_RED = BIT(1),
	GIP_RM_FRET_YELLOW = BIT(2),
	GIP_RM_FRET_BLUE = BIT(3),
	GIP_RM_FRET_ORANGE = BIT(4),
};

struct gip_riffmaster_pkt_input {
	__le16 buttons;
	u8 tilt;
	u8 whammy;
	u8 pickup;		/* unused: no physical switch on riffmaster */
	u8 frets_upper;
	u8 frets_lower;
	u8 autocal_light;
	__le16 autocal_audio;
	__le16 joystick_x;	/* unmapped */
	__le16 joystick_y;	/* mapped to ABS_RY (RPCS3's pickup axis) */
} __packed;

struct gip_riffmaster {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_auth auth;
	struct gip_input input;
	u8 pickup_position;		/* 0..GIP_RM_PICKUP_POSITIONS-1 */
	bool pickup_stick_deflected;	/* edge-detect for the flick gesture */
};

/* Chosen so RPCS3's axis normalization reproduces the real PS3/Wii
 * guitar notch bytes (PlasticBand's Rock Band 5-Fret Guitar notes).
 */
static const s16 gip_riffmaster_pickup_values[GIP_RM_PICKUP_POSITIONS] = {
	-26347, -13233, 5784, 12980, 26085,
};

static s16 gip_riffmaster_pickup_value(u8 position)
{
	return gip_riffmaster_pickup_values[position];
}

static int gip_riffmaster_init_input(struct gip_riffmaster *guitar)
{
	struct input_dev *dev = guitar->input.dev;
	int err;

	/* Uses the evdev codes RPCS3 binds by default for each PS3 pad
	 * control, so this device works in RPCS3 with zero manual rebinding.
	 */
	input_set_capability(dev, EV_KEY, BTN_MODE);
	input_set_capability(dev, EV_KEY, BTN_START);
	input_set_capability(dev, EV_KEY, BTN_SELECT);
	input_set_capability(dev, EV_KEY, BTN_A);	/* green fret */
	input_set_capability(dev, EV_KEY, BTN_B);	/* red fret */
	input_set_capability(dev, EV_KEY, BTN_X);	/* blue fret */
	input_set_capability(dev, EV_KEY, BTN_Y);	/* yellow fret */
	input_set_capability(dev, EV_KEY, BTN_TL);	/* orange fret */
	input_set_capability(dev, EV_KEY, BTN_TR);	/* tilt (digital, see above) */
	input_set_abs_params(dev, ABS_Z, 0, 255, 0, 0);	/* solo modifier */
	input_set_abs_params(dev, ABS_RX, -32768, 32767, 0, 0);	/* whammy */
	input_set_abs_params(dev, ABS_RY, -32768, 32767, 0, 0);	/* stick Y (RPCS3's pickup axis) */
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);

	err = input_register_device(dev);
	if (err)
		dev_err(&guitar->client->dev, "%s: register failed: %d\n",
			__func__, err);

	return err;
}

static int gip_riffmaster_op_battery(struct gip_client *client,
				     enum gip_battery_type type,
				     enum gip_battery_level level)
{
	struct gip_riffmaster *guitar = dev_get_drvdata(&client->dev);

	gip_report_battery(&guitar->battery, type, level);

	return 0;
}

static int gip_riffmaster_op_authenticate(struct gip_client *client,
					  void *data, u32 len)
{
	struct gip_riffmaster *guitar = dev_get_drvdata(&client->dev);

	return gip_auth_process_pkt(&guitar->auth, data, len);
}

static int gip_riffmaster_op_guide_button(struct gip_client *client, bool down)
{
	struct gip_riffmaster *guitar = dev_get_drvdata(&client->dev);

	input_report_key(guitar->input.dev, BTN_MODE, down);
	input_sync(guitar->input.dev);

	return 0;
}

static int gip_riffmaster_op_input(struct gip_client *client, void *data, u32 len)
{
	struct gip_riffmaster *guitar = dev_get_drvdata(&client->dev);
	struct gip_riffmaster_pkt_input *pkt = data;
	struct input_dev *dev = guitar->input.dev;
	u16 buttons;
	u8 frets;
	bool solo;
	s16 stick_y;
	bool deflected;

	if (len < sizeof(*pkt))
		return -EINVAL;

	buttons = le16_to_cpu(pkt->buttons);

	/* Merges the upper/lower fret rows into 5 buttons + a solo modifier,
	 * matching RPCS3's harmonix_rockband_guitar pad type (5 frets + one
	 * Solo Modifier, not 10 distinct fret buttons). Everything below is
	 * reported on RPCS3's default evdev binding for the corresponding
	 * control, so no manual rebinding is needed.
	 */
	frets = pkt->frets_upper | pkt->frets_lower;
	solo = pkt->frets_lower || (buttons & GIP_RM_BTN_SOLO);

	input_report_key(dev, BTN_START, buttons & GIP_RM_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_RM_BTN_VIEW);
	input_report_key(dev, BTN_A, frets & GIP_RM_FRET_GREEN);
	input_report_key(dev, BTN_B, frets & GIP_RM_FRET_RED);
	input_report_key(dev, BTN_Y, frets & GIP_RM_FRET_YELLOW);
	input_report_key(dev, BTN_X, frets & GIP_RM_FRET_BLUE);
	input_report_key(dev, BTN_TL, frets & GIP_RM_FRET_ORANGE);
	input_report_key(dev, BTN_TR, pkt->tilt > GIP_RM_TILT_THRESHOLD);
	input_report_abs(dev, ABS_Z, solo ? 255 : 0);
	/* Whammy: not yet confirmed working in-game (RB3/RPCS3). */
	input_report_abs(dev, ABS_RX, pkt->whammy * 128);
	/* Pickup switch: see GIP_RM_PICKUP_POSITIONS comment above. */
	stick_y = (s16)le16_to_cpu(pkt->joystick_y);
	deflected = stick_y > GIP_RM_PICKUP_STICK_THRESHOLD ||
		    stick_y < -GIP_RM_PICKUP_STICK_THRESHOLD;
	if (deflected && !guitar->pickup_stick_deflected) {
		if (stick_y > 0 && guitar->pickup_position < GIP_RM_PICKUP_POSITIONS - 1)
			guitar->pickup_position++;
		else if (stick_y < 0 && guitar->pickup_position > 0)
			guitar->pickup_position--;
	}
	guitar->pickup_stick_deflected = deflected;
	input_report_abs(dev, ABS_RY, gip_riffmaster_pickup_value(guitar->pickup_position));
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_RM_BTN_DPAD_R) -
					 !!(buttons & GIP_RM_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_RM_BTN_DPAD_D) -
					 !!(buttons & GIP_RM_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_riffmaster_probe(struct gip_client *client)
{
	struct gip_riffmaster *guitar;
	int err;

	guitar = devm_kzalloc(&client->dev, sizeof(*guitar), GFP_KERNEL);
	if (!guitar)
		return -ENOMEM;

	guitar->client = client;
	guitar->pickup_position = GIP_RM_PICKUP_POSITIONS / 2;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_init_battery(&guitar->battery, client, GIP_RM_NAME);
	if (err)
		return err;

	err = gip_auth_start_handshake(&guitar->auth, client);
	if (err)
		return err;

	err = gip_init_input(&guitar->input, client, GIP_RM_NAME);
	if (err)
		return err;

	err = gip_riffmaster_init_input(guitar);
	if (err)
		return err;

	dev_set_drvdata(&client->dev, guitar);

	return 0;
}

static struct gip_driver gip_riffmaster_driver = {
	.name = "xone-gip-pdp-riffmaster",
	/* Same class string the real Jaguar/Stratocaster uses - that's what
	 * the hardware announces over GIP. pdp_jaguar.c isn't built by this
	 * fork, so there's nothing else registered for it to race against.
	 */
	.class = "PDP.Xbox.Guitar.Jaguar",
	.ops = {
		.battery = gip_riffmaster_op_battery,
		.authenticate = gip_riffmaster_op_authenticate,
		.guide_button = gip_riffmaster_op_guide_button,
		.input = gip_riffmaster_op_input,
	},
	.probe = gip_riffmaster_probe,
};
module_gip_driver(gip_riffmaster_driver);

MODULE_ALIAS("gip:PDP.Xbox.Guitar.Jaguar");
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_AUTHOR("Scott K Logan <logans@cottsay.net>");
MODULE_AUTHOR("Dan Leehr <leehro@gmail.com>");
MODULE_DESCRIPTION("xone GIP PDP Riffmaster driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
