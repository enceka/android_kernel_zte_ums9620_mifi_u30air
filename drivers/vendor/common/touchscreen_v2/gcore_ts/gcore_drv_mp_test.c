/*
 * GalaxyCore touchscreen driver
 *
 * Copyright (C) 2021 GalaxyCore Incorporated
 *
 * Copyright (C) 2021 Neo Chen <neo_chen@gcoreinc.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include "gcore_drv_common.h"
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/gpio.h>

#define MP_TEST_PROC_FILENAME "ts03_selftest"

#define MP_TEST_OF_PREFIX                  "ts03,"
#define MP_TEST_OF_ITEM_TEST_INT_PIN       MP_TEST_OF_PREFIX"test-int-pin"
#define MP_TEST_OF_ITEM_TEST_CHIP_ID       MP_TEST_OF_PREFIX"test-chip-id"
#define MP_TEST_OF_TEST_CHIP_ID            MP_TEST_OF_PREFIX"chip-id"
#define MP_TEST_OF_ITEM_TEST_OPEN          MP_TEST_OF_PREFIX"test-open"
#define MP_TEST_OF_TEST_OPEN_CB            MP_TEST_OF_PREFIX"open-cb"
#define MP_TEST_OF_TEST_OPEN_MIN           MP_TEST_OF_PREFIX"open-min"
#define MP_TEST_OF_ITEM_TEST_SHORT         MP_TEST_OF_PREFIX"test-short"
#define MP_TEST_OF_TEST_SHORT_CB           MP_TEST_OF_PREFIX"short-cb"
#define MP_TEST_OF_TEST_SHORT_MIN          MP_TEST_OF_PREFIX"short-min"

#define TEST_BEYOND_MAX_LIMIT		0x0001
#define TEST_BEYOND_MIN_LIMIT		0x0002
#define TEST_GT_OPEN				0x0200
#define TEST_GT_SHORT				0x0400
#define MODULE_TYPE_ERR			0x00080000
#define EMPTY_NUM  4
int empty_place[EMPTY_NUM] = { 8, 9, -1, -1 };
extern int gcore_tptest_result;
extern char gcore_mp_test_ini_path[];
DECLARE_COMPLETION(mp_test_complete);

struct gcore_mp_data {
	struct proc_dir_entry *mp_proc_entry;

	int test_int_pin;
	int int_pin_test_result;

	int test_chip_id;
	int chip_id_test_result;
	u8 chip_id[2];

	int test_open;
	int open_test_result;
	int open_cb;
	int open_min;
	u8 *open_data;
	u16 *open_node_val;

	int test_short;
	int short_test_result;
	int short_cb;
	int short_min;
	u8 *short_data;
	u16 *short_node_val;

	int test_rawdata;
	int rawdata_test_result;
	u8 *raw_data;
	u16 *rawdata_node_val;
	u16 *rawdata_dec_h;
	u16 *rawdata_dec_l;
	u16 *rawdata_avg_h;
	u16 *rawdata_avg_l;
	int rawdata_max;
	int rawdata_range_max;
	int rawdata_range_min;

	int test_noise;
	int noise_test_result;
	int peak_to_peak;
	u8 *noise_data;
	u16 *noise_node_val;

	struct gcore_dev *gdev;

};

struct gcore_mp_data *g_mp_data = NULL;
struct task_struct *mp_thread = NULL;

static int gcore_mp_test_fn_init(struct gcore_dev *gdev);
static void gcore_mp_test_fn_remove(struct gcore_dev *gdev);

static void dump_mp_data_to_seq_file(struct seq_file *m, const u16 *data, int rows, int cols);

struct gcore_exp_fn mp_test_fn = {
	.fn_type = GCORE_MP_TEST,
	.wait_int = false,
	.event_flag = false,
	.init = gcore_mp_test_fn_init,
	.remove = gcore_mp_test_fn_remove,
};

static void *gcore_seq_start(struct seq_file *m, loff_t *pos)
{
	return *pos < 1 ? (void *)1 : NULL;
}

static void *gcore_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void gcore_seq_stop(struct seq_file *m, void *v)
{

}

static int gcore_seq_show(struct seq_file *m, void *v)
{
	struct gcore_mp_data *mp_data = (struct gcore_mp_data *)m->private;
	int rows = RAWDATA_ROW;
	int cols = RAWDATA_COLUMN;

	GTP_DEBUG("gcore seq show");

	if (mp_data == NULL) {
		GTP_ERROR("seq_file private data = NULL");
		return -EPERM;
	}

	if (mp_data->test_int_pin) {
		seq_printf(m, "Int-Pin Test %s!\n\n", mp_data->int_pin_test_result == 0 ? "PASS" : "FAIL");
	}

	if (mp_data->test_chip_id) {
		seq_printf(m, "Chip-id Test %s!\n\n", mp_data->chip_id_test_result == 0 ? "PASS" : "FAIL");
	}

	if (mp_data->test_open) {
		if (mp_data->open_test_result == 0) {
			seq_puts(m, "Open Test PASS!\n\n");
		} else {
			seq_puts(m, "Open Test FAIL!\n");
			dump_mp_data_to_seq_file(m, mp_data->open_node_val, rows, cols);
			seq_putc(m, '\n');
		}

	}

	if (mp_data->test_short) {
		if (mp_data->short_test_result == 0) {
			seq_puts(m, "Short Test PASS!\n\n");
		} else {
			seq_puts(m, "Short Test FAIL!\n");
			dump_mp_data_to_seq_file(m, mp_data->short_node_val, rows, cols);
			seq_putc(m, '\n');
		}

	}

	return 0;
}

const struct seq_operations gcore_mptest_seq_ops = {
	.start = gcore_seq_start,
	.next = gcore_seq_next,
	.stop = gcore_seq_stop,
	.show = gcore_seq_show,
};

int gcore_parse_mp_test_dt(struct gcore_mp_data *mp_data)
{
	struct device_node *np = mp_data->gdev->bus_device->dev.of_node;

	GTP_DEBUG("gcore parse mp test dt");

	mp_data->test_int_pin = of_property_read_bool(np, MP_TEST_OF_ITEM_TEST_INT_PIN);

	mp_data->test_chip_id = of_property_read_bool(np, MP_TEST_OF_ITEM_TEST_CHIP_ID);
	if (mp_data->test_chip_id) {
		of_property_read_u8_array(np, MP_TEST_OF_TEST_CHIP_ID, mp_data->chip_id,
					  sizeof(mp_data->chip_id));
	}

	mp_data->test_open = of_property_read_bool(np, MP_TEST_OF_ITEM_TEST_OPEN);
	if (mp_data->test_open) {
		of_property_read_u32(np, MP_TEST_OF_TEST_OPEN_CB, &mp_data->open_cb);
		of_property_read_u32(np, MP_TEST_OF_TEST_OPEN_MIN, &mp_data->open_min);
	}

	mp_data->test_short = of_property_read_bool(np, MP_TEST_OF_ITEM_TEST_SHORT);
	if (mp_data->test_short) {
		of_property_read_u32(np, MP_TEST_OF_TEST_SHORT_CB, &mp_data->short_cb);
		of_property_read_u32(np, MP_TEST_OF_TEST_SHORT_MIN, &mp_data->short_min);
	}

	return 0;
}

int gcore_parse_mp_test_ini(struct gcore_mp_data *mp_data)
{
	u8 *tmp = NULL;
	u8 *buff = NULL;
	int ret = 0;
	int chip_type = 0;
	const struct firmware *fw = NULL;
	struct device *dev = &mp_data->gdev->bus_device->dev;

	ret = request_firmware(&fw, gcore_mp_test_ini_path, dev);
	if (ret == 0) {
		GTP_INFO("firmware request(%s) success", gcore_mp_test_ini_path);
		buff = kzalloc(fw->size, GFP_KERNEL);
		GTP_DEBUG("firmware size is:%d", fw->size);
		if (!buff) {
			GTP_ERROR("buffer kzalloc fail");
			return -EPERM;
		} 
		memcpy(buff, fw->data, fw->size);
		GTP_DEBUG("buff is: %s", buff);
		release_firmware(fw);
		/* test int pin */
		tmp = strstr(buff, "test-int-pin=");
		ret = sscanf(tmp, "test-int-pin=%d", &mp_data->test_int_pin);
		if (ret < 0)
			GTP_ERROR("%s read test-int-pin error.\n", __func__);
		GTP_DEBUG("test-int-pin:%s", mp_data->test_int_pin ? "y" : "n");

		/* test chip id */
		tmp = strstr(buff, "test-chip-id=");
		ret = sscanf(tmp, "test-chip-id=%d", &mp_data->test_chip_id);
		if (ret < 0)
			GTP_ERROR("%s read test chip id error.\n", __func__);
		GTP_DEBUG("test-chip-id:%s", mp_data->test_chip_id ? "y" : "n");

		tmp = strstr(buff, "chip-id=[");
		ret = sscanf(tmp, "chip-id=[%x]", &chip_type);
		if (ret < 0)
			GTP_ERROR("%s read chip id error.\n", __func__);
		GTP_DEBUG("chip-id:%x", chip_type);
		mp_data->chip_id[0] = (u8) (chip_type >> 8);
		mp_data->chip_id[1] = (u8) chip_type;

		/* test open */
		tmp = strstr(buff, "test-open=");
		ret = sscanf(tmp, "test-open=%d", &mp_data->test_open);
		if (ret < 0)
			GTP_ERROR("%s read test open error.\n", __func__);
		GTP_DEBUG("test-open:%s", mp_data->test_open ? "y" : "n");

		tmp = strstr(buff, "open_cb=");
		ret = sscanf(tmp, "open_cb=%d", &mp_data->open_cb);
		if (ret < 0)
			GTP_ERROR("%s read open cb error.\n", __func__);
		GTP_DEBUG("read open cb:%d", mp_data->open_cb);

		tmp = strstr(buff, "open_min=");
		ret = sscanf(tmp, "open_min=%d", &mp_data->open_min);
		if (ret < 0)
			GTP_ERROR("%s read open min error.\n", __func__);
		GTP_DEBUG("read open min:%d", mp_data->open_min);

		/* test short */
		tmp = strstr(buff, "test-short=");
		ret = sscanf(tmp, "test-short=%d", &mp_data->test_short);
		if (ret < 0)
			GTP_ERROR("%s read test short error.\n", __func__);
		GTP_DEBUG("test-short:%s", mp_data->test_short ? "y" : "n");

		tmp = strstr(buff, "short_cb=");
		ret = sscanf(tmp, "short_cb=%d", &mp_data->short_cb);
		if (ret < 0)
			GTP_ERROR("%s read short cb error.\n", __func__);
		GTP_DEBUG("read short cb:%d", mp_data->short_cb);

		tmp = strstr(buff, "short_min=");
		ret = sscanf(tmp, "short_min=%d", &mp_data->short_min);
		if (ret < 0)
			GTP_ERROR("%s read short min error.\n", __func__);
		GTP_DEBUG("read short min:%d", mp_data->short_min);

		/* test rawdata */
		tmp = strstr(buff, "test-rawdata=");
		ret = sscanf(tmp, "test-rawdata=%d", &mp_data->test_rawdata);
		if (ret < 0)
			GTP_ERROR("%s test-rawdata error.\n", __func__);
		GTP_DEBUG("test-rawdata:%s", mp_data->test_rawdata ? "y" : "n");

		tmp = strstr(buff, "rawdata_max=");
		ret = sscanf(tmp, "rawdata_max=%d", &mp_data->rawdata_max);
		if (ret < 0)
			GTP_ERROR("%s rawdata_max error.\n", __func__);
		GTP_DEBUG("read rawdata max:%d", mp_data->rawdata_max);

		tmp = strstr(buff, "rawdata_range=");
		ret = sscanf(tmp, "rawdata_range=[%d %d]", &mp_data->rawdata_range_min, &mp_data->rawdata_range_max);
		if (ret < 0)
			GTP_ERROR("%s rawdata range error.\n", __func__);
		GTP_DEBUG("read range min:%d max:%d", mp_data->rawdata_range_min, mp_data->rawdata_range_max);

		/* test noise */
		tmp = strstr(buff, "test-noise=");
		ret = sscanf(tmp, "test-noise=%d", &mp_data->test_noise);
		if (ret < 0)
			GTP_ERROR("%s test-noise error.\n", __func__);
		GTP_DEBUG("test-noise:%s", mp_data->test_noise ? "y" : "n");

		tmp = strstr(buff, "peak_to_peak=");
		ret = sscanf(tmp, "peak_to_peak=%d", &mp_data->peak_to_peak);
		if (ret < 0)
			GTP_ERROR("%s peak to peak error.\n", __func__);
		GTP_DEBUG("read peak to peak:%d", mp_data->peak_to_peak);

		kfree(buff);
	} else {
		GTP_ERROR("firmware request(%s) fail,ret=%d", gcore_mp_test_ini_path, ret);
	}

	return 0;
}

