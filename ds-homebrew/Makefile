# Démo dslink pour BlocksDS (https://blocksds.skylyrac.net)
# Compilation : make   ->  dslink_demo.nds
#
# Basé sur le Makefile par défaut des ROM ARM9 de BlocksDS. Si votre version
# de BlocksDS diffère, partez de l'exemple "dswifi" fourni avec BlocksDS et
# ajoutez simplement source/dslink.c et le dossier ../common aux includes.

BLOCKSDS     ?= /opt/blocksds/core

NAME         := dslink_demo
GAME_TITLE   := dslink demo
GAME_SUBTITLE := DS <-> PC via ESP32
GAME_AUTHOR  := homebrew

SOURCEDIRS   := source
INCLUDEDIRS  := source ../common

LIBS         := -ldswifi9 -lnds9
LIBDIRS      := $(BLOCKSDS)/libs/dswifi $(BLOCKSDS)/libs/libnds

include $(BLOCKSDS)/sys/default_makefiles/rom_arm9/Makefile
