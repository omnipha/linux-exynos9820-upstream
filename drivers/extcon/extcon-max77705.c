// SPDX-License-Identifier: GPL-2.0
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/extcon-provider.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define MAX77705_HW_REV			0x00
#define MAX77705_FW_REV			0x01
#define MAX77705_UIC_INT		0x02
#define MAX77705_CC_INT			0x03
#define MAX77705_USBC_STATUS2		0x07
#define MAX77705_CC_STATUS0		0x0a
#define MAX77705_PD_STATUS1		0x0d
#define MAX77705_OPCODE_WRITE		0x21
#define MAX77705_OPCODE_WRITE_END	0x41
#define MAX77705_OPCODE_READ		0x51

#define MAX77705_UIC_APCMDRES		BIT(7)
#define MAX77705_UIC_SYSMSG		BIT(6)
#define MAX77705_CC_STAT_INT		BIT(0)
#define MAX77705_CCSTAT			GENMASK(2, 0)
#define MAX77705_CCSTAT_SINK		1
#define MAX77705_PD_DATAROLE_DFP	BIT(7)
#define MAX77705_SYSMSG_BOOT_WDT	0x03
#define MAX77705_SYSMSG_BOOT_POR	0x05
#define MAX77705_OP_CTRL1_W		0x06
#define MAX77705_MUIC_COM_USB		0x09
#define MAX77705_CMD_TIMEOUT_MS		3000
#define MAX77705_POLL_INTERVAL_MS	1000

struct max77705_muic {
	struct device *dev;
	struct regmap *regmap;
	struct extcon_dev *edev;
	struct delayed_work work;
	bool routed;
	bool mailbox_failed;
};

static const unsigned int max77705_muic_cables[] = {
	EXTCON_USB,
	EXTCON_NONE,
};

static const struct regmap_range max77705_muic_write_ranges[] = {
	regmap_reg_range(MAX77705_OPCODE_WRITE, MAX77705_OPCODE_WRITE + 1),
	regmap_reg_range(MAX77705_OPCODE_WRITE_END, MAX77705_OPCODE_WRITE_END),
};

static const struct regmap_access_table max77705_muic_write_table = {
	.yes_ranges = max77705_muic_write_ranges,
	.n_yes_ranges = ARRAY_SIZE(max77705_muic_write_ranges),
};

static const struct regmap_config max77705_muic_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX77705_OPCODE_READ,
	.wr_table = &max77705_muic_write_table,
};

static int max77705_muic_read_uic(struct max77705_muic *muic, unsigned int *uic,
				  bool *reset)
{
	unsigned int sysmsg;
	int ret;

	ret = regmap_read(muic->regmap, MAX77705_UIC_INT, uic);
	if (ret || !(*uic & MAX77705_UIC_SYSMSG))
		return ret;

	ret = regmap_read(muic->regmap, MAX77705_USBC_STATUS2, &sysmsg);
	if (ret)
		return ret;

	if (sysmsg == MAX77705_SYSMSG_BOOT_WDT || sysmsg == MAX77705_SYSMSG_BOOT_POR)
		*reset = true;

	return 0;
}

static int max77705_muic_route_usb(struct max77705_muic *muic)
{
	static const u8 cmd[] = { MAX77705_OP_CTRL1_W, MAX77705_MUIC_COM_USB };
	unsigned long timeout;
	unsigned int val;
	bool reset = false;
	int ret;

	ret = max77705_muic_read_uic(muic, &val, &reset);
	if (ret)
		return ret;

	ret = regmap_bulk_write(muic->regmap, MAX77705_OPCODE_WRITE, cmd, sizeof(cmd));
	if (ret)
		return ret;

	ret = regmap_write(muic->regmap, MAX77705_OPCODE_WRITE_END, 0);
	if (ret)
		return ret;

	timeout = jiffies + msecs_to_jiffies(MAX77705_CMD_TIMEOUT_MS);
	for (;;) {
		reset = false;
		ret = max77705_muic_read_uic(muic, &val, &reset);
		if (ret)
			return ret;
		if (reset) {
			muic->mailbox_failed = true;
			return -EIO;
		}
		if (val & MAX77705_UIC_APCMDRES)
			break;
		if (time_after_eq(jiffies, timeout)) {
			muic->mailbox_failed = true;
			return -ETIMEDOUT;
		}
		usleep_range(5000, 6000);
	}

	ret = regmap_read(muic->regmap, MAX77705_OPCODE_READ, &val);
	if (ret)
		return ret;
	if (val != MAX77705_OP_CTRL1_W) {
		muic->mailbox_failed = true;
		return -EIO;
	}

	return 0;
}

