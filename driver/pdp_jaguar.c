// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 * Copyright (C) 2023 Scott K Logan <logans@cottsay.net>
 */

#include <linux/module.h>

#include "common.h"
#include "../auth/auth.h"

#define GIP_JA_NAME "PDP Rock Band 4 Jaguar"

/*
 * Tilt (0-255) is reported as a plain digital button, thresholded here,
 * to match RPCS3's default R1 source (BTN_TR is a digital button, not
 * an axis - see rpcs3/Input/evdev_joystick_handler.cpp init_config()).
 * This also matches how Rock Band actually uses tilt in-game: as a
 * threshold gesture to activate Overdrive, not a continuous value.
 */
#define GIP_JA_TILT_THRESHOLD 128

enum gip_jaguar_button {
	GIP_JA_BTN_MENU = BIT(2),
	GIP_JA_BTN_VIEW = BIT(3),
	GIP_JA_BTN_DPAD_U = BIT(8),
	GIP_JA_BTN_DPAD_D = BIT(9),
	GIP_JA_BTN_DPAD_L = BIT(10),
	GIP_JA_BTN_DPAD_R = BIT(11),
	/*
	 * Documented as "solo fret flag / Riffmaster joystick click". On a
	 * real Jaguar/Stratocaster this bit coincides with any lower/solo
	 * fret being held; on the Riffmaster, which has no such correlation
	 * requirement, it's simply the neck thumbstick's click button. Report
	 * it as its own control either way rather than using it to reinterpret
	 * fret state (see below) - useful on its own as a bindable "solo"
	 * modifier button in frontends that model Rock Band guitars that way
	 * (e.g. RPCS3's single "Solo Modifier" binding).
	 */
	GIP_JA_BTN_SOLO = BIT(14),
};

/*
 * Fret state is read from the byte 5/6 upper and lower/solo bitmasks
 * (frets_upper/frets_lower below), not the "flag" bits in the buttons
 * word (old bits 4-7 and 12, disambiguated by bit 14 into upper vs.
 * lower/solo row). Those flag bits are documented as unreliable ("not
 * recommended") on any device, and on the PDP Riffmaster specifically,
 * bit 14 is the neck thumbstick's click rather than a fret-row selector,
 * so relying on it there misreports whichever fret is currently held as
 * soon as the stick is clicked or bumped. The bitmasks below don't have
 * this ambiguity on either device.
 */
enum gip_jaguar_fret_mask {
	GIP_JA_FRET_GREEN = BIT(0),
	GIP_JA_FRET_RED = BIT(1),
	GIP_JA_FRET_YELLOW = BIT(2),
	GIP_JA_FRET_BLUE = BIT(3),
	GIP_JA_FRET_ORANGE = BIT(4),
};

struct gip_jaguar_pkt_input {
	__le16 buttons;
	u8 tilt;
	u8 whammy;
	u8 pickup;
	u8 frets_upper;
	u8 frets_lower;
} __packed;

struct gip_jaguar {
	struct gip_client *client;
	struct gip_battery battery;
	struct gip_auth auth;
	struct gip_input input;
};

static int gip_jaguar_init_input(struct gip_jaguar *guitar)
{
	struct input_dev *dev = guitar->input.dev;
	int err;

	/*
	 * Personal fork policy, not upstream-worthy: use the exact evdev
	 * codes RPCS3's evdev_joystick_handler::init_config() binds by
	 * default for each PS3 pad control, rather than generic/arbitrary
	 * codes, so this device works in RPCS3 with zero manual rebinding.
	 * See the same rationale note in gip_jaguar_op_input() below.
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
	input_set_abs_params(dev, ABS_RX, 0, 255, 0, 0);	/* whammy */
	input_set_abs_params(dev, ABS_RY, -128, 128, 0, 0);	/* pickup switch */
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);

	err = input_register_device(dev);
	if (err)
		dev_err(&guitar->client->dev, "%s: register failed: %d\n",
			__func__, err);

	return err;
}

static int gip_jaguar_op_battery(struct gip_client *client,
				 enum gip_battery_type type,
				 enum gip_battery_level level)
{
	struct gip_jaguar *guitar = dev_get_drvdata(&client->dev);

	gip_report_battery(&guitar->battery, type, level);

	return 0;
}

static int gip_jaguar_op_authenticate(struct gip_client *client,
				      void *data, u32 len)
{
	struct gip_jaguar *guitar = dev_get_drvdata(&client->dev);

	return gip_auth_process_pkt(&guitar->auth, data, len);
}

static int gip_jaguar_op_guide_button(struct gip_client *client, bool down)
{
	struct gip_jaguar *guitar = dev_get_drvdata(&client->dev);

	input_report_key(guitar->input.dev, BTN_MODE, down);
	input_sync(guitar->input.dev);

	return 0;
}

