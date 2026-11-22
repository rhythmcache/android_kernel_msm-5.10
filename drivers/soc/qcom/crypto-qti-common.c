// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common crypto library for storage encryption.
 *
 * Copyright (c) 2020-2021, Linux Foundation. All rights reserved.
 */

#include <linux/crypto-qti-common.h>
#include <linux/module.h>
#include <linux/delay.h>
#include "crypto-qti-ice-regs.h"
#include "crypto-qti-platform.h"

static int ice_check_fuse_setting(void __iomem *ice_mmio)
{
	uint32_t regval;
	uint32_t version, major, minor;

	version = ice_readl(ice_mmio, ICE_REGS_VERSION);
	major = (version & ICE_CORE_MAJOR_REV_MASK) >> ICE_CORE_MAJOR_REV;
	minor = (version & ICE_CORE_MINOR_REV_MASK) >> ICE_CORE_MINOR_REV;

	// Check fuse setting is not supported on ICE 3.2 onwards
	if ((major == 0x03) && (minor >= 0x02))
		return 0;
	regval = ice_readl(ice_mmio, ICE_REGS_FUSE_SETTING);
	regval &= (ICE_FUSE_SETTING_MASK |
		ICE_FORCE_HW_KEY0_SETTING_MASK |
		ICE_FORCE_HW_KEY1_SETTING_MASK);
	if (regval) {
		pr_err("%s: Fuses are blown\n", __func__);
		return -EIO;
	}

	return 0;
}

static int ice_check_version(void __iomem *ice_mmio)
{
	uint32_t version, major, minor, step;

	version = ice_readl(ice_mmio, ICE_REGS_VERSION);
	major = (version & ICE_CORE_MAJOR_REV_MASK) >> ICE_CORE_MAJOR_REV;
	minor = (version & ICE_CORE_MINOR_REV_MASK) >> ICE_CORE_MINOR_REV;
	step = (version & ICE_CORE_STEP_REV_MASK) >> ICE_CORE_STEP_REV;

	if (major < ICE_CORE_CURRENT_MAJOR_VERSION) {
		pr_err("%s: Unknown ICE device at %pK, rev %d.%d.%d\n",
			__func__, ice_mmio, major, minor, step);
		return -ENODEV;
	}

	pr_info("Found QC Inline Crypto Engine (ICE) v%d.%d.%d\n",
		major, minor, step);
	return 0;
}

static void ice_low_power_and_optimization_enable(void __iomem *ice_mmio)
{
	uint32_t regval;

	regval = ice_readl(ice_mmio, ICE_REGS_ADVANCED_CONTROL);
	/* Enable low power mode sequence */
	regval |= 0x7000;
	ice_writel(ice_mmio, regval, ICE_REGS_ADVANCED_CONTROL);

	/* Optimization enable sequence */
	regval |= 0xD807100;
	ice_writel(ice_mmio, regval, ICE_REGS_ADVANCED_CONTROL);
	wmb();
}

static int ice_wait_bist_status(void __iomem *ice_mmio)
{
	int count;
	uint32_t regval;

	for (count = 0; count < QTI_ICE_MAX_BIST_CHECK_COUNT; count++) {
		regval = ice_readl(ice_mmio, ICE_REGS_BIST_STATUS);
		if (!(regval & QTI_ICE_BIST_STATUS_MASK))
			break;
		udelay(50);
	}

	if (regval & QTI_ICE_BIST_STATUS_MASK) {
		pr_err("%s: Error in BIST status 0x%x\n", __func__, regval);
		return -ETIMEDOUT;
	}

	return 0;
}

