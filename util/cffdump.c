/*
 * Copyright (c) 2012 Rob Clark <robdclark@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>

#include "redump.h"
#include "disasm.h"
#include "rnn.h"
#include "rnndec.h"

/* ************************************************************************* */
/* originally based on kernel recovery dump code: */
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "a2xx.xml.h"
#include "a3xx.xml.h"
#include "a4xx.xml.h"

typedef enum {
	true = 1, false = 0,
} bool;

static bool needs_wfi = false;
static bool dump_shaders = false;
static bool no_color = false;
static bool summary = false;
static bool allregs = false;
static int vertices;
static unsigned gpu_id = 220;

static const char *levels[] = {
		"\t",
		"\t\t",
		"\t\t\t",
		"\t\t\t\t",
		"\t\t\t\t\t",
		"\t\t\t\t\t\t",
		"\t\t\t\t\t\t\t",
		"\t\t\t\t\t\t\t\t",
		"\t\t\t\t\t\t\t\t\t",
		"x",
		"x",
		"x",
		"x",
		"x",
		"x",
};

#define NAME(x)	[x] = #x

static const char *fmt_name[] = {
		NAME(FMT_1_REVERSE),
		NAME(FMT_1),
		NAME(FMT_8),
		NAME(FMT_1_5_5_5),
		NAME(FMT_5_6_5),
		NAME(FMT_6_5_5),
		NAME(FMT_8_8_8_8),
		NAME(FMT_2_10_10_10),
		NAME(FMT_8_A),
		NAME(FMT_8_B),
		NAME(FMT_8_8),
		NAME(FMT_Cr_Y1_Cb_Y0),
		NAME(FMT_Y1_Cr_Y0_Cb),
		NAME(FMT_5_5_5_1),
		NAME(FMT_8_8_8_8_A),
		NAME(FMT_4_4_4_4),
		NAME(FMT_10_11_11),
		NAME(FMT_11_11_10),
		NAME(FMT_DXT1),
		NAME(FMT_DXT2_3),
		NAME(FMT_DXT4_5),
		NAME(FMT_24_8),
		NAME(FMT_24_8_FLOAT),
		NAME(FMT_16),
		NAME(FMT_16_16),
		NAME(FMT_16_16_16_16),
		NAME(FMT_16_EXPAND),
		NAME(FMT_16_16_EXPAND),
		NAME(FMT_16_16_16_16_EXPAND),
		NAME(FMT_16_FLOAT),
		NAME(FMT_16_16_FLOAT),
		NAME(FMT_16_16_16_16_FLOAT),
		NAME(FMT_32),
		NAME(FMT_32_32),
		NAME(FMT_32_32_32_32),
		NAME(FMT_32_FLOAT),
		NAME(FMT_32_32_FLOAT),
		NAME(FMT_32_32_32_32_FLOAT),
		NAME(FMT_32_AS_8),
		NAME(FMT_32_AS_8_8),
		NAME(FMT_16_MPEG),
		NAME(FMT_16_16_MPEG),
		NAME(FMT_8_INTERLACED),
		NAME(FMT_32_AS_8_INTERLACED),
		NAME(FMT_32_AS_8_8_INTERLACED),
		NAME(FMT_16_INTERLACED),
		NAME(FMT_16_MPEG_INTERLACED),
		NAME(FMT_16_16_MPEG_INTERLACED),
		NAME(FMT_DXN),
		NAME(FMT_8_8_8_8_AS_16_16_16_16),
		NAME(FMT_DXT1_AS_16_16_16_16),
		NAME(FMT_DXT2_3_AS_16_16_16_16),
		NAME(FMT_DXT4_5_AS_16_16_16_16),
		NAME(FMT_2_10_10_10_AS_16_16_16_16),
		NAME(FMT_10_11_11_AS_16_16_16_16),
		NAME(FMT_11_11_10_AS_16_16_16_16),
		NAME(FMT_32_32_32_FLOAT),
		NAME(FMT_DXT3A),
		NAME(FMT_DXT5A),
		NAME(FMT_CTX1),
		NAME(FMT_DXT3A_AS_1_1_1_1),
};

static void dump_commands(uint32_t *dwords, uint32_t sizedwords, int level);
static void dump_register_val(uint32_t regbase, uint32_t dword, int level);

struct buffer {
	void *hostptr;
	unsigned int gpuaddr, len;
};

static struct buffer buffers[512];
static int nbuffers;

static int buffer_contains_gpuaddr(struct buffer *buf, uint32_t gpuaddr, uint32_t len)
{
	return (buf->gpuaddr <= gpuaddr) && (gpuaddr < (buf->gpuaddr + buf->len));
}

static int buffer_contains_hostptr(struct buffer *buf, void *hostptr)
{
	return (buf->hostptr <= hostptr) && (hostptr < (buf->hostptr + buf->len));
}

#define GET_PM4_TYPE3_OPCODE(x) ((*(x) >> 8) & 0xFF)
#define GET_PM4_TYPE0_REGIDX(x) ((*(x)) & 0x7FFF)

static uint32_t gpuaddr(void *hostptr)
{
	int i;
	for (i = 0; i < nbuffers; i++)
		if (buffer_contains_hostptr(&buffers[i], hostptr))
			return buffers[i].gpuaddr + (hostptr - buffers[i].hostptr);
	return 0;
}

static void *hostptr(uint32_t gpuaddr)
{
	int i;
	if (!gpuaddr)
		return 0;
	for (i = 0; i < nbuffers; i++)
		if (buffer_contains_gpuaddr(&buffers[i], gpuaddr, 0))
			return buffers[i].hostptr + (gpuaddr - buffers[i].gpuaddr);
	return 0;
}

static unsigned hostlen(uint32_t gpuaddr)
{
	int i;
	if (!gpuaddr)
		return 0;
	for (i = 0; i < nbuffers; i++)
		if (buffer_contains_gpuaddr(&buffers[i], gpuaddr, 0))
			return buffers[i].len + buffers[i].gpuaddr - gpuaddr;
	return 0;
}

static void dump_hex(uint32_t *dwords, uint32_t sizedwords, int level)
{
	int i;
	for (i = 0; i < sizedwords; i++) {
		if ((i % 8) == 0)
			printf("%08x:%s", gpuaddr(dwords), levels[level]);
		else
			printf(" ");
		printf("%08x", *(dwords++));
		if ((i % 8) == 7)
			printf("\n");
	}
	if (i % 8)
		printf("\n");
}

