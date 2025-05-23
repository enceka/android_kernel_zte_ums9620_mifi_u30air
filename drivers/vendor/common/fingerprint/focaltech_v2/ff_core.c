/*
 *
 * FocalTech FingerPrint driver.
 *
 * Copyright (c) 2017-2022, FocalTech Systems, Ltd., all rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*****************************************************************************
* Included header files
*****************************************************************************/
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/printk.h>
#include <linux/signal.h>
#include <linux/init.h>
#include <linux/poll.h>
#include <linux/bug.h>
#include <linux/types.h>
#include <linux/param.h>
#include <linux/errno.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

#include "ff_log.h"
#include "ff_core.h"
#if defined(CONFIG_FINGERPRINT_FOCALTECH_SPI_SUPPORT) || defined(CONFIG_TRUSTKERNEL_TEE_SUPPORT)
#include "ff_spi.h"
#endif

#ifdef CONFIG_FINGERPRINT_FOCALTECH_ARCH_MTK
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
#include "mtk_disp_notify.h"
#include "mtk_panel_ext.h"
#elif IS_ENABLED(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#endif
#endif

#ifdef CONFIG_FINGERPRINT_FOCALTECH_ARCH_MTK
#if IS_ENABLED(CONFIG_DRM)
#if IS_ENABLED(CONFIG_DRM_PANEL)
#include <drm/drm_panel.h>
#else
#include <linux/msm_drm_notify.h>
#endif //CONFIG_DRM_PANEL

#elif IS_ENABLED(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#endif //CONFIG_DRM
#endif

#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
struct zlog_mod_info focaltech_zlog_fp_dev = {
	.module_no = ZLOG_MODULE_FP,
	.name = "fingerprint",
	.device_name = "ShenAo",
	.ic_name = "focaltech",
	.module_name = "FP",
	.fops = NULL,
};
#endif

/*****************************************************************************
* Private constant and macro definitions using #define
*****************************************************************************/
/* Define the driver version string. */
#define FF_DRV_VERSION              "V3.2.0-20230925"

/* Define the driver name. */
#define FF_DRV_NAME                 "focaltech_fp"
#define FF_WAKELOCK_TIMEOUT         3000

/*power voltage range*/
#define FF_VTG_MIN_UV               2800000
#define FF_VTG_MAX_UV               3300000

// Logging driver to logcat through uevent mechanism.
#undef LOG_TAG
#define LOG_TAG                     "focaltech"


/*****************************************************************************
* Static variable or functions
*****************************************************************************/


/*****************************************************************************
* Global variable or extern global variabls/functions
*****************************************************************************/
ff_context_t *g_ff_ctx;
/* Log level can be runtime configurable by 'FF_IOC_SYNC_CONFIG'. */
ff_log_level_t g_ff_log_level = (ff_log_level_t)(__FF_EARLY_LOG_LEVEL);


#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPICLK_SUPPORT
extern void mt_spi_enable_master_clk(struct spi_device *spidev);
extern void mt_spi_disable_master_clk(struct spi_device *spidev);
#endif

/*****************************************************************************
* Static function prototypes
*****************************************************************************/
static int ff_init_pins(ff_context_t *ff_ctx);
static int ff_init_power(ff_context_t *ff_ctx);
static int ff_register_irq(ff_context_t *ff_ctx);
static void ff_unprobe(ff_context_t *ff_ctx);
//static int fts_notifier_callback_init(ff_context_t *ff_ctx);
//static int fts_notifier_callback_exit(ff_context_t *ff_ctx);

#define ZLOG_MOD_NAME	"[ZTE_LDD_FP]"
#define ZLOG_VENDOR_NAME	"[FOCALTECH]"

int ff_log_printf(ff_log_level_t level, const char *tag, const char *fmt, ...)
{
    /* Prepare the storage. */
    va_list ap;
    static char uevent_env_buf[128];
    char *ptr = uevent_env_buf;
    int n, available = sizeof(uevent_env_buf);
    ff_context_t *ff_ctx = g_ff_ctx;

    /* Fill logging level. */
    available -= sprintf(uevent_env_buf, "FF_LOG=%1d", level);
    ptr += strlen(uevent_env_buf);

    /* Fill logging message. */
    va_start(ap, fmt);
    vsnprintf(ptr, available, fmt, ap);
    va_end(ap);

    /* Send to ff_device. */
    if (likely(ff_ctx) && unlikely(ff_ctx->ff_config.logcat_driver)
        && (ff_ctx->event_type == FF_EVENT_NETLINK)) {
        char *uevent_env[2] = {uevent_env_buf, NULL};
        kobject_uevent_env(&ff_ctx->fp_dev->kobj, KOBJ_CHANGE, uevent_env);
    }

    /* Native output. */
    switch (level) {
    case FF_LOG_LEVEL_ERR:
        n = pr_err("%s%s[E]: %s\n", ZLOG_MOD_NAME, ZLOG_VENDOR_NAME, ptr);
        break;
    case FF_LOG_LEVEL_WRN:
        n = pr_err("%s%s[W]: %s\n", ZLOG_MOD_NAME, ZLOG_VENDOR_NAME, ptr);
        break;
    case FF_LOG_LEVEL_INF:
        n = pr_err("%s%s[I]: %s\n", ZLOG_MOD_NAME, ZLOG_VENDOR_NAME, ptr);
        break;
    case FF_LOG_LEVEL_DBG:
    case FF_LOG_LEVEL_VBS:
    default:
        n = pr_err("%s%s: %s\n", ZLOG_MOD_NAME, ZLOG_VENDOR_NAME, ptr);
        break;
    }
    return n;
}

/*disable/enable irq*/
static int ff_set_irq(ff_context_t *ff_ctx, bool enable)
{
    FF_LOGD("[%s] enter", __func__);
    if (!ff_ctx || (ff_ctx->irq_num <= 0) || (!ff_ctx->b_driver_inited)) {
        FF_LOGE("ff_ctx/irq_num(%d)/b_driver_inited is invalid", ff_ctx->irq_num);
        return -EINVAL;
    }

    FF_LOGI("set irq : [%d] -> [%d]", ff_ctx->b_irq_enabled, enable);
    if (!ff_ctx->b_irq_enabled && enable) {
        enable_irq(ff_ctx->irq_num);
        ff_ctx->b_irq_enabled = true;
    } else if (ff_ctx->b_irq_enabled && !enable) {
        disable_irq(ff_ctx->irq_num);
        ff_ctx->b_irq_enabled = false;
    }

    FF_LOGD("[%s] exit", __func__);
    return 0;
}

/*reset device*/
static int ff_set_reset(ff_context_t *ff_ctx, bool high)
{
    int ret = 0;

    FF_LOGD("[%s] enter", __func__);
    if (!ff_ctx || (!ff_ctx->b_driver_inited)) {
        FF_LOGE("ff_ctx/b_driver_inited is invalid");
        return -EINVAL;
    }

    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        FF_LOGI("use pinctrl to control reset, status %s", high ? "high" : "low");
        if (high && ff_ctx->pins_reset_high)
            ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_reset_high);
        else if (!high && ff_ctx->pins_reset_low)
            ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_reset_low);
    }

    if (!ff_ctx->b_use_pinctrl && gpio_is_valid(ff_ctx->reset_gpio)) {
        FF_LOGI("use gpio(%d) to control reset, status %s", ff_ctx->reset_gpio, high ? "high" : "low");
        ret = gpio_direction_output(ff_ctx->reset_gpio, high);
    }

    if (ret < 0) {
        FF_LOGE("set reset to %d failed, ret = %d", high, ret);
    } else {
        FF_LOGD("set reset to %d success", high);
    }

    FF_LOGD("[%s] exit", __func__);
    return ret;
}

static int ff_power_on(ff_context_t *ff_ctx, bool on)
{
    int ret = 0;

    FF_LOGI("[%s] enter", __func__);
    if (!ff_ctx || (!ff_ctx->b_driver_inited)) {
        FF_LOGE("ff_ctx/b_driver_inited is invalid");
        return -EINVAL;
    }

    if (ff_ctx->b_power_always_on) {
        FF_LOGW("power is always on, no control power");
        return 0;
    }

    if (ff_ctx->b_power_on ^ on) {
        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            FF_LOGI("use regulator to control power, set power to %d", on);
            if (on) {
                ret = regulator_enable(ff_ctx->ff_vdd);
            } else {
                ret = regulator_disable(ff_ctx->ff_vdd);
            }
        }

        if (!ff_ctx->b_use_regulator) {
            if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
                FF_LOGI("use pinctrl to control power, set power to %d", on);
                if (on && ff_ctx->pins_power_high) {
                    ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_power_high);
                } else if (!on && ff_ctx->pins_power_low) {
                    ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_power_low);
                }
            }

            if (!ff_ctx->b_use_pinctrl && gpio_is_valid(ff_ctx->vdd_gpio)) {
                FF_LOGI("use gpio(%d) to control power, set power to %d", ff_ctx->vdd_gpio, on);
                ret = gpio_direction_output(ff_ctx->vdd_gpio, on);
            }
        }

        if (ret < 0) {
            FF_LOGE("set power to %d failed, ret = %d", on, ret);
        } else {
            FF_LOGD("set power to %d success", on);
            ff_ctx->b_power_on = on;
        }
    }

    FF_LOGI("[%s] exit", __func__);
    return ret;
}