int crypto_qti_init_crypto(const struct ice_mmio_data *mmio_data)
{
	int err = 0;
	void __iomem *ice_mmio;

	if (!mmio_data || !mmio_data->ice_base_mmio) {
		pr_err("%s: Invalid mmio_data\n", __func__);
		return -EINVAL;
	}
	ice_mmio = mmio_data->ice_base_mmio;

	err = ice_check_version(ice_mmio);
	if (err) {
		pr_err("%s: check version failed, err %d\n", __func__, err);
		return err;
	}

	err = ice_check_fuse_setting(ice_mmio);
	if (err) {
		pr_err("%s: check fuse failed, err %d\n", __func__, err);
		return err;
	}

	ice_low_power_and_optimization_enable(ice_mmio);

	err = ice_wait_bist_status(ice_mmio);
	if (err) {
		pr_err("%s: BIST status failed, err %d\n", __func__, err);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL(crypto_qti_init_crypto);

int crypto_qti_enable(const struct ice_mmio_data *mmio_data)
{
	if (!mmio_data || !mmio_data->ice_base_mmio)
		return -EINVAL;

	ice_low_power_and_optimization_enable(mmio_data->ice_base_mmio);
	return ice_wait_bist_status(mmio_data->ice_base_mmio);
}
EXPORT_SYMBOL(crypto_qti_enable);

void crypto_qti_disable(void)
{
	crypto_qti_disable_platform();
}
EXPORT_SYMBOL(crypto_qti_disable);

int crypto_qti_resume(const struct ice_mmio_data *mmio_data)
{
	if (!mmio_data || !mmio_data->ice_base_mmio)
		return -EINVAL;

	return ice_wait_bist_status(mmio_data->ice_base_mmio);
}
EXPORT_SYMBOL(crypto_qti_resume);

int crypto_qti_debug(const struct ice_mmio_data *mmio_data)
{
	return 0;
}
EXPORT_SYMBOL(crypto_qti_debug);

int crypto_qti_keyslot_program(const struct ice_mmio_data *mmio_data,
			       const struct blk_crypto_key *key,
			       unsigned int slot,
			       u8 data_unit_mask, int capid)
{
	int err1 = 0, err2 = 0;

	if (!mmio_data) {
		pr_err("%s: mmio_data is invalid\n", __func__);
		return -EINVAL;
	}

	err1 = crypto_qti_program_key(mmio_data, key, slot,
				      data_unit_mask, capid);
	if (err1) {
		pr_err("%s: program key failed with error %d\n",
			__func__, err1);
		err2 = crypto_qti_invalidate_key(mmio_data, slot);
		if (err2) {
			pr_err("%s: invalidate key failed with error %d\n",
				__func__, err2);
		}
	}

	return err1;
}
EXPORT_SYMBOL(crypto_qti_keyslot_program);

int crypto_qti_keyslot_evict(const struct ice_mmio_data *mmio_data,
			     unsigned int slot)
{
	int err = 0;

	if (!mmio_data) {
		pr_err("%s: mmio_data is invalid\n", __func__);
		return -EINVAL;
	}

	err = crypto_qti_invalidate_key(mmio_data, slot);
	if (err) {
		pr_err("%s: invalidate key failed with error %d\n",
			__func__, err);
		return err;
	}

	return err;
}
EXPORT_SYMBOL(crypto_qti_keyslot_evict);

int crypto_qti_derive_raw_secret(const u8 *wrapped_key,
				 unsigned int wrapped_key_size, u8 *secret,
				 unsigned int secret_size)
{
	int err = 0;

	if (wrapped_key_size <= RAW_SECRET_SIZE) {
		pr_err("%s: Invalid wrapped_key_size: %u\n",
				__func__, wrapped_key_size);
		return -EINVAL;
	}
	if (secret_size != RAW_SECRET_SIZE) {
		pr_err("%s: Invalid secret size: %u\n", __func__, secret_size);
		return -EINVAL;
	}

	err = crypto_qti_derive_raw_secret_platform(wrapped_key,
						    wrapped_key_size,
						    secret, secret_size);
	return err;
}
EXPORT_SYMBOL(crypto_qti_derive_raw_secret);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Common crypto library for storage encryption");
