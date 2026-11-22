/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020-2021, Linux Foundation. All rights reserved.
 */

#ifndef _UFSHCD_CRYPTO_QTI_H
#define _UFSHCD_CRYPTO_QTI_H

#include "ufshcd.h"

#if IS_ENABLED(CONFIG_SCSI_UFS_CRYPTO_QTI)
int ufshcd_qti_hba_init_crypto_capabilities(struct ufs_hba *hba);
static inline void ufshcd_crypto_qti_set_vops(struct ufs_hba *hba) {}
#else
static inline int ufshcd_qti_hba_init_crypto_capabilities(struct ufs_hba *hba)
{
	return 0;
}
static inline void ufshcd_crypto_qti_set_vops(struct ufs_hba *hba) {}
#endif

#endif /* _UFSHCD_CRYPTO_QTI_H */