static int ff_set_spiclk(ff_context_t *ff_ctx, bool enable)
{
    FF_LOGD("[%s] enter", __func__);
#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPICLK_SUPPORT
    if (ff_ctx && ff_ctx->b_spiclk && ff_ctx->spi) {
        FF_LOGV("set spi clock : [%d] -> [%d]", ff_ctx->b_spiclk_enabled, enable);
        if (!ff_ctx->b_spiclk_enabled && enable) {
            mt_spi_enable_master_clk(ff_ctx->spi);
            ff_ctx->b_spiclk_enabled = true;
        } else if (ff_ctx->b_spiclk_enabled && !enable) {
            mt_spi_disable_master_clk(ff_ctx->spi);
            ff_ctx->b_spiclk_enabled = false;
        }
    } else {
        FF_LOGW("spi clock setting isn't supported");
    }
#endif
    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int ff_register_input(ff_context_t *ff_ctx)
{
    int ret = 0;
    int i = 0;
    ff_driver_config_t *config = &ff_ctx->ff_config;
    struct input_dev *input;

    FF_LOGD("[%s] enter", __func__);
    /* Allocate the input device. */
    input = input_allocate_device();
    if (!input) {
        FF_LOGE("input_allocate_device() failed");
        return (-ENOMEM);
    }

    /* Register the key event capabilities. */
    for (i = 0; i < FF_GK_MAX; i++) {
        if (config->gesture_keycode[i] > 0) {
            FF_LOGD("register gesture keycode : [%d]", config->gesture_keycode[i]);
            input_set_capability(input, EV_KEY, config->gesture_keycode[i]);
        }
    }

    /* Register the allocated input device. */
    input->name = "ff_key";
    ret = input_register_device(input);
    if (ret < 0) {
        FF_LOGE("input_register_device failed, ret = %d", ret);
        input_free_device(input);
        input = NULL;
        return (-ENODEV);
    } else {
        FF_LOGI("input_register_device success");
    }

    ff_ctx->input = input;
    FF_LOGD("[%s] exit", __func__);
    return ret;
}

static int ff_ctl_init_driver(ff_context_t *ff_ctx)
{
    int ret = 0;
    FF_LOGI("[%s] enter", __func__);

    if ((!ff_ctx) || ff_ctx->b_driver_inited) {
        FF_LOGE("ff_ctx is null, or driver has already initialized");
        return -EINVAL;
    }

    ret = ff_init_pins(ff_ctx);
    if (ret < 0) {
        FF_LOGE("init gpio pins failed, ret = %d", ret);
        return ret;
    } else {
        FF_LOGI("init gpio pins success");
    }

    ret = ff_init_power(ff_ctx);
    if (ret < 0) {
        FF_LOGE("init power failed, ret = %d", ret);
        goto err_init_power;
    } else {
        FF_LOGI("init power success");
    }

    ret = ff_register_irq(ff_ctx);
    if (ret < 0) {
        FF_LOGE("register ff irq failed, ret = %d", ret);
        goto err_request_irq;
    } else {
        FF_LOGI("register ff irq success");
    }

    if (ff_ctx->b_gesture_support) {
        /* Initialize the input subsystem. */
        ret = ff_register_input(ff_ctx);
        if (ret < 0) {
            FF_LOGE("ff_register_input failed, ret = %d", ret);
            goto err_register_input;
        } else {
            FF_LOGI("ff_register_input success");
        }
    }

    /* Enable SPI clock. */
    ret = ff_set_spiclk(ff_ctx, 1);
    if (ret < 0) {
        FF_LOGE("enable spi clock failed");
        goto err_enable_spiclk;
    } else {
        FF_LOGD("enable spi clock success");
    }

    if (ff_ctx->b_screen_onoff_event) {
        /*ret = fts_notifier_callback_init(ff_ctx);
        if (ret) {
            FF_LOGE("init notifier callback failed");
        }*/
    }

    ff_ctx->b_driver_inited = true;
    FF_LOGI("[%s] normal exit", __func__);
    return 0;

err_enable_spiclk:
    if (ff_ctx->b_gesture_support) {
        input_unregister_device(ff_ctx->input);
        ff_ctx->input = NULL;
    }
err_register_input:
    if (ff_ctx->irq_num > 0) {
        disable_irq_wake(ff_ctx->irq_num);
        free_irq(ff_ctx->irq_num, ff_ctx);
        ff_ctx->irq_num = -1;
    }
err_request_irq:
    if (!ff_ctx->b_power_always_on) {
        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            regulator_put(ff_ctx->ff_vdd);
        }
        if (!ff_ctx->b_use_regulator && !ff_ctx->b_use_pinctrl) {
            if (gpio_is_valid(ff_ctx->vdd_gpio)) {
                gpio_free(ff_ctx->vdd_gpio);
            }
        }
    }
err_init_power:
    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        devm_pinctrl_put(ff_ctx->pinctrl);
        ff_ctx->pins_irq_as_int = NULL;
        ff_ctx->pins_reset_low = NULL;
        ff_ctx->pins_reset_high = NULL;
        ff_ctx->pins_power_low = NULL;
        ff_ctx->pins_power_high = NULL;
    } else if (!ff_ctx->b_use_pinctrl) {
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            gpio_free(ff_ctx->irq_gpio);
        }
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            gpio_free(ff_ctx->reset_gpio);
        }
    }

    FF_LOGE("[%s] abnormal exit", __func__);
    return ret;
}

static int ff_ctl_free_driver(ff_context_t *ff_ctx)
{
    int ret = 0;
    FF_LOGI("[%s] enter", __func__);

    if ((!ff_ctx) || !ff_ctx->b_driver_inited) {
        FF_LOGE("ff_ctx is null, or driver has not initialized");
        return -EINVAL;
    }

    if (ff_ctx->b_screen_onoff_event) {
        //fts_notifier_callback_exit(ff_ctx);
    }

    /* Disable SPI clock. */
    ret = ff_set_spiclk(ff_ctx, 0);
    if (ret < 0) {
        FF_LOGE("disable spi clock failed");
    } else {
        FF_LOGD("disable spi clock success");
    }

    /* De-initialize the input subsystem. */
    if (ff_ctx->input) {
        input_unregister_device(ff_ctx->input);
        FF_LOGI("input_unregister_device");
        ff_ctx->input = NULL;
    }

    /*free irq*/
    if (ff_ctx->irq_num > 0) {
        disable_irq_wake(ff_ctx->irq_num);
        free_irq(ff_ctx->irq_num, ff_ctx);
        FF_LOGI("disable_irq_wake and free_irq");
        ff_ctx->irq_num = -1;
    }

    /*free power pin*/
    if (!ff_ctx->b_power_always_on) {
        if (ff_ctx->b_power_on) {
            ff_set_reset(ff_ctx, 0);
            FF_LOGI("set reset to low");
            msleep(5);
            ff_power_on(ff_ctx, 0);
            FF_LOGI("set power to low");
            msleep(10);
        }

        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            regulator_put(ff_ctx->ff_vdd);
            FF_LOGI("regulator_put vdd");
        }
        if (!ff_ctx->b_use_regulator && !ff_ctx->b_use_pinctrl) {
            if (gpio_is_valid(ff_ctx->vdd_gpio)) {
                gpio_free(ff_ctx->vdd_gpio);
                FF_LOGI("gpio_free vdd_gpio");
            }
        }
    }

    /*free rst/int pins*/
    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        devm_pinctrl_put(ff_ctx->pinctrl);
        ff_ctx->pins_irq_as_int = NULL;
        ff_ctx->pins_reset_low = NULL;
        ff_ctx->pins_reset_high = NULL;
        ff_ctx->pins_power_low = NULL;
        ff_ctx->pins_power_high = NULL;
    } else if (!ff_ctx->b_use_pinctrl) {
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            gpio_free(ff_ctx->irq_gpio);
            FF_LOGI("gpio_free irq_gpio");
        }
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            gpio_free(ff_ctx->reset_gpio);
            FF_LOGI("gpio_free reset_gpio");
        }
    }

    ff_ctx->b_driver_inited = false;
    FF_LOGI("[%s] exit", __func__);
    return ret;
}

