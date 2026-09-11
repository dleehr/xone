// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Severin von Wnuck-Lipinski <severinvonw@outlook.de>
 * Copyright (C) 2023 Scott K Logan <logans@cottsay.net>
 */

#include <linux/module.h>

#include "common.h"
#include "../auth/auth.h"

#define GIP_JA_NAME "PDP Rock Band 4 Jaguar"

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
 * Fret state is intentionally *not* read from the "flag" bits in the
 * buttons word (old bits 4-7 and 12, disambiguated by GIP_JA_BTN_SOLO
 * above to select upper vs. lower/solo row). On a real Jaguar/Stratocaster
 * that works fine, but the flag bits are documented as unreliable ("not
 * recommended"), and on the PDP Riffmaster - which announces this same
 * GIP class - GIP_JA_BTN_SOLO doesn't reliably correlate with which row
 * is held (it's a separate thumbstick click), so using it to reinterpret
 * fret state there misreports whichever fret is currently held as soon as
 * the stick is clicked or bumped. The upper/lower fret bitmasks below
 * don't have this ambiguity and work identically on both devices.
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

	input_set_capability(dev, EV_KEY, BTN_MODE);
	input_set_capability(dev, EV_KEY, BTN_START);
	input_set_capability(dev, EV_KEY, BTN_SELECT);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY1);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY2);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY3);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY4);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY5);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY6);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY7);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY8);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY9);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY10);
	input_set_capability(dev, EV_KEY, BTN_TRIGGER_HAPPY11);
	input_set_abs_params(dev, ABS_Y, 0, 255, 0, 0);
	input_set_abs_params(dev, ABS_Z, 0, 255, 0, 0);
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

	if (len < sizeof(*pkt))
		return -EINVAL;

	buttons = le16_to_cpu(pkt->buttons);

	input_report_key(dev, BTN_START, buttons & GIP_JA_BTN_MENU);
	input_report_key(dev, BTN_SELECT, buttons & GIP_JA_BTN_VIEW);
	input_report_key(dev, BTN_TRIGGER_HAPPY1,
			 pkt->frets_upper & GIP_JA_FRET_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY2,
			 pkt->frets_upper & GIP_JA_FRET_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY3,
			 pkt->frets_upper & GIP_JA_FRET_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY4,
			 pkt->frets_upper & GIP_JA_FRET_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY5,
			 pkt->frets_upper & GIP_JA_FRET_ORANGE);
	input_report_key(dev, BTN_TRIGGER_HAPPY6,
			 pkt->frets_lower & GIP_JA_FRET_GREEN);
	input_report_key(dev, BTN_TRIGGER_HAPPY7,
			 pkt->frets_lower & GIP_JA_FRET_RED);
	input_report_key(dev, BTN_TRIGGER_HAPPY8,
			 pkt->frets_lower & GIP_JA_FRET_YELLOW);
	input_report_key(dev, BTN_TRIGGER_HAPPY9,
			 pkt->frets_lower & GIP_JA_FRET_BLUE);
	input_report_key(dev, BTN_TRIGGER_HAPPY10,
			 pkt->frets_lower & GIP_JA_FRET_ORANGE);
	input_report_key(dev, BTN_TRIGGER_HAPPY11,
			 buttons & GIP_JA_BTN_SOLO);
	input_report_abs(dev, ABS_Y, pkt->whammy);
	input_report_abs(dev, ABS_Z, pkt->tilt);
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