int gcore_mp_test_int_pin(struct gcore_mp_data *mp_data)
{
	u8 read_buf[4] = { 0 };

	GTP_DEBUG("gcore mp test int pin");

	mutex_lock(&mp_data->gdev->transfer_lock);
	mp_data->gdev->irq_disable(mp_data->gdev);

	gcore_enter_idm_mode();

	usleep_range(1000, 1100);

	gcore_idm_read_reg(0xC0000098, read_buf, sizeof(read_buf));
	GTP_DEBUG("reg addr: 0xC0000098 read_buf: %x %x %x %x",
		  read_buf[0], read_buf[1], read_buf[2], read_buf[3]);
	usleep_range(1000, 1100);

	read_buf[0] &= 0x7F;
	read_buf[2] &= 0x7F;

	gcore_idm_write_reg(0xC0000098, read_buf, sizeof(read_buf));

	usleep_range(1000, 1100);

	if (gpio_get_value(mp_data->gdev->irq_gpio) == 1) {
		GTP_ERROR("INT pin state != LOW test fail!");
		goto fail1;
	}

	read_buf[2] |= BIT7;

	gcore_idm_write_reg(0xC0000098, read_buf, sizeof(read_buf));

	usleep_range(1000, 1100);

	if (gpio_get_value(mp_data->gdev->irq_gpio) == 0) {
		GTP_ERROR("INT pin state != HIGH test fail!");
		goto fail1;
	}

	gcore_exit_idm_mode();

	mp_data->gdev->irq_enable(mp_data->gdev);
	mutex_unlock(&mp_data->gdev->transfer_lock);

	return 0;

fail1:
	mp_data->gdev->irq_enable(mp_data->gdev);
	mutex_unlock(&mp_data->gdev->transfer_lock);
	return -EPERM;
}

