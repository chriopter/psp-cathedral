TARGET = cathedral
OBJS = main.o util.o glass.o church.o music.o

CFLAGS = -O2 -G0 -Wall -ffast-math $(EXTRA_CFLAGS)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspgum_vfpu -lpspvfpu -lpspgu -lpspdebug -lpspmp3 -lpspaudio -lpsputility -lpspctrl -lpspdisplay -lpspge -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Lux Aeterna
PSP_EBOOT_ICON = media/xmb/ICON0.PNG
PSP_EBOOT_PIC1 = media/xmb/PIC1.PNG
PSP_EBOOT_ICON1 = media/xmb/ICON1.PMF
PSP_EBOOT_SND0 = media/xmb/SND0.AT3

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
