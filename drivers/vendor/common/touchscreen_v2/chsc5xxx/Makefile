#semitouch touchpanel driver
ccflags-y += -I$(srctree)/drivers/vendor/common/touchscreen/
ccflags-y += -I$(srctree)/drivers/vendor/common/touchscreen/chsc5xxx/firmware_config/$(CONFIG_TS_FIRMWARE)/

obj-$(CONFIG_TOUCHSCREEN_CHSC5XXX)	    += semi_touch_driver.o

semi_touch_driver-objs += basic_util.o
semi_touch_driver-objs += i2c_communication.o
semi_touch_driver-objs += qual_core.o
semi_touch_driver-objs += semi_touch_function.o
semi_touch_driver-objs += semi_touch_upgrade.o
semi_touch_driver-objs += semi_touch_apk.o
semi_touch_driver-objs += semi_touch_device.o
semi_touch_driver-objs += semi_touch_custom.o
semi_touch_driver-objs += semi_touch_test_5448.o
semi_touch_driver-objs += semi_common_interface.o