static void dump_float(float *dwords, uint32_t sizedwords, int level)
{
	int i;
	for (i = 0; i < sizedwords; i++) {
		if ((i % 8) == 0)
			printf("%08x:%s", gpuaddr(dwords), levels[level]);
		else
			printf(" ");
		printf("%8f", *(dwords++));
		if ((i % 8) == 7)
			printf("\n");
	}
	if (i % 8)
		printf("\n");
}

/* I believe the surface format is low bits:
#define RB_COLOR_INFO__COLOR_FORMAT_MASK                   0x0000000fL
comments in sys2gmem_tex_const indicate that address is [31:12], but
looks like at least some of the bits above the format have different meaning..
*/
static void parse_dword_addr(uint32_t dword, uint32_t *gpuaddr,
		uint32_t *flags, uint32_t mask)
{
	*gpuaddr = dword & ~mask;
	*flags   = dword & mask;
}


#define INVALID_RB_CMD 0xaaaaaaaa

/* CP timestamp register */
#define	REG_CP_TIMESTAMP		 REG_SCRATCH_REG0


static uint32_t type0_reg_vals[0x7fff];
static uint8_t type0_reg_written[sizeof(type0_reg_vals)/8];


static struct {
	uint32_t config;
	uint32_t address;
	uint32_t length;
} vsc_pipe_data[7];

static void reg_vsc_pipe_config(const char *name, uint32_t dword, int level)
{
	int idx;
	sscanf(name, "VSC_PIPE_CONFIG_%x", &idx) ||
		sscanf(name, "VSC_PIPE[0x%x].CONFIG", &idx) ||
		sscanf(name, "VSC_PIPE[%d].CONFIG", &idx);
	vsc_pipe_data[idx].config = dword;
}

static void reg_vsc_pipe_data_address(const char *name, uint32_t dword, int level)
{
	int idx;
	sscanf(name, "VSC_PIPE_DATA_ADDRESS_%x", &idx) ||
		sscanf(name, "VSC_PIPE[0x%x].DATA_ADDRESS", &idx) ||
		sscanf(name, "VSC_PIPE[%d].DATA_ADDRESS", &idx);
	vsc_pipe_data[idx].address = dword;
}

static void reg_vsc_pipe_data_length(const char *name, uint32_t dword, int level)
{
	int idx;
	void *buf;

	sscanf(name, "VSC_PIPE_DATA_LENGTH_%x", &idx) ||
		sscanf(name, "VSC_PIPE[0x%x].DATA_LENGTH", &idx) ||
		sscanf(name, "VSC_PIPE[%d].DATA_LENGTH", &idx);

	vsc_pipe_data[idx].length = dword;

	if (summary)
		return;

	/* as this is the last register in the triplet written, we dump
	 * the pipe data here..
	 */
	buf = hostptr(vsc_pipe_data[idx].address);
	if (buf) {
		/* not sure how much of this is useful: */
		dump_hex(buf, min(vsc_pipe_data[idx].length/4, 16), level+1);
	}
}

/*
 * A3xx registers:
 */

typedef struct {
	uint32_t fetchsize  : 7;
	uint32_t bufstride  : 10;
	uint32_t switchnext : 1;
	uint32_t indexcode  : 6;
	uint32_t steprate   : 8;
} vfd_fetch_state_t;
static vfd_fetch_state_t vfd_fetch_state[0xf];

static void reg_vfd_fetch_instr_0_x(const char *name, uint32_t dword, int level)
{
	int idx;

	/* this is a bit ugly way, but oh well.. */
	sscanf(name, "VFD_FETCH_INSTR_0_%x", &idx) ||
		sscanf(name, "VFD_FETCH[0x%x].INSTR_0", &idx) ||
		sscanf(name, "VFD_FETCH[%d].INSTR_0", &idx);

	vfd_fetch_state[idx] = *(vfd_fetch_state_t *)&dword;
}

static void reg_vfd_fetch_instr_1_x(const char *name, uint32_t dword, int level)
{
	int idx;
	void *buf;

	/* this is a bit ugly way, but oh well.. */
	sscanf(name, "VFD_FETCH_INSTR_1_%x", &idx) ||
		sscanf(name, "VFD_FETCH[0x%x].INSTR_1", &idx) ||
		sscanf(name, "VFD_FETCH[%d].INSTR_1", &idx);

	if (summary)
		return;

	buf = hostptr(dword);

	if (buf) {
		// XXX we probably need to know min/max vtx to know the
		// right values to dump..
		uint32_t sizedwords = vfd_fetch_state[idx].fetchsize + 1;
		dump_float(buf, sizedwords, level+1);
		dump_hex(buf, sizedwords, level+1);
	}
}

static void reg_dump_scratch(const char *name, uint32_t dword, int level)
{
	unsigned regbase;
	printf("%s:", levels[level]);
	for (regbase = REG_AXXX_CP_SCRATCH_REG0;
			regbase <= REG_AXXX_CP_SCRATCH_REG7;
			regbase++) {
		printf(" %08x", type0_reg_vals[regbase]);
	}
	printf("\n");
}

static void reg_dump_gpuaddr(const char *name, uint32_t dword, int level)
{
	void *buf;

	if (summary)
		return;

	buf = hostptr(dword);
	if (buf) {
		uint32_t sizedwords = 64;
		dump_hex(buf, sizedwords, level+1);
	}
}

static void reg_disasm_gpuaddr(const char *name, uint32_t dword, int level)
{
	void *buf;

	dword &= 0xfffffff0;

	if (summary)
		return;

	buf = hostptr(dword);
	if (buf) {
		uint32_t sizedwords = hostlen(dword) / 4;
		dump_hex(buf, 64, level+1);
		disasm_a3xx(buf, sizedwords, level+2, SHADER_FRAGMENT);
	}
}

// HACK:
#define REG_A2XX_VSC_PIPE_CONFIG(i0)        (0x00000c06 + 0x3*(i0))
#define REG_A2XX_VSC_PIPE_DATA_ADDRESS(i0)  (0x00000c07 + 0x3*(i0))
#define REG_A2XX_VSC_PIPE_DATA_LENGTH(i0)   (0x00000c08 + 0x3*(i0))
#define REG_A3XX_VSC_PIPE_CONFIG(i0)        (0x00000c06 + 0x3*(i0))
#define REG_A3XX_VSC_PIPE_DATA_ADDRESS(i0)  (0x00000c07 + 0x3*(i0))
#define REG_A3XX_VSC_PIPE_DATA_LENGTH(i0)   (0x00000c08 + 0x3*(i0))