int gcore_mp_test_chip_id(struct gcore_mp_data *mp_data)
{
	u8 buf[3] = { 0 };

	GTP_DEBUG("gcore mp test chip id");

	gcore_idm_read_id(buf, sizeof(buf));

	if ((buf[1] == mp_data->chip_id[0]) && (buf[2] == mp_data->chip_id[1])) {
		GTP_DEBUG("gcore mp test chip id match success!");
		return 0;
	}
	GTP_DEBUG("gcore mp test chip id match failed!");
	return -EPERM;
}

int gcore_mp_test_item_open(struct gcore_mp_data *mp_data)
{
	u8 cb_high = (u8) (mp_data->open_cb >> 8);
	u8 cb_low = (u8) mp_data->open_cb;
	u8 cmd[] = { 0x40, 0xAA, 0x00, 0x32, 0x73, 0x71, cb_high, cb_low, 0x01 };
	struct gcore_dev *gdev = mp_data->gdev;
	int ret = 0;
	int i = 0;
	int j = 0;
	u8 *opendata = NULL;
	u16 node_data = 0;
	bool jump = 0;

	GTP_DEBUG("gcore mp test item open");

	gdev->fw_event = FW_READ_OPEN;
	gdev->firmware = mp_data->open_data;
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
	gdev->fw_xfer = 2048;
#else
	gdev->fw_xfer = RAW_DATA_SIZE;
#endif

	mutex_lock(&gdev->transfer_lock);

	mp_test_fn.wait_int = true;

	ret = gcore_bus_write(cmd, sizeof(cmd));
	if (ret) {
		GTP_ERROR("write mp test open cmd fail!");
		mutex_unlock(&gdev->transfer_lock);
		return -EPERM;
	}

	mutex_unlock(&gdev->transfer_lock);

	if (!wait_for_completion_interruptible_timeout(&mp_test_complete, 1 * HZ)) {
		GTP_ERROR("mp test item open timeout.");
		return -EPERM;
	}

	opendata = mp_data->open_data;

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {
		node_data = (opendata[1] << 8) | opendata[0];

		mp_data->open_node_val[i] = node_data;

		opendata += 2;
	}

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {
		node_data = mp_data->open_node_val[i];
		/* GTP_DEBUG("i:%d open node_data:%d", i, node_data); */
		jump = 0;

		for (j = 0; j < EMPTY_NUM; j++) {
			if (i == empty_place[j]) {
				GTP_DEBUG("empty node %d need not judge.", i);
				jump = 1;
			}
		}

		if (!jump) {
			if (node_data < mp_data->open_min) {
				GTP_ERROR("i: %d node_data %d < open_min test fail!", i, node_data);

				return -EPERM;
			}
		}
	}

	return 0;
}

int gcore_mp_test_item_open_reply(u8 *buf, int len)
{
	int ret = 0;

	GTP_INFO("gcore_mp_test_item_open_reply");
	if (buf == NULL) {
		GTP_ERROR("receive buffer is null!");
		return -EPERM;
	}

	ret = gcore_bus_read(buf, len);
	if (ret) {
		GTP_ERROR("read mp test open data error.");
		return -EPERM;
	}

	mp_test_fn.wait_int = false;
	complete(&mp_test_complete);

	return 0;
}

