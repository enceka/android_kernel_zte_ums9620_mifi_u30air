# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_TYPEC)		+= typec.o
typec-y				:= class.o mux.o bus.o
obj-$(CONFIG_TYPEC)		+= altmodes/
obj-$(CONFIG_TYPEC_TCPM)	+= tcpm/
obj-$(CONFIG_TYPEC_UCSI)	+= ucsi/
obj-$(CONFIG_TYPEC_TPS6598X)	+= tps6598x.o
obj-$(CONFIG_TYPEC)		+= mux/
obj-$(CONFIG_USB_SC27XX_TYPEC)	+= sc27xx_typec.o

ifeq ($(CONFIG_VENDOR_ZTE_LOG_EXCEPTION),m)
	ccflags-y += -DCONFIG_VENDOR_ZTE_LOG_EXCEPTION
endif