/*
 * Registers with special handling (rnndec_decode() handles rest):
 */
static const const struct {
	void (*fxn)(const char *name, uint32_t dword, int level);
} reg_axxx[0x7fff] = {
#define REG(x, fxn) [REG_AXXX_ ## x] = { fxn }
		REG(CP_SCRATCH_REG0, reg_dump_scratch),
		REG(CP_SCRATCH_REG1, reg_dump_scratch),
		REG(CP_SCRATCH_REG2, reg_dump_scratch),
		REG(CP_SCRATCH_REG3, reg_dump_scratch),
		REG(CP_SCRATCH_REG4, reg_dump_scratch),
		REG(CP_SCRATCH_REG5, reg_dump_scratch),
		REG(CP_SCRATCH_REG6, reg_dump_scratch),
		REG(CP_SCRATCH_REG7, reg_dump_scratch),
#undef REG
}, reg_a2xx[0x7fff] = {
#define REG(x, fxn) [REG_A2XX_ ## x] = { fxn }
		REG(VSC_PIPE_CONFIG(0), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(0), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(0), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(1), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(1), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(1), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(2), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(2), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(2), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(3), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(3), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(3), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(4), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(4), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(4), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(5), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(5), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(5), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(6), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(6), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(6), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(7), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(7), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(7), reg_vsc_pipe_data_length),
#undef REG
}, reg_a3xx[0x7fff] = {
#define REG(x, fxn) [REG_A3XX_ ## x] = { fxn }
		REG(VSC_SIZE_ADDRESS, reg_dump_gpuaddr),
		REG(VSC_PIPE_CONFIG(0), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(0), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(0), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(1), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(1), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(1), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(2), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(2), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(2), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(3), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(3), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(3), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(4), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(4), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(4), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(5), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(5), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(5), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(6), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(6), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(6), reg_vsc_pipe_data_length),
		REG(VSC_PIPE_CONFIG(7), reg_vsc_pipe_config),
		REG(VSC_PIPE_DATA_ADDRESS(7), reg_vsc_pipe_data_address),
		REG(VSC_PIPE_DATA_LENGTH(7), reg_vsc_pipe_data_length),
#if 0
		REG(VFD_FETCH_INSTR_0(0), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(0), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(1), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(1), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(2), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(2), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(3), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(3), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(4), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(4), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(5), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(5), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(6), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(6), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(7), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(7), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(8), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(8), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(9), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(9), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(10), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(10), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(11), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(11), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(12), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(12), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(13), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(13), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(14), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(14), reg_vfd_fetch_instr_1_x),
		REG(VFD_FETCH_INSTR_0(15), reg_vfd_fetch_instr_0_x),
		REG(VFD_FETCH_INSTR_1(15), reg_vfd_fetch_instr_1_x),
#endif
		REG(SP_VS_PVT_MEM_ADDR_REG, reg_dump_gpuaddr),
		REG(SP_FS_PVT_MEM_ADDR_REG, reg_dump_gpuaddr),
		REG(SP_VS_OBJ_START_REG, reg_disasm_gpuaddr),
		REG(SP_FS_OBJ_START_REG, reg_disasm_gpuaddr),
		REG(TPL1_TP_FS_BORDER_COLOR_BASE_ADDR, reg_dump_gpuaddr),
#undef REG
}, reg_a4xx[0x7fff] = {
#define REG(x, fxn) [REG_A4XX_ ## x] = { fxn }
		REG(SP_VS_PVT_MEM_ADDR, reg_dump_gpuaddr),
		REG(SP_FS_PVT_MEM_ADDR, reg_dump_gpuaddr),
		REG(SP_VS_OBJ_START, reg_disasm_gpuaddr),
		REG(SP_FS_OBJ_START, reg_disasm_gpuaddr),
#undef REG
}, *type0_reg;

static struct rnndeccontext *vc;
static struct rnndeccontext *vc_nocolor;
static struct rnndb *db;
struct rnndomain *dom[2];
static bool initialized = false;

static void init_rnn(char *file, char *domain)
{
	/* prepare rnn stuff for lookup */
	rnn_parsefile(db, file);
	rnn_prepdb(db);
	dom[0] = rnn_finddomain(db, domain);
	if (!strcmp(domain, "A4XX")) {
		/* I think even the common registers move around in A4XX.. */
		dom[1] = dom[0];
	} else {
		dom[1] = rnn_finddomain(db, "AXXX");
	}
	if (!dom[0] && dom[1]) {
		fprintf(stderr, "Could not find domain %s in %s\n", domain, file);
		exit(1);
	}
	initialized = true;
}

static void init_a2xx(void)
{
	type0_reg = reg_a2xx;
	init_rnn("adreno/a2xx.xml", "A2XX");
}

static void init_a3xx(void)
{
	type0_reg = reg_a3xx;
	init_rnn("adreno/a3xx.xml", "A3XX");
}

static void init_a4xx(void)
{
	type0_reg = reg_a4xx;
	init_rnn("adreno/a4xx.xml", "A4XX");
}

static void init(void)
{
	if (!initialized) {
		/* default to a2xx so we can still parse older rd files prior to RD_GPU_ID */
		init_a2xx();
	}
}

static struct rnndomain *finddom(uint32_t regbase)
{
	if (rnndec_checkaddr(vc, dom[0], regbase, 0))
		return dom[0];
	return dom[1];
}

static const char *regname(uint32_t regbase, int color)
{
	static char buf[1024];
	struct rnndecaddrinfo *info;

	init();

	info = rnndec_decodeaddr(color ? vc : vc_nocolor, finddom(regbase), regbase, 0);
	if (info) {
		strncpy(buf, info->name, sizeof(buf));
		free(info->name);
		free(info);
		return buf;
	}
	return NULL;
}

static void dump_register_val(uint32_t regbase, uint32_t dword, int level)
{
	struct rnndecaddrinfo *info =
			rnndec_decodeaddr(vc, finddom(regbase), regbase, 0);

	if (info && info->typeinfo) {
		char *decoded = rnndec_decodeval(vc, info->typeinfo, dword, info->width);
		printf("%s%s: %s\n", levels[level], info->name, decoded);
		free(decoded);
	} else if (info) {
		printf("%s%s: %08x\n", levels[level], info->name, dword);

	} else {
		printf("%s<%04x>: %08x\n", levels[level], regbase, dword);
	}

	if (info) {
		free(info->name);
		free(info);
	}
}

