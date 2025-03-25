/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRC_T10DIF_H
#define _LINUX_CRC_T10DIF_H

#include <linux/types.h>

#define CRC_T10DIF_DIGEST_SIZE 2
#define CRC_T10DIF_BLOCK_SIZE 1

u16 crc_t10dif_arch(u16 crc, const u8 *p, size_t len);
u16 crc_t10dif_generic(u16 crc, const u8 *p, size_t len);

static inline u16 crc_t10dif_update(u16 crc, const u8 *p, size_t len)
{
	if (IS_ENABLED(CONFIG_CRC_T10DIF_ARCH))
		return crc_t10dif_arch(crc, p, len);
	return crc_t10dif_generic(crc, p, len);
}

static inline u16 crc_t10dif(const u8 *p, size_t len)
{
	return crc_t10dif_update(0, p, len);
}

#if IS_ENABLED(CONFIG_CRC_T10DIF_ARCH)
bool crc_t10dif_is_optimized(void);
#else
static inline bool crc_t10dif_is_optimized(void)
{
	return false;
}
#endif

#endif