int gcore_mp_test_item_short(struct gcore_mp_data *mp_data)
{
	u8 cb_high = (u8) (mp_data->short_cb >> 8);
	u8 cb_low = (u8) mp_data->short_cb;
	u8 cmd[] = { 0x40, 0xAA, 0x00, 0x32, 0x73, 0x71, cb_high, cb_low, 0x02 };
	struct gcore_dev *gdev = mp_data->gdev;
	int ret = 0;
	int i = 0;
	int j = 0;
	u8 *shortdata = NULL;
	u16 node_data = 0;
	bool jump = 0;

	GTP_INFO("gcore mp test item short");

	gdev->fw_event = FW_READ_SHORT;
	gdev->firmware = mp_data->short_data;
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
	gdev->fw_xfer = 2048;
#else
	gdev->fw_xfer = RAW_DATA_SIZE;
#endif

	mutex_lock(&gdev->transfer_lock);

	mp_test_fn.wait_int = true;

	ret = gcore_bus_write(cmd, sizeof(cmd));
	if (ret) {
		GTP_ERROR("write mp test open cmd fail!");
		mutex_unlock(&gdev->transfer_lock);
		return -EPERM;
	}

	mutex_unlock(&gdev->transfer_lock);

	if (!wait_for_completion_interruptible_timeout(&mp_test_complete, 1 * HZ)) {
		GTP_ERROR("mp test item short timeout.");
		return -EPERM;
	}

	shortdata = mp_data->short_data;

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {
		node_data = (shortdata[1] << 8) | shortdata[0];

		mp_data->short_node_val[i] = node_data;

		shortdata += 2;
	}

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {
		node_data = mp_data->short_node_val[i];
		/* GTP_DEBUG("i:%d short node_data:%d", i, node_data); */
		jump = 0;

		for (j = 0; j < EMPTY_NUM; j++) {
			if (i == empty_place[j]) {
				GTP_DEBUG("empty node %d need not judge.", i);
				jump = 1;
			}
		}

		if (!jump) {
			if (node_data < mp_data->short_min) {
				GTP_ERROR("i: %d node_data %d < short_min test fail!", i, node_data);

				return -EPERM;
			}
		}
	}

	return 0;
}

int gcore_mp_test_item_rawdata(struct gcore_mp_data *mp_data)
{
	u8 cmd[] = { 0x40, 0xAA, 0x00, 0x32, 0x73, 0x71, 0x00, 0x00, 0x03 };
	struct gcore_dev *gdev = mp_data->gdev;
	u8 *rawdata = NULL;
	int ret = 0;
	int i = 0;
	int j = 0;
	u16 node_data = 0;
#ifdef MP_RAWDATA_DEC_TEST
	u16 *rndata = mp_data->rawdata_node_val;
	u16 *dec_value = NULL;
	int rows = RAWDATA_ROW;
	int cols = RAWDATA_COLUMN;
#endif
	bool jump = 0;


	GTP_DEBUG("gcore mp test item rawdata");

	gdev->fw_event = FW_READ_RAWDATA;
	gdev->firmware = mp_data->raw_data;
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
	gdev->fw_xfer = 2048;
#else
	gdev->fw_xfer = RAW_DATA_SIZE;
#endif

	mutex_lock(&gdev->transfer_lock);

	mp_test_fn.wait_int = true;

	ret = gcore_bus_write(cmd, sizeof(cmd));
	if (ret) {
		GTP_ERROR("write mp test open cmd fail!");
		mutex_unlock(&gdev->transfer_lock);
		return -EPERM;
	}

	mutex_unlock(&gdev->transfer_lock);

	if (!wait_for_completion_interruptible_timeout(&mp_test_complete, 1 * HZ)) {
		GTP_ERROR("mp test item short timeout.");
		return -EPERM;
	}

	rawdata = mp_data->raw_data;

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {
		node_data = (rawdata[1] << 8) | rawdata[0];

		mp_data->rawdata_node_val[i] = node_data;

		rawdata += 2;
	}

#ifdef MP_RAWDATA_DEC_TEST
	dec_value = mp_data->rawdata_dec_h;
	rndata = mp_data->rawdata_node_val;
	for (j = 0; j < rows; j++) {
		for (i = 0; i < cols; i++) {
			if (i == cols - 1) {
				dec_value[i] = abs(rndata[i - 1] - rndata[i]) * 100 / rndata[i];
			} else {
				dec_value[i] = abs(rndata[i + 1] - rndata[i]) * 100 / rndata[i];
			}

			/* GTP_DEBUG("rawdata cal i:%d val:%d", i, dec_value); */
		}

		rndata += cols;
		dec_value += cols;
	}

	dec_value = mp_data->rawdata_dec_l;
	rndata = mp_data->rawdata_node_val;
	for (j = 0; j < cols; j++) {
		for (i = 0; i < rows; i++) {
			if (i == rows - 1) {
				dec_value[i*cols+j] = abs(rndata[i*cols+j-cols] - rndata[i*cols+j]) * 100 / rndata[i*cols+j];
			} else {
				dec_value[i*cols+j] = abs(rndata[i*cols+j+cols] - rndata[i*cols+j]) * 100 / rndata[i*cols+j];
			}

			/* GTP_DEBUG("rawdata cal i:%d val:%d", i, dec_value); */
		}

	}

	dec_value = mp_data->rawdata_avg_h;
	rndata = mp_data->rawdata_node_val;
	for (j = 0; j < rows; j++) {
		int max = 0;
		int min = 0;
		int avg = 0;
		int sum = 0;

		for (i = 0; i < cols; i++) {
			if (rndata[i] > rndata[max]) {
				max = i;
			}

			if (rndata[i] < rndata[min]) {
				min = i;
			}
		}

		/* GTP_DEBUG("avgh max:%d min:%d", max, min); */

		if (max == min) {
			avg = rndata[max];
		} else {
			for (i = 0; i < cols; i++) {
				if (i != max && i != min) {
					sum += rndata[i];
				}
			}
			avg = sum/(cols-2);
		}

	/* GTP_DEBUG("avgh sum:%d avg:%d", sum, avg); */

		for (i = 0; i < cols; i++) {
			dec_value[i] = abs(avg - rndata[i]) * 100 / rndata[i];
		}

	/*GTP_DEBUG("rawdata cal i:%d val:%d", i, dec_value); */


		rndata += cols;
		dec_value += cols;
	}


	dec_value = mp_data->rawdata_avg_l;
	rndata = mp_data->rawdata_node_val;
	for (j = 0; j < cols; j++) {
		int max = j;
		int min = j;
		int avg = 0;
		int sum = 0;

		for (i = 0; i < rows; i++) {
			if (rndata[i*cols+j] > rndata[max]) {
				max = i*cols+j;
			}

			if (rndata[i*cols+j] < rndata[min]) {
				min = i*cols+j;
			}
		}

		/* GTP_DEBUG("avgl max:%d min:%d", max/18, min/18); */

		if (max == min) {
			avg = rndata[max];
		} else {
			for (i = 0; i < rows; i++) {
				if ((i*cols+j) != max && (i*cols+j) != min) {
					sum += rndata[i*cols+j];
				}
			}
			avg = sum/(rows-2);
		}

		/*	GTP_DEBUG("avgl sum:%d avg:%d", sum, avg); */

		for (i = 0; i < rows; i++) {
			dec_value[i*cols+j] = abs(avg - rndata[i*cols+j]) * 100 / rndata[i*cols+j];
		}

		/* GTP_DEBUG("rawdata cal i:%d val:%d", i, dec_value); */
	}
#endif

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {

		jump = 0;

		for (j = 0; j < EMPTY_NUM; j++) {
			if (i == empty_place[j]) {
				GTP_DEBUG("empty node %d need not judge.", i);
				jump = 1;
			}
		}

		if (!jump) {
			node_data = mp_data->rawdata_node_val[i];
			if ((node_data > mp_data->rawdata_range_max) ||
					(node_data < mp_data->rawdata_range_min)) {
				GTP_ERROR("rawdata range %d data %d not in range test fail!", i, node_data);

				return -EPERM;
			}

#ifdef MP_RAWDATA_DEC_TEST
			node_data = mp_data->rawdata_dec_h[i];
			if (node_data > mp_data->rawdata_max) {
				GTP_ERROR("dech %d n_data %d > rawdata_max test fail!", i, node_data);

				return -EPERM;
			}

			node_data = mp_data->rawdata_dec_l[i];
			if (node_data > mp_data->rawdata_max) {
				GTP_ERROR("decl %d n_data %d > rawdata_max test fail!", i, node_data);

				return -EPERM;
			}

			node_data = mp_data->rawdata_avg_h[i];
			if (node_data > mp_data->rawdata_max) {
				GTP_ERROR("avgh %d n_data %d > rawdata_max test fail!", i, node_data);

				return -EPERM;
			}

			node_data = mp_data->rawdata_avg_l[i];
			if (node_data > mp_data->rawdata_max) {
				GTP_ERROR("avgl %d n_data %d > rawdata_max test fail!", i, node_data);

				return -EPERM;
			}
#endif
		}
	}

	return 0;
}

