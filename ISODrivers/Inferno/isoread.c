#include <pspkernel.h>
#include <pspreg.h>
#include <stdio.h>
#include <string.h>
#include <systemctrl.h>
#include <systemctrl_se.h>
#include <pspsysmem_kernel.h>
#include <psprtc.h>
#include <psputilsforkernel.h>
#include <pspthreadman_kernel.h>
#include "utils.h"
#include "printk.h"
#include "libs.h"
#include "utils.h"
#include "systemctrl.h"
#include "systemctrl_se.h"
#include "systemctrl_private.h"
#include "inferno.h"

extern int sceKernelBootFromGo635(void);
extern int sceKernelBootFromGo620(void);

// 0x00002784
struct IoReadArg g_read_arg;

// 0x00002484
void *g_sector_buf = NULL;

// 0x0000248C
int g_iso_opened = 0;

// 0x000023D0
SceUID g_iso_fd = -1;

// 0x000023D4
int g_total_sectors = -1;

// 0x00002488
static int g_is_ciso = 0;

// 0x000024C0
static void *g_ciso_block_buf = NULL;

// 0x000024C4, size CISO_DEC_BUFFER_SIZE, align 64
static void *g_ciso_dec_buf = NULL;

// 0x00002704
static int g_CISO_cur_idx = 0;

// 0x00002700
static int g_ciso_dec_buf_offset = -1;

// 0x00002720
static u32 g_ciso_total_block = 0;

struct CISO_header {
	u8 magic[4];  // 0
	u32 header_size;  // 4
	u64 total_bytes; // 8
	u32 block_size; // 16
	u8 ver; // 20
	u8 align;  // 21
	u8 rsv_06[2];  // 22
};

// 0x00002708
static struct CISO_header g_CISO_hdr;

// 0x00002500
static u32 g_CISO_idx_cache[CISO_IDX_BUFFER_SIZE/4] __attribute__((aligned(64)));

static int sceKernelBootFromGo(void)
{
	if(psp_fw_version == FW_635)
		return sceKernelBootFromGo635();
	else if(psp_fw_version == FW_620)
		return sceKernelBootFromGo620();

	return -1;
}

// 0x00000368
static void wait_until_ms0_ready(void)
{
	int ret, status = 0, bootfrom;
	const char *drvname;

	drvname = "mscmhc0:";

	if(psp_model == PSP_GO) {
		bootfrom = sceKernelBootFromGo();
		printk("%s: bootfrom: 0x%08X\n", __func__, bootfrom);

		if(bootfrom == 0x50) {
			drvname = "mscmhcemu0:";
		}
	}

	while( 1 ) {
		ret = sceIoDevctl(drvname, 0x02025801, 0, 0, &status, sizeof(status));

		if(ret < 0) {
			sceKernelDelayThread(20000);
			continue;
		}

		if(status == 4) {
			break;
		}

		sceKernelDelayThread(20000);
	}
}

// 0x00000EE4
static int ciso_get_nsector(SceUID fd)
{
//	return g_CISO_hdr.total_bytes / g_CISO_hdr.block_size;;
	return g_ciso_total_block;
}

// 0x00000E58
static int iso_get_nsector(SceUID fd)
{
	SceOff off, total;

	off = sceIoLseek(fd, 0, PSP_SEEK_CUR);
	total = sceIoLseek(fd, 0, PSP_SEEK_END);
	sceIoLseek(fd, off, PSP_SEEK_SET);

	return total / ISO_SECTOR_SIZE;
}

// 0x00000E58
static int get_nsector(void)
{
	if(g_is_ciso) {
		return ciso_get_nsector(g_iso_fd);
	}

	return iso_get_nsector(g_iso_fd);
}