static void dump_register(uint32_t regbase, uint32_t dword, int level)
{
	init();

	if (!summary) {
		dump_register_val(regbase, dword, level);
	}

	if (type0_reg[regbase].fxn) {
		type0_reg[regbase].fxn(regname(regbase, 0), dword, level);
	} else if (reg_axxx[regbase].fxn) {
		reg_axxx[regbase].fxn(regname(regbase, 0), dword, level);
	}
}

static bool is_banked_reg(uint32_t regbase)
{
	return (0x2000 <= regbase) && (regbase < 0x2400);
}

static void dump_registers(uint32_t regbase,
		uint32_t *dwords, uint32_t sizedwords, int level)
{
	while (sizedwords--) {
		int last_summary = summary;

		/* access to non-banked registers needs a WFI:
		 * TODO banked register range for a2xx??
		 */
		if (needs_wfi && !is_banked_reg(regbase))
			printf("NEEDS WFI: %s (%x)\n", regname(regbase, 1), regbase);

		type0_reg_vals[regbase] = *dwords;
		type0_reg_written[regbase/8] |= (1 << (regbase % 8));
		dump_register(regbase, *dwords, level);
		regbase++;
		dwords++;
		summary = last_summary;
	}
}

static void dump_domain(uint32_t *dwords, uint32_t sizedwords, int level,
		const char *name)
{
	struct rnndomain *dom;
	int i;

	init();

	dom = rnn_finddomain(db, name);

	if (!dom)
		return;

	for (i = 0; i < sizedwords; i++) {
		struct rnndecaddrinfo *info = rnndec_decodeaddr(vc, dom, i, 0);
		char *decoded;
		if (!(info && info->typeinfo))
			break;
		decoded = rnndec_decodeval(vc, info->typeinfo, dwords[i], info->width);
		printf("%s%s\n", levels[level], decoded);
		free(decoded);
		free(info->name);
		free(info);
	}
}

static void cp_im_loadi(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t start = dwords[1] >> 16;
	uint32_t size  = dwords[1] & 0xffff;
	const char *type = NULL, *ext = NULL;
	enum shader_t disasm_type;

	switch (dwords[0]) {
	case 0:
		type = "vertex";
		ext = "vo";
		disasm_type = SHADER_VERTEX;
		break;
	case 1:
		type = "fragment";
		ext = "fo";
		disasm_type = SHADER_FRAGMENT;
		break;
	default:
		type = "<unknown>"; break;
	}

	printf("%s%s shader, start=%04x, size=%04x\n", levels[level], type, start, size);
	disasm_a2xx(dwords + 2, sizedwords - 2, level+2, disasm_type);

	/* dump raw shader: */
	if (ext && dump_shaders) {
		static int n = 0;
		char filename[8];
		int fd;
		sprintf(filename, "%04d.%s", n++, ext);
		fd = open(filename, O_WRONLY| O_TRUNC | O_CREAT, 0644);
		write(fd, dwords + 2, (sizedwords - 2) * 4);
	}
}

static void cp_load_state(uint32_t *dwords, uint32_t sizedwords, int level)
{
	enum adreno_state_block state_block_id = (dwords[0] >> 19) & 0x7;
	enum adreno_state_type state_type = dwords[1] & 0x3;
	uint32_t num_unit = (dwords[0] >> 22) & 0x1ff;
	uint32_t ext_src_addr = dwords[1] & 0xfffffffc;
	void *contents = NULL;
	int i;

	/* we could either have a ptr to other gpu buffer, or directly have
	 * contents inline:
	 */
	if (ext_src_addr)
		contents = hostptr(ext_src_addr);
	else
		contents = dwords + 2;

	if (!contents)
		return;

	switch (state_block_id) {
	case SB_FRAG_SHADER:
	case SB_VERT_SHADER:
		if (state_type == ST_SHADER) {
			enum shader_t disasm_type;
			const char *ext = NULL;

			/* shaders:
			 *
			 * note: num_unit seems to be # of instruction groups, where
			 * an instruction group has 4 64bit instructions.
			 */
			if (state_block_id == SB_VERT_SHADER) {
				ext = "vo3";
				disasm_type = SHADER_VERTEX;
			} else {
				ext = "fo3";
				disasm_type = SHADER_FRAGMENT;
			}

			if (contents)
				disasm_a3xx(contents, num_unit * 4 * 2, level+2, disasm_type);

			/* dump raw shader: */
			if (ext && dump_shaders) {
				static int n = 0;
				char filename[8];
				int fd;
				sprintf(filename, "%04d.%s", n++, ext);
				fd = open(filename, O_WRONLY| O_TRUNC | O_CREAT, 0644);
				write(fd, dwords + 2, (sizedwords - 2) * 4);
			}
		} else {
			/* uniforms/consts:
			 *
			 * note: num_unit seems to be # of pairs of dwords??
			 */
			dump_float(contents, num_unit*2, level+1);
			dump_hex(contents, num_unit*2, level+1);
		}
		break;
	case SB_VERT_MIPADDR:
	case SB_FRAG_MIPADDR:
		if (state_type == ST_CONSTANTS) {
			uint32_t *addrs = contents;

			/* mipmap consts block just appears to be array of num_unit gpu addr's: */
			for (i = 0; i < num_unit; i++) {
				void *ptr = hostptr(addrs[i]);
				printf("%s%2d: %08x\n", levels[level+1], i, addrs[i]);
				if (ptr)
					dump_hex(ptr, 16, level+1);
			}
		} else {
			goto unknown;
		}
		break;
	case SB_FRAG_TEX:
	case SB_VERT_TEX:
		if (state_type == ST_SHADER) {
			for (i = 0; i < num_unit; i++) {
				uint32_t *texsamp = &((uint32_t *)contents)[2 * i];

				/* work-around to reduce noise for opencl blob which always
				 * writes the max # regardless of # of textures used
				 */
				if ((num_unit == 16) && (texsamp[0] == 0) && (texsamp[1] == 0))
					break;

				if ((300 <= gpu_id) && (gpu_id < 400))
					dump_domain(texsamp, 2, level+2, "A3XX_TEX_SAMP");
				else if ((400 <= gpu_id) && (gpu_id < 500))
					dump_domain(texsamp, 2, level+2, "A4XX_TEX_SAMP");
				dump_hex(texsamp, 2, level+1);
			}
		} else {
			for (i = 0; i < num_unit; i++) {
				uint32_t *texconst = &((uint32_t *)contents)[4 * i];

				/* work-around to reduce noise for opencl blob which always
				 * writes the max # regardless of # of textures used
				 */
				if ((num_unit == 16) &&
					(texconst[0] == 0) && (texconst[1] == 0) &&
					(texconst[2] == 0) && (texconst[3] == 0))
					break;

				if ((300 <= gpu_id) && (gpu_id < 400))
					dump_domain(texconst, 4, level+2, "A3XX_TEX_CONST");
				else if ((400 <= gpu_id) && (gpu_id < 500))
					dump_domain(texconst, 4, level+2, "A4XX_TEX_CONST");
				dump_hex(texconst, 4, level+1);
			}
		}
		break;
	default:
unknown:
		/* hmm.. */
		dump_hex(contents, num_unit, level+1);
		break;
	}

}