int gcore_mp_test_item_noise(struct gcore_mp_data *mp_data)
{
	u8 cmd[] = { 0x40, 0xAA, 0x00, 0x64, 0x73, 0x71, 0x00, 0x00, 0x04 };
	struct gcore_dev *gdev = mp_data->gdev;
	u8 *noisedata = NULL;
	int ret = 0;
	int i = 0;
	int j = 0;
	u16 node_data = 0;
	bool jump = 0;


	GTP_DEBUG("gcore mp test item noise");

	gdev->fw_event = FW_READ_NOISE;
	gdev->firmware = mp_data->noise_data;
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
	gdev->fw_xfer = 2048;
#else
	gdev->fw_xfer = RAW_DATA_SIZE;
#endif

	mutex_lock(&gdev->transfer_lock);

	mp_test_fn.wait_int = true;

	ret = gcore_bus_write(cmd, sizeof(cmd));
	if (ret) {
		GTP_ERROR("write mp test open cmd fail!");
		mutex_unlock(&gdev->transfer_lock);
		return -EPERM;
	}

	mutex_unlock(&gdev->transfer_lock);

	if (!wait_for_completion_interruptible_timeout(&mp_test_complete, 3 * HZ)) {
		GTP_ERROR("mp test item noise timeout.");
		mp_test_fn.wait_int = false;
		return EPERM;
	}

	noisedata = mp_data->noise_data;

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {
		node_data = (noisedata[1] << 8) | noisedata[0];

		node_data = node_data / 2;

		mp_data->noise_node_val[i] = node_data;

		noisedata += 2;
	}

	for (i = 0; i < RAW_DATA_SIZE / 2; i++) {
		node_data = mp_data->noise_node_val[i];
		jump = 0;

		for (j = 0; j < EMPTY_NUM; j++) {
			if (i == empty_place[j]) {
				GTP_DEBUG("empty node %d need not judge.", i);
				jump = 1;
			}
		}

		if (!jump) {
			if (node_data > mp_data->peak_to_peak) {
				GTP_ERROR("i: %d node_data %d > peak_to_peak test fail!", \
										i, node_data);

				return -EPERM;
			}
		}
	}

	return 0;
}

