TARGET = cathedral
OBJS = main.o

CFLAGS = -O2 -G0 -Wall -ffast-math $(EXTRA_CFLAGS)
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBS = -lpspgum_vfpu -lpspvfpu -lpspgu -lpspdebug -lpspctrl -lpspdisplay -lpspge -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Lux Aeterna
PSP_EBOOT_ICON = ICON0.PNG
PSP_EBOOT_PIC1 = PIC1.PNG

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
