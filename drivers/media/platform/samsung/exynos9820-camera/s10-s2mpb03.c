// SPDX-License-Identifier: GPL-2.0-or-later
/* Galaxy S10 second camera PMIC (S2MPB03 @0x56 on CMGP1). Register layout and
 * voltage ranges from Samsung Electronics' GPL s2mpb03.c / s2mpb03.h (2016).
 *
 * The S10 rails on this PMIC (Samsung board file names):
 *   ldo1   VDDD_1.0V_CAM     S5K2L4 core (main camera, not up yet)
 *   ldo2   VDDQ_1.1V_CAM     S5K2L4
 *   ldo3   VDDIO_1.8V_CAM    rear-flex camera I/O (tele AF actuator bus needs it)
 *   ldo4   VDDD_RET_1.0V_CAM S5K2L4 retention
 *   ldo5   VDDA_2.4V_CAM     rear EEPROM
 *   ldo6   VDDA_2.8V_CAM     rear flex analog
 *   ldo7   VDD_VM_2.8V_OIS   OIS MCU
 *
 * Unlike s10-s2mpb02 the boot values of this PMIC are not yet characterised,
 * so the driver snapshots every register it may touch at probe and writes the
 * snapshot back on unload and shutdown. Probe refuses to touch an LDO whose
 * enable bit is already set. LDO1/2/4 use the vendor 12.5 mV step (other rails use 25 mV).
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include "s10-s2mpb02.h"

static bool regulators = true;
module_param(regulators, bool, 0400);
MODULE_PARM_DESC(regulators, "Register the S10 second camera supplies (default: true)");
static bool preserve_active_ldo3;
module_param(preserve_active_ldo3, bool, 0400);
MODULE_PARM_DESC(preserve_active_ldo3,
	"Preserve LDO3 if already on; allow the test to manage it if it is off");
static bool preserve_cold_active_ldo4;
module_param(preserve_cold_active_ldo4, bool, 0400);
MODULE_PARM_DESC(preserve_cold_active_ldo4,
	"Preserve LDO4 if already on; allow the test to manage it if it is off");

#define PB03_ID		0x00
#define PB03_STATUS	0x01
#define PB03_CTRL	0x02
#define PB03_LCTRL(n)	(0x02 + (n))	/* LDOn control: enable in bit 7, code in 5:0 */

#define PB03_LDO_EN	0x80

/* One S2MPB03 exists on this board; the regmap callback must leave a shared,
 * already-on LDO3 read-only while still permitting it to be managed if off.
 */
static bool pb03_preserve_ldo3;

/* Managed LDOs. Order matters for restore. */
static const unsigned int pb03_ldos[] = { 1, 2, 3, 4, 5, 6, 7 };

struct pb03_boot_reg {
	unsigned int reg;
	unsigned int value;
};

static bool pb03_writable(struct device *dev, unsigned int reg)
{
	unsigned int i;

	if (!regulators)
		return false;
	if (pb03_preserve_ldo3 && reg == PB03_LCTRL(3))
		return false;
	for (i = 0; i < ARRAY_SIZE(pb03_ldos); i++)
		if (PB03_LCTRL(pb03_ldos[i]) == reg)
			return true;
	return false;
}

static const struct regmap_config pb03_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x0b,
	.cache_type = REGCACHE_NONE,
	.writeable_reg = pb03_writable,
};

static const struct regulator_ops pb03_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
};

/* LDO3 is shared with the other rear cameras. If it is already on at the
 * measured 1.8-V setting, preserve it physically while allowing references.
 */
static int pb03_preserved_ldo3_set_voltage_sel(struct regulator_dev *rdev,
						       unsigned int sel)
{
	int old_sel = regulator_get_voltage_sel_regmap(rdev);

	if (old_sel < 0)
		return old_sel;
	return old_sel == sel ? 0 : -EPERM;
}

