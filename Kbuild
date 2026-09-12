xone_gip-y := bus/bus.o bus/protocol.o auth/auth.o auth/crypto.o driver/common.o
xone_wired-y := transport/wired.o
xone_dongle-y := transport/dongle.o transport/mt76.o
xone_gip_gamepad-y := driver/gamepad.o
xone_gip_headset-y := driver/headset.o
xone_gip_chatpad-y := driver/chatpad.o
xone_gip_madcatz_strat-y := driver/madcatz_strat.o
xone_gip_madcatz_glam-y := driver/madcatz_glam.o
# pdp_jaguar.o (stock, real Jaguar/Stratocaster) is intentionally not built by
# this personal fork - see driver/pdp_riffmaster.c, which targets only the
# PDP Riffmaster and shares no build target with it.
xone_gip_pdp_riffmaster-y := driver/pdp_riffmaster.o

obj-m := xone_gip.o \
	xone_wired.o \
	xone_dongle.o \
	xone_gip_gamepad.o \
	xone_gip_headset.o \
	xone_gip_chatpad.o \
	xone_gip_madcatz_strat.o \
	xone_gip_madcatz_glam.o \
	xone_gip_pdp_riffmaster.o