static int ff_ctl_report_key_event(ff_context_t *ff_ctx, unsigned long arg)
{
    ff_key_event_t kevent;

    FF_LOGD("[%s] enter", __func__);
    if (copy_from_user(&kevent, (ff_key_event_t *)arg, sizeof(ff_key_event_t))) {
        FF_LOGE("copy_from_user(ff_key_event_t) failed");
        return (-EFAULT);
    }

    FF_LOGI("key code = [%d], value = [%d]", kevent.code, kevent.value);
    if (ff_ctx->input && kevent.code) {
        input_report_key(ff_ctx->input, kevent.code, kevent.value);
        input_sync(ff_ctx->input);
    }

    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int ff_ctl_get_version(ff_context_t *ff_ctx, unsigned long arg)
{
    char version[FF_DRV_VERSION_LEN] = { 0 };

    FF_LOGD("[%s] enter", __func__);
    strncpy(version, FF_DRV_VERSION, FF_DRV_VERSION_LEN);
    if (copy_to_user((void *)arg, version, FF_DRV_VERSION_LEN)) {
        FF_LOGE("copy_to_user(version) failed.");
        return (-EFAULT);
    }

    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int ff_ctl_acquire_wakelock(ff_context_t *ff_ctx, unsigned long arg)
{
    u32 wake_lock_ms = 0;
    FF_LOGD("[%s] enter", __func__);

    if (copy_from_user(&wake_lock_ms, (u32 *)arg, sizeof(u32))) {
        FF_LOGE("copy_from_user(wake_lock_ms) failed.");
        return (-EFAULT);
    }

    FF_LOGD("wake_lock_ms = [%u]", wake_lock_ms);
    if (wake_lock_ms == 0) {
        __pm_stay_awake(ff_ctx->p_ws_ctl);
    } else {
        __pm_wakeup_event(ff_ctx->p_ws_ctl, wake_lock_ms);
    }

    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int ff_ctl_release_wakelock(ff_context_t *ff_ctx)
{
    FF_LOGD("[%s] enter", __func__);
    __pm_relax(ff_ctx->p_ws_ctl);
    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int ff_ctl_sync_config(ff_context_t *ff_ctx, unsigned long arg)
{
    int  i = 0;
    ff_driver_config_t driver_config;

    FF_LOGD("[%s] enter", __func__);
    if (copy_from_user(&driver_config, (ff_driver_config_t *)arg, sizeof(ff_driver_config_t))) {
        FF_LOGE("copy_from_user(ff_driver_config_t) failed");
        return (-EFAULT);
    }

    /*check gesture is supported or not*/
    ff_ctx->b_gesture_support = false;
    for (i = 0; i < FF_GK_MAX; i++) {
        if (driver_config.gesture_keycode[i] > 0) {
            ff_ctx->b_gesture_support = true;
            ff_ctx->ff_config.gesture_keycode[i] = driver_config.gesture_keycode[i];
        }
    }

    if (ff_ctx->b_gesture_support) {
        FF_LOGI("gesture is supported");
    }

    /* Take logging level effect. */
    FF_LOGI("set log : [%d] -> [%d]", g_ff_log_level, driver_config.log_level);
    //g_ff_log_level = driver_config.log_level;
    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int ff_ctl_fasync(int fd, struct file *filp, int mode)
{
    int err = 0;
    ff_context_t *ff_ctx = filp->private_data;

    FF_LOGD("[%s] enter", __func__);
    if (!ff_ctx) {
        FF_LOGE("ff_ctx is null");
        return -ENODATA;
    }

    FF_LOGI("%s : mode = [0x%08x]", __func__, mode);
    err = fasync_helper(fd, filp, mode, &ff_ctx->async_queue);

    FF_LOGD("[%s] exit", __func__);
    return err;
}

static long ff_ctl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    ff_context_t *ff_ctx = filp->private_data;

    FF_LOGD("[%s] enter", __func__);
    if (!ff_ctx) {
        FF_LOGE("ff_ctx is null");
        return -ENODATA;
    }

    FF_LOGD("ioctl cmd : [%x], arg : [%lx]", (int)cmd, arg);
    switch (cmd) {
    case FF_IOC_GET_EVENT_INFO:
        FF_LOGI("FF_IOC_GET_EVENT_INFO, get event info : [%d]", ff_ctx->poll_event);
        ret = __put_user(ff_ctx->poll_event, (int __user *)arg);
        ff_ctx->poll_event = FF_POLLEVT_NONE;
        break;
    case FF_IOC_INIT_DRIVER:
        FF_LOGI("FF_IOC_INIT_DRIVER");
        if (ff_ctx->b_driver_inited) {
            FF_LOGI("ff driver inited, need free first");
            ret = ff_ctl_free_driver(ff_ctx);
            if (ret < 0) {
                FF_LOGE("free driver failed");
                return ret;
            }
        }
        ret = ff_ctl_init_driver(ff_ctx);
        break;
    case FF_IOC_FREE_DRIVER:
        FF_LOGI("FF_IOC_FREE_DRIVER");
        break;
    case FF_IOC_RESET_DEVICE:
        FF_LOGI("FF_IOC_RESET_DEVICE, need low->high");
        ret = ff_set_reset(ff_ctx, 0);
        msleep(10);
        ret = ff_set_reset(ff_ctx, 1);
        break;
    case FF_IOC_RESET_DEVICE_HL:
        FF_LOGI("FF_IOC_RESET_DEVICE_HL");
        ret = ff_set_reset(ff_ctx, !!arg);
        break;
    case FF_IOC_ENABLE_IRQ:
        FF_LOGI("FF_IOC_ENABLE_IRQ");
        ret = ff_set_irq(ff_ctx, 1);
        break;
    case FF_IOC_DISABLE_IRQ:
        FF_LOGI("FF_IOC_DISABLE_IRQ");
        ret = ff_set_irq(ff_ctx, 0);
        break;
    case FF_IOC_ENABLE_SPI_CLK:
        FF_LOGD("FF_IOC_ENABLE_SPI_CLK");
        ret = ff_set_spiclk(ff_ctx, 1);
        break;
    case FF_IOC_DISABLE_SPI_CLK:
        FF_LOGD("FF_IOC_DISABLE_SPI_CLK");
        ret = ff_set_spiclk(ff_ctx, 0);
        break;
    case FF_IOC_ENABLE_POWER:
        FF_LOGI("FF_IOC_ENABLE_POWER");
        ret = ff_power_on(ff_ctx, 1);
        break;
    case FF_IOC_DISABLE_POWER:
        FF_LOGI("FF_IOC_DISABLE_POWER");
        ret = ff_power_on(ff_ctx, 0);
        break;
    case FF_IOC_REPORT_KEY_EVENT:
        FF_LOGI("FF_IOC_REPORT_KEY_EVENT");
        ret = ff_ctl_report_key_event(ff_ctx, arg);
        break;
    case FF_IOC_SYNC_CONFIG:
        FF_LOGI("FF_IOC_SYNC_CONFIG");
        ret = ff_ctl_sync_config(ff_ctx, arg);
        break;
    case FF_IOC_GET_VERSION:
        FF_LOGI("FF_IOC_GET_VERSION");
        ret = ff_ctl_get_version(ff_ctx, arg);
        break;
    case FF_IOC_SET_VENDOR_INFO:
        FF_LOGI("FF_IOC_SET_VENDOR_INFO");
        break;
    case FF_IOC_WAKELOCK_ACQUIRE:
        FF_LOGI("FF_IOC_WAKELOCK_ACQUIRE");
        ret = ff_ctl_acquire_wakelock(ff_ctx, arg);
        break;
    case FF_IOC_WAKELOCK_RELEASE:
        FF_LOGI("FF_IOC_WAKELOCK_RELEASE");
        ret = ff_ctl_release_wakelock(ff_ctx);
        break;
    case FF_IOC_GET_LOGLEVEL:
        FF_LOGI("FF_IOC_GET_LOGLEVEL, get loglevel : [%d]", g_ff_log_level);
        //ret = __put_user(g_ff_log_level, (int __user *)arg);
        break;
    case FF_IOC_SET_LOGLEVEL:
        FF_LOGI("FF_IOC_SET_LOGLEVEL");
        //ret = __get_user(g_ff_log_level, (int __user *)arg);
        FF_LOGI("set log level : [%d]", g_ff_log_level);
        break;
    case FF_IOC_GET_FEATURE:
        FF_LOGI("FF_IOC_GET_FEATURE, feature_ver : [%04xh]", ff_ctx->feature.ver);
        ret = copy_to_user((ff_feature_t __user *)arg, &ff_ctx->feature, sizeof(ff_feature_t));
        break;
    case FF_IOC_GET_EVENT_TYPE:
        FF_LOGI("FF_IOC_GET_EVENT_TYPE, get event_type : [%d]", ff_ctx->event_type);
        ret = __put_user(ff_ctx->event_type, (int __user *)arg);
        break;
    case FF_IOC_SET_EVENT_TYPE:
        FF_LOGI("FF_IOC_SET_EVENT_TYPE");
        ret = __get_user(ff_ctx->event_type, (int __user *)arg);
        FF_LOGI("set event type : [%d]", ff_ctx->event_type);
        break;
    case FF_IOC_GET_SPICLK_HZ:
        FF_LOGI("FF_IOC_GET_SPICLK_HZ, get spi-clk [%d] hz", ff_ctx->spi_clk_hz);
        ret = __put_user(ff_ctx->spi_clk_hz, (uint32_t __user *)arg);
        break;
    case FF_IOC_UNPROBE:
        FF_LOGI("FF_IOC_UNPROBE");
        ff_unprobe(ff_ctx);
        break;
    default:
        ret = (-EINVAL);
        break;
    }

    FF_LOGD("[%s] exit", __func__);
    return ret;
}

static unsigned int ff_ctl_poll(struct file *filp, struct poll_table_struct *wait)
{
    unsigned int mask = 0;
    ff_context_t *ff_ctx = filp->private_data;

    FF_LOGD("[%s] enter", __func__);
    if (!ff_ctx) {
        FF_LOGE("ff_ctx is null");
        return -ENODATA;
    }

    poll_wait(filp, &ff_ctx->wait_queue_head, wait);
    if (ff_ctx->poll_event != FF_POLLEVT_NONE) {
        mask |= POLLIN | POLLRDNORM;
    }

    FF_LOGD("[%s] exit", __func__);
    return mask;
}

static int ff_ctl_open(struct inode *inode, struct file *filp)
{
    ff_context_t *ff_ctx = container_of(inode->i_cdev, ff_context_t, ff_cdev);
    FF_LOGI("[%s] enter", __func__);
    /*Don't allow open device while b_ff_probe is 0*/
    if (!ff_ctx || !ff_ctx->b_ff_probe) {
        FF_LOGE("ff_ctx is null/probe(%d) failed", ff_ctx->b_ff_probe);
        return -EINVAL;
    }
    filp->private_data = ff_ctx;

#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
    ff_ctx->zlog_fp_client = zlog_register_client(&focaltech_zlog_fp_dev);
    if (ff_ctx->zlog_fp_client) {
        FF_LOGI("[%s] zlog_register_focaltechfp_client success", __func__);
    } else {
        FF_LOGE("[%s] zlog_register_focaltechfp_client fail", __func__);
    }
#endif

    FF_LOGI("[%s] exit", __func__);
    return 0;
}

static int ff_ctl_release(struct inode *inode, struct file *filp)
{
    int err = 0;
    ff_context_t *ff_ctx = filp->private_data;
    FF_LOGI("[%s] enter", __func__);

    if (ff_ctx->b_driver_inited) {
        err = ff_ctl_free_driver(ff_ctx);
        if (err < 0) {
            FF_LOGE("ff_ctl_free_driver failed");
        } else {
            FF_LOGI("ff_ctl_free_driver success");
        }
    }

    /* Remove this filp from the asynchronously notified filp's. */
    err = ff_ctl_fasync(-1, filp, 0);

#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
    if (ff_ctx->zlog_fp_client) {
        zlog_unregister_client(ff_ctx->zlog_fp_client);
        FF_LOGI("[%s] zlog_unregister_client focaltech_zlog_fp_dev", __func__);
    }
#endif

    FF_LOGI("[%s] exit", __func__);
    return err;
}

#ifdef CONFIG_COMPAT
static long ff_ctl_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    int err = 0;
    FF_LOGD("[%s] enter", __func__);

    err = ff_ctl_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));

    FF_LOGD("[%s] exit", __func__);
    return err;
}
#endif
///////////////////////////////////////////////////////////////////////////////

static struct file_operations ff_fops = {
    .owner          = THIS_MODULE,
    .fasync         = ff_ctl_fasync,
    .unlocked_ioctl = ff_ctl_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl   = ff_ctl_compat_ioctl,
#endif
    .poll           = ff_ctl_poll,
    .open           = ff_ctl_open,
    .release        = ff_ctl_release,
};

int fts_power_sequence(bool power_on)
{
    ff_context_t *ff_ctx = g_ff_ctx;

    FF_LOGD("[%s] enter", __func__);
    if (!ff_ctx) {
        FF_LOGE("ff_ctx is null");
        return -EINVAL;
    }

    if (power_on) {
        FF_LOGI("power sequence on");
        ff_set_reset(ff_ctx, 0);
        msleep(5);
        ff_power_on(ff_ctx, 1);
        msleep(10);
        ff_set_reset(ff_ctx, 1);
        msleep(10);
    } else {
        FF_LOGI("power sequence off, delay 100ms");
        ff_set_reset(ff_ctx, 0);
        msleep(5);
        ff_power_on(ff_ctx, 0);
        msleep(100);
    }

    FF_LOGD("[%s] exit", __func__);
    return 0;
}

int ff_init_driver(void)
{
    int ret = 0;
    ff_context_t *ff_ctx = g_ff_ctx;

    FF_LOGI("[%s] enter", __func__);
    if ((!ff_ctx) || ff_ctx->b_driver_inited) {
        FF_LOGE("ff_ctx is null, or driver has already initialized");
        return -EINVAL;
    }

    ret = ff_init_pins(ff_ctx);
    if (ret < 0) {
        FF_LOGE("init gpio pins failed, ret = %d", ret);
        return ret;
    } else {
        FF_LOGI("init gpio pins success");
    }

    ret = ff_init_power(ff_ctx);
    if (ret < 0) {
        FF_LOGE("init power failed, ret = %d", ret);
        goto err_init_power;
    } else {
        FF_LOGI("init power success");
    }

    /* Enable SPI clock. */
    ret = ff_set_spiclk(ff_ctx, 1);
    if (ret < 0) {
        FF_LOGE("enable spi clock failed");
        goto err_enable_spiclk;
    }

    ff_ctx->b_driver_inited = true;
    FF_LOGI("[%s] normal exit", __func__);
    return 0;

err_enable_spiclk:
    if (!ff_ctx->b_power_always_on) {
        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            regulator_put(ff_ctx->ff_vdd);
        }
        if (!ff_ctx->b_use_regulator && !ff_ctx->b_use_pinctrl) {
            if (gpio_is_valid(ff_ctx->vdd_gpio)) {
                gpio_free(ff_ctx->vdd_gpio);
            }
        }
    }
err_init_power:
    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        devm_pinctrl_put(ff_ctx->pinctrl);
        ff_ctx->pins_irq_as_int = NULL;
        ff_ctx->pins_reset_low = NULL;
        ff_ctx->pins_reset_high = NULL;
        ff_ctx->pins_power_low = NULL;
        ff_ctx->pins_power_high = NULL;
    } else if (!ff_ctx->b_use_pinctrl) {
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            gpio_free(ff_ctx->irq_gpio);
        }
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            gpio_free(ff_ctx->reset_gpio);
        }
    }

    FF_LOGE("[%s] abnormal exit", __func__);
    return ret;
}

