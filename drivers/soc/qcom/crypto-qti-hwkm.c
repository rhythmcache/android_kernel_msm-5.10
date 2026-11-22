// SPDX-License-Identifier: GPL-2.0-only
/*
 * Crypto HWKM library for storage encryption.
 *
 * Copyright (c) 2020-2021, Linux Foundation. All rights reserved.
 */

#include <linux/slab.h>
#include <linux/crypto-qti-common.h>
#include <linux/hwkm.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>

#include "crypto-qti-ice-regs.h"
#include "crypto-qti-platform.h"

#define TPKEY_SLOT_ICEMEM_SLAVE		0x92
#define KEYMANAGER_ICE_MAP_SLOT(slot)	((slot * 2) + 10)
#define GP_KEYSLOT			140
#define RAW_SECRET_KEYSLOT		141

#define SLOT_EMPTY_ERROR		0x1000
#define INLINECRYPT_CTX			"inline encryption key"
#define RAW_SECRET_CTX			"raw secret"
#define BYTE_ORDER_VAL			8
#define KEY_WRAPPED_SIZE		68

static bool qti_hwkm_init_done;

union crypto_cfg {
	__le32 regval[2];
	struct {
		u8 dusize;
		u8 capidx;
		u8 nop;
		u8 cfge;
		u8 dumb[4];
	};
};

static int crypto_qti_hwkm_evict_slot(unsigned int slot, bool double_key)
{
	struct hwkm_cmd cmd_clear;
	struct hwkm_rsp rsp_clear;

	memset(&cmd_clear, 0, sizeof(cmd_clear));
	cmd_clear.op = KEY_SLOT_CLEAR;
	cmd_clear.clear.dks = slot;
	if (double_key)
		cmd_clear.clear.is_double_key = true;
	return qti_hwkm_handle_cmd(&cmd_clear, &rsp_clear);
}

int crypto_qti_program_key(const struct ice_mmio_data *mmio_data,
			   const struct blk_crypto_key *key, unsigned int slot,
			   unsigned int data_unit_mask, int capid)
{
	int err_program = 0;
	int err_clear = 0;
	struct hwkm_cmd cmd_unwrap;
	struct hwkm_cmd cmd_kdf;
	struct hwkm_rsp rsp_unwrap;
	struct hwkm_rsp rsp_kdf;

	struct hwkm_key_policy policy_kdf = {
		.security_lvl = MANAGED_KEY,
		.hw_destination = ICEMEM_SLAVE,
		.key_type = GENERIC_KEY,
		.enc_allowed = true,
		.dec_allowed = true,
		.alg_allowed = AES256_XTS,
		.km_by_nsec_allowed = true,
	};
	struct hwkm_bsve bsve_kdf = {
		.enabled = true,
		.km_swc_en = true,
		.km_child_key_policy_en = true,
	};
	union crypto_cfg cfg;

	if ((key->size) <= RAW_SECRET_SIZE) {
		pr_err("%s: Incorrect key size %d\n", __func__, key->size);
		return -EINVAL;
	}

	pr_err("crypto_qti_hwkm: program_key slot %d: enabling clocks\n", slot);
	mdelay(50);
	err_program = qti_hwkm_clocks(true);
	if (err_program) {
		pr_err("%s: Error enabling clocks %d\n", __func__, err_program);
		return err_program;
	}

	if (!qti_hwkm_init_done) {
		pr_err("crypto_qti_hwkm: initializing HWKM slave at %pK\n",
			mmio_data ? mmio_data->ice_hwkm_mmio : NULL);
		mdelay(50);
		err_program = qti_hwkm_init(mmio_data ? mmio_data->ice_hwkm_mmio : NULL);
		if (err_program) {
			pr_err("%s: Error with HWKM init %d\n", __func__, err_program);
			qti_hwkm_clocks(false);
			return -EINVAL;
		}
		qti_hwkm_init_done = true;
		pr_err("crypto_qti_hwkm: HWKM slave init success\n");
		mdelay(50);
	}

	// Failsafe, clear GP_KEYSLOT incase it is not empty for any reason
	err_clear = crypto_qti_hwkm_evict_slot(GP_KEYSLOT, false);
	if (err_clear && (err_clear != SLOT_EMPTY_ERROR)) {
		pr_err("%s: Error clearing ICE slot %d, err %d\n",
			__func__, GP_KEYSLOT, err_clear);
		qti_hwkm_clocks(false);
		return -EINVAL;
	}

	/* Unwrap keyblob into a non ICE slot using TP key */
	cmd_unwrap.op = KEY_UNWRAP_IMPORT;
	cmd_unwrap.unwrap.dks = GP_KEYSLOT;
	cmd_unwrap.unwrap.kwk = TPKEY_SLOT_ICEMEM_SLAVE;
	if ((key->size) == KEY_WRAPPED_SIZE) {
		cmd_unwrap.unwrap.sz = key->size;
		memcpy(cmd_unwrap.unwrap.wkb, key->raw, cmd_unwrap.unwrap.sz);
	} else {
		cmd_unwrap.unwrap.sz = (key->size) - RAW_SECRET_SIZE;
		memcpy(cmd_unwrap.unwrap.wkb, (key->raw) + RAW_SECRET_SIZE, cmd_unwrap.unwrap.sz);
	}

	pr_err("crypto_qti_hwkm: unwrapping key into GP_KEYSLOT %d (size=%d)\n",
		GP_KEYSLOT, cmd_unwrap.unwrap.sz);
	mdelay(50);
	err_program = qti_hwkm_handle_cmd(&cmd_unwrap, &rsp_unwrap);
	if (err_program) {
		pr_err("%s: Error with key unwrap %d\n", __func__, err_program);
		qti_hwkm_clocks(false);
		return -EINVAL;
	}

	// Failsafe, clear ICE keyslot incase it is not empty for any reason
	err_clear = crypto_qti_hwkm_evict_slot(KEYMANAGER_ICE_MAP_SLOT(slot), true);
	if (err_clear && (err_clear != SLOT_EMPTY_ERROR)) {
		pr_err("%s: Error clearing ICE slot %d, err %d\n",
			__func__, KEYMANAGER_ICE_MAP_SLOT(slot), err_clear);
		qti_hwkm_clocks(false);
		return -EINVAL;
	}

	/* Derive a 512-bit key which will be the key to encrypt/decrypt data */
	cmd_kdf.op = SYSTEM_KDF;
	cmd_kdf.kdf.dks = KEYMANAGER_ICE_MAP_SLOT(slot);
	cmd_kdf.kdf.kdk = GP_KEYSLOT;
	cmd_kdf.kdf.policy = policy_kdf;
	cmd_kdf.kdf.bsve = bsve_kdf;
	cmd_kdf.kdf.sz = round_up(strlen(INLINECRYPT_CTX), BYTE_ORDER_VAL);
	memset(cmd_kdf.kdf.ctx, 0, HWKM_MAX_CTX_SIZE);
	memcpy(cmd_kdf.kdf.ctx, INLINECRYPT_CTX, strlen(INLINECRYPT_CTX));

	memset(&cfg, 0, sizeof(cfg));
	cfg.dusize = data_unit_mask;
	cfg.capidx = capid;
	cfg.cfge = 0x80;

	if (mmio_data && mmio_data->ice_base_mmio) {
		ice_writel(mmio_data->ice_base_mmio, 0x0,
			(ICE_LUT_KEYS_CRYPTOCFG_R_16 + ICE_LUT_KEYS_CRYPTOCFG_OFFSET*slot));
	}
	/* Make sure CFGE is cleared */
	wmb();

	pr_err("crypto_qti_hwkm: deriving key into ICE slot %d\n", KEYMANAGER_ICE_MAP_SLOT(slot));
	mdelay(50);
	err_program = qti_hwkm_handle_cmd(&cmd_kdf, &rsp_kdf);
	if (err_program) {
		pr_err("%s: Error with key derive %d\n", __func__, err_program);
		qti_hwkm_clocks(false);
		return -EINVAL;
	}

	if (mmio_data && mmio_data->ice_base_mmio) {
		ice_writel(mmio_data->ice_base_mmio, cfg.regval[0],
			(ICE_LUT_KEYS_CRYPTOCFG_R_16 + ICE_LUT_KEYS_CRYPTOCFG_OFFSET*slot));
	}
	/* Make sure CFGE is enabled before moving forward */
	wmb();

	pr_err("crypto_qti_hwkm: programmed ICE slot %d successfully\n", slot);
	mdelay(50);

	// Clear GP_KEYSLOT once key has been derived into ICE slot
	err_clear = crypto_qti_hwkm_evict_slot(GP_KEYSLOT, false);
	if (err_clear) {
		pr_err("%s: Error clearing ICE slot %d, err %d\n",
			__func__, GP_KEYSLOT, err_clear);
		qti_hwkm_clocks(false);
		return -EINVAL;
	}

	qti_hwkm_clocks(false);
	return 0;
}
EXPORT_SYMBOL(crypto_qti_program_key);

