// SPDX-License-Identifier: GPL-2.0-only
/*
 * Galaxy S10 S2MPB02 camera sub-PMIC BUCK1 regulator (0x59 on USI10).
 *
 * Samsung's DT marks S2MPB02_BUCK1 as 1.3 V, always-on and boot-on. The
 * state observed on this unit before the driver binds is B1CTRL1=0x58 and
 * B1CTRL2=0xc0 (selector 1.6 V). The meaning of bit 6 in 0x58 is not
 * established, and no supply topology towards the camera rails is claimed.
 *
 * This driver manages solely BUCK1 (registers 0x0b and 0x0c). All other
 * registers are left untouched.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include "s10-s2mpb02.h"

#define PB02_REG_ID		0x00
#define PB02_REG_B1CTRL1	0x0b
#define PB02_REG_B1CTRL2	0x0c
#define PB02_REG_BST_CTRL2	0x1d

#define PB02_BUCK1_MIN_UV	400000
#define PB02_BUCK1_STEP_UV	6250
#define PB02_BUCK1_N_VOLTAGES	256
#define PB02_BUCK1_VSEL_MASK	0xff
#define PB02_BUCK1_ENABLE_MASK	0xc0
#define PB02_BUCK1_ENABLE_VAL	0xc0

#define PB02_BOOT_B1CTRL1	0x58
#define PB02_BOOT_B1CTRL2	0xc0
#define PB02_LIVE_B1CTRL1	0xd8
#define PB02_LIVE_B1CTRL2	0x90

static bool pb02_writable(struct device *dev, unsigned int reg)
{
	return reg == PB02_REG_B1CTRL1 || reg == PB02_REG_B1CTRL2;
}

static const struct regmap_config pb02_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x32,
	.cache_type = REGCACHE_NONE,
	.writeable_reg = pb02_writable,
};

static const struct regulator_ops pb02_buck1_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
};

static const struct regulator_desc pb02_buck1_desc = {
	.name = "s2mpb02-buck1",
	.of_match = "s2mpb02-buck1",
	.regulators_node = "regulators",
	.id = 0,
	.ops = &pb02_buck1_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.min_uV = PB02_BUCK1_MIN_UV,
	.uV_step = PB02_BUCK1_STEP_UV,
	.n_voltages = PB02_BUCK1_N_VOLTAGES,
	.vsel_reg = PB02_REG_B1CTRL2,
	.vsel_mask = PB02_BUCK1_VSEL_MASK,
	.enable_reg = PB02_REG_B1CTRL1,
	.enable_mask = PB02_BUCK1_ENABLE_MASK,
	.enable_val = PB02_BUCK1_ENABLE_VAL,
	.enable_time = 100,
};

struct pb02 {
	struct regmap *map;
};

static DEFINE_MUTEX(pb02_lock);
static struct device *pb02_ready_dev;

static void pb02_unready(void *unused)
{
	mutex_lock(&pb02_lock);
	pb02_ready_dev = NULL;
	mutex_unlock(&pb02_lock);
}

int s10_s2mpb02_buck1_require(struct device *consumer)
{
	struct device_link *link;
	int ret = -EPROBE_DEFER;

	mutex_lock(&pb02_lock);
	if (pb02_ready_dev) {
		link = device_link_add(consumer, pb02_ready_dev,
				       DL_FLAG_AUTOREMOVE_CONSUMER);
		if (link && READ_ONCE(link->status) == DL_STATE_CONSUMER_PROBE)
			ret = 0;
	}
	mutex_unlock(&pb02_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(s10_s2mpb02_buck1_require);

static int pb02_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { .dev = dev };
	struct regulator_dev *rdev;
	struct pb02 *pb02;
	unsigned int id, bst2, c1, c2;
	int ret;

	if (client->addr != 0x59 ||
	    (!of_machine_is_compatible("samsung,beyond1lte") &&
	     !of_machine_is_compatible("samsung,beyond2lte")))
		return -ENODEV;

	pb02 = devm_kzalloc(dev, sizeof(*pb02), GFP_KERNEL);
	if (!pb02)
		return -ENOMEM;

	pb02->map = devm_regmap_init_i2c(client, &pb02_regmap_config);
	if (IS_ERR(pb02->map))
		return PTR_ERR(pb02->map);
	dev_set_drvdata(dev, pb02);

	ret = regmap_read(pb02->map, PB02_REG_ID, &id);
	ret = ret ?: regmap_read(pb02->map, PB02_REG_BST_CTRL2, &bst2);
	ret = ret ?: regmap_read(pb02->map, PB02_REG_B1CTRL1, &c1);
	ret = ret ?: regmap_read(pb02->map, PB02_REG_B1CTRL2, &c2);
	if (ret)
		return dev_err_probe(dev, ret, "S2MPB02 read failed\n");

	dev_info(dev, "S2MPB02 ID=%#04x BST2=%#04x B1CTRL1=%#04x B1CTRL2=%#04x\n",
		 id, bst2, c1, c2);

	if (id != 0x00 || bst2 != 0x90)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected chip ID/BST2: %#04x/%#04x\n",
				     id, bst2);

	if (c1 == PB02_BOOT_B1CTRL1 && c2 == PB02_BOOT_B1CTRL2) {
		ret = regmap_write(pb02->map, PB02_REG_B1CTRL2, PB02_LIVE_B1CTRL2);
		if (ret)
			return dev_err_probe(dev, ret, "BUCK1 voltage write failed\n");
		usleep_range(2000, 2500);
		ret = regmap_read(pb02->map, PB02_REG_B1CTRL2, &c2);
		if (ret || c2 != PB02_LIVE_B1CTRL2) {
			dev_err(dev, "BUCK1 voltage readback failed: %#04x\n", c2);
			return dev_err_probe(dev, ret ?: -EIO,
					     "BUCK1 voltage not confirmed\n");
		}
		ret = regmap_update_bits(pb02->map, PB02_REG_B1CTRL1,
					 PB02_BUCK1_ENABLE_MASK, PB02_BUCK1_ENABLE_VAL);
		if (ret)
			return dev_err_probe(dev, ret, "BUCK1 enable update failed\n");
		usleep_range(5000, 6000);
	} else if (c1 == PB02_BOOT_B1CTRL1 && c2 == PB02_LIVE_B1CTRL2) {
		ret = regmap_update_bits(pb02->map, PB02_REG_B1CTRL1,
					 PB02_BUCK1_ENABLE_MASK, PB02_BUCK1_ENABLE_VAL);
		if (ret)
			return dev_err_probe(dev, ret, "BUCK1 enable update failed\n");
		usleep_range(5000, 6000);
	} else if ((c1 & PB02_BUCK1_ENABLE_MASK) == PB02_BUCK1_ENABLE_VAL &&
		   c2 == PB02_LIVE_B1CTRL2) {
		dev_info(dev, "BUCK1 already active (%#04x/%#04x)\n", c1, c2);
	} else if ((c1 & PB02_BUCK1_ENABLE_MASK) == PB02_BUCK1_ENABLE_VAL &&
		   c2 == PB02_BOOT_B1CTRL2) {
		ret = regmap_write(pb02->map, PB02_REG_B1CTRL2, PB02_LIVE_B1CTRL2);
		if (ret)
			return dev_err_probe(dev, ret, "BUCK1 voltage write failed\n");
		usleep_range(25000, 26000);
	} else {
		return dev_err_probe(dev, -ENODEV,
				     "unsupported BUCK1 state: %#04x/%#04x\n",
				     c1, c2);
	}

	ret = regmap_read(pb02->map, PB02_REG_B1CTRL1, &c1);
	ret = ret ?: regmap_read(pb02->map, PB02_REG_B1CTRL2, &c2);
	if (ret || (c1 & PB02_BUCK1_ENABLE_MASK) != PB02_BUCK1_ENABLE_VAL ||
	    c2 != PB02_LIVE_B1CTRL2)
		return dev_err_probe(dev, ret ?: -EIO,
				     "BUCK1 final state invalid: %#04x/%#04x\n",
				     c1, c2);
	dev_info(dev, "BUCK1 confirmed at 1.3 V selector (%#04x/%#04x)\n", c1, c2);

	config.regmap = pb02->map;
	rdev = devm_regulator_register(dev, &pb02_buck1_desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register BUCK1\n");

	ret = devm_add_action_or_reset(dev, pb02_unready, NULL);
	if (ret)
		return ret;

	mutex_lock(&pb02_lock);
	pb02_ready_dev = dev;
	mutex_unlock(&pb02_lock);

	return 0;
}

static const struct of_device_id pb02_match[] = {
	{ .compatible = "samsung,s2mpb02-camera-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, pb02_match);

static struct i2c_driver pb02_driver = {
	.driver = {
		.name = "s10-s2mpb02",
		.of_match_table = pb02_match,
	},
	.probe = pb02_probe,
};
module_i2c_driver(pb02_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Samsung S2MPB02 camera sub-PMIC BUCK1 regulator");