int ff_free_driver(void)
{
    int ret = 0;
    ff_context_t *ff_ctx = g_ff_ctx;

    FF_LOGI("[%s] enter", __func__);
    if ((!ff_ctx) || !ff_ctx->b_driver_inited) {
        FF_LOGE("ff_ctx is null, or driver has not initialized");
        return -EINVAL;
    }

    /* Disable SPI clock. */
    ret = ff_set_spiclk(ff_ctx, 0);
    if (ret < 0) {
        FF_LOGE("disable spi clock failed");
    }

    /*free power pin*/
    if (!ff_ctx->b_power_always_on) {
        if (ff_ctx->b_power_on) {
            ff_set_reset(ff_ctx, 0);
            msleep(5);
            ff_power_on(ff_ctx, 0);
            msleep(10);
        }

        if (ff_ctx->b_use_regulator && ff_ctx->ff_vdd) {
            regulator_put(ff_ctx->ff_vdd);
        }
        if (!ff_ctx->b_use_regulator && !ff_ctx->b_use_pinctrl) {
            if (gpio_is_valid(ff_ctx->vdd_gpio)) {
                gpio_free(ff_ctx->vdd_gpio);
            }
        }
    }

    /*free rst/int pins*/
    if (ff_ctx->b_use_pinctrl && ff_ctx->pinctrl) {
        devm_pinctrl_put(ff_ctx->pinctrl);
        ff_ctx->pins_irq_as_int = NULL;
        ff_ctx->pins_reset_low = NULL;
        ff_ctx->pins_reset_high = NULL;
        ff_ctx->pins_power_low = NULL;
        ff_ctx->pins_power_high = NULL;
    } else if (!ff_ctx->b_use_pinctrl) {
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            gpio_free(ff_ctx->irq_gpio);
        }
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            gpio_free(ff_ctx->reset_gpio);
        }
    }

    ff_ctx->b_driver_inited = false;
    FF_LOGI("[%s] exit", __func__);
    return ret;
}

/* ff_log node */
static ssize_t ff_log_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    FF_LOGD("[%s] enter", __func__);
    count += snprintf(buf + count, PAGE_SIZE, "log level:%d\n", g_ff_log_level);
    FF_LOGD("[%s] exit", __func__);
    return count;
}

static ssize_t ff_log_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    int value = 0;
    FF_LOGD("[%s] enter", __func__);
    sscanf(buf, "%d", &value);
    FF_LOGI("log level = %d -> %d", g_ff_log_level, value);
    g_ff_log_level = value;
    FF_LOGD("[%s] exit", __func__);
    return count;
}

/* ff_reset interface */
static ssize_t ff_reset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGD("[%s] enter", __func__);
    if (ff_ctx && ff_ctx->b_driver_inited) {
        ff_set_reset(ff_ctx, 0);
        msleep(10);
        ff_set_reset(ff_ctx, 1);
        count = snprintf(buf, PAGE_SIZE, "fingerprint device has been reset\n");
    }
    FF_LOGD("[%s] exit", __func__);
    return count;
}

static ssize_t ff_reset_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGD("[%s] enter", __func__);
    if (buf[0] == '1') {
        ff_set_reset(ff_ctx, 1);
    } else if (buf[0] == '0') {
        ff_set_reset(ff_ctx, 0);
    }
    FF_LOGD("[%s] exit", __func__);
    return count;
}

/* ff_irq interface */
static ssize_t ff_irq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGD("[%s] enter", __func__);
    if (ff_ctx && (ff_ctx->irq_num > 0) && ff_ctx->b_driver_inited) {
        struct irq_desc *desc = irq_to_desc(ff_ctx->irq_num);
        count = snprintf(buf, PAGE_SIZE, "irq_depth = %d\n", desc->depth);
    }
    FF_LOGD("[%s] exit", __func__);
    return count;
}

static ssize_t ff_irq_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGD("[%s] enter", __func__);
    if (buf[0] == '1') {
        ff_set_irq(ff_ctx, 1);
    } else if (buf[0] == '0') {
        ff_set_irq(ff_ctx, 0);
    }
    FF_LOGD("[%s] exit", __func__);
    return count;
}