int crypto_qti_invalidate_key(const struct ice_mmio_data *mmio_data,
			      unsigned int slot)
{
	int err_clear = 0;

	if (!qti_hwkm_init_done)
		return 0;

	pr_err("crypto_qti_hwkm: invalidate_key slot %d\n", slot);
	mdelay(50);
	qti_hwkm_clocks(true);
	err_clear = crypto_qti_hwkm_evict_slot(KEYMANAGER_ICE_MAP_SLOT(slot), true);
	if (err_clear && (err_clear != SLOT_EMPTY_ERROR)) {
		pr_err("%s: Error clearing ICE slot %d, err %d\n",
			__func__, KEYMANAGER_ICE_MAP_SLOT(slot), err_clear);
		qti_hwkm_clocks(false);
		return -EINVAL;
	}

	if (mmio_data && mmio_data->ice_base_mmio) {
		ice_writel(mmio_data->ice_base_mmio, 0x0,
			(ICE_LUT_KEYS_CRYPTOCFG_R_16 + ICE_LUT_KEYS_CRYPTOCFG_OFFSET*slot));
	}
	/* Make sure CFGE is cleared before moving forward */
	wmb();

	qti_hwkm_clocks(false);
	return 0;
}
EXPORT_SYMBOL(crypto_qti_invalidate_key);

void crypto_qti_disable_platform(void)
{
	qti_hwkm_init_done = false;
}
EXPORT_SYMBOL(crypto_qti_disable_platform);

int crypto_qti_derive_raw_secret_platform(
				const u8 *wrapped_key,
				unsigned int wrapped_key_size, u8 *secret,
				unsigned int secret_size)
{
	pr_err("crypto_qti_hwkm: derive_raw_secret_platform start: wrapped_sz=%u, secret_sz=%u\n",
		wrapped_key_size, secret_size);
	mdelay(50);
	memcpy(secret, wrapped_key, secret_size);
	pr_err("crypto_qti_hwkm: derive_raw_secret_platform SUCCESS (hlos copy)\n");
	mdelay(50);
	return 0;
}
EXPORT_SYMBOL(crypto_qti_derive_raw_secret_platform);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Crypto HWKM library for storage encryption");