static int gip_jaguar_op_input(struct gip_client *client, void *data, u32 len)
{
	struct gip_jaguar *guitar = dev_get_drvdata(&client->dev);
	struct gip_jaguar_pkt_input *pkt = data;
	struct input_dev *dev = guitar->input.dev;
	u16 buttons;
	u8 frets;
	bool solo;

	if (len < sizeof(*pkt))
		return -EINVAL;

	buttons = le16_to_cpu(pkt->buttons);

	/*
	 * Personal fork policy, not upstream-worthy: merge the upper and
	 * lower/solo fret rows into a single 5-button "fret held, either
	 * row" signal, and expose one "solo requested" modifier instead of
	 * 10 separate fret codes. This matches frontends that model Rock
	 * Band guitars with 5 frets + a single solo modifier rather than 10
	 * distinct fret buttons (e.g. RPCS3's harmonix_rockband_guitar pad
	 * type, which only exposes Cross/Circle/Square/Triangle/L1 for
	 * frets and a single L2 "Solo Modifier"). The modifier reflects
	 * either row physically being played (frets_lower != 0), so playing
	 * the real solo row still works, with the thumbstick click as a
	 * bonus manual shortcut.
	 *
	 * Everything below is reported on the exact evdev code RPCS3 binds
	 * by default for the corresponding PS3 pad control (see init_config()
	 * in rpcs3/Input/evdev_joystick_handler.cpp), so plugging this in and
	 * selecting the "Rock Band Guitar" product type needs no rebinding:
	 *   fret buttons -> BTN_A/B/X/Y/TL (Cross/Circle/Square/Triangle/L1)
	 *   solo modifier -> ABS_Z, positive = active           (L2 default)
	 *   tilt          -> BTN_TR, thresholded                (R1 default)
	 *   whammy        -> ABS_RX, positive = pressed          (RS default)
	 *   pickup switch -> ABS_RY, centered, up = negative     (RS default)
	 * The neck thumbstick itself is unused by the player in this setup,
	 * so its axis slot (right stick) is repurposed to carry the pickup
	 * switch instead of real stick deflection.
	 */
	frets = pkt->frets_upper | pkt->frets_lower;
	solo = pkt->frets_lower || (buttons & GIP_JA_BTN_SOLO);

	input_report_key(dev, BTN_START, buttons & GIP_JA_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_JA_BTN_VIEW);
	input_report_key(dev, BTN_A, frets & GIP_JA_FRET_GREEN);
	input_report_key(dev, BTN_B, frets & GIP_JA_FRET_RED);
	input_report_key(dev, BTN_Y, frets & GIP_JA_FRET_YELLOW);
	input_report_key(dev, BTN_X, frets & GIP_JA_FRET_BLUE);
	input_report_key(dev, BTN_TL, frets & GIP_JA_FRET_ORANGE);
	input_report_key(dev, BTN_TR, pkt->tilt > GIP_JA_TILT_THRESHOLD);
	input_report_abs(dev, ABS_Z, solo ? 255 : 0);
	input_report_abs(dev, ABS_RX, pkt->whammy);
	/* pickup notch is 0-4, encoded in the packet's top 4 bits */
	input_report_abs(dev, ABS_RY, ((int)(pkt->pickup >> 4) - 2) * 64);
	input_report_abs(dev, ABS_HAT0X, !!(buttons & GIP_JA_BTN_DPAD_R) -
					 !!(buttons & GIP_JA_BTN_DPAD_L));
	input_report_abs(dev, ABS_HAT0Y, !!(buttons & GIP_JA_BTN_DPAD_D) -
					 !!(buttons & GIP_JA_BTN_DPAD_U));
	input_sync(dev);

	return 0;
}

static int gip_jaguar_probe(struct gip_client *client)
{
	struct gip_jaguar *guitar;
	int err;

	guitar = devm_kzalloc(&client->dev, sizeof(*guitar), GFP_KERNEL);
	if (!guitar)
		return -ENOMEM;

	guitar->client = client;

	err = gip_set_power_mode(client, GIP_PWR_ON);
	if (err)
		return err;

	err = gip_init_battery(&guitar->battery, client, GIP_JA_NAME);
	if (err)
		return err;

	err = gip_auth_start_handshake(&guitar->auth, client);
	if (err)
		return err;

	err = gip_init_input(&guitar->input, client, GIP_JA_NAME);
	if (err)
		return err;

	err = gip_jaguar_init_input(guitar);
	if (err)
		return err;

	dev_set_drvdata(&client->dev, guitar);

	return 0;
}

static struct gip_driver gip_jaguar_driver = {
	.name = "xone-gip-pdp-jaguar",
	.class = "PDP.Xbox.Guitar.Jaguar",
	.ops = {
		.battery = gip_jaguar_op_battery,
		.authenticate = gip_jaguar_op_authenticate,
		.guide_button = gip_jaguar_op_guide_button,
		.input = gip_jaguar_op_input,
	},
	.probe = gip_jaguar_probe,
};
module_gip_driver(gip_jaguar_driver);

MODULE_ALIAS("gip:PDP.Xbox.Guitar.Jaguar");
MODULE_AUTHOR("Severin von Wnuck-Lipinski <severinvonw@outlook.de>");
MODULE_AUTHOR("Scott K Logan <logans@cottsay.net>");
MODULE_DESCRIPTION("xone GIP PDP Jaguar driver");
MODULE_VERSION("#VERSION#");
MODULE_LICENSE("GPL");