/* ff_info interface */
static ssize_t ff_info_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    ssize_t count = 0;
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGD("[%s] enter", __func__);
    if (ff_ctx) {
        count += snprintf(buf + count, PAGE_SIZE, "gpio, reset = %d, int = %d-%d, power = %d\n",
                          ff_ctx->reset_gpio, ff_ctx->irq_gpio, ff_ctx->irq_num, ff_ctx->vdd_gpio);
        count += snprintf(buf + count, PAGE_SIZE, "power_always_on = %d, use_regulator = %d, use_pinctrl = %d\n",
                          ff_ctx->b_power_always_on, ff_ctx->b_use_regulator, ff_ctx->b_use_pinctrl);
        count += snprintf(buf + count, PAGE_SIZE, "read_chip = %d, ree = %d, screen on/off = %d\n",
                          ff_ctx->b_read_chipid, ff_ctx->b_ree, ff_ctx->b_screen_onoff_event);

        count += snprintf(buf + count, PAGE_SIZE, "driver_init = %d, power_on = %d, irq_en = %d\n",
                          ff_ctx->b_driver_inited, ff_ctx->b_power_on, ff_ctx->b_irq_enabled);

        count += snprintf(buf + count, PAGE_SIZE, "spiclk_support = %d, spiclk_enabled = %d\n",
                          ff_ctx->b_spiclk, ff_ctx->b_spiclk_enabled);

        count += snprintf(buf + count, PAGE_SIZE, "gesture_en = %d, %d-%d-%d-%d\n", ff_ctx->b_gesture_support,
                          ff_ctx->ff_config.gesture_keycode[0], ff_ctx->ff_config.gesture_keycode[1],
                          ff_ctx->ff_config.gesture_keycode[2], ff_ctx->ff_config.gesture_keycode[3]);

        count += snprintf(buf + count, PAGE_SIZE, "chid id = 0x%x\n", ff_ctx->chip_id);
    }
    FF_LOGD("[%s] exit", __func__);
    return count;
}

static ssize_t ff_info_store(
    struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct platform_device *pdev = to_platform_device(dev);
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGD("[%s] enter", __func__);
    if (buf[0] == '1') {
        ff_ctl_init_driver(ff_ctx);
    } else if (buf[0] == '0') {
        ff_ctl_free_driver(ff_ctx);
    }
    FF_LOGD("[%s] exit", __func__);
    return count;
}

static DEVICE_ATTR(ff_log, S_IRUGO | S_IWUSR, ff_log_show, ff_log_store);
static DEVICE_ATTR(ff_reset, S_IRUGO | S_IWUSR, ff_reset_show, ff_reset_store);
static DEVICE_ATTR(ff_irq, S_IRUGO | S_IWUSR, ff_irq_show, ff_irq_store);
static DEVICE_ATTR(ff_info, S_IRUGO | S_IWUSR, ff_info_show, ff_info_store);
static struct attribute *ff_attrs[] = {
    &dev_attr_ff_log.attr,
    &dev_attr_ff_reset.attr,
    &dev_attr_ff_irq.attr,
    &dev_attr_ff_info.attr,
    NULL
};
static struct attribute_group ff_attrs_group = {
    .attrs = ff_attrs
};

static void ff_work_device_event(struct work_struct *ws)
{
    ff_context_t *ff_ctx = container_of(ws, ff_context_t, event_work);
    char *uevent_env[2] = {"FF_INTERRUPT", NULL};
    FF_LOGD("[%s] enter", __func__);

    /* System should keep wakeup for 2 seconds at least. */
    __pm_wakeup_event(ff_ctx->p_ws, FF_WAKELOCK_TIMEOUT);

    FF_LOGD("%s(irq = %d, ..) toggled", __func__, ff_ctx->irq_num);
    if (ff_ctx->event_type == FF_EVENT_POLL) {
        ff_ctx->poll_event |= FF_POLLEVT_INTERRUPT;
        wake_up_interruptible(&ff_ctx->wait_queue_head);
    } else if (ff_ctx->event_type == FF_EVENT_NETLINK) {
        kobject_uevent_env(&ff_ctx->fp_dev->kobj, KOBJ_CHANGE, uevent_env);
    } else {
        FF_LOGE("unknowed event type = %d", ff_ctx->event_type);
    }

    FF_LOGD("[%s] exit", __func__);
}


static irqreturn_t ff_irq_handler(int irq, void *dev_id)
{
    ff_context_t *ff_ctx = (ff_context_t *)dev_id;

    if (ff_ctx && (likely(irq == ff_ctx->irq_num))) {
        if (ff_ctx->ff_config.enable_fasync && ff_ctx->async_queue) {
            kill_fasync(&ff_ctx->async_queue, SIGIO, POLL_IN);
        } else {
            schedule_work(&ff_ctx->event_work);
        }
    }

    return IRQ_HANDLED;
}


static void ff_parse_dt(struct device *dev, ff_context_t *ff_ctx)
{
    struct device_node *np = dev->of_node;

    FF_LOGI("[%s] enter", __func__);
    if (!np) {
        FF_LOGE("device node is null");
        return ;
    }

    /*check if power is always on*/
    ff_ctx->b_power_always_on = of_property_read_bool(np, "focaltech_fp,power-always-on");

    /*check if power regulator is used*/
    ff_ctx->b_use_regulator = of_property_read_bool(np, "focaltech_fp,use-regulator");

    /*check if pinctrl(irq and reset) is used*/
    ff_ctx->b_use_pinctrl = of_property_read_bool(np, "focaltech_fp,use-pinctrl");

    /*check if need read chip id*/
    ff_ctx->b_read_chipid = of_property_read_bool(np, "focaltech_fp,read-chip");

    /*check gpios*/
    ff_ctx->reset_gpio = of_get_named_gpio(np, "focaltech_fp,reset-gpio", 0);
    if (!gpio_is_valid(ff_ctx->reset_gpio)) {
        FF_LOGD("unable to get ff reset gpio");
    }

    ff_ctx->irq_gpio = of_get_named_gpio(np, "focaltech_fp,irq-gpio", 0);
    if (!gpio_is_valid(ff_ctx->irq_gpio)) {
        FF_LOGD("unable to get ff irq gpio");
    }

    ff_ctx->vdd_gpio = of_get_named_gpio(np, "focaltech_fp,vdd-gpio", 0);
    if (!gpio_is_valid(ff_ctx->vdd_gpio)) {
        FF_LOGD("unable to get ff vdd gpio");
    }

    FF_LOGI("irq gpio = [%d], reset gpio = [%d], vdd gpio = [%d]", ff_ctx->irq_gpio,
            ff_ctx->reset_gpio, ff_ctx->vdd_gpio);
    FF_LOGI("power_always_on = [%d], use_regulator = [%d], use_pinctrl = [%d]",
            ff_ctx->b_power_always_on, ff_ctx->b_use_regulator, ff_ctx->b_use_pinctrl);
    FF_LOGI("read_chip = [%d], ree = [%d], spiclk = [%d], screen on/off = [%d]", ff_ctx->b_read_chipid,
            ff_ctx->b_ree, ff_ctx->b_spiclk, ff_ctx->b_screen_onoff_event);

    FF_LOGI("[%s] exit", __func__);
}