static int mp_test_event_handler(void *p)
{
	struct gcore_dev *gdev = (struct gcore_dev *)p;
	/*struct sched_param param = {.sched_priority = 4 };

	sched_setscheduler(current, SCHED_RR, &param);*/

	do {
		set_current_state(TASK_INTERRUPTIBLE);

		wait_event_interruptible(gdev->wait, mp_test_fn.event_flag == true);
		mp_test_fn.event_flag = false;

		if (mutex_is_locked(&gdev->transfer_lock)) {
			GTP_DEBUG("fw event is locked, ignore");
			continue;
		}

		mutex_lock(&gdev->transfer_lock);

		switch (gdev->fw_event) {
		case FW_READ_OPEN:
			gcore_mp_test_item_open_reply(gdev->firmware, gdev->fw_xfer);
			break;

		case FW_READ_SHORT:
			gcore_mp_test_item_open_reply(gdev->firmware, gdev->fw_xfer);
			break;

		case FW_READ_RAWDATA:
			gcore_mp_test_item_open_reply(gdev->firmware, gdev->fw_xfer);
			break;
		case FW_READ_NOISE:
			gcore_mp_test_item_open_reply(gdev->firmware, gdev->fw_xfer);
		default:
			break;
		}

		mutex_unlock(&gdev->transfer_lock);

	} while (!kthread_should_stop());

	return 0;
}

int gcore_alloc_mp_test_mem(struct gcore_mp_data *mp_data)
{
	if (mp_data->test_open) {
		if (mp_data->open_data == NULL) {
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
			mp_data->open_data = kzalloc(2048, GFP_KERNEL);
#else
			mp_data->open_data = kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
#endif
			if (!mp_data->open_data) {
				GTP_ERROR("allocate mp test open_data mem fail!");
				return -ENOMEM;
			}
		}

		if (mp_data->open_node_val == NULL) {
			mp_data->open_node_val = kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (!mp_data->open_node_val) {
				GTP_ERROR("allocate mp test open_node_val mem fail!");
				return -ENOMEM;
			}
		}

	}

	if (mp_data->test_short) {
		if (mp_data->short_data == NULL) {
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
			mp_data->short_data = kzalloc(2048, GFP_KERNEL);
#else
			mp_data->short_data = kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
#endif
			if (!mp_data->short_data) {
				GTP_ERROR("allocate mp test short_data mem fail!");
				return -ENOMEM;
			}
		}

		if (mp_data->short_node_val == NULL) {
			mp_data->short_node_val = kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (!mp_data->short_node_val) {
				GTP_ERROR("allocate mp test short_node_val mem fail!");
				return -ENOMEM;
			}
		}
	}

	if (mp_data->test_rawdata) {
		if (mp_data->raw_data == NULL) {
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
			mp_data->raw_data = kzalloc(2048, GFP_KERNEL);
#else
			mp_data->raw_data = kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
#endif
			if (IS_ERR_OR_NULL(mp_data->raw_data)) {
				GTP_ERROR("allocate mp test raw_data mem fail!");
				return -ENOMEM;
			}
		}

		if (mp_data->rawdata_node_val == NULL) {
			mp_data->rawdata_node_val = (u16 *) kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (IS_ERR_OR_NULL(mp_data->rawdata_node_val)) {
				GTP_ERROR("allocate mp test rawdata_node_val mem fail!");
				return -ENOMEM;
			}
		}

#ifdef MP_RAWDATA_DEC_TEST
		if (mp_data->rawdata_dec_h == NULL) {
			mp_data->rawdata_dec_h = (u16 *) kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (IS_ERR_OR_NULL(mp_data->rawdata_dec_h)) {
				GTP_ERROR("allocate mp test rawdata dec h data mem fail!");
				return -ENOMEM;
			}
		}

		if (mp_data->rawdata_dec_l == NULL) {
			mp_data->rawdata_dec_l = (u16 *) kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (IS_ERR_OR_NULL(mp_data->rawdata_dec_l)) {
				GTP_ERROR("allocate mp test rawdata dec l data mem fail!");
				return -ENOMEM;
			}
		}

		if (mp_data->rawdata_avg_h == NULL) {
			mp_data->rawdata_avg_h = (u16 *) kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (IS_ERR_OR_NULL(mp_data->rawdata_avg_h)) {
				GTP_ERROR("allocate mp test rawdata avg h data mem fail!");
				return -ENOMEM;
			}
		}

		if (mp_data->rawdata_avg_l == NULL) {
			mp_data->rawdata_avg_l = (u16 *) kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (IS_ERR_OR_NULL(mp_data->rawdata_avg_l)) {
				GTP_ERROR("allocate mp test rawdata avg l data mem fail!");
				return -ENOMEM;
			}
		}
#endif
	}

	if (mp_data->test_noise) {
		if (mp_data->noise_data == NULL) {
#ifdef CONFIG_TOUCH_DRIVER_INTERFACE_SPI
			mp_data->noise_data = kzalloc(2048, GFP_KERNEL);
#else
			mp_data->noise_data = kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
#endif
			if (IS_ERR_OR_NULL(mp_data->noise_data)) {
				GTP_ERROR("allocate mp test noise_data mem fail!");
				return -ENOMEM;
			}
		}

		memset(mp_data->noise_data, 0, RAW_DATA_SIZE);
		
		if (mp_data->noise_node_val == NULL) {
			mp_data->noise_node_val = (u16 *) kzalloc(RAW_DATA_SIZE, GFP_KERNEL);
			if (IS_ERR_OR_NULL(mp_data->noise_node_val)) {
				GTP_ERROR("allocate mp test noise_node_val mem fail!");
				return -ENOMEM;
			}
		}

		memset(mp_data->noise_node_val, 0, RAW_DATA_SIZE);
		
	}
	return 0;
}