static void dump_tex_const(uint32_t *dwords, uint32_t sizedwords, uint32_t val, int level)
{
	uint32_t w, h, p;
	uint32_t gpuaddr, flags, mip_gpuaddr, mip_flags;
	uint32_t min, mag, swiz, clamp_x, clamp_y, clamp_z;
	static const char *filter[] = {
			"point", "bilinear", "bicubic",
	};
	static const char *clamp[] = {
			"wrap", "mirror", "clamp-last-texel",
	};
	static const char swiznames[] = "xyzw01??";

	/* see sys2gmem_tex_const[] in adreno_a2xxx.c */

	/* Texture, FormatXYZW=Unsigned, ClampXYZ=Wrap/Repeat,
	 * RFMode=ZeroClamp-1, Dim=1:2d, pitch
	 */
	p = (dwords[0] >> 22) << 5;
	clamp_x = (dwords[0] >> 10) & 0x3;
	clamp_y = (dwords[0] >> 13) & 0x3;
	clamp_z = (dwords[0] >> 16) & 0x3;

	/* Format=6:8888_WZYX, EndianSwap=0:None, ReqSize=0:256bit, DimHi=0,
	 * NearestClamp=1:OGL Mode
	 */
	parse_dword_addr(dwords[1], &gpuaddr, &flags, 0xfff);

	/* Width, Height, EndianSwap=0:None */
	w = (dwords[2] & 0x1fff) + 1;
	h = ((dwords[2] >> 13) & 0x1fff) + 1;

	/* NumFormat=0:RF, DstSelXYZW=XYZW, ExpAdj=0, MagFilt=MinFilt=0:Point,
	 * Mip=2:BaseMap
	 */
	mag = (dwords[3] >> 19) & 0x3;
	min = (dwords[3] >> 21) & 0x3;
	swiz = (dwords[3] >> 1) & 0xfff;

	/* VolMag=VolMin=0:Point, MinMipLvl=0, MaxMipLvl=1, LodBiasH=V=0,
	 * Dim3d=0
	 */
	// XXX

	/* BorderColor=0:ABGRBlack, ForceBC=0:diable, TriJuice=0, Aniso=0,
	 * Dim=1:2d, MipPacking=0
	 */
	parse_dword_addr(dwords[5], &mip_gpuaddr, &mip_flags, 0xfff);

	printf("%sset texture const %04x\n", levels[level], val);
	printf("%sclamp x/y/z: %s/%s/%s\n", levels[level+1],
			clamp[clamp_x], clamp[clamp_y], clamp[clamp_z]);
	printf("%sfilter min/mag: %s/%s\n", levels[level+1], filter[min], filter[mag]);
	printf("%sswizzle: %c%c%c%c\n", levels[level+1],
			swiznames[(swiz >> 0) & 0x7], swiznames[(swiz >> 3) & 0x7],
			swiznames[(swiz >> 6) & 0x7], swiznames[(swiz >> 9) & 0x7]);
	printf("%saddr=%08x (flags=%03x), size=%dx%d, pitch=%d, format=%s\n",
			levels[level+1], gpuaddr, flags, w, h, p,
			fmt_name[flags & 0xf]);
	printf("%smipaddr=%08x (flags=%03x)\n", levels[level+1],
			mip_gpuaddr, mip_flags);
}

static void dump_shader_const(uint32_t *dwords, uint32_t sizedwords, uint32_t val, int level)
{
	int i;
	printf("%sset shader const %04x\n", levels[level], val);
	for (i = 0; i < sizedwords; ) {
		uint32_t gpuaddr, flags;
		parse_dword_addr(dwords[i++], &gpuaddr, &flags, 0xf);
		void *addr = hostptr(gpuaddr);
		if (addr) {
			uint32_t size = dwords[i++];
			printf("%saddr=%08x, size=%d, format=%s\n", levels[level+1],
					gpuaddr, size, fmt_name[flags & 0xf]);
			// TODO maybe dump these as bytes instead of dwords?
			size = (size + 3) / 4; // for now convert to dwords
			dump_hex(addr, min(size, 64), level + 1);
			if (size > min(size, 64))
				printf("%s\t\t...\n", levels[level+1]);
			dump_float(addr, min(size, 64), level + 1);
			if (size > min(size, 64))
				printf("%s\t\t...\n", levels[level+1]);
		}
	}
}

static void cp_set_const(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t val = dwords[0] & 0xffff;
	switch((dwords[0] >> 16) & 0xf) {
	case 0x0:
		dump_float((float *)(dwords+1), sizedwords-1, level+1);
		break;
	case 0x1:
		/* need to figure out how const space is partitioned between
		 * attributes, textures, etc..
		 */
		if (val < 0x78) {
			dump_tex_const(dwords+1, sizedwords-1, val, level);
		} else {
			dump_shader_const(dwords+1, sizedwords-1, val, level);
		}
		break;
	case 0x2:
		printf("%sset bool const %04x\n", levels[level], val);
		break;
	case 0x3:
		printf("%sset loop const %04x\n", levels[level], val);
		break;
	case 0x4:
		val += 0x2000;
		if (dwords[0] & 0x80000000) {
			uint32_t srcreg = dwords[1];
			uint32_t dstval = dwords[2];
			/* TODO: not sure what happens w/ payload != 2.. */
			assert(sizedwords == 3);
			assert(srcreg < ARRAY_SIZE(type0_reg_vals));

			dstval += type0_reg_vals[srcreg];

			dump_registers(val, &dstval, 1, level+1);
		} else {
			dump_registers(val, dwords+1, sizedwords-1, level+1);
		}
		break;
	}
}