static int ff_init_pins(ff_context_t *ff_ctx)
{
    int ret = 0;

    FF_LOGI("[%s] enter", __func__);
    if (ff_ctx->b_use_pinctrl) {
        /*get pinctrl from DTS*/
        ff_ctx->pinctrl = devm_pinctrl_get(&ff_ctx->pdev->dev);
        if (IS_ERR_OR_NULL(ff_ctx->pinctrl)) {
            ret = PTR_ERR(ff_ctx->pinctrl);
            FF_LOGE("Failed to get pinctrl, please check dts, ret = %d", ret);
            return ret;
        }

        ff_ctx->pins_reset_low = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_reset_low");
        if (IS_ERR_OR_NULL(ff_ctx->pins_reset_low)) {
            ret = PTR_ERR(ff_ctx->pins_reset_low);
            FF_LOGE("ff_pins_reset_low not found, ret = %d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            return ret;
        }

        ff_ctx->pins_reset_high = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_reset_high");
        if (IS_ERR_OR_NULL(ff_ctx->pins_reset_high)) {
            ret = PTR_ERR(ff_ctx->pins_reset_high);
            FF_LOGE("pins_reset_high not found, ret = %d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            return ret;
        }

        ff_ctx->pins_irq_as_int = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_irq_as_int");
        if (IS_ERR_OR_NULL(ff_ctx->pins_irq_as_int)) {
            ret = PTR_ERR(ff_ctx->pins_irq_as_int);
            FF_LOGE("ff_pins_irq_as_int not found, ret = %d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            return ret;
        }

        ff_ctx->pins_power_low = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_power_low");
        if (IS_ERR_OR_NULL(ff_ctx->pins_power_low)) {
            ret = PTR_ERR(ff_ctx->pins_power_low);
            FF_LOGE("ff_pins_power_low not found, ret = %d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            return ret;
        }

        ff_ctx->pins_power_high = pinctrl_lookup_state(ff_ctx->pinctrl, "ff_pins_power_high");
        if (IS_ERR_OR_NULL(ff_ctx->pins_power_high)) {
            ret = PTR_ERR(ff_ctx->pins_power_high);
            FF_LOGE("ff_pins_power_high not found, ret = %d", ret);
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            return ret;
        }

        /*set irq to INT mode as default*/
        ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_irq_as_int);
        if (ret < 0) {
            FF_LOGE("pinctrl_select_state:pins_irq_as_int failed");
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            ff_ctx->pins_power_high = NULL;
            return ret;
        }

        /*set reset to low as default*/
        ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_reset_low);
        if (ret < 0) {
            FF_LOGE("pinctrl_select_state:pins_reset_low failed");
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            ff_ctx->pins_power_high = NULL;
            return ret;
        }

        /*set power to low as default*/
        ret = pinctrl_select_state(ff_ctx->pinctrl, ff_ctx->pins_power_low);
        if (ret < 0) {
            FF_LOGE("pinctrl_select_state:pins_power_low failed");
            devm_pinctrl_put(ff_ctx->pinctrl);
            ff_ctx->pins_reset_low = NULL;
            ff_ctx->pins_reset_high = NULL;
            ff_ctx->pins_irq_as_int = NULL;
            ff_ctx->pins_power_low = NULL;
            ff_ctx->pins_power_high = NULL;
            return ret;
        }

        FF_LOGI("get irq-reset-power pinctrl success");
    } else {
        /* request irq gpio */
        if (gpio_is_valid(ff_ctx->irq_gpio)) {
            FF_LOGI("irq gpio = [%d]", ff_ctx->irq_gpio);
            ret = gpio_request(ff_ctx->irq_gpio, "ff_irq_gpio");
            if (ret < 0) {
                FF_LOGE("irq gpio request failed, ret = %d", ret);
#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
                if (ff_ctx->zlog_fp_client) {
                    zlog_client_record(ff_ctx->zlog_fp_client, "Failed to request focaltechfp irq gpio\n");
                    zlog_client_notify(ff_ctx->zlog_fp_client,  ZLOG_FP_REQUEST_INT_GPIO_ERROR_NO);
                }
#endif
                return ret;
            } else {
                FF_LOGI("irq gpio request success");
            }

            ret = gpio_direction_input(ff_ctx->irq_gpio);
            if (ret < 0) {
                FF_LOGE("gpio_direction_input for irq gpio failed, ret = %d", ret);
                gpio_free(ff_ctx->irq_gpio);
                return ret;
            } else {
                FF_LOGI("gpio_direction_input for irq gpio success");
            }
        }

        /* request reset gpio */
        if (gpio_is_valid(ff_ctx->reset_gpio)) {
            FF_LOGI("reset gpio = [%d]", ff_ctx->reset_gpio);
            ret = gpio_request(ff_ctx->reset_gpio, "ff_reset_gpio");
            if (ret) {
                FF_LOGE("reset gpio request failed, ret = %d", ret);
#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
                if (ff_ctx->zlog_fp_client) {
                    zlog_client_record(ff_ctx->zlog_fp_client, "Failed to request focaltechfp rst gpio\n");
                    zlog_client_notify(ff_ctx->zlog_fp_client,  ZLOG_FP_REQUEST_RST_GPIO_ERROR_NO);
                }
#endif
                if (gpio_is_valid(ff_ctx->irq_gpio)) {
                    gpio_free(ff_ctx->irq_gpio);
                }
                return ret;
            } else {
                FF_LOGI("reset gpio request success");
            }

            ret = gpio_direction_output(ff_ctx->reset_gpio, 0);
            if (ret < 0) {
                FF_LOGE("gpio_direction_output for reset gpio failed, ret = %d", ret);
                if (gpio_is_valid(ff_ctx->irq_gpio)) {
                    gpio_free(ff_ctx->irq_gpio);
                }
                gpio_free(ff_ctx->reset_gpio);
                return ret;
            } else {
                FF_LOGI("gpio_direction_output 0 for reset gpio success");
            }
        }

        FF_LOGI("get irq and reset gpio success");
    }

    FF_LOGI("[%s] exit", __func__);
    return 0;
}

static int ff_init_power(ff_context_t *ff_ctx)
{
    int ret = 0;

    FF_LOGI("[%s] enter", __func__);

    if (ff_ctx->b_power_always_on) {
        FF_LOGI("power is always on, no init power");
        return 0;
    }

    if (ff_ctx->b_use_regulator) {
        /*get regulator*/
        ff_ctx->ff_vdd = regulator_get(&ff_ctx->pdev->dev, "vdd");
        if (IS_ERR_OR_NULL(ff_ctx->ff_vdd)) {
            ret = PTR_ERR(ff_ctx->ff_vdd);
            FF_LOGE("regulator_get vdd failed, ret = %d", ret);
#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
            if (ff_ctx->zlog_fp_client) {
                zlog_client_record(ff_ctx->zlog_fp_client, "Failed to regulator get and set focaltechfp vdd\n");
                zlog_client_notify(ff_ctx->zlog_fp_client,  ZLOG_FP_REGULATOR_GET_SET_ERROR_NO);
            }
#endif
            return ret;
        } else {
            FF_LOGI("regulator_get vdd success");
        }

        if (regulator_count_voltages(ff_ctx->ff_vdd) > 0) {
            ret = regulator_set_voltage(ff_ctx->ff_vdd, FF_VTG_MIN_UV, FF_VTG_MAX_UV);
            if (ret) {
                FF_LOGE("regulator_set_voltage vdd failed, ret = %d", ret);
#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
                if (ff_ctx->zlog_fp_client) {
                    zlog_client_record(ff_ctx->zlog_fp_client, "Failed to regulator get and set focaltechfp vdd\n");
                    zlog_client_notify(ff_ctx->zlog_fp_client,  ZLOG_FP_REGULATOR_GET_SET_ERROR_NO);
                }
#endif
                regulator_put(ff_ctx->ff_vdd);
                return ret;
            } else {
                FF_LOGI("regulator_set_voltage vdd success");
            }
        }
        FF_LOGI("regulator_get vdd and regulator_set_voltage success");
    } else if (ff_ctx->b_use_pinctrl) {
        FF_LOGI("get power pinctrl success");
    } else {
        if (gpio_is_valid(ff_ctx->vdd_gpio)) {
            ret = gpio_request(ff_ctx->vdd_gpio, "ff_vdd_gpio");
            if (ret < 0) {
                FF_LOGE("gpio_request pwr gpio failed, ret = %d", ret);
#ifdef CONFIG_VENDOR_ZTE_LOG_EXCEPTION
                if (ff_ctx->zlog_fp_client) {
                    zlog_client_record(ff_ctx->zlog_fp_client, "Failed to request focaltechfp pwr gpio\n");
                    zlog_client_notify(ff_ctx->zlog_fp_client,  ZLOG_FP_REQUEST_PWR_GPIO_ERROR_NO);
                }
#endif
                return ret;
            } else {
                FF_LOGI("gpio_request pwr gpio success");
            }

            ret = gpio_direction_output(ff_ctx->vdd_gpio, 0);
            if (ret < 0) {
                FF_LOGE("gpio_direction_output pwr gpio failed, ret = %d", ret);
                gpio_free(ff_ctx->vdd_gpio);
                return ret;
            } else {
                FF_LOGI("gpio_direction_output pwr success");
            }

            FF_LOGI("gpio_request and gpio_direction_output pwr gpio(%d) success", ff_ctx->vdd_gpio);
        }
    }

    ff_ctx->b_power_on = false;
    FF_LOGI("[%s] exit", __func__);
    return 0;
}

static int ff_register_irq(ff_context_t *ff_ctx)
{
    int ret = 0;
    unsigned long irqflags = 0;
    struct device_node *np = ff_ctx->pdev->dev.of_node;

    FF_LOGI("[%s] enter", __func__);
    /*get irq num*/
    ff_ctx->irq_num = gpio_to_irq(ff_ctx->irq_gpio);
    FF_LOGI("irq_gpio = [%d], irq_num = [%d]", ff_ctx->irq_gpio, ff_ctx->irq_num);
    if (np && ff_ctx->b_use_pinctrl && (ff_ctx->irq_num <= 0)) {
        ff_ctx->irq_num = irq_of_parse_and_map(np, 0);
        FF_LOGI("irq_num = [%d]", ff_ctx->irq_num);
    }

    if (ff_ctx->irq_num > 0) {
        irqflags = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
        ret = request_irq(ff_ctx->irq_num, ff_irq_handler, irqflags, FF_DRV_NAME, ff_ctx);
        if (ret < 0) {
            FF_LOGE("request_irq failed, ret = %d", ret);
            return ret;
        } else {
            FF_LOGI("request_irq success, irqflags = [%d]", irqflags);
        }
        ff_ctx->b_irq_enabled = true;
        enable_irq_wake(ff_ctx->irq_num);
    } else {
        FF_LOGE("irq_num(%d) is invalid", ff_ctx->irq_num);
        ret = ff_ctx->irq_num;
    }

    FF_LOGI("[%s] exit", __func__);
    return ret;
}

/*create /dev/focaltech_fp*/
static int ff_register_device(ff_context_t *ff_ctx)
{
    int ret = 0;
    dev_t ff_devno;

    FF_LOGI("[%s] enter", __func__);
    ret = alloc_chrdev_region(&ff_devno, 0, 1, FF_DRV_NAME);
    if (ret < 0) {
        FF_LOGE("can't get ff major number, ret = %d", ret);
        return ret;
    }

    cdev_init(&ff_ctx->ff_cdev, &ff_fops);
    ff_ctx->ff_cdev.owner = THIS_MODULE;
    ff_ctx->ff_cdev.ops = &ff_fops;
    ret = cdev_add(&ff_ctx->ff_cdev, ff_devno, 1);
    if (ret < 0) {
        FF_LOGE("ff cdev add failed, ret = %d", ret);
        unregister_chrdev_region(ff_devno, 1);
        return ret;
    } else {
        FF_LOGI("ff cdev add success");
    }
    FF_LOGI("ff devno = [%x], [%x]", (int)ff_devno, (int)ff_ctx->ff_cdev.dev);

    ff_ctx->ff_class = class_create(THIS_MODULE, FF_DRV_NAME);
    if (IS_ERR(ff_ctx->ff_class)) {
        ret = PTR_ERR(ff_ctx->ff_class);
        FF_LOGE("create ff class failed, ret = %d", ret);
        cdev_del(&ff_ctx->ff_cdev);
        unregister_chrdev_region(ff_devno, 1);
        return ret;
    } else {
        FF_LOGI("create ff class success");
    }

    ff_ctx->fp_dev = device_create(ff_ctx->ff_class, NULL, ff_devno, ff_ctx, FF_DRV_NAME);
    if (IS_ERR(ff_ctx->fp_dev)) {
        ret = PTR_ERR(ff_ctx->fp_dev);
        FF_LOGE("ff device_create failed, ret = %d", ret);
        class_destroy(ff_ctx->ff_class);
        cdev_del(&ff_ctx->ff_cdev);
        unregister_chrdev_region(ff_devno, 1);
        return ret;
    } else {
        FF_LOGI("ff device_create success");
    }

    FF_LOGI("[%s] exit", __func__);
    return 0;
}

/*screen on/off callback*/
/*static int ff_suspend_resume(ff_context_t *ff_ctx, bool is_suspend)
{
    char *uevent_env[2];
    bool this_screen_on;

    FF_LOGD("%s enter", __func__);
    if (is_suspend) {
        uevent_env[0] = "FF_SCREEN_OFF";
        this_screen_on = false;
    } else {
        uevent_env[0] = "FF_SCREEN_ON";
        this_screen_on = true;
    }

    if (ff_ctx->b_screen_onoff_event && (ff_ctx->b_screen_on ^ this_screen_on)) {
        FF_LOGD("screen:%d->%d", ff_ctx->b_screen_on, this_screen_on);
        ff_ctx->b_screen_on = this_screen_on;
        if (ff_ctx->event_type == FF_EVENT_POLL) {
            ff_ctx->poll_event |= this_screen_on ? FF_POLLEVT_SCREEN_ON : FF_POLLEVT_SCREEN_OFF;
            wake_up_interruptible(&ff_ctx->wait_queue_head);
        } else if (ff_ctx->event_type == FF_EVENT_NETLINK) {
            uevent_env[1] = NULL;
            kobject_uevent_env(&ff_ctx->fp_dev->kobj, KOBJ_CHANGE, uevent_env);
        }  else FF_LOGE("unknowed event type:%d", ff_ctx->event_type);
    }

    FF_LOGD("%s exit", __func__);
    return 0;
}*/

#ifdef CONFIG_FINGERPRINT_FOCALTECH_ARCH_MTK
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *v)
{
    ff_context_t *ff_ctx = container_of(self, ff_context_t, fb_notif);
    FF_LOGD("[%s] enter", __func__);
    if (ff_ctx && v) {
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
        const unsigned long event_enum[2] = {MTK_DISP_EARLY_EVENT_BLANK, MTK_DISP_EVENT_BLANK};
        const int blank_enum[2] = {MTK_DISP_BLANK_POWERDOWN, MTK_DISP_BLANK_UNBLANK};
        int blank_value = *((int *)v);
#elif IS_ENABLED(CONFIG_FB)
        const unsigned long event_enum[2] = {FB_EARLY_EVENT_BLANK, FB_EVENT_BLANK};
        const int blank_enum[2] = {FB_BLANK_POWERDOWN, FB_BLANK_UNBLANK};
        int blank_value = *((int *)(((struct fb_event *)v)->data));
#endif
        FF_LOGI("notifier, event = [%lu], blank = [%d]", event, blank_value);
        if ((blank_enum[1] == blank_value) && (event_enum[1] == event)) {
            ff_suspend_resume(ff_ctx, false);
        } else if ((blank_enum[0] == blank_value) && (event_enum[0] == event)) {
            ff_suspend_resume(ff_ctx, true);
        } else {
            FF_LOGD("notifier, event = [%lu], blank = [%d], not care", event, blank_value);
        }
    } else {
        FF_LOGE("ts_data/v is null");
        return -EINVAL;
    }
    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int fts_notifier_callback_init(ff_context_t *ff_ctx)
{
    int ret = 0;
    FF_LOGD("[%s] enter", __func__);
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
    FF_LOGI("init notifier with mtk_disp_notifier_register");
    ff_ctx->fb_notif.notifier_call = fb_notifier_callback;
    ret = mtk_disp_notifier_register("fts_ts_notifier", &ff_ctx->fb_notif);
    if (ret < 0) {
        FF_LOGE("[DRM]mtk_disp_notifier_register failed, ret = %d", ret);
    }
#elif IS_ENABLED(CONFIG_FB)
    FF_LOGI("init notifier with fb_register_client");
    ff_ctx->fb_notif.notifier_call = fb_notifier_callback;
    ret = fb_register_client(&ff_ctx->fb_notif);
    if (ret) {
        FF_LOGE("[FB]fb_register_client failed, ret = %d", ret);
    }
#endif
    FF_LOGD("[%s] exit", __func__);
    return ret;
}

static int fts_notifier_callback_exit(ff_context_t *ff_ctx)
{
    FF_LOGD("[%s] enter", __func__);
#if IS_ENABLED(CONFIG_DRM_MEDIATEK_V2)
    if (mtk_disp_notifier_unregister(&ff_ctx->fb_notif))
        FF_LOGE("[DRM]Error occurred while unregistering disp_notifier");
#elif IS_ENABLED(CONFIG_FB)
    if (fb_unregister_client(&ff_ctx->fb_notif))
        FF_LOGE("[FB]Error occurred while unregistering fb_notifier");
#endif
    FF_LOGD("[%s] exit", __func__);
    return 0;
}
#endif

#ifdef CONFIG_FINGERPRINT_FOCALTECH_ARCH_MTK
#if IS_ENABLED(CONFIG_DRM)
#if IS_ENABLED(CONFIG_DRM_PANEL)
static struct drm_panel *active_panel;
static int drm_check_dt(ff_context_t *ff_ctx)
{
    int i = 0;
    int count = 0;
    struct device_node *node = NULL;
    struct drm_panel *panel = NULL;
    struct device_node *np = NULL;

    if (ff_ctx && ff_ctx->pdev->dev.of_node) {
        np = ff_ctx->pdev->dev.of_node;
        count = of_count_phandle_with_args(np, "panel", NULL);
        if (count <= 0) {
            FF_LOGE("find drm_panel count(%d) failed", count);
            return -ENODEV;
        }

        for (i = 0; i < count; i++) {
            node = of_parse_phandle(np, "panel", i);
            panel = of_drm_find_panel(node);
            of_node_put(node);
            if (!IS_ERR(panel)) {
                FF_LOGI("find drm_panel successfully");
                active_panel = panel;
                return 0;
            }
        }
    }

    FF_LOGE("no find drm_panel");
    return -ENODEV;
}
#endif //CONFIG_DRM_PANEL
#endif //CONFIG_DRM


static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *v)
{
    ff_context_t *ff_ctx = container_of(self, ff_context_t, fb_notif);
    FF_LOGD("[%s] enter", __func__);
    if (ff_ctx && v) {
#if IS_ENABLED(CONFIG_DRM)
        int blank_value = *(((struct msm_drm_notifier *)v)->data);
#if IS_ENABLED(CONFIG_DRM_PANEL)
        const unsigned long event_enum[2] = {DRM_PANEL_EARLY_EVENT_BLANK, DRM_PANEL_EVENT_BLANK};
        const int blank_enum[2] = {DRM_PANEL_BLANK_POWERDOWN, DRM_PANEL_BLANK_UNBLANK};
#else //CONFIG_DRM_PANEL
        const unsigned long event_enum[2] = {MSM_DRM_EARLY_EVENT_BLANK, MSM_DRM_EVENT_BLANK};
        const int blank_enum[2] = {MSM_DRM_BLANK_POWERDOWN, MSM_DRM_BLANK_UNBLANK};
#endif //CONFIG_DRM_PANEL

#elif IS_ENABLED(CONFIG_FB)
        const unsigned long event_enum[2] = {FB_EARLY_EVENT_BLANK, FB_EVENT_BLANK};
        const int blank_enum[2] = {FB_BLANK_POWERDOWN, FB_BLANK_UNBLANK};
        int blank_value = *((int *)(((struct fb_event *)v)->data));
#endif //CONFIG_DRM
        FF_LOGI("notifier, event = [%lu], blank = [%d]", event, blank_value);
        if ((blank_enum[1] == blank_value) && (event_enum[1] == event)) {
            ff_suspend_resume(ff_ctx, false);
        } else if ((blank_enum[0] == blank_value) && (event_enum[0] == event)) {
            ff_suspend_resume(ff_ctx, true);
        } else {
            FF_LOGD("notifier, event = [%lu], blank = [%d], not care", event, blank_value);
        }
    } else {
        FF_LOGE("ts_data/v is null");
        return -EINVAL;
    }
    FF_LOGD("[%s] exit", __func__);
    return 0;
}

static int fts_notifier_callback_init(ff_context_t *ff_ctx)
{
    int ret = 0;
    FF_LOGD("[%s] enter", __func__);
#if IS_ENABLED(CONFIG_DRM)
    ff_ctx->fb_notif.notifier_call = drm_notifier_callback;
#if IS_ENABLED(CONFIG_DRM_PANEL)
    FF_LOGI("init notifier with drm_panel_notifier_register");
    ret = drm_check_dt(ff_ctx);
    if (ret) {
        FF_LOGE("parse drm-panel failed");
    }
    if (active_panel) {
        ret = drm_panel_notifier_register(active_panel, &ff_ctx->fb_notif);
        if (ret) {
            FF_LOGE("[DRM]drm_panel_notifier_register failed, ret = %d", ret);
        }
    }
#else
    FF_LOGI("init notifier with msm_drm_register_client");
    ret = msm_drm_register_client(&ff_ctx->fb_notif);
    if (ret) {
        FF_LOGE("[DRM]msm_drm_register_client failed, ret = %d", ret);
    }
#endif //CONFIG_DRM_PANEL

#elif IS_ENABLED(CONFIG_FB)
    FF_LOGI("init notifier with fb_register_client");
    ff_ctx->fb_notif.notifier_call = fb_notifier_callback;
    ret = fb_register_client(&ff_ctx->fb_notif);
    if (ret) {
        FF_LOGE("[FB]fb_register_client failed, ret = %d", ret);
    }

#endif //CONFIG_DRM
    FF_LOGD("[%s] exit", __func__);
    return ret;
}

static int fts_notifier_callback_exit(ff_context_t *ff_ctx)
{
    FF_LOGD("[%s] enter", __func__);
#if IS_ENABLED(CONFIG_DRM)
#if IS_ENABLED(CONFIG_DRM_PANEL)
    if (active_panel) {
        drm_panel_notifier_unregister(active_panel, &ff_ctx->fb_notif);
    }
#else
    if (msm_drm_unregister_client(&ff_ctx->fb_notif)) {
        FF_LOGE("[DRM]Error occurred while unregistering fb_notifier");
    }
#endif

#elif IS_ENABLED(CONFIG_FB)
    if (fb_unregister_client(&ff_ctx->fb_notif)) {
        FF_LOGE("[FB]Error occurred while unregistering fb_notifier");
    }
#endif //CONFIG_DRM
    FF_LOGD("[%s] exit", __func__);
    return 0;
}
#endif

static int ff_probe(struct platform_device *pdev)
{
    int ret = 0;
    ff_context_t *ff_ctx = NULL;

    FF_LOGI("[%s] enter", __func__);
    ff_ctx = kzalloc(sizeof(ff_context_t), GFP_KERNEL);
    if (!ff_ctx) {
        FF_LOGE("kzalloc for ff_context_t failed");
        return -ENOMEM;
    }

    ff_ctx->chip_id = 0x0000;
    ff_ctx->probe_id_ret = 0;
    ff_ctx->spi_clk_hz = 0;
    ff_ctx->b_screen_onoff_event = true;
    ff_ctx->b_screen_on = true;
#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPICLK_SUPPORT
    ff_ctx->b_spiclk = true;
#else
    ff_ctx->b_spiclk = false;
#endif

    /*parse dts*/
    ff_parse_dt(&pdev->dev, ff_ctx);

    /*variables*/
    ff_ctx->pdev = pdev;
    ff_ctx->feature.ver = 0x0101;
    ff_ctx->feature.fbs_wakelock = true;
    ff_ctx->feature.fbs_power_const = (ff_ctx->b_power_always_on) ? true : false;
    ff_ctx->feature.fbs_screen_onoff = (ff_ctx->b_screen_onoff_event) ? true : false;
    ff_ctx->feature.fbs_pollevt = true;
    ff_ctx->feature.fbs_reset_hl = true;
    ff_ctx->feature.fbs_spiclk = (ff_ctx->b_spiclk) ? true : false;
    ff_ctx->feature.fbs_spiclk_hz = false;
    ff_ctx->poll_event = FF_POLLEVT_NONE;
    ff_ctx->event_type = FF_EVENT_NETLINK;

    /* Init the interrupt workqueue. */
    INIT_WORK(&ff_ctx->event_work, ff_work_device_event);
    init_waitqueue_head(&ff_ctx->wait_queue_head);

    /* Init the wake lock. */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0))
    wakeup_source_init(&ff_ctx->wake_lock, "ff_wake_lock");
    wakeup_source_init(&ff_ctx->wake_lock_ctl, "ff_wake_lock_ctl");
    ff_ctx->p_ws = &ff_ctx->wake_lock;
    ff_ctx->p_ws_ctl = &ff_ctx->wake_lock_ctl;