void gcore_free_mp_test_mem(struct gcore_mp_data *mp_data)
{
	if (mp_data->test_open) {
		kfree(mp_data->open_data);
		mp_data->open_data = NULL;

		kfree(mp_data->open_node_val);
		mp_data->open_node_val = NULL;
	}

	if (mp_data->test_short) {
		kfree(mp_data->short_data);
		mp_data->short_data = NULL;

		kfree(mp_data->short_node_val);
		mp_data->short_node_val = NULL;
	}

	if (mp_data->test_rawdata) {
		kfree(mp_data->raw_data);
		mp_data->raw_data = NULL;

		kfree(mp_data->rawdata_node_val);
		mp_data->rawdata_node_val = NULL;

#ifdef MP_RAWDATA_DEC_TEST
		kfree(mp_data->rawdata_dec_h);
		mp_data->rawdata_dec_h = NULL;

		kfree(mp_data->rawdata_dec_l);
		mp_data->rawdata_dec_l = NULL;

		kfree(mp_data->rawdata_avg_h);
		mp_data->rawdata_avg_h = NULL;

		kfree(mp_data->rawdata_avg_l);
		mp_data->rawdata_avg_l = NULL;
#endif
	}

	if (mp_data->test_noise) {
		kfree(mp_data->noise_data);
		mp_data->noise_data = NULL;

		kfree(mp_data->noise_node_val);
		mp_data->noise_node_val = NULL;
	}
}

int dump_mp_data_row_to_buffer(char *buf, size_t size, const u16 *data,
			       int cols, const char *prefix, const char *suffix, char seperator)
{
	int c, count = 0;

	if (prefix) {
		count += scnprintf(buf, size, "%s", prefix);
	}

	for (c = 0; c < cols; c++) {
		count += scnprintf(buf + count, size - count, "%4u%c ", data[c], seperator);
	}

	if (suffix) {
		count += scnprintf(buf + count, size - count, "%s", suffix);
	}

	return count;

}

int dump_mp_data_to_buffer(const u16 *data, int rows, int cols)
{
	int r = 0;

	const u16 *n_val = data;

	GTP_DEBUG("dump mp data to buffer: row: %d	col: %d", rows, cols);

	for (r = 0; r < rows; r++) {
		char linebuf[256];
		int len;
		len = dump_mp_data_row_to_buffer(linebuf, sizeof(linebuf), n_val, cols, NULL, "\n", ',');
		n_val += cols;
		tpd_copy_to_tp_firmware_data(linebuf);
	}

	return 0;

}

void dump_mp_data_to_seq_file(struct seq_file *m, const u16 *data, int rows, int cols)
{
	int r = 0;
	const u16 *n_vals = data;

	for (r = 0; r < rows; r++) {
		char linebuf[256];
		int len;

		len = dump_mp_data_row_to_buffer(linebuf, sizeof(linebuf), n_vals, cols, NULL, "\n", ',');
		seq_puts(m, linebuf);

		n_vals += cols;
	}
}

int gcore_save_mp_data_to_file(struct gcore_mp_data *mp_data)
{
	int rows = RAWDATA_ROW;
	int cols = RAWDATA_COLUMN;
	int ret = 0;

	GTP_DEBUG("save mp data to file");
	if (mp_data->test_open) {
		tpd_copy_to_tp_firmware_data("\n======================================================= open test ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->open_node_val, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp open test data to buffer failed");
			return ret;
		}
	}

	if (mp_data->test_short) {
		tpd_copy_to_tp_firmware_data("\n======================================================= short test ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->short_node_val, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp short test data to buffer failed");
			return ret;
		}
	}

	if (mp_data->test_rawdata) {
		tpd_copy_to_tp_firmware_data("\n======================================================= rawdata test ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->rawdata_node_val, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp test rawdata to buffer failed");
		}

	if (mp_data->test_noise) {
		tpd_copy_to_tp_firmware_data("\n======================================================= noise test ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->noise_node_val, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp test noise to buffer failed");
		}
	}

#ifdef MP_RAWDATA_DEC_TEST
		tpd_copy_to_tp_firmware_data("\n======================================================= rawdata DECH ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->rawdata_dec_h, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp test rawdata devc h to file failed");
		}

		tpd_copy_to_tp_firmware_data("\n======================================================= rawdata DECL ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->rawdata_dec_l, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp test rawdata devc l to file failed");
		}
		tpd_copy_to_tp_firmware_data("\n======================================================= rawdata AVGH ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->rawdata_avg_h, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp test rawdata avg h to file failed");
		}
		tpd_copy_to_tp_firmware_data("\n======================================================= rawdata AVGL ============================================================\n");
		ret = dump_mp_data_to_buffer(mp_data->rawdata_avg_l, rows, cols);
		if (ret < 0) {
			GTP_ERROR("dump mp test rawdata avg l to file failed");
		}
#endif
	}
	return 0;
}