// 0x00000F00
static int is_ciso(SceUID fd)
{
	int ret;

	g_CISO_hdr.magic[0] = '\0';
	g_ciso_dec_buf_offset = 0x7FFFFFFF;

	sceIoLseek(fd, 0, PSP_SEEK_SET);
	ret = sceIoRead(fd, &g_CISO_hdr, sizeof(g_CISO_hdr));

	if(ret != sizeof(g_CISO_hdr)) {
		ret = -1;
		printk("%s: -> %d\n", __func__, ret);
		goto exit;
	}

	if(*(u32*)g_CISO_hdr.magic == 0x4F534943) { // CISO
		g_CISO_cur_idx = -1;
		g_ciso_total_block = g_CISO_hdr.total_bytes / g_CISO_hdr.block_size;
		printk("%s: total block %d\n", __func__, g_ciso_total_block);

		if(g_ciso_dec_buf == NULL) {
			g_ciso_dec_buf = oe_malloc(CISO_DEC_BUFFER_SIZE + 64);

			if(g_ciso_dec_buf == NULL) {
				ret = -2;
				printk("%s: -> %d\n", __func__, ret);
				goto exit;
			}

			if((u32)g_ciso_dec_buf & 63)
				g_ciso_dec_buf = (void*)(((u32)g_ciso_dec_buf & (~63)) + 64);
		}

		if(g_ciso_block_buf == NULL) {
			g_ciso_block_buf = oe_malloc(ISO_SECTOR_SIZE);

			if(g_ciso_block_buf == NULL) {
				ret = -3;
				printk("%s: -> %d\n", __func__, ret);
				goto exit;
			}
		}

		ret = 0;
	} else {
		ret = 0x8002012F;
	}

exit:
	return ret;
}

// 0x000009D4
int iso_open(void)
{
	int ret;

	wait_until_ms0_ready();
	sceIoClose(g_iso_fd);
	g_iso_opened = 0;

	g_iso_fd = sceIoOpen(g_iso_fn, 0x000F0001, 0777);

	if(g_iso_fd < 0) {
		return -1;
	}

	g_is_ciso = 0;
	ret = is_ciso(g_iso_fd);

	if(ret >= 0) {
		g_is_ciso = 1;
	}

	g_iso_opened = 1;
	g_total_sectors = get_nsector();

	return 0;
}

// 0x00000BB4
static int read_raw_data(u8* addr, u32 size, int offset)
{
	int ret;
	int i;

	i = 0;

	do {
		i++;
		ret = sceIoLseek32(g_iso_fd, offset, PSP_SEEK_SET);

		if(ret >= 0) {
			i = 0;
			break;
		} else {
			iso_open();
		}
	} while(i < 16);

	if(i == 16) {
		ret = 0x80010013;
		goto exit;
	}

	for(i=0; i<16; ++i) {
		ret = sceIoRead(g_iso_fd, addr, size);

		if(ret >= 0) {
			i = 0;
			break;
		} else {
			iso_open();
		}
	}

	if(i == 16) {
		ret = 0x80010013;
		goto exit;
	}

exit:
	return ret;
}