static void max77705_muic_work(struct work_struct *work)
{
	struct max77705_muic *muic = container_of(to_delayed_work(work),
						  struct max77705_muic, work);
	unsigned int uic, cc_int, cc, pd;
	bool reset = false, sink_device;
	int ret;

	ret = max77705_muic_read_uic(muic, &uic, &reset);
	if (ret)
		goto error;
	ret = regmap_read(muic->regmap, MAX77705_CC_INT, &cc_int);
	if (ret)
		goto error;
	ret = regmap_read(muic->regmap, MAX77705_CC_STATUS0, &cc);
	if (ret)
		goto error;
	ret = regmap_read(muic->regmap, MAX77705_PD_STATUS1, &pd);
	if (ret)
		goto error;

	sink_device = FIELD_GET(MAX77705_CCSTAT, cc) == MAX77705_CCSTAT_SINK &&
		      !(pd & MAX77705_PD_DATAROLE_DFP);
	if (!sink_device || reset || (cc_int & MAX77705_CC_STAT_INT))
		muic->routed = false;

	if (sink_device && !muic->routed && !muic->mailbox_failed) {
		ret = max77705_muic_route_usb(muic);
		if (ret) {
			dev_warn(muic->dev, "USB routing failed: %d%s\n", ret,
				 muic->mailbox_failed ? "; mailbox disabled until rebind" : "");
		} else {
			muic->routed = true;
			dev_info(muic->dev, "USB data path routed\n");
		}
	}

	ret = extcon_set_state_sync(muic->edev, EXTCON_USB, sink_device && muic->routed);
	if (ret)
		dev_warn_ratelimited(muic->dev, "cannot update extcon state: %d\n", ret);
	goto out;

error:
	dev_warn_ratelimited(muic->dev, "cannot read CCIC status: %d\n", ret);
out:
	queue_delayed_work(system_freezable_wq, &muic->work,
			   msecs_to_jiffies(MAX77705_POLL_INTERVAL_MS));
}

static int max77705_muic_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct max77705_muic *muic;
	unsigned int hw, fw;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP, "raw I2C required\n");

	muic = devm_kzalloc(dev, sizeof(*muic), GFP_KERNEL);
	if (!muic)
		return -ENOMEM;
	muic->dev = dev;

	muic->regmap = devm_regmap_init_i2c(client, &max77705_muic_regmap_config);
	if (IS_ERR(muic->regmap))
		return dev_err_probe(dev, PTR_ERR(muic->regmap), "regmap init failed\n");

	ret = regmap_read(muic->regmap, MAX77705_HW_REV, &hw);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read hardware revision\n");
	ret = regmap_read(muic->regmap, MAX77705_FW_REV, &fw);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read firmware revision\n");
	if ((hw == 0 && fw == 0) || (hw == 0xff && fw == 0xff))
		return dev_err_probe(dev, -ENODEV, "invalid CCIC identity\n");

	muic->edev = devm_extcon_dev_allocate(dev, max77705_muic_cables);
	if (IS_ERR(muic->edev))
		return dev_err_probe(dev, PTR_ERR(muic->edev), "extcon allocate failed\n");
	ret = devm_extcon_dev_register(dev, muic->edev);
	if (ret)
		return dev_err_probe(dev, ret, "extcon register failed\n");
	ret = devm_delayed_work_autocancel(dev, &muic->work, max77705_muic_work);
	if (ret)
		return dev_err_probe(dev, ret, "cannot init delayed work\n");

	queue_delayed_work(system_freezable_wq, &muic->work, 0);
	return 0;
}

static const struct of_device_id max77705_muic_of_match[] = {
	{ .compatible = "maxim,max77705-muic" },
	{ }
};
MODULE_DEVICE_TABLE(of, max77705_muic_of_match);

static struct i2c_driver max77705_muic_driver = {
	.driver = {
		.name = "max77705-muic",
		.of_match_table = max77705_muic_of_match,
	},
	.probe = max77705_muic_probe,
};
module_i2c_driver(max77705_muic_driver);

MODULE_DESCRIPTION("Maxim MAX77705 CCIC USB routing extcon driver");
MODULE_LICENSE("GPL");