int gcore_start_mp_test(void)
{
	struct gcore_mp_data *mp_data = g_mp_data;
	int test_result = 0;
	int ret = 0;
	struct gcore_dev *gdev = fn_data.gdev;

	GTP_DEBUG("gcore start mp test.");
	gdev->tp_self_test = true;
	gcore_parse_mp_test_ini(mp_data);

	ret = gcore_alloc_mp_test_mem(mp_data);
	if (ret) {
		gcore_free_mp_test_mem(g_mp_data);
		return ret;
	}

	if (mp_data->test_int_pin) {
		mp_data->int_pin_test_result = gcore_mp_test_int_pin(mp_data);
		if (mp_data->int_pin_test_result) {
			GTP_DEBUG("Int pin test result fail!");
		}
	}
	usleep_range(1000, 1100);

	if (mp_data->test_chip_id) {
		mp_data->chip_id_test_result = gcore_mp_test_chip_id(mp_data);
		if (mp_data->chip_id_test_result) {
			gcore_tptest_result = gcore_tptest_result | MODULE_TYPE_ERR;
			GTP_DEBUG("Chip id test result fail!");
		}
	}
	usleep_range(1000, 1100);

	if (mp_data->test_open || mp_data->test_short || mp_data->test_rawdata) {
		GTP_DEBUG("mp test begin to updata mp bin");

#ifdef CONFIG_GCORE_AUTO_UPDATE_FW_HOSTDOWNLOAD
		if (gcore_auto_update_hostdownload_proc(gcore_mp_FW)) {
			GTP_ERROR("mp bin update hostdownload proc fail");
		}
/* gcore_request_firmware_update_work(NULL); */
#else
		if (gcore_mp_firmware_update()) {
			GTP_ERROR("mp bin update hostdownload proc fail");
		}
#endif
		else {
			usleep_range(1000, 1100);

			if (mp_data->test_open) {
				mp_data->open_test_result = gcore_mp_test_item_open(mp_data);
				if (mp_data->open_test_result) {
					gcore_tptest_result = gcore_tptest_result | TEST_GT_OPEN;
					GTP_DEBUG("Open test result fail!");
				}
			}
			usleep_range(1000, 1100);

			if (mp_data->test_short) {
				mp_data->short_test_result = gcore_mp_test_item_short(mp_data);
				if (mp_data->short_test_result) {
					gcore_tptest_result = gcore_tptest_result | TEST_GT_SHORT;
					GTP_DEBUG("Short test result fail!");
				}
			}

			msleep(100);

			if (mp_data->test_rawdata) {
				mp_data->rawdata_test_result = gcore_mp_test_item_rawdata(mp_data);
				if (mp_data->rawdata_test_result) {
					gcore_tptest_result = gcore_tptest_result | TEST_BEYOND_MAX_LIMIT;
					GTP_DEBUG("Rawdata test result fail!");
				}
			}

			msleep(100);

			if (mp_data->test_noise) {
				mp_data->noise_test_result = gcore_mp_test_item_noise(mp_data);
				if (mp_data->noise_test_result) {
					gcore_tptest_result = gcore_tptest_result | TEST_BEYOND_MAX_LIMIT;
					GTP_DEBUG("Noise test result fail!");
				}
			}
			msleep(100);
		}

	}

	gcore_save_mp_data_to_file(mp_data);
	gdev->irq_disable(gdev);
#ifdef CONFIG_GCORE_AUTO_UPDATE_FW_HOSTDOWNLOAD
	gcore_request_firmware_update_work(NULL);
#else
	gcore_upgrade_soft_reset();
#endif
	msleep(100);
	gdev->irq_enable(gdev);
	test_result = (mp_data->int_pin_test_result || mp_data->chip_id_test_result
		       || mp_data->open_test_result || mp_data->short_test_result
		       || mp_data->rawdata_test_result || mp_data->noise_test_result) ? -1 : 0;

	GTP_DEBUG("start mp test result:%d", test_result);
	gdev->tp_self_test = false;
	return test_result;

}

static int32_t gcore_mp_test_open(struct inode *inode, struct file *file)
{
	struct gcore_mp_data *mp_data = PDE_DATA(inode);
	int ret = 0;

	if (mp_data == NULL) {
		GTP_ERROR("Open selftest proc with mp data = NULL");
		return -EPERM;
	}

	GTP_DEBUG("gcore mp test open");

	gcore_parse_mp_test_dt(mp_data);

	ret = gcore_alloc_mp_test_mem(mp_data);
	if (ret) {
		gcore_free_mp_test_mem(g_mp_data);
	}

	if (mp_data->test_int_pin) {
		mp_data->int_pin_test_result = gcore_mp_test_int_pin(mp_data);
	}

	usleep_range(1000, 1100);

	if (mp_data->test_chip_id) {
		mp_data->chip_id_test_result = gcore_mp_test_chip_id(mp_data);
	}

	usleep_range(1000, 1100);

	if (mp_data->test_open || mp_data->test_short) {
		GTP_DEBUG("mp test begin to updata mp bin");

#ifdef CONFIG_GCORE_AUTO_UPDATE_FW_HOSTDOWNLOAD
		if (gcore_auto_update_hostdownload_proc(gcore_mp_FW)) {
			GTP_ERROR("mp bin update hostdownload proc fail");
		}
/* gcore_request_firmware_update_work(NULL); */
#else
		if (gcore_mp_firmware_update()) {
			GTP_ERROR("mp bin update hostdownload proc fail");
		}
#endif
		else {
			usleep_range(1000, 1100);

			if (mp_data->test_open) {
				mp_data->open_test_result = gcore_mp_test_item_open(mp_data);
			}

			usleep_range(1000, 1100);

			if (mp_data->test_short) {
				mp_data->short_test_result = gcore_mp_test_item_short(mp_data);
			}
		}

	}

	gcore_save_mp_data_to_file(mp_data);

	if (seq_open(file, &gcore_mptest_seq_ops)) {
		GTP_ERROR("seq open fail!");
		return -EPERM;
	}

	((struct seq_file *)file->private_data)->private = mp_data;

	return 0;
}

struct file_operations gcore_mp_fops = {
	.owner = THIS_MODULE,
	.open = gcore_mp_test_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

int gcore_mp_test_fn_init(struct gcore_dev *gdev)
{
	struct gcore_mp_data *mp_data = NULL;

	GTP_DEBUG("gcore mp test fn init");

	mp_thread = kthread_run(mp_test_event_handler, gdev, "gcore_mptest");

	mp_data = kzalloc(sizeof(struct gcore_mp_data), GFP_KERNEL);
	if (!mp_data) {
		GTP_ERROR("allocate mp_data mem fail! failed");
		return -EPERM;
	}

	mp_data->gdev = gdev;

	g_mp_data = mp_data;

	mp_data->mp_proc_entry = proc_create_data(MP_TEST_PROC_FILENAME,
						  S_IRUGO, NULL, &gcore_mp_fops, mp_data);
	if (mp_data->mp_proc_entry == NULL) {
		GTP_ERROR("create mp test proc entry selftest failed");
		goto fail1;
	}

	return 0;

fail1:
	kfree(mp_data);
	return -EPERM;

}

void gcore_mp_test_fn_remove(struct gcore_dev *gdev)
{
	kthread_stop(mp_thread);

	if (g_mp_data->mp_proc_entry) {
		remove_proc_entry(MP_TEST_PROC_FILENAME, NULL);
	}

	gcore_free_mp_test_mem(g_mp_data);

	kfree(g_mp_data);
	g_mp_data = NULL;
}