// 0x00001018
static int read_cso_sector(u8 *addr, int sector)
{
	int ret;
	int n_sector;
	int offset, next_offset;
	int size;

	n_sector = sector - g_CISO_cur_idx;

	// not within sector idx cache?
	if(g_CISO_cur_idx == -1 || n_sector < 0 || n_sector >= NELEMS(g_CISO_idx_cache)) {
		ret = read_raw_data((u8*)g_CISO_idx_cache, sizeof(g_CISO_idx_cache), (sector << 2) + sizeof(struct CISO_header));

		if(ret < 0) {
			ret = -4;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		g_CISO_cur_idx = sector;
		n_sector = 0;
	}

	offset = (g_CISO_idx_cache[n_sector] & 0x7FFFFFFF) << g_CISO_hdr.align;

	// is plain?
	if(g_CISO_idx_cache[n_sector] & 0x80000000) {
		return read_raw_data(addr, ISO_SECTOR_SIZE, offset);
	}

	sector++;
	n_sector = sector - g_CISO_cur_idx;

	if(g_CISO_cur_idx == -1 || n_sector < 0 || n_sector >= NELEMS(g_CISO_idx_cache)) {
		ret = read_raw_data((u8*)g_CISO_idx_cache, sizeof(g_CISO_idx_cache), (sector << 2) + sizeof(struct CISO_header));

		if(ret < 0) {
			ret = -5;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		g_CISO_cur_idx = sector;
		n_sector = 0;
	}

	next_offset = (g_CISO_idx_cache[n_sector] & 0x7FFFFFFF) << g_CISO_hdr.align;
	size = next_offset - offset;
	
	if(size <= ISO_SECTOR_SIZE)
		size = ISO_SECTOR_SIZE;

	if(offset < g_ciso_dec_buf_offset || size + offset >= g_ciso_dec_buf_offset + CISO_DEC_BUFFER_SIZE) {
		ret = read_raw_data(g_ciso_dec_buf, CISO_DEC_BUFFER_SIZE, offset);

		/* May not reach CISO_DEC_BUFFER_SIZE */	
		if(ret < 0) {
			g_ciso_dec_buf_offset = 0xFFF00000;
			ret = -6;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		g_ciso_dec_buf_offset = offset;
	}

	ret = sceKernelDeflateDecompress(addr, ISO_SECTOR_SIZE, g_ciso_dec_buf + offset - g_ciso_dec_buf_offset, 0);

	return ret < 0 ? ret : ISO_SECTOR_SIZE;
}

// 0x00001220
static int read_cso_data(u8* addr, u32 size, int offset)
{
	u32 cur_block = offset / ISO_SECTOR_SIZE;
	int ret;
	int read_bytes;
	int pos = offset & 0x7FF;

	if(pos) {
		ret = read_cso_sector(g_ciso_block_buf, cur_block);

		if(ret != ISO_SECTOR_SIZE) {
			ret = -7;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		read_bytes = MIN(size, (ISO_SECTOR_SIZE - pos));
		memcpy(addr, g_ciso_block_buf + pos, read_bytes);
		size -= read_bytes;
		addr += read_bytes;
		cur_block++;
	} else {
		read_bytes = 0;
	}

	// more than 1 block left
	if(size / ISO_SECTOR_SIZE > 0) {
		int i;
		int block_cnt = size / ISO_SECTOR_SIZE;

		for(i=0; i<block_cnt; ++i) {
			ret = read_cso_sector(addr, cur_block);

			if(ret != ISO_SECTOR_SIZE) {
				ret = -8;
				printk("%s: -> %d\n", __func__, ret);

				return ret;
			}

			cur_block++;
			addr += ISO_SECTOR_SIZE;
			read_bytes += ISO_SECTOR_SIZE;
			size -= ISO_SECTOR_SIZE;
		}
	}

	if(size != 0) {
		ret = read_cso_sector(g_ciso_block_buf, cur_block);

		if(ret != ISO_SECTOR_SIZE) {
			ret = -9;
			printk("%s: -> %d\n", __func__, ret);

			return ret;
		}

		memcpy(addr, g_ciso_block_buf, size);
		read_bytes += size;
	}

	return read_bytes;
}

// 0x00000C7C
int iso_read(struct IoReadArg *args)
{
	if(g_is_ciso != 0) {
		return read_cso_data(args->address, args->size, args->offset);
	}

	return read_raw_data(args->address, args->size, args->offset);
}

// 0x000003E0
int iso_read_with_stack(u32 offset, void *ptr, u32 data_len)
{
	int ret, retv;

	ret = sceKernelWaitSema(g_umd9660_sema_id, 1, 0);

	if(ret < 0) {
		return -1;
	}

	g_read_arg.offset = offset;
	g_read_arg.address = ptr;
	g_read_arg.size = data_len;
	retv = sceKernelExtendKernelStack(0x2000, (void*)&iso_read, &g_read_arg);

	ret = sceKernelSignalSema(g_umd9660_sema_id, 1);

	if(ret < 0) {
		return -1;
	}

	return retv;
}