static void cp_event_write(uint32_t *dwords, uint32_t sizedwords, int level)
{
	printf("%sevent %s\n", levels[level],
			rnndec_decode_enum(vc, "vgt_event_type", dwords[0]));
}

static void dump_register_summary(int level)
{
	static uint32_t lastvals[ARRAY_SIZE(type0_reg_vals)];
	uint32_t i;

	/* dump current state of registers: */
	printf("%scurrent register values\n", levels[level]);
	for (i = 0; i < 0x7fff; i++) {
		uint32_t regbase = i;
		uint32_t lastval = type0_reg_vals[regbase];
		/* skip registers that have zero: */
		if (!lastval && !allregs)
			continue;
		if (!(type0_reg_written[regbase/8] & (1 << (regbase % 8))))
			continue;
		if (lastval != lastvals[regbase]) {
			printf("!");
			lastvals[regbase] = lastval;
		}
		dump_register(regbase, lastval, level+1);
	}
}

static uint32_t draw_indx_common(uint32_t *dwords, int level)
{
	uint32_t prim_type     = dwords[1] & 0x1f;
	uint32_t source_select = (dwords[1] >> 6) & 0x3;
	uint32_t num_indices   = dwords[2];

	printf("%sprim_type:     %s (%d)\n", levels[level],
			rnndec_decode_enum(vc, "pc_di_primtype", prim_type),
			prim_type);
	printf("%ssource_select: %s (%d)\n", levels[level],
			rnndec_decode_enum(vc, "pc_di_src_sel", source_select),
			source_select);
	printf("%snum_indices:   %d\n", levels[level], num_indices);

	vertices += num_indices;

	return num_indices;
}
static void cp_draw_indx(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t num_indices = draw_indx_common(dwords, level);
	bool saved_summary = summary;

	summary = false;

	/* if we have an index buffer, dump that: */
	if (sizedwords == 5) {
		void *ptr = hostptr(dwords[3]);
		printf("%sgpuaddr:       %08x\n", levels[level], dwords[3]);
		printf("%sidx_size:      %d\n", levels[level], dwords[4]);
		if (ptr) {
			enum pc_di_index_size size =
					((dwords[1] >> 11) & 1) | ((dwords[1] >> 12) & 2);
			int i;
			printf("%sidxs:         ", levels[level]);
			if (size == INDEX_SIZE_8_BIT) {
				uint8_t *idx = ptr;
				for (i = 0; i < dwords[4]; i++)
					printf(" %u", idx[i]);
			} else if (size == INDEX_SIZE_16_BIT) {
				uint16_t *idx = ptr;
				for (i = 0; i < dwords[4]/2; i++)
					printf(" %u", idx[i]);
			} else if (size == INDEX_SIZE_32_BIT) {
				uint32_t *idx = ptr;
				for (i = 0; i < dwords[4]/4; i++)
					printf(" %u", idx[i]);
			}
			printf("\n");
			dump_hex(ptr, dwords[4]/4, level+1);
		}
	}

	/* don't bother dumping registers for the dummy draw_indx's.. */
	if (num_indices > 0)
		dump_register_summary(level);

	summary = saved_summary;

	needs_wfi = true;
}

static void cp_draw_indx_2(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t num_indices = draw_indx_common(dwords, level);
	enum pc_di_index_size size =
			((dwords[1] >> 11) & 1) | ((dwords[1] >> 12) & 2);
	void *ptr = &dwords[3];
	int i, sz = 0;
	bool saved_summary = summary;

	summary = false;

	/* CP_DRAW_INDX_2 has embedded/inline idx buffer: */
	printf("%sidxs:         ", levels[level]);
	if (size == INDEX_SIZE_8_BIT) {
		uint8_t *idx = ptr;
		for (i = 0; i < num_indices; i++)
			printf(" %u", idx[i]);
		sz = num_indices;
	} else if (size == INDEX_SIZE_16_BIT) {
		uint16_t *idx = ptr;
		for (i = 0; i < num_indices; i++)
			printf(" %u", idx[i]);
		sz = num_indices * 2;
	} else if (size == INDEX_SIZE_32_BIT) {
		uint32_t *idx = ptr;
		for (i = 0; i < num_indices; i++)
			printf(" %u", idx[i]);
		sz = num_indices * 4;
	}
	printf("\n");
	dump_hex(ptr, sz / 4, level+1);

	/* don't bother dumping registers for the dummy draw_indx's.. */
	if (num_indices > 0)
		dump_register_summary(level);

	summary = saved_summary;
}

static void cp_draw_indx_offset(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t num_indices = dwords[2];
	bool saved_summary = summary;

	summary = false;

	/* don't bother dumping registers for the dummy draw_indx's.. */
	if (num_indices > 0)
		dump_register_summary(level);

	summary = saved_summary;
}

static void cp_run_cl(uint32_t *dwords, uint32_t sizedwords, int level)
{
	bool saved_summary = summary;

	summary = false;

	dump_register_summary(level);

	summary = saved_summary;
}

static void cp_indirect(uint32_t *dwords, uint32_t sizedwords, int level)
{
	/* traverse indirect buffers */
	int i;
	uint32_t ibaddr = dwords[0];
	uint32_t ibsize = dwords[1];
	uint32_t *ptr = NULL;

	if (!summary) {
		printf("%sibaddr:%08x\n", levels[level], ibaddr);
		printf("%sibsize:%08x\n", levels[level], ibsize);
	} else {
		level--;
	}

	/* map gpuaddr back to hostptr: */
	for (i = 0; i < nbuffers; i++) {
		if (buffer_contains_gpuaddr(&buffers[i], ibaddr, ibsize)) {
			ptr = buffers[i].hostptr + (ibaddr - buffers[i].gpuaddr);
			break;
		}
	}

	if (ptr) {
		dump_commands(ptr, ibsize, level);
	} else {
		fprintf(stderr, "could not find: %08x (%d)\n", ibaddr, ibsize);
	}
}

static void cp_wfi(uint32_t *dwords, uint32_t sizedwords, int level)
{
	needs_wfi = false;
}

static void cp_mem_write(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t gpuaddr = dwords[0];
	printf("%sgpuaddr:%08x\n", levels[level], gpuaddr);
	dump_float((float *)&dwords[1], sizedwords-1, level+1);
}

static void cp_rmw(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t val = dwords[0] & 0xffff;
	uint32_t and = dwords[1];
	uint32_t or  = dwords[2];
	if (!summary)
		printf("%srmw (%s & 0x%08x) | 0x%08x)\n", levels[level], regname(val, 1), and, or);
	if (needs_wfi)
		printf("NEEDS WFI: rmw (%s & 0x%08x) | 0x%08x)\n", regname(val, 1), and, or);
	type0_reg_vals[val] = (type0_reg_vals[val] & and) | or;
	type0_reg_written[val/8] |= (1 << (val % 8));
}

static void cp_set_draw_state(uint32_t *dwords, uint32_t sizedwords, int level)
{
	uint32_t count = dwords[0] & 0xffff;
	uint32_t addr = dwords[1];
	uint32_t *ptr = hostptr(addr);

	if (ptr) {
		uint32_t i;

		dump_hex(ptr, count, level+1);

		for (i = 0; i < count; ) {
			uint32_t regbase = ptr[i] & 0xffff;
			uint32_t count2 = (ptr[i] >> 16) + 1;
			dump_registers(regbase, &ptr[i+1], count2, level+1);
			i += count2 + 1;
		}
	}
}


#define CP(x, fxn)   [CP_ ## x] = { fxn }
static const struct {
	void (*fxn)(uint32_t *dwords, uint32_t sizedwords, int level);
} type3_op[0xff] = {
		CP(ME_INIT, NULL),
		CP(NOP, NULL),
		CP(INDIRECT_BUFFER, cp_indirect),
		CP(INDIRECT_BUFFER_PFD, cp_indirect),
		CP(WAIT_FOR_IDLE, cp_wfi),
		CP(WAIT_REG_MEM, NULL),
		CP(WAIT_REG_EQ, NULL),
		CP(WAIT_REG_GTE, NULL),
		CP(WAIT_UNTIL_READ, NULL),
		CP(WAIT_IB_PFD_COMPLETE, NULL),
		CP(REG_RMW, cp_rmw),
		CP(REG_TO_MEM, NULL),
		CP(MEM_WRITE, cp_mem_write),
		CP(MEM_WRITE_CNTR, NULL),
		CP(COND_EXEC, NULL),
		CP(COND_WRITE, NULL),
		CP(EVENT_WRITE, cp_event_write),
		CP(EVENT_WRITE_SHD, NULL),
		CP(EVENT_WRITE_CFL, NULL),
		CP(EVENT_WRITE_ZPD, NULL),
		CP(RUN_OPENCL, cp_run_cl),
		CP(DRAW_INDX, cp_draw_indx),
		CP(DRAW_INDX_2, cp_draw_indx_2),
		CP(DRAW_INDX_BIN, NULL),
		CP(DRAW_INDX_2_BIN, NULL),
		CP(VIZ_QUERY, NULL),
		CP(SET_STATE, NULL),
		CP(SET_CONSTANT, cp_set_const),
		CP(IM_LOAD, NULL),
		CP(IM_LOAD_IMMEDIATE, cp_im_loadi),
		CP(LOAD_CONSTANT_CONTEXT, NULL),
		CP(INVALIDATE_STATE, NULL),
		CP(SET_SHADER_BASES, NULL),
		CP(SET_BIN_MASK, NULL),
		CP(SET_BIN_SELECT, NULL),
		CP(CONTEXT_UPDATE, NULL),
		CP(INTERRUPT, NULL),
		CP(IM_STORE, NULL),
		CP(SET_PROTECTED_MODE, NULL),

		/* for a20x */
		//CP(SET_BIN_BASE_OFFSET, NULL),

		/* for a22x */
		CP(SET_DRAW_INIT_FLAGS, NULL),

		/* for a3xx */
		CP(LOAD_STATE, cp_load_state),
		CP(SET_BIN_DATA, NULL),
		CP(SET_BIN, NULL),

		/* for a4xx */
		CP(SET_DRAW_STATE, cp_set_draw_state),
		CP(DRAW_INDX_OFFSET, cp_draw_indx_offset),
};

static void dump_commands(uint32_t *dwords, uint32_t sizedwords, int level)
{
	int dwords_left = sizedwords;
	uint32_t count = 0; /* dword count including packet header */
	uint32_t val;
	const char *name;

	while (dwords_left > 0) {
		int type = dwords[0] >> 30;
		if (!summary)
			printf("t%d", type);
		switch (type) {
		case 0x0: /* type-0 */
			count = (dwords[0] >> 16)+2;
			val = GET_PM4_TYPE0_REGIDX(dwords);
			if (!summary) {
				printf("%swrite %s%s\n", levels[level+1], regname(val, 1),
						(dwords[0] & 0x8000) ? " (same register)" : "");
			}
			dump_registers(val, dwords+1, count-1, level+2);
			if (!summary)
				dump_hex(dwords, count, level+1);
			break;
		case 0x1: /* type-1 */
			count = 3;
			val = dwords[0] & 0xfff;
			if (!summary)
				printf("%swrite %s\n", levels[level+1], regname(val, 1));
			dump_registers(val, dwords+1, 1, level+2);
			val = (dwords[0] >> 12) & 0xfff;
			if (!summary)
				printf("%swrite %s\n", levels[level+1], regname(val, 1));
			dump_registers(val, dwords+2, 1, level+2);
			if (!summary)
				dump_hex(dwords, count, level+1);
			break;
		case 0x2: /* type-2 */
			printf("%sNOP\n", levels[level+1]);
			count = 1;
			if (!summary)
				dump_hex(dwords, count, level+1);
			break;
		case 0x3: /* type-3 */
			count = ((dwords[0] >> 16) & 0x3fff) + 2;
			val = GET_PM4_TYPE3_OPCODE(dwords);
			init();
			name = rnndec_decode_enum(vc, "adreno_pm4_type3_packets", val);
			printf("\t%sopcode: %s%s%s (%02x) (%d dwords)%s\n", levels[level],
					vc->colors->bctarg, name, vc->colors->reset,
					val, count, (dwords[0] & 0x1) ? " (predicated)" : "");
			if (name)
				dump_domain(dwords+1, count-1, level+2, name);
			if (type3_op[val].fxn)
				type3_op[val].fxn(dwords+1, count-1, level+1);
			dump_hex(dwords, count, level+1);
			break;
		default:
			fprintf(stderr, "bad type!\n");
			return;
		}

		dwords += count;
		dwords_left -= count;

		//printf("*** dwords_left=%d, count=%d\n", dwords_left, count);
	}

	if (dwords_left < 0)
		printf("**** this ain't right!! dwords_left=%d\n", dwords_left);
}

ssize_t readn(int fd, void *buf, int nbytes)
{
	char *ptr = buf;
	int ret = 0;
	while (nbytes > 0) {
		int n = read(fd, ptr, nbytes);
		if (n < 0)
			return n;
		if (n == 0)
			break;
		ptr += n;
		nbytes -= n;
		ret += n;
	}
	return ret;
}

static int check_extension(const char *path, const char *ext)
{
	return strcmp(path + strlen(path) - strlen(ext), ext) == 0;
}

int main(int argc, char **argv)
{
	enum rd_sect_type type = RD_NONE;
	void *buf = NULL;
	int fd, sz, i, n = 1;
	int got_gpu_id = 0;
	int start = 0, end = 0x7ffffff, draw = 0;
	const char *filename;

	while (1) {
		if (!strcmp(argv[n], "--verbose")) {
			disasm_set_debug(PRINT_RAW);
			n++;
			continue;
		}

		if (!strcmp(argv[n], "--dump-shaders")) {
			dump_shaders = true;
			n++;
			continue;
		}

		if (!strcmp(argv[n], "--no-color")) {
			no_color = true;
			n++;
			continue;
		}

		if (!strcmp(argv[n], "--summary")) {
			summary = true;
			n++;
			continue;
		}

		if (!strcmp(argv[n], "--allregs")) {
			allregs = true;
			n++;
			continue;
		}

		if (!strcmp(argv[n], "--start")) {
			n++;
			start = atoi(argv[n]);
			n++;
			continue;
		}

		if (!strcmp(argv[n], "--end")) {
			n++;
			end = atoi(argv[n]);
			n++;
			continue;
		}

		break;
	}

	if (argc-n != 1)
		fprintf(stderr, "usage: %s [--dump-shaders] testlog.rd\n", argv[0]);

	filename = argv[n];

	if (!strcmp(filename, "-"))
		fd = 0;
	else
		fd = open(filename, O_RDONLY);
	if (fd < 0)
		fprintf(stderr, "could not open: %s\n", argv[n]);

	rnn_init();
	db = rnn_newdb();
	vc_nocolor = rnndec_newcontext(db);
	vc_nocolor->colors = &envy_null_colors;
	if (no_color) {
		vc = vc_nocolor;
	} else {
		vc = rnndec_newcontext(db);
		vc->colors = &envy_def_colors;
	}

	if (check_extension(filename, ".txt")) {
		/* read in from hexdump.. this could probably be more flexibile,
		 * but right now the format is:
		 *
		 *   "%x(ignored): %x %x %x %x %x %x %x %x
		 *
		 * and buf size is hard coded..  this is just for a quick hack
		 * I needed, if txt input is really useful this should be made
		 * less lame..
		 */
#define SZ 40960
		char *strbuf  = calloc(SZ, 1);
		uint32_t *buf = calloc(SZ, 1);
		uint32_t *bufp = buf;
		uint32_t dummy, sizedwords = 0;
		int n;

		readn(fd, strbuf, SZ);

		do {
			n = sscanf(strbuf, "%x: %x %x %x %x %x %x %x %x", &dummy,
							&bufp[0], &bufp[1], &bufp[2], &bufp[3],
							&bufp[4], &bufp[5], &bufp[6], &bufp[7]);
			if (n <= 0)
				break;

			sizedwords += n - 1;
			bufp += 8;

			/* scan fwd until next newline: */
			while (strbuf[0] != '\n')
				strbuf++;
			strbuf++;

		} while (1);

		init_a3xx();

		printf("############################################################\n");
		printf("cmdstream: %d dwords\n", sizedwords);
		dump_commands(buf, sizedwords, 0);
		printf("############################################################\n");
		printf("vertices: %d\n", vertices);
	}

	while ((readn(fd, &type, sizeof(type)) > 0) && (readn(fd, &sz, 4) > 0)) {
		free(buf);

		needs_wfi = false;

		buf = malloc(sz + 1);
		((char *)buf)[sz] = '\0';
		readn(fd, buf, sz);

		switch(type) {
		case RD_TEST:
			printf("test: %s\n", (char *)buf);
			break;
		case RD_CMD:
			printf("cmd: %s\n", (char *)buf);
			break;
		case RD_VERT_SHADER:
			printf("vertex shader:\n%s\n", (char *)buf);
			break;
		case RD_FRAG_SHADER:
			printf("fragment shader:\n%s\n", (char *)buf);
			break;
		case RD_GPUADDR:
			buffers[nbuffers].gpuaddr = ((uint32_t *)buf)[0];
			buffers[nbuffers].len = ((uint32_t *)buf)[1];
			break;
		case RD_BUFFER_CONTENTS:
			buffers[nbuffers].hostptr = buf;
			nbuffers++;
			assert(nbuffers < ARRAY_SIZE(buffers));
			buf = NULL;
			break;
		case RD_CMDSTREAM_ADDR:
			if ((start <= draw) && (draw <= end)) {
				printf("############################################################\n");
				printf("cmdstream: %d dwords\n", ((uint32_t *)buf)[1]);
				dump_commands(hostptr(((uint32_t *)buf)[0]),
						((uint32_t *)buf)[1], 0);
				printf("############################################################\n");
				printf("vertices: %d\n", vertices);
			}
			draw++;
			for (i = 0; i < nbuffers; i++) {
				free(buffers[i].hostptr);
				buffers[i].hostptr = NULL;
			}
			nbuffers = 0;
			break;
		case RD_GPU_ID:
			if (!got_gpu_id) {
				gpu_id = *((unsigned int *)buf);
				printf("gpu_id: %d\n", gpu_id);
				if (gpu_id >= 400)
					init_a4xx();
				else if (gpu_id >= 300)
					init_a3xx();
				else
					init_a2xx();
				got_gpu_id = 1;
			}
			break;
		default:
			break;
		}
	}

	return 0;
}