#else
    ff_ctx->p_ws = wakeup_source_register(&pdev->dev, "ff_wake_lock");
    ff_ctx->p_ws_ctl = wakeup_source_register(&pdev->dev, "ff_wake_lock_ctl");
#endif
    if (!ff_ctx->p_ws || !ff_ctx->p_ws_ctl) {
        FF_LOGE("init ff wakeup_source failed");
        goto err_init_ws;
    } else {
        FF_LOGI("init ff wakeup_source success");
    }

    ret = ff_register_device(ff_ctx);
    if (ret < 0) {
        FF_LOGE("register device(dev/focaltech_fp) failed, ret = %d", ret);
        goto err_register_device;
    } else {
        FF_LOGI("register device(dev/focaltech_fp) success");
    }

    ret = sysfs_create_group(&pdev->dev.kobj, &ff_attrs_group);
    if (ret < 0) {
        FF_LOGE("sysfs_create_group failed, ret = %d", ret);
    } else {
        FF_LOGI("sysfs_create_group success");
    }

    platform_set_drvdata(pdev, ff_ctx);
    ff_ctx->b_ff_probe = true;
    g_ff_ctx = ff_ctx;

#if IS_ENABLED(CONFIG_TRUSTKERNEL_TEE_SUPPORT)
    /* trustkernel maybe can't register SPI, so read id here. */
    if (ff_ctx->b_read_chipid) {
        ret = ff_probe_id();
        if (ret < 0) {
            FF_LOGE("ff_probe_id failed, ret = %d", ret);
        }
    }
#endif

    FF_LOGI("[%s] normal exit", __func__);
    return 0;

err_init_ws:
err_register_device:
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0))
    wakeup_source_trash(&ff_ctx->wake_lock);
    wakeup_source_trash(&ff_ctx->wake_lock_ctl);
    ff_ctx->p_ws = NULL;
    ff_ctx->p_ws_ctl = NULL;
#else
    wakeup_source_unregister(ff_ctx->p_ws);
    wakeup_source_unregister(ff_ctx->p_ws_ctl);
#endif
    cancel_work_sync(&ff_ctx->event_work);

    kfree(ff_ctx);
    ff_ctx = NULL;

    FF_LOGI("[%s] abnormal exit", __func__);
    return ret;
}

static void ff_unprobe(ff_context_t *ff_ctx)
{
    FF_LOGI("[%s] enter", __func__);
    if (ff_ctx && ff_ctx->b_ff_probe) {
        ff_ctx->b_ff_probe = false;
        if (ff_ctx->b_driver_inited) {
            ff_ctl_free_driver(ff_ctx);
        }

        sysfs_remove_group(&ff_ctx->pdev->dev.kobj, &ff_attrs_group);

        /* De-init the wake lock. */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0))
        wakeup_source_trash(&ff_ctx->wake_lock);
        wakeup_source_trash(&ff_ctx->wake_lock_ctl);
        ff_ctx->p_ws = NULL;
        ff_ctx->p_ws_ctl = NULL;
#else
        wakeup_source_unregister(ff_ctx->p_ws);
        wakeup_source_unregister(ff_ctx->p_ws_ctl);
#endif
        cancel_work_sync(&ff_ctx->event_work);
    }
    FF_LOGI("[%s] exit", __func__);
}

static int ff_remove(struct platform_device *pdev)
{
    ff_context_t *ff_ctx = platform_get_drvdata(pdev);

    FF_LOGI("[%s] enter", __func__);
    if (ff_ctx) {
        ff_unprobe(ff_ctx);

        device_destroy(ff_ctx->ff_class, ff_ctx->ff_cdev.dev);
        class_destroy(ff_ctx->ff_class);
        cdev_del(&ff_ctx->ff_cdev);
        unregister_chrdev_region(ff_ctx->ff_cdev.dev, 1);

        platform_set_drvdata(pdev, NULL);
        kfree(ff_ctx);
        ff_ctx = NULL;
    }
    FF_LOGI("[%s] exit", __func__);
    return 0;
}

static const struct of_device_id ff_dt_match[] = {
    {.compatible = "focaltech,fp", },
    {},
};
MODULE_DEVICE_TABLE(of, ff_dt_match);

static struct platform_driver ff_driver = {
    .driver = {
        .name = FF_DRV_NAME,
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(ff_dt_match),
    },
    .probe = ff_probe,
    .remove = ff_remove,
};


int focaltech_fp_driver_v2_init(void)
{
    int ret = 0;
    FF_LOGI("[%s] enter, driver_time:2023-10-31", __func__);
    ret = platform_driver_register(&ff_driver);
    if (ret < 0) {
        FF_LOGE("ff platform driver init failed");
        return ret;
    } else {
        FF_LOGI("ff platform driver init success");
    }

#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPI_SUPPORT
    ret = ff_spi_init();
    if (ret < 0) {
        FF_LOGE("ff spi driver init failed");
        platform_driver_unregister(&ff_driver);
        return ret;
    } else {
        FF_LOGI("ff spi driver init success");
    }

#if IS_ENABLED(CONFIG_MICROTRUST_TEE_SUPPORT)
    if (g_ff_ctx && g_ff_ctx->b_read_chipid && (g_ff_ctx->probe_id_ret < 0)) {
        FF_LOGE("ff spi driver probe id failed");
        platform_driver_unregister(&ff_driver);
        ff_spi_exit();
        ret = g_ff_ctx->probe_id_ret;
    }
#endif
#endif
    FF_LOGI("[%s] exit", __func__);
    return ret;
}

void focaltech_fp_driver_v2_exit(void)
{
    FF_LOGI("[%s] enter", __func__);
    platform_driver_unregister(&ff_driver);

#ifdef CONFIG_FINGERPRINT_FOCALTECH_SPI_SUPPORT
    ff_spi_exit();
#endif
    FF_LOGI("[%s] exit", __func__);
}

/*module_init(ff_driver_init);
module_exit(ff_driver_exit);*/

MODULE_AUTHOR("FocalTech Fingerprint Department");
MODULE_DESCRIPTION("FocalTech Fingerprint Driver");
MODULE_LICENSE("GPL v2");