static int pb03_preserved_ldo3_enable(struct regulator_dev *rdev)
{
	return 0;
}

static int pb03_preserved_ldo3_disable(struct regulator_dev *rdev)
{
	return 0;
}

static const struct regulator_ops pb03_preserved_ldo3_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = pb03_preserved_ldo3_set_voltage_sel,
	.is_enabled = regulator_is_enabled_regmap,
	.enable = pb03_preserved_ldo3_enable,
	.disable = pb03_preserved_ldo3_disable,
};

/* If the sensor-retention LDO4 is already on at the observed 700-mV cold
 * setting, the preflight changes its selector only after asserting reset and
 * leaves its enable state untouched until the saved register is restored.
 */
static const struct regulator_ops pb03_cold_ldo4_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.enable = pb03_preserved_ldo3_enable,
	.disable = pb03_preserved_ldo3_disable,
};

/* LDO1/2/4: 700 mV + 12.5 mV; LDO3: 700 mV + 25 mV; others: 1.8 V + 25 mV. */
#define PB03_LDO(_n, _min, _step) { \
	.name = "s2mpb03-ldo" #_n, .of_match = "s2mpb03-ldo" #_n, \
	.regulators_node = "regulators", \
	.id = _n, .ops = &pb03_ops, .type = REGULATOR_VOLTAGE, \
	.owner = THIS_MODULE, .min_uV = _min, .uV_step = _step, \
	.n_voltages = 64, .vsel_reg = PB03_LCTRL(_n), .vsel_mask = 0x3f, \
	.enable_reg = PB03_LCTRL(_n), .enable_mask = 0x80, .enable_time = 150, \
}

static const struct regulator_desc pb03_desc[] = {
	PB03_LDO(1, 700000, 12500),
	PB03_LDO(2, 700000, 12500),
	PB03_LDO(3, 700000, 25000),
	PB03_LDO(4, 700000, 12500),
	PB03_LDO(5, 1800000, 25000),
	PB03_LDO(6, 1800000, 25000),
	PB03_LDO(7, 1800000, 25000),
};

struct pb03 {
	struct regmap *map;
	struct pb03_boot_reg saved[ARRAY_SIZE(pb03_ldos)];
	struct regulator_desc desc[ARRAY_SIZE(pb03_desc)];
	bool preserve_ldo3;
	bool cold_active_ldo4;
};

static ssize_t status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pb03 *pb03 = dev_get_drvdata(dev);
	unsigned int val;
	ssize_t len = 0;
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(pb03_ldos); i++) {
		ret = regmap_read(pb03->map, PB03_LCTRL(pb03_ldos[i]), &val);
		if (ret)
			return ret;
		len += sysfs_emit_at(buf, len, "%02x=%02x\n", PB03_LCTRL(pb03_ldos[i]), val);
	}
	return len;
}
static DEVICE_ATTR_RO(status);
static struct attribute *pb03_attrs[] = { &dev_attr_status.attr, NULL };
ATTRIBUTE_GROUPS(pb03);

static void pb03_restore(void *data)
{
	struct pb03 *pb03 = data;
	int i;

	for (i = ARRAY_SIZE(pb03_ldos) - 1; i >= 0; i--)
		if (!(pb03->preserve_ldo3 && pb03->saved[i].reg == PB03_LCTRL(3)))
			regmap_write(pb03->map, pb03->saved[i].reg,
				     pb03->saved[i].value);
}

static void pb03_shutdown(struct i2c_client *client)
{
	struct pb03 *pb03 = dev_get_drvdata(&client->dev);

	if (regulators)
		pb03_restore(pb03);
}

static int pb03_probe(struct i2c_client *client)
{
	struct regulator_config config = { .dev = &client->dev };
	struct regulator_dev *rdev;
	struct pb03 *pb03;
	unsigned int id, status;
	int ret, i;
	bool active_ldo3 = false, active_ldo4 = false, unexpected_enabled = false;

	if (client->addr != 0x56 ||
	    (!of_machine_is_compatible("samsung,beyond1lte") &&
	     !of_machine_is_compatible("samsung,beyond2lte")))
		return -ENODEV;

	ret = s10_s2mpb02_buck1_require(&client->dev);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "S2MPB02 BUCK1 not initialized\n");

	pb03 = devm_kzalloc(&client->dev, sizeof(*pb03), GFP_KERNEL);
	if (!pb03)
		return -ENOMEM;

	pb03->map = devm_regmap_init_i2c(client, &pb03_regmap_config);
	if (IS_ERR(pb03->map))
		return PTR_ERR(pb03->map);
	dev_set_drvdata(&client->dev, pb03);

	/* Snapshot before anything else; the read-only boot state is unknown. */
	for (i = 0; i < ARRAY_SIZE(pb03_ldos); i++) {
		ret = regmap_read(pb03->map, PB03_LCTRL(pb03_ldos[i]), &pb03->saved[i].value);
		if (ret)
			return dev_err_probe(&client->dev, ret, "LDO register read failed\n");
		pb03->saved[i].reg = PB03_LCTRL(pb03_ldos[i]);
		if (pb03->saved[i].value & PB03_LDO_EN) {
			if (regulators && preserve_active_ldo3 && pb03_ldos[i] == 3 &&
			    (pb03->saved[i].value & 0x3f) == 0x2c) {
				active_ldo3 = true;
			} else if (regulators && preserve_cold_active_ldo4 &&
				   pb03_ldos[i] == 4 && pb03->saved[i].value == 0x80) {
				active_ldo4 = true;
			} else if (regulators) {
				unexpected_enabled = true;
			}
		}
		dev_info(&client->dev, "LDO%u initial=%#x\n",
			 pb03_ldos[i], pb03->saved[i].value);
	}
	if (regulators && unexpected_enabled)
		return dev_err_probe(&client->dev, -EBUSY,
			"an unapproved LDO is already enabled; no rails registered\n");
	pb03->preserve_ldo3 = active_ldo3;
	pb03->cold_active_ldo4 = active_ldo4;
	pb03_preserve_ldo3 = active_ldo3;

	ret = regmap_read(pb03->map, PB03_ID, &id);
	ret = ret ?: regmap_read(pb03->map, PB03_STATUS, &status);
	if (ret)
		return ret;
	dev_info(&client->dev, "S2MPB03 ID=%#x STATUS=%#x; %s\n", id, status,
		 regulators ? "camera regulators" : "read-only identification");
	if (!regulators)
		return 0;

	ret = devm_add_action_or_reset(&client->dev, pb03_restore, pb03);
	if (ret)
		return ret;
	memcpy(pb03->desc, pb03_desc, sizeof(pb03_desc));
	if (pb03->preserve_ldo3)
		pb03->desc[2].ops = &pb03_preserved_ldo3_ops;
	if (pb03->cold_active_ldo4)
		pb03->desc[3].ops = &pb03_cold_ldo4_ops;
	config.regmap = pb03->map;
	for (i = 0; i < ARRAY_SIZE(pb03->desc); i++) {
		rdev = devm_regulator_register(&client->dev, &pb03->desc[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&client->dev, PTR_ERR(rdev),
				"regulator %s failed\n", pb03->desc[i].name);
	}
	return 0;
}

static const struct of_device_id pb03_match[] = {
	{ .compatible = "samsung,s2mpb03-camera-test" }, { }
};
MODULE_DEVICE_TABLE(of, pb03_match);
static struct i2c_driver pb03_driver = {
	.driver = { .name = "s10-s2mpb03", .of_match_table = pb03_match,
		.dev_groups = pb03_groups },
	.probe = pb03_probe,
	.shutdown = pb03_shutdown,
};
module_i2c_driver(pb03_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Samsung S2MPB03 camera PMIC regulator");
