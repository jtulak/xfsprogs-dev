/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write the Free Software Foundation,
 * Inc.,  51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "libxfs.h"
#include <ctype.h>
#ifdef ENABLE_BLKID
#  include <blkid/blkid.h>
#endif /* ENABLE_BLKID */
#include "xfs_multidisk.h"
#include "libxcmd.h"

/*
 * Prototypes for internal functions.
 */
static void conflict(char opt, char *tab[], int oldidx, int newidx);
static void illegal(const char *value, const char *opt);
static __attribute__((noreturn)) void usage (void);
static __attribute__((noreturn)) void reqval(char opt, char *tab[], int idx);
static void respec(char opt, char *tab[], int idx);
static void unknown(char opt, char *s);
static int  ispow2(unsigned int i);

/*
 * The configured block and sector sizes are defined as global variables so
 * that they don't need to be passed to functions that require them.
 */
uint64_t		blocksize;
uint64_t		sectorsize;

#define MAX_OPTS	16
#define MAX_SUBOPTS	16
#define SUBOPT_NEEDS_VAL	(-1LL)
#define MAX_CONFLICTS	8
#define LAST_CONFLICT	(-1)

/*
 * Table for parsing mkfs parameters.
 *
 * !!! NOTE ==================================================================
 *
 * If you are adding a new option, or changing an existing one,
 * please, update also xfstests - test xfs/191-input-validation
 * with expected behaviour (simple fail/pass).
 *
 * !!! END OF NOTE ===========================================================
 *
 * Description of the structure members follows:
 *
 * index MANDATORY
 *   An integer denoting the position of the specific option in opts array,
 *   counting from 0 up to MAX_OPTS.
 *
 * name MANDATORY
 *   Name is a single char, e.g., for '-d file', name is 'd'.
 *
 * subopts MANDATORY
 *   Subopts is a list of strings naming suboptions. In the example above,
 *   it would contain "file". The last entry of this list has to be NULL.
 *
 * subopt_params MANDATORY
 *   This is a list of structs tied with subopts. For each entry in subopts,
 *   a corresponding entry has to be defined:
 *
 * subopt_params struct:
 *   index MANDATORY
 *     This number, starting from zero, denotes which item in subopt_params
 *     it is. The index has to be the same as is the order in subopts list,
 *     so we can access the right item both in subopt_param and subopts.
 *
 *   seen INTERNAL
 *     Do not set this flag when definning a subopt. It is used to remeber that
 *     this subopt was already seen, for example for conflicts detection.
 *
 *   str_seen INTERNAL
 *     Do not set. It is used internally for respecification, when some options
 *     has to be parsed twice - at first as a string, then later as a number.
 *
 *   convert OPTIONAL
 *     A flag signalling whether the user-given value can use suffixes.
 *     If you want to allow the use of user-friendly values like 13k, 42G,
 *     set it to true.
 *
 *   is_power_2 OPTIONAL
 *     An optional flag for subopts where the given value has to be a power
 *     of two.
 *
 *   conflicts MANDATORY
 *     If your subopt is in a conflict with some other option, specify it.
 *     Accepts the .index values of the conflicting subopts and the last
 *     member of this list has to be LAST_CONFLICT.
 *
 *   minval, maxval OPTIONAL
 *     These options are used for automatic range check and they have to be
 *     always used together in pair. If you don't want to limit the max value,
 *     use something like UINT_MAX. If no value is given, then you must either
 *     supply your own validation, or refuse any value in the 'case
 *     X_SOMETHING' block. If you forget to define the min and max value, but
 *     call a standard function for validating user's value, it will cause an
 *     error message notifying you about this issue.
 *
 *     (Said in another way, you can't have minval and maxval both equal
 *     to zero. But if one value is different: minval=0 and maxval=1,
 *     then it is OK.)
 *
 *   flagval MANDATORY
 *     The value used if user specifies the subopt, but no value.
 *     If the subopt accepts some values (-d file=[1|0]), then this
 *     sets what is used with simple specifying the subopt (-d file).
 *     A special SUBOPT_NEEDS_VAL can be used to require a user-given
 *     value in any case.
 *
 *   raw_input INTERNAL
 *     Filled raw string from the user, so we never lose that information e.g.
 *     to print it back in case of an issue.
 *
 *   value OPTIONAL
 *     The actual value used in computations and for creating the filesystem.
 *     It is filled with user input and anything you write here now is
 *     overwritten if user specifies the subopt. But he does not, then whatever
 *     you put there is used as the default. Can be omitted if the default
 *     is 0.
 *     (If the field is a string and not a number, this value is set to
 *     a positive non-zero number on an user input.)
 *
 * !!! NOTE ==================================================================
 *
 * If you are adding a new option, or changing an existing one,
 * please, update also xfstests - test xfs/191-input-validation
 * with expected behaviour (simple fail/pass).
 *
 * !!! END OF NOTE ===========================================================
 */
struct opt_params {
	int		index;
	const char	name;
	const char	*subopts[MAX_SUBOPTS];

	struct subopt_param {
		int		index;
		bool		seen;
		bool		str_seen;
		bool		convert;
		bool		is_power_2;
		int		conflicts[MAX_CONFLICTS];
		uint64_t	minval;
		uint64_t	maxval;
		uint64_t	flagval;
		const char	*raw_input;
		uint64_t	value;
	}		subopt_params[MAX_SUBOPTS];
} opts[MAX_OPTS] = {
#define OPT_B	0
	{
		.index = OPT_B,
		.name = 'b',
		.subopts = {
#define	B_LOG		0
			"log",
#define	B_SIZE		1
			"size",
			NULL
		},
		.subopt_params = {
			{ .index = B_LOG,
			  .conflicts = { B_SIZE,
					 LAST_CONFLICT },
			  .minval = XFS_MIN_BLOCKSIZE_LOG,
			  .maxval = XFS_MAX_BLOCKSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = B_SIZE,
			  .convert = true,
			  .is_power_2 = true,
			  .conflicts = { B_LOG,
					 LAST_CONFLICT },
			  .minval = XFS_MIN_BLOCKSIZE,
			  .maxval = XFS_MAX_BLOCKSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
		},
	},
#define OPT_D	1
	{
		.index = OPT_D,
		.name = 'd',
		.subopts = {
	#define	D_AGCOUNT	0
			"agcount",
	#define	D_FILE		1
			"file",
	#define	D_NAME		2
			"name",
	#define	D_SIZE		3
			"size",
	#define D_SUNIT		4
			"sunit",
	#define D_SWIDTH	5
			"swidth",
	#define D_AGSIZE	6
			"agsize",
	#define D_SU		7
			"su",
	#define D_SW		8
			"sw",
	#define D_SECTLOG	9
			"sectlog",
	#define D_SECTSIZE	10
			"sectsize",
	#define D_NOALIGN	11
			"noalign",
	#define D_RTINHERIT	12
			"rtinherit",
	#define D_PROJINHERIT	13
			"projinherit",
	#define D_EXTSZINHERIT	14
			"extszinherit",
			NULL
		},
		.subopt_params = {
			{ .index = D_AGCOUNT,
			  .conflicts = { D_AGSIZE,
					 LAST_CONFLICT },
			  .minval = 1,
			  .maxval = XFS_MAX_AGNUMBER,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_FILE,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = D_NAME,
			  .conflicts = { LAST_CONFLICT },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SIZE,
			  .conflicts = { LAST_CONFLICT },
			  .convert = true,
			  .minval = XFS_AG_MIN_BYTES,
			  .maxval = LLONG_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SUNIT,
			  .conflicts = { D_NOALIGN,
					 D_SU,
					 D_SW,
					 LAST_CONFLICT },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SWIDTH,
			  .conflicts = { D_NOALIGN,
					 D_SU,
					 D_SW,
					 LAST_CONFLICT },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_AGSIZE,
			  .conflicts = { D_AGCOUNT,
					 LAST_CONFLICT },
			  .convert = true,
			  .minval = XFS_AG_MIN_BYTES,
			  .maxval = XFS_AG_MAX_BYTES,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SU,
			  .conflicts = { D_NOALIGN,
					 D_SUNIT,
					 D_SWIDTH,
					 LAST_CONFLICT },
			  .convert = true,
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SW,
			  .conflicts = { D_NOALIGN,
					 D_SUNIT,
					 D_SWIDTH,
					 LAST_CONFLICT },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SECTLOG,
			  .conflicts = { D_SECTSIZE,
					 LAST_CONFLICT },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SECTSIZE,
			  .conflicts = { D_SECTLOG,
					 LAST_CONFLICT },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_NOALIGN,
			  .conflicts = { D_SU,
					 D_SW,
					 D_SUNIT,
					 D_SWIDTH,
					 LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = D_RTINHERIT,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = D_PROJINHERIT,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_EXTSZINHERIT,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
		},
	},
#define OPT_I	2
	{
		.index = OPT_I,
		.name = 'i',
		.subopts = {
#define	I_ALIGN		0
			"align",
#define	I_LOG		1
			"log",
#define	I_MAXPCT	2
			"maxpct",
#define	I_PERBLOCK	3
			"perblock",
#define	I_SIZE		4
			"size",
#define	I_ATTR		5
			"attr",
#define	I_PROJID32BIT	6
			"projid32bit",
#define I_SPINODES	7
			"sparse",
			NULL
		},
		.subopt_params = {
			{ .index = I_ALIGN,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = I_LOG,
			  .conflicts = { I_PERBLOCK,
					 I_SIZE,
					 LAST_CONFLICT },
			  .minval = XFS_DINODE_MIN_LOG,
			  .maxval = XFS_DINODE_MAX_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_MAXPCT,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 100,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_PERBLOCK,
			  .conflicts = { I_LOG,
					 I_SIZE,
					 LAST_CONFLICT },
			  .is_power_2 = true,
			  .minval = XFS_MIN_INODE_PERBLOCK,
			  .maxval = XFS_MAX_BLOCKSIZE / XFS_DINODE_MIN_SIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_SIZE,
			  .conflicts = { I_PERBLOCK,
					 I_LOG,
					 LAST_CONFLICT },
			  .is_power_2 = true,
			  .minval = XFS_DINODE_MIN_SIZE,
			  .maxval = XFS_DINODE_MAX_SIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_ATTR,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 2,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_PROJID32BIT,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = I_SPINODES,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
		},
	},
#define OPT_L	3
	{
		.index = OPT_L,
		.name = 'l',
		.subopts = {
	#define	L_AGNUM		0
			"agnum",
	#define	L_INTERNAL	1
			"internal",
	#define	L_SIZE		2
			"size",
	#define L_VERSION	3
			"version",
	#define L_SUNIT		4
			"sunit",
	#define L_SU		5
			"su",
	#define L_DEV		6
			"logdev",
	#define	L_SECTLOG	7
			"sectlog",
	#define	L_SECTSIZE	8
			"sectsize",
	#define	L_FILE		9
			"file",
	#define	L_NAME		10
			"name",
	#define	L_LAZYSBCNTR	11
			"lazy-count",
			NULL
		},
		.subopt_params = {
			{ .index = L_AGNUM,
			  .conflicts = { L_DEV,
					 LAST_CONFLICT },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_INTERNAL,
			  .conflicts = { L_FILE,
					 L_DEV,
					 LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = L_SIZE,
			  .conflicts = { LAST_CONFLICT },
			  .convert = true,
			  .minval = 2 * 1024 * 1024LL,	/* XXX: XFS_MIN_LOG_BYTES */
			  .maxval = XFS_MAX_LOG_BYTES,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_VERSION,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 1,
			  .maxval = 2,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SUNIT,
			  .conflicts = { L_SU,
					 LAST_CONFLICT },
			  .minval = 1,
			  .maxval = BTOBB(XLOG_MAX_RECORD_BSIZE),
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SU,
			  .conflicts = { L_SUNIT,
					 LAST_CONFLICT },
			  .convert = true,
			  .minval = BBTOB(1),
			  .maxval = XLOG_MAX_RECORD_BSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_DEV,
			  .conflicts = { L_AGNUM,
					 L_INTERNAL,
					 LAST_CONFLICT },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SECTLOG,
			  .conflicts = { L_SECTSIZE,
					 LAST_CONFLICT },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SECTSIZE,
			  .conflicts = { L_SECTLOG,
					 LAST_CONFLICT },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_FILE,
			  .conflicts = { L_INTERNAL,
					 LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = L_NAME,
			  .conflicts = { L_AGNUM,
					 L_INTERNAL,
					 LAST_CONFLICT },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_LAZYSBCNTR,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
		},
	},
#define OPT_N	4
	{
		.index = OPT_N,
		.name = 'n',
		.subopts = {
	#define	N_LOG		0
			"log",
	#define	N_SIZE		1
			"size",
	#define	N_VERSION	2
			"version",
	#define	N_FTYPE		3
			"ftype",
		NULL,
		},
		.subopt_params = {
			{ .index = N_LOG,
			  .conflicts = { N_SIZE,
					 LAST_CONFLICT },
			  .minval = XFS_MIN_REC_DIRSIZE,
			  .maxval = XFS_MAX_BLOCKSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = N_SIZE,
			  .conflicts = { N_LOG,
					 LAST_CONFLICT },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = 1 << XFS_MIN_REC_DIRSIZE,
			  .maxval = XFS_MAX_BLOCKSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = N_VERSION,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 2,
			  .maxval = 2,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = N_FTYPE,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
		},
	},
#define OPT_R	5
	{
		.index = OPT_R,
		.name = 'r',
		.subopts = {
	#define	R_EXTSIZE	0
			"extsize",
	#define	R_SIZE		1
			"size",
	#define	R_DEV		2
			"rtdev",
	#define	R_FILE		3
			"file",
	#define	R_NAME		4
			"name",
	#define R_NOALIGN	5
			"noalign",
			NULL
		},
		.subopt_params = {
			{ .index = R_EXTSIZE,
			  .conflicts = { LAST_CONFLICT },
			  .convert = true,
			  .minval = XFS_MIN_RTEXTSIZE,
			  .maxval = XFS_MAX_RTEXTSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_SIZE,
			  .conflicts = { LAST_CONFLICT },
			  .convert = true,
			  .minval = 0,
			  .maxval = LLONG_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_DEV,
			  .conflicts = { LAST_CONFLICT },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_FILE,
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			  .conflicts = { LAST_CONFLICT },
			},
			{ .index = R_NAME,
			  .conflicts = { LAST_CONFLICT },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_NOALIGN,
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			  .conflicts = { LAST_CONFLICT },
			},
		},
	},
#define OPT_S	6
	{
		.index = OPT_S,
		.name = 's',
		.subopts = {
	#define	S_LOG		0
			"log",
	#define	S_SECTLOG	1
			"sectlog",
	#define	S_SIZE		2
			"size",
	#define	S_SECTSIZE	3
			"sectsize",
			NULL
		},
		.subopt_params = {
			{ .index = S_LOG,
			  .conflicts = { S_SIZE,
					 S_SECTSIZE,
					 LAST_CONFLICT },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = S_SECTLOG,
			  .conflicts = { S_SIZE,
					 S_SECTSIZE,
					 LAST_CONFLICT },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = S_SIZE,
			  .conflicts = { S_LOG,
					 S_SECTLOG,
					 LAST_CONFLICT },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = S_SECTSIZE,
			  .conflicts = { S_LOG,
					 S_SECTLOG,
					 LAST_CONFLICT },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
		},
	},
#define OPT_M	7
	{
		.index = OPT_M,
		.name = 'm',
		.subopts = {
	#define	M_CRC		0
			"crc",
	#define M_FINOBT	1
			"finobt",
	#define M_UUID		2
			"uuid",
	#define M_RMAPBT	3
			"rmapbt",
	#define M_REFLINK	4
			"reflink",
			NULL
		},
		.subopt_params = {
			{ .index = M_CRC,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = M_FINOBT,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = M_UUID,
			  .conflicts = { LAST_CONFLICT },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = M_RMAPBT,
			.conflicts = { LAST_CONFLICT },
			.minval = 0,
			.maxval = 1,
			.flagval = 0,
			},
			{ .index = M_REFLINK,
			  .conflicts = { LAST_CONFLICT },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 0,
			},
		},
	},
};

#define TERABYTES(count, blog)	((uint64_t)(count) << (40 - (blog)))
#define GIGABYTES(count, blog)	((uint64_t)(count) << (30 - (blog)))
#define MEGABYTES(count, blog)	((uint64_t)(count) << (20 - (blog)))

/*
 * Use this macro before we have superblock and mount structure
 */
#define	DTOBT(d)	((xfs_rfsblock_t)((d) >> (blocklog - BBSHIFT)))

/*
 * Use this for block reservations needed for mkfs's conditions
 * (basically no fragmentation).
 */
#define	MKFS_BLOCKRES_INODE	\
	((uint)(mp->m_ialloc_blks + (mp->m_in_maxlevels - 1)))
#define	MKFS_BLOCKRES(rb)	\
	((uint)(MKFS_BLOCKRES_INODE + XFS_DA_NODE_MAXDEPTH + \
	(XFS_BM_MAXLEVELS(mp, XFS_DATA_FORK) - 1) + (rb)))

/* amount (in bytes) we zero at the beginning and end of the device to
 * remove traces of other filesystems, raid superblocks, etc.
 */
#define WHACK_SIZE (128 * 1024)

static inline void
set_conf_raw(int opt, int subopt, const char *value)
{
	opts[opt].subopt_params[subopt].raw_input = value;
}

static inline const char *
get_conf_raw(int opt, int subopt)
{
	return opts[opt].subopt_params[subopt].raw_input;
}

static uint64_t getnum(const char *str, struct opt_params *opts, int index);

/*
 * Get and set values to the opts table.
 */
static inline uint64_t
get_conf_val(int opt, int subopt)
{
	return opts[opt].subopt_params[subopt].value;
}

static void
set_conf_val(int opt, int subopt, uint64_t val)
{
	struct subopt_param *sp = &opts[opt].subopt_params[subopt];

	sp->value = val;
}

/*
 * A wrapper for getnum and set_conf_val.
 */
static inline uint64_t
parse_conf_val(int opt, int subopt, char *value)
{
	uint64_t num = getnum(value, &opts[opt], subopt);

	set_conf_val(opt, subopt, num);
	return num;
}


/*
 * Convert lsu to lsunit for 512 bytes blocks and check validity of the values.
 */
static void
calc_stripe_factors(
	int		dsu,
	int		dsw,
	int		dsectsz,
	int		lsu,
	int		lsectsz,
	uint64_t	*dsunit,
	uint64_t	*dswidth,
	uint64_t	*lsunit)
{
	/* Handle data sunit/swidth options */
	if ((*dsunit && !*dswidth) || (!*dsunit && *dswidth)) {
		fprintf(stderr,
			_("both data sunit and data swidth options "
			"must be specified\n"));
		usage();
	}

	if (dsu || dsw) {
		if ((dsu && !dsw) || (!dsu && dsw)) {
			fprintf(stderr,
				_("both data su and data sw options "
				"must be specified\n"));
			usage();
		}

		if (dsu % dsectsz) {
			fprintf(stderr,
				_("data su must be a multiple of the "
				"sector size (%d)\n"), dsectsz);
			usage();
		}

		*dsunit  = BTOBBT(dsu);
		*dswidth = *dsunit * dsw;
	}

	if (*dsunit && (*dswidth % *dsunit != 0)) {
		fprintf(stderr,
			_("data stripe width (%"PRIu64") must be a multiple of the "
			"data stripe unit (%"PRIu64")\n"), *dswidth, *dsunit);
		usage();
	}

	/* Handle log sunit options */

	if (lsu)
		*lsunit = BTOBBT(lsu);

	/* verify if lsu/lsunit is a multiple block size */
	if (lsu % blocksize != 0) {
		fprintf(stderr,
_("log stripe unit (%d) must be a multiple of the block size (%"PRIu64")\n"),
		lsu, blocksize);
		exit(1);
	}
	if ((BBTOB(*lsunit) % blocksize != 0)) {
		fprintf(stderr,
_("log stripe unit (%"PRIu64") must be a multiple of the block size (%"PRIu64")\n"),
		BBTOB(*lsunit), blocksize);
		exit(1);
	}
}

static void
check_device_type(
	const char	*name,
	int		*isfile,
	bool		no_size,
	bool		no_name,
	int		*create,
	bool		force_overwrite,
	const char	*optname)
{
	struct stat statbuf;

	if (*isfile && (no_size || no_name)) {
		fprintf(stderr,
	_("if -%s file then -%s name and -%s size are required\n"),
			optname, optname, optname);
		usage();
	}

	if (!name) {
		fprintf(stderr, _("No device name specified\n"));
		usage();
	}

	if (stat(name, &statbuf)) {
		if (errno == ENOENT && *isfile) {
			if (create)
				*create = 1;
			return;
		}

		fprintf(stderr,
	_("Error accessing specified device %s: %s\n"),
				name, strerror(errno));
		usage();
		return;
	}

	if (!force_overwrite && check_overwrite(name)) {
		fprintf(stderr,
	_("%s: Use the -f option to force overwrite.\n"),
			progname);
		exit(1);
	}

	/*
	 * We only want to completely truncate and recreate an existing file if
	 * we were specifically told it was a file. Set the create flag only in
	 * this case to trigger that behaviour.
	 */
	if (S_ISREG(statbuf.st_mode)) {
		if (!*isfile)
			*isfile = 1;
		else if (create)
			*create = 1;
		return;
	}

	if (S_ISBLK(statbuf.st_mode)) {
		if (*isfile) {
			fprintf(stderr,
	_("specified \"-%s file\" on a block device %s\n"),
				optname, name);
			usage();
		}
		return;
	}

	fprintf(stderr,
	_("specified device %s not a file or block device\n"),
		name);
	usage();
}

static void
fixup_log_stripe_unit(
	int		lsflag,
	int		sunit,
	xfs_rfsblock_t	*logblocks,
	int		blocklog)
{
	uint64_t	tmp_logblocks;

	/*
	 * Make sure that the log size is a multiple of the stripe unit
	 */
	if ((*logblocks % sunit) != 0) {
		if (!lsflag) {
			tmp_logblocks = ((*logblocks + (sunit - 1))
						/ sunit) * sunit;
			/*
			 * If the log is too large, round down
			 * instead of round up
			 */
			if ((tmp_logblocks > XFS_MAX_LOG_BLOCKS) ||
			    ((tmp_logblocks << blocklog) > XFS_MAX_LOG_BYTES)) {
				tmp_logblocks = (*logblocks / sunit) * sunit;
			}
			*logblocks = tmp_logblocks;
		} else {
			fprintf(stderr, _("log size %"PRIu64" is not a multiple "
					  "of the log stripe unit %d\n"),
				*logblocks, sunit);
			usage();
		}
	}
}

static xfs_fsblock_t
fixup_internal_log_stripe(
	xfs_mount_t	*mp,
	int		lsflag,
	xfs_fsblock_t	logstart,
	uint64_t	agsize,
	int		sunit,
	xfs_rfsblock_t	*logblocks,
	int		blocklog,
	uint64_t	*lalign)
{
	if ((logstart % sunit) != 0) {
		logstart = ((logstart + (sunit - 1))/sunit) * sunit;
		*lalign = 1;
	}

	fixup_log_stripe_unit(lsflag, sunit, logblocks, blocklog);

	if (*logblocks > agsize - XFS_FSB_TO_AGBNO(mp, logstart)) {
		fprintf(stderr,
			_("Due to stripe alignment, the internal log size "
			"(%"PRIu64") is too large.\n"), *logblocks);
		fprintf(stderr, _("Must fit within an allocation group.\n"));
		usage();
	}
	return logstart;
}

void
validate_log_size(uint64_t logblocks, int blocklog, int min_logblocks)
{
	if (logblocks < min_logblocks) {
		fprintf(stderr,
	_("log size %"PRIu64" blocks too small, minimum size is %d blocks\n"),
			logblocks, min_logblocks);
		usage();
	}
	if (logblocks > XFS_MAX_LOG_BLOCKS) {
		fprintf(stderr,
	_("log size %"PRIu64" blocks too large, maximum size is %lld blocks\n"),
			logblocks, XFS_MAX_LOG_BLOCKS);
		usage();
	}
	if ((logblocks << blocklog) > XFS_MAX_LOG_BYTES) {
		fprintf(stderr,
	_("log size %"PRIu64" bytes too large, maximum size is %lld bytes\n"),
			(logblocks << blocklog), XFS_MAX_LOG_BYTES);
		usage();
	}
}

static int
calc_default_imaxpct(
	int		blocklog,
	uint64_t	dblocks)
{
	/*
	 * This returns the % of the disk space that is used for
	 * inodes, it changes relatively to the FS size:
	 *  - over  50 TB, use 1%,
	 *  - 1TB - 50 TB, use 5%,
	 *  - under  1 TB, use XFS_DFL_IMAXIMUM_PCT (25%).
	 */

	if (dblocks < TERABYTES(1, blocklog)) {
		return XFS_DFL_IMAXIMUM_PCT;
	} else if (dblocks < TERABYTES(50, blocklog)) {
		return 5;
	}

	return 1;
}

static void
validate_ag_geometry(
	int		blocklog,
	uint64_t	dblocks,
	uint64_t	agsize,
	uint64_t	agcount)
{
	if (agsize < XFS_AG_MIN_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("agsize (%"PRIu64" blocks) too small, need at least %"PRIu64" blocks\n"),
			agsize,
			(uint64_t)XFS_AG_MIN_BLOCKS(blocklog));
		usage();
	}

	if (agsize > XFS_AG_MAX_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("agsize (%"PRIu64" blocks) too big, maximum is %"PRIu64" blocks\n"),
			agsize,
			(uint64_t)XFS_AG_MAX_BLOCKS(blocklog));
		usage();
	}

	if (agsize > dblocks) {
		fprintf(stderr,
	_("agsize (%"PRIu64" blocks) too big, data area is %"PRIu64" blocks\n"),
			agsize, dblocks);
			usage();
	}

	if (agsize < XFS_AG_MIN_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("too many allocation groups for size = %"PRIu64"\n"),
				agsize);
		fprintf(stderr, _("need at most %"PRIu64" allocation groups\n"),
			(uint64_t) (dblocks / XFS_AG_MIN_BLOCKS(blocklog) +
				(dblocks % XFS_AG_MIN_BLOCKS(blocklog) != 0)));
		usage();
	}

	if (agsize > XFS_AG_MAX_BLOCKS(blocklog)) {
		fprintf(stderr,
	_("too few allocation groups for size = %"PRIu64"\n"), agsize);
		fprintf(stderr,
	_("need at least %"PRIu64" allocation groups\n"),
		(uint64_t)(dblocks / XFS_AG_MAX_BLOCKS(blocklog) +
			(dblocks % XFS_AG_MAX_BLOCKS(blocklog) != 0)));
		usage();
	}

	/*
	 * If the last AG is too small, reduce the filesystem size
	 * and drop the blocks.
	 */
	if ( dblocks % agsize != 0 &&
	     (dblocks % agsize < XFS_AG_MIN_BLOCKS(blocklog))) {
		fprintf(stderr,
	_("last AG size %"PRIu64" blocks too small, minimum size is %"PRIu64" blocks\n"),
			(dblocks % agsize),
			(uint64_t)XFS_AG_MIN_BLOCKS(blocklog));
		usage();
	}

	/*
	 * If agcount is too large, make it smaller.
	 */
	if (agcount > XFS_MAX_AGNUMBER + 1) {
		fprintf(stderr,
	_("%"PRIu64" allocation groups is too many, maximum is %"PRIu64"\n"),
			agcount, (uint64_t)XFS_MAX_AGNUMBER + 1);
		usage();
	}
}

static void
zero_old_xfs_structures(
	libxfs_init_t		*xi,
	xfs_sb_t		*new_sb)
{
	void 			*buf;
	xfs_sb_t 		sb;
	__uint32_t		bsize;
	int			i;
	xfs_off_t		off;

	/*
	 * We open regular files with O_TRUNC|O_CREAT. Nothing to do here...
	 */
	if (xi->disfile && xi->dcreat)
		return;

	/*
	 * read in existing filesystem superblock, use its geometry
	 * settings and zero the existing secondary superblocks.
	 */
	buf = memalign(libxfs_device_alignment(), new_sb->sb_sectsize);
	if (!buf) {
		fprintf(stderr,
	_("error reading existing superblock -- failed to memalign buffer\n"));
		return;
	}
	memset(buf, 0, new_sb->sb_sectsize);

	/*
	 * If we are creating an image file, it might be of zero length at this
	 * point in time. Hence reading the existing superblock is going to
	 * return zero bytes. It's not a failure we need to warn about in this
	 * case.
	 */
	off = pread(xi->dfd, buf, new_sb->sb_sectsize, 0);
	if (off != new_sb->sb_sectsize) {
		if (!xi->disfile)
			fprintf(stderr,
	_("error reading existing superblock: %s\n"),
				strerror(errno));
		goto done;
	}
	libxfs_sb_from_disk(&sb, buf);

	/*
	 * perform same basic superblock validation to make sure we
	 * actually zero secondary blocks
	 */
	if (sb.sb_magicnum != XFS_SB_MAGIC || sb.sb_blocksize == 0)
		goto done;

	for (bsize = 1, i = 0; bsize < sb.sb_blocksize &&
			i < sizeof(sb.sb_blocksize) * NBBY; i++)
		bsize <<= 1;

	if (i < XFS_MIN_BLOCKSIZE_LOG || i > XFS_MAX_BLOCKSIZE_LOG ||
			i != sb.sb_blocklog)
		goto done;

	if (sb.sb_dblocks > ((uint64_t)sb.sb_agcount * sb.sb_agblocks) ||
			sb.sb_dblocks < ((uint64_t)(sb.sb_agcount - 1) *
					 sb.sb_agblocks + XFS_MIN_AG_BLOCKS))
		goto done;

	/*
	 * block size and basic geometry seems alright, zero the secondaries.
	 */
	memset(buf, 0, new_sb->sb_sectsize);
	off = 0;
	for (i = 1; i < sb.sb_agcount; i++)  {
		off += sb.sb_agblocks;
		if (pwrite(xi->dfd, buf, new_sb->sb_sectsize,
					off << sb.sb_blocklog) == -1)
			break;
	}
done:
	free(buf);
}

static void
discard_blocks(dev_t dev, uint64_t nsectors)
{
	int fd;

	/*
	 * We intentionally ignore errors from the discard ioctl.  It is
	 * not necessary for the mkfs functionality but just an optimization.
	 */
	fd = libxfs_device_to_fd(dev);
	if (fd > 0)
		platform_discard_blocks(fd, 0, nsectors << 9);
}

struct sb_feat_args {
	int	log_version;
	int	attr_version;
	int	dir_version;
	int	spinodes;
	int	finobt;
	bool	inode_align;
	bool	nci;
	bool	lazy_sb_counters;
	bool	projid16bit;
	bool	crcs_enabled;
	bool	dirftype;
	bool	parent_pointers;
	bool	rmapbt;
	bool	reflink;
};

static void
sb_set_features(
	struct xfs_sb		*sbp,
	struct sb_feat_args	*fp,
	int			sectsize,
	int			lsectsize,
	int			dsunit)
{

	sbp->sb_versionnum = XFS_DFL_SB_VERSION_BITS;
	if (fp->crcs_enabled)
		sbp->sb_versionnum |= XFS_SB_VERSION_5;
	else
		sbp->sb_versionnum |= XFS_SB_VERSION_4;

	if (fp->inode_align)
		sbp->sb_versionnum |= XFS_SB_VERSION_ALIGNBIT;
	if (dsunit)
		sbp->sb_versionnum |= XFS_SB_VERSION_DALIGNBIT;
	if (fp->log_version == 2)
		sbp->sb_versionnum |= XFS_SB_VERSION_LOGV2BIT;
	if (fp->attr_version == 1)
		sbp->sb_versionnum |= XFS_SB_VERSION_ATTRBIT;
	if (sectsize > BBSIZE || lsectsize > BBSIZE)
		sbp->sb_versionnum |= XFS_SB_VERSION_SECTORBIT;
	if (fp->nci)
		sbp->sb_versionnum |= XFS_SB_VERSION_BORGBIT;


	sbp->sb_features2 = 0;
	if (fp->lazy_sb_counters)
		sbp->sb_features2 |= XFS_SB_VERSION2_LAZYSBCOUNTBIT;
	if (!fp->projid16bit)
		sbp->sb_features2 |= XFS_SB_VERSION2_PROJID32BIT;
	if (fp->parent_pointers)
		sbp->sb_features2 |= XFS_SB_VERSION2_PARENTBIT;
	if (fp->crcs_enabled)
		sbp->sb_features2 |= XFS_SB_VERSION2_CRCBIT;
	if (fp->attr_version == 2)
		sbp->sb_features2 |= XFS_SB_VERSION2_ATTR2BIT;

	/* v5 superblocks have their own feature bit for dirftype */
	if (fp->dirftype && !fp->crcs_enabled)
		sbp->sb_features2 |= XFS_SB_VERSION2_FTYPE;

	/* update whether extended features are in use */
	if (sbp->sb_features2 != 0)
		sbp->sb_versionnum |= XFS_SB_VERSION_MOREBITSBIT;

	/*
	 * Due to a structure alignment issue, sb_features2 ended up in one
	 * of two locations, the second "incorrect" location represented by
	 * the sb_bad_features2 field. To avoid older kernels mounting
	 * filesystems they shouldn't, set both field to the same value.
	 */
	sbp->sb_bad_features2 = sbp->sb_features2;

	if (!fp->crcs_enabled)
		return;

	/* default features for v5 filesystems */
	sbp->sb_features_compat = 0;
	sbp->sb_features_ro_compat = 0;
	sbp->sb_features_incompat = XFS_SB_FEAT_INCOMPAT_FTYPE;
	sbp->sb_features_log_incompat = 0;

	if (fp->finobt)
		sbp->sb_features_ro_compat = XFS_SB_FEAT_RO_COMPAT_FINOBT;
	if (fp->rmapbt)
		sbp->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_RMAPBT;
	if (fp->reflink)
		sbp->sb_features_ro_compat |= XFS_SB_FEAT_RO_COMPAT_REFLINK;

	/*
	 * Sparse inode chunk support has two main inode alignment requirements.
	 * First, sparse chunk alignment must match the cluster size. Second,
	 * full chunk alignment must match the inode chunk size.
	 *
	 * Copy the already calculated/scaled inoalignmt to spino_align and
	 * update the former to the full inode chunk size.
	 */
	if (fp->spinodes) {
		sbp->sb_spino_align = sbp->sb_inoalignmt;
		sbp->sb_inoalignmt = XFS_INODES_PER_CHUNK *
			sbp->sb_inodesize >> sbp->sb_blocklog;
		sbp->sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_SPINODES;
	}

}

static __attribute__((noreturn)) void
illegal_option(
	const char		*value,
	struct opt_params	*opts,
	int			index,
	const char		*reason)
{
	fprintf(stderr,
		_("Illegal value %s for -%c %s option. %s\n"),
		value, opts->name, opts->subopts[index],
		reason ? reason : "");
	usage();
}

/*
 * Check for conflicts and option respecification.
 */
static void
check_opt(
	struct opt_params	*opts,
	int			index,
	bool			str_seen)
{
	struct subopt_param	*sp = &opts->subopt_params[index];
	int			i;

	if (sp->index != index) {
		fprintf(stderr,
	("Developer screwed up option parsing (%d/%d)! Please report!\n"),
			sp->index, index);
		reqval(opts->name, (char **)opts->subopts, index);
	}

	/*
	 * Check for respecification of the option. This is more complex than it
	 * seems because some options are parsed twice - once as a string during
	 * input parsing, then later the string is passed to getnum for
	 * conversion into a number and bounds checking. Hence the two variables
	 * used to track the different uses based on the @str parameter passed
	 * to us.
	 */
	if (!str_seen) {
		if (sp->seen)
			respec(opts->name, (char **)opts->subopts, index);
		sp->seen = true;
	} else {
		if (sp->str_seen)
			respec(opts->name, (char **)opts->subopts, index);
		sp->str_seen = true;
	}

	/* check for conflicts with the option */
	for (i = 0; i < MAX_CONFLICTS; i++) {
		int conflict_opt = sp->conflicts[i];

		if (conflict_opt == LAST_CONFLICT)
			break;
		if (opts->subopt_params[conflict_opt].seen ||
		    opts->subopt_params[conflict_opt].str_seen)
			conflict(opts->name, (char **)opts->subopts,
				 conflict_opt, index);
	}
}

static uint64_t
getnum(
	const char		*str,
	struct opt_params	*opts,
	int			index)
{
	struct subopt_param	*sp = &opts->subopt_params[index];
	uint64_t		c;
	int			ret;

	check_opt(opts, index, false);
	set_conf_raw(opts->index, index, str);
	/* empty strings might just return a default value */
	if (!str || *str == '\0') {
		if (sp->flagval == SUBOPT_NEEDS_VAL)
			reqval(opts->name, (char **)opts->subopts, index);
		return sp->flagval;
	}

	if (sp->minval == 0 && sp->maxval == 0) {
		fprintf(stderr,
			_("Option -%c %s has undefined minval/maxval."
			  "Can't verify value range. This is a bug.\n"),
			opts->name, opts->subopts[index]);
		exit(1);
	}

	/*
	 * Some values are pure numbers, others can have suffixes that define
	 * the units of the number. Those get passed to cvtnum(), otherwise we
	 * convert it ourselves to guarantee there is no trailing garbage in the
	 * number.
	 */
	if (sp->convert) {
		ret = cvtnum(blocksize, sectorsize, str, &c);
		switch(ret) {
			case 0:
				break;
			case -EINVAL:
				illegal_option(str, opts, index,
					_("Parse error, ret: -EINVAL"));
				break;
			case -ERANGE:
				illegal_option(str, opts, index,
					_("Parse error, ret: -ERANGE"));
				break;
			default:
				illegal_option(str, opts, index,
					_("Parse error."));
				break;
		}
	} else {
		char		*str_end;

		c = strtoull(str, &str_end, 0);
		if (c == 0 && str_end == str)
			illegal_option(str, opts, index, NULL);
		if (*str_end != '\0')
			illegal_option(str, opts, index, NULL);
	}

	/* Validity check the result. */
	if (c < sp->minval)
		illegal_option(str, opts, index, _("value is too small"));
	else if (c > sp->maxval)
		illegal_option(str, opts, index, _("value is too large"));
	if (sp->is_power_2 && !ispow2(c))
		illegal_option(str, opts, index, _("value must be a power of 2"));
	return c;
}

/*
 * Option is a string - do all the option table work, and check there
 * is actually an option string. Otherwise we don't do anything with the string
 * here - validation will be done later when the string is converted to a value
 * or used as a file/device path.
 */
static char *
getstr(
	char			*str,
	struct opt_params	*opts,
	int			index)
{
	check_opt(opts, index, true);

	/* empty strings for string options are not valid */
	if (!str || *str == '\0')
		reqval(opts->name, (char **)opts->subopts, index);
	return str;
}

int
main(
	int			argc,
	char			**argv)
{
	uint64_t		agcount;
	xfs_agf_t		*agf;
	xfs_agi_t		*agi;
	xfs_agnumber_t		agno;
	uint64_t		agsize;
	xfs_alloc_rec_t		*arec;
	struct xfs_btree_block	*block;
	bool			blflag;
	uint64_t		blocklog;
	bool			bsflag;
	uint64_t		bsize;
	xfs_buf_t		*buf;
	uint64_t		c;
	bool			daflag;
	uint64_t		dasize;
	xfs_rfsblock_t		dblocks;
	char			*dfile;
	uint64_t		dirblocklog;
	uint64_t		dirblocksize;
	uint64_t		dbytes;
	uint64_t		dsu;
	uint64_t		dsw;
	uint64_t		dsunit;
	uint64_t		dswidth;
	bool			force_overwrite;
	struct fsxattr		fsx;
	bool			ilflag;
	uint64_t		imaxpct;
	bool			imflag;
	uint64_t		inodelog;
	uint64_t		inopblock;
	bool			ipflag;
	bool			isflag;
	uint64_t		isize;
	char			*label = NULL;
	bool			laflag;
	uint64_t		lalign;
	bool			ldflag;
	bool			liflag;
	xfs_agnumber_t		logagno;
	xfs_rfsblock_t		logblocks;
	char			*logfile;
	uint64_t		loginternal;
	uint64_t		logbytes;
	xfs_fsblock_t		logstart;
	bool			lvflag;
	bool			lsflag;
	bool			lsuflag;
	bool			lsunitflag;
	uint64_t		lsectorlog;
	uint64_t		lsectorsize;
	bool			lslflag;
	bool			lssflag;
	uint64_t		lsu;
	uint64_t		lsunit;
	uint64_t		min_logblocks;
	xfs_mount_t		*mp;
	xfs_mount_t		mbuf;
	xfs_extlen_t		nbmblocks;
	bool			nlflag;
	bool			nodsflag;
	bool			norsflag;
	xfs_alloc_rec_t		*nrec;
	bool			nsflag;
	bool			nvflag;
	bool			Nflag;
	uint64_t		discard = 1;
	char			*p;
	char			*protofile;
	char			*protostring;
	bool			qflag;
	xfs_rfsblock_t		rtblocks;
	uint64_t		rtbytes;
	xfs_extlen_t		rtextblocks;
	xfs_rtblock_t		rtextents;
	uint64_t		rtextbytes;
	char			*rtfile;
	xfs_sb_t		*sbp;
	uint64_t		sectorlog;
	uint64_t		sector_mask;
	bool			slflag;
	bool			ssflag;
	uint64_t		tmp_agsize;
	uuid_t			uuid;
	uint64_t		worst_freelist;
	libxfs_init_t		xi;
	struct fs_topology	ft;
	struct sb_feat_args	sb_feat = {
		.finobt = 1,
		.spinodes = 0,
		.log_version = 2,
		.attr_version = 2,
		.dir_version = XFS_DFL_DIR_VERSION,
		.inode_align = XFS_IFLAG_ALIGN,
		.nci = false,
		.lazy_sb_counters = true,
		.projid16bit = false,
		.crcs_enabled = true,
		.dirftype = true,
		.parent_pointers = false,
		.rmapbt = false,
		.reflink = false,
	};

	platform_uuid_generate(&uuid);
	progname = basename(argv[0]);
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);

	blflag = bsflag = slflag = ssflag = lslflag = lssflag = 0;
	blocklog = blocksize = 0;
	sectorlog = lsectorlog = 0;
	sectorsize = lsectorsize = 0;
	agsize = dasize = dblocks = 0;
	daflag = ilflag = imflag = ipflag = isflag = false;
	liflag = laflag = lsflag = lsuflag = lsunitflag = ldflag = lvflag = false;
	loginternal = 1;
	logagno = logblocks = rtblocks = rtextblocks = 0;
	Nflag = nlflag = nsflag = nvflag = false;
	dirblocklog = dirblocksize = 0;
	qflag = false;
	imaxpct = inodelog = inopblock = isize = 0;
	dfile = logfile = rtfile = NULL;
	protofile = NULL;
	rtbytes = rtextbytes = logbytes = dbytes = 0;
	dsu = dsw = dsunit = dswidth = lalign = lsu = lsunit = 0;
	nodsflag = norsflag = false;
	force_overwrite = false;
	worst_freelist = 0;
	memset(&fsx, 0, sizeof(fsx));

	memset(&xi, 0, sizeof(xi));
	xi.isdirect = LIBXFS_DIRECT;
	xi.isreadonly = LIBXFS_EXCLUSIVELY;

	while ((c = getopt(argc, argv, "b:d:i:l:L:m:n:KNp:qr:s:CfV")) != EOF) {
		switch (c) {
		case 'C':
		case 'f':
			force_overwrite = true;
			break;
		case 'b':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_B].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case B_LOG:
					blocklog = getnum(value, &opts[OPT_B],
								B_LOG);
					blocksize = 1 << blocklog;
					blflag = 1;
					break;
				case B_SIZE:
					blocksize = getnum(value, &opts[OPT_B],
							   B_SIZE);
					blocklog = libxfs_highbit32(blocksize);
					bsflag = 1;
					break;
				default:
					unknown('b', value);
				}
			}
			break;
		case 'd':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_D].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case D_AGCOUNT:
					agcount = getnum(value, &opts[OPT_D],
							 D_AGCOUNT);
					daflag = 1;
					break;
				case D_AGSIZE:
					agsize = getnum(value, &opts[OPT_D],
								D_AGSIZE);
					dasize = 1;
					break;
				case D_FILE:
					xi.disfile = getnum(value,
						&opts[OPT_D], D_FILE);
					break;
				case D_NAME:
					xi.dname = getstr(value, &opts[OPT_D],
								D_NAME);
					break;
				case D_SIZE:
					dbytes = getnum(value, &opts[OPT_D],
								D_SIZE);
					break;
				case D_SUNIT:
					dsunit = getnum(value, &opts[OPT_D],
								D_SUNIT);
					break;
				case D_SWIDTH:
					dswidth = getnum(value, &opts[OPT_D],
							 D_SWIDTH);
					break;
				case D_SU:
					dsu = getnum(value, &opts[OPT_D],
								D_SU);
					break;
				case D_SW:
					dsw = getnum(value, &opts[OPT_D],
								D_SW);
					break;
				case D_NOALIGN:
					nodsflag = getnum(value, &opts[OPT_D],
								D_NOALIGN);
					break;
				case D_SECTLOG:
					sectorlog = getnum(value, &opts[OPT_D],
							   D_SECTLOG);
					sectorsize = 1 << sectorlog;
					slflag = 1;
					break;
				case D_SECTSIZE:
					sectorsize = getnum(value,
						&opts[OPT_D], D_SECTSIZE);
					sectorlog =
						libxfs_highbit32(sectorsize);
					ssflag = 1;
					break;
				case D_RTINHERIT:
					c = getnum(value, &opts[OPT_D],
								D_RTINHERIT);
					if (c)
						fsx.fsx_xflags |=
							XFS_DIFLAG_RTINHERIT;
					break;
				case D_PROJINHERIT:
					fsx.fsx_projid = getnum(value,
						&opts[OPT_D], D_PROJINHERIT);
					fsx.fsx_xflags |=
						XFS_DIFLAG_PROJINHERIT;
					break;
				case D_EXTSZINHERIT:
					fsx.fsx_extsize = getnum(value,
						&opts[OPT_D], D_EXTSZINHERIT);
					fsx.fsx_xflags |=
						XFS_DIFLAG_EXTSZINHERIT;
					break;
				default:
					unknown('d', value);
				}
			}
			break;
		case 'i':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_I].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case I_ALIGN:
					sb_feat.inode_align = getnum(value,
						&opts[OPT_I], I_ALIGN);
					break;
				case I_LOG:
					inodelog = getnum(value, &opts[OPT_I],
								I_LOG);
					isize = 1 << inodelog;
					ilflag = 1;
					break;
				case I_MAXPCT:
					imaxpct = getnum(value, &opts[OPT_I],
							 I_MAXPCT);
					imflag = 1;
					break;
				case I_PERBLOCK:
					inopblock = getnum(value, &opts[OPT_I],
							   I_PERBLOCK);
					ipflag = 1;
					break;
				case I_SIZE:
					isize = getnum(value, &opts[OPT_I],
								I_SIZE);
					inodelog = libxfs_highbit32(isize);
					isflag = 1;
					break;
				case I_ATTR:
					sb_feat.attr_version =
						getnum(value, &opts[OPT_I],
								I_ATTR);
					break;
				case I_PROJID32BIT:
					sb_feat.projid16bit =
						!getnum(value, &opts[OPT_I],
							I_PROJID32BIT);
					break;
				case I_SPINODES:
					sb_feat.spinodes = getnum(value,
								&opts[OPT_I],
								I_SPINODES);
					break;
				default:
					unknown('i', value);
				}
			}
			break;
		case 'l':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_L].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case L_AGNUM:
					logagno = getnum(value, &opts[OPT_L],
								L_AGNUM);
					laflag = 1;
					break;
				case L_FILE:
					xi.lisfile = getnum(value,
						&opts[OPT_L], L_FILE);
					break;
				case L_INTERNAL:
					loginternal = getnum(value,
						&opts[OPT_L], L_INTERNAL);
					liflag = 1;
					break;
				case L_SU:
					lsu = getnum(value, &opts[OPT_L],
								L_SU);
					lsuflag = 1;
					break;
				case L_SUNIT:
					lsunit = getnum(value, &opts[OPT_L],
								L_SUNIT);
					lsunitflag = 1;
					break;
				case L_NAME:
				case L_DEV:
					logfile = getstr(value, &opts[OPT_L],
								L_NAME);
					xi.logname = logfile;
					ldflag = 1;
					loginternal = 0;
					break;
				case L_VERSION:
					sb_feat.log_version =
						getnum(value, &opts[OPT_L],
								L_VERSION);
					lvflag = 1;
					break;
				case L_SIZE:
					logbytes = getnum(value,
						&opts[OPT_L], L_SIZE);
					break;
				case L_SECTLOG:
					lsectorlog = getnum(value,
						&opts[OPT_L], L_SECTLOG);
					lsectorsize = 1 << lsectorlog;
					lslflag = 1;
					break;
				case L_SECTSIZE:
					lsectorsize = getnum(value,
						&opts[OPT_L], L_SECTSIZE);
					lsectorlog =
						libxfs_highbit32(lsectorsize);
					lssflag = 1;
					break;
				case L_LAZYSBCNTR:
					sb_feat.lazy_sb_counters =
						getnum(value, &opts[OPT_L],
							L_LAZYSBCNTR);
					break;
				default:
					unknown('l', value);
				}
			}
			break;
		case 'L':
			if (strlen(optarg) > sizeof(sbp->sb_fname))
				illegal(optarg, "L");
			label = optarg;
			break;
		case 'm':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_M].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case M_CRC:
					sb_feat.crcs_enabled =
						getnum(value, &opts[OPT_M],
								M_CRC);
					if (sb_feat.crcs_enabled)
						sb_feat.dirftype = true;
					break;
				case M_FINOBT:
					sb_feat.finobt = getnum(
						value, &opts[OPT_M], M_FINOBT);
					break;
				case M_UUID:
					if (!value || *value == '\0')
						reqval('m', subopts, M_UUID);
					if (platform_uuid_parse(value, &uuid))
						illegal(optarg, "m uuid");
					break;
				case M_RMAPBT:
					sb_feat.rmapbt = getnum(
						value, &opts[OPT_M], M_RMAPBT);
					break;
				case M_REFLINK:
					sb_feat.reflink =
						getnum(value, &opts[OPT_M],
							M_REFLINK);
					break;
				default:
					unknown('m', value);
				}
			}
			break;
		case 'n':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_N].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case N_LOG:
					dirblocklog = getnum(value,
						&opts[OPT_N], N_LOG);
					dirblocksize = 1 << dirblocklog;
					nlflag = 1;
					break;
				case N_SIZE:
					dirblocksize = getnum(value,
						&opts[OPT_N], N_SIZE);
					dirblocklog =
						libxfs_highbit32(dirblocksize);
					nsflag = 1;
					break;
				case N_VERSION:
					value = getstr(value, &opts[OPT_N],
								N_VERSION);
					if (!strcasecmp(value, "ci")) {
						/* ASCII CI mode */
						sb_feat.nci = true;
					} else {
						sb_feat.dir_version =
							getnum(value,
							       &opts[OPT_N],
							       N_VERSION);
					}
					nvflag = 1;
					break;
				case N_FTYPE:
					sb_feat.dirftype = getnum(value,
						&opts[OPT_N], N_FTYPE);
					break;
				default:
					unknown('n', value);
				}
			}
			break;
		case 'N':
			Nflag = 1;
			break;
		case 'K':
			discard = 0;
			break;
		case 'p':
			if (protofile)
				respec('p', NULL, 0);
			protofile = optarg;
			break;
		case 'q':
			qflag = 1;
			break;
		case 'r':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_R].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case R_EXTSIZE:
					rtextbytes = getnum(value,
						&opts[OPT_R], R_EXTSIZE);
					break;
				case R_FILE:
					xi.risfile = getnum(value,
						&opts[OPT_R], R_FILE);
					break;
				case R_NAME:
				case R_DEV:
					xi.rtname = getstr(value, &opts[OPT_R],
							   R_NAME);
					break;
				case R_SIZE:
					rtbytes = getnum(value, &opts[OPT_R],
								R_SIZE);
					break;
				case R_NOALIGN:
					norsflag = getnum(value, &opts[OPT_R],
								R_NOALIGN);
					break;
				default:
					unknown('r', value);
				}
			}
			break;
		case 's':
			p = optarg;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_S].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case S_LOG:
				case S_SECTLOG:
					if (lssflag)
						conflict('s', subopts,
							 S_SECTSIZE,
							 S_SECTLOG);
					sectorlog = getnum(value, &opts[OPT_S],
							   S_SECTLOG);
					lsectorlog = sectorlog;
					sectorsize = 1 << sectorlog;
					lsectorsize = sectorsize;
					lslflag = slflag = 1;
					break;
				case S_SIZE:
				case S_SECTSIZE:
					if (lslflag)
						conflict('s', subopts,
							 S_SECTLOG,
							 S_SECTSIZE);
					sectorsize = getnum(value,
						&opts[OPT_S], S_SECTSIZE);
					lsectorsize = sectorsize;
					sectorlog =
						libxfs_highbit32(sectorsize);
					lsectorlog = sectorlog;
					lssflag = ssflag = 1;
					break;
				default:
					unknown('s', value);
				}
			}
			break;
		case 'V':
			printf(_("%s version %s\n"), progname, VERSION);
			exit(0);
		case '?':
			unknown(optopt, "");
		}
	}
	if (argc - optind > 1) {
		fprintf(stderr, _("extra arguments\n"));
		usage();
	} else if (argc - optind == 1) {
		dfile = xi.volname = getstr(argv[optind],
					&opts[OPT_D], D_NAME);
	} else
		dfile = xi.dname;

	/*
	 * Blocksize and sectorsize first, other things depend on them
	 * For RAID4/5/6 we want to align sector size and block size,
	 * so we need to start with the device geometry extraction too.
	 */
	if (!blflag && !bsflag) {
		blocklog = XFS_DFL_BLOCKSIZE_LOG;
		blocksize = 1 << XFS_DFL_BLOCKSIZE_LOG;
	}
	if (blocksize < XFS_MIN_BLOCKSIZE || blocksize > XFS_MAX_BLOCKSIZE) {
		fprintf(stderr, _("illegal block size %"PRIu64"\n"), blocksize);
		usage();
	}
	if (sb_feat.crcs_enabled && blocksize < XFS_MIN_CRC_BLOCKSIZE) {
		fprintf(stderr,
_("Minimum block size for CRC enabled filesystems is %d bytes.\n"),
			XFS_MIN_CRC_BLOCKSIZE);
		usage();
	}
	if (sb_feat.crcs_enabled && !sb_feat.dirftype) {
		fprintf(stderr, _("cannot disable ftype with crcs enabled\n"));
		usage();
	}

	if (!slflag && !ssflag) {
		sectorlog = XFS_MIN_SECTORSIZE_LOG;
		sectorsize = XFS_MIN_SECTORSIZE;
	}
	if (!lslflag && !lssflag) {
		lsectorlog = sectorlog;
		lsectorsize = sectorsize;
	}

	/*
	 * Before anything else, verify that we are correctly operating on
	 * files or block devices and set the control parameters correctly.
	 * Explicitly disable direct IO for image files so we don't error out on
	 * sector size mismatches between the new filesystem and the underlying
	 * host filesystem.
	 */
	check_device_type(dfile, &xi.disfile, !dbytes, !dfile,
			  Nflag ? NULL : &xi.dcreat, force_overwrite, "d");
	if (!loginternal)
		check_device_type(xi.logname, &xi.lisfile, !logbytes,
				  !xi.logname, Nflag ? NULL : &xi.lcreat,
				  force_overwrite, "l");
	if (xi.rtname)
		check_device_type(xi.rtname, &xi.risfile, !rtbytes, !xi.rtname,
				  Nflag ? NULL : &xi.rcreat,
				  force_overwrite, "r");
	if (xi.disfile || xi.lisfile || xi.risfile)
		xi.isdirect = 0;

	memset(&ft, 0, sizeof(ft));
	get_topology(&xi, &ft, force_overwrite);

	if (!ssflag) {
		/*
		 * Unless specified manually on the command line use the
		 * advertised sector size of the device.  We use the physical
		 * sector size unless the requested block size is smaller
		 * than that, then we can use logical, but warn about the
		 * inefficiency.
		 */

		/* Older kernels may not have physical/logical distinction */
		if (!ft.psectorsize)
			ft.psectorsize = ft.lsectorsize;

		sectorsize = ft.psectorsize ? ft.psectorsize :
					      XFS_MIN_SECTORSIZE;

		if ((blocksize < sectorsize) && (blocksize >= ft.lsectorsize)) {
			fprintf(stderr,
_("specified blocksize %"PRIu64" is less than device physical sector size %d\n"),
				blocksize, ft.psectorsize);
			fprintf(stderr,
_("switching to logical sector size %d\n"),
				ft.lsectorsize);
			sectorsize = ft.lsectorsize ? ft.lsectorsize :
						      XFS_MIN_SECTORSIZE;
		}
	}

	if (!ssflag) {
		sectorlog = libxfs_highbit32(sectorsize);
		if (loginternal) {
			lsectorsize = sectorsize;
			lsectorlog = sectorlog;
		}
	}

	if (sectorsize < XFS_MIN_SECTORSIZE ||
	    sectorsize > XFS_MAX_SECTORSIZE || sectorsize > blocksize) {
		if (ssflag)
			fprintf(stderr, _("illegal sector size %"PRIu64"\n"), sectorsize);
		else
			fprintf(stderr,
_("block size %"PRIu64" cannot be smaller than logical sector size %d\n"),
				blocksize, ft.lsectorsize);
		usage();
	}
	if (sectorsize < ft.lsectorsize) {
		fprintf(stderr, _("illegal sector size %"PRIu64"; hw sector is %d\n"),
			sectorsize, ft.lsectorsize);
		usage();
	}
	if (lsectorsize < XFS_MIN_SECTORSIZE ||
	    lsectorsize > XFS_MAX_SECTORSIZE || lsectorsize > blocksize) {
		fprintf(stderr, _("illegal log sector size %"PRIu64"\n"), lsectorsize);
		usage();
	} else if (lsectorsize > XFS_MIN_SECTORSIZE && !lsu && !lsunit) {
		lsu = blocksize;
		sb_feat.log_version = 2;
	}

	/*
	 * Now we have blocks and sector sizes set up, check parameters that are
	 * no longer optional for CRC enabled filesystems.  Catch them up front
	 * here before doing anything else.
	 */
	if (sb_feat.crcs_enabled) {
		/* minimum inode size is 512 bytes, ipflag checked later */
		if ((isflag || ilflag) && inodelog < XFS_DINODE_DFL_CRC_LOG) {
			fprintf(stderr,
_("Minimum inode size for CRCs is %d bytes\n"),
				1 << XFS_DINODE_DFL_CRC_LOG);
			usage();
		}

		/* inodes always aligned */
		if (!sb_feat.inode_align) {
			fprintf(stderr,
_("Inodes always aligned for CRC enabled filesytems\n"));
			usage();
		}

		/* lazy sb counters always on */
		if (!sb_feat.lazy_sb_counters) {
			fprintf(stderr,
_("Lazy superblock counted always enabled for CRC enabled filesytems\n"));
			usage();
		}

		/* version 2 logs always on */
		if (sb_feat.log_version != 2) {
			fprintf(stderr,
_("V2 logs always enabled for CRC enabled filesytems\n"));
			usage();
		}

		/* attr2 always on */
		if (sb_feat.attr_version != 2) {
			fprintf(stderr,
_("V2 attribute format always enabled on CRC enabled filesytems\n"));
			usage();
		}

		/* 32 bit project quota always on */
		/* attr2 always on */
		if (sb_feat.projid16bit) {
			fprintf(stderr,
_("32 bit Project IDs always enabled on CRC enabled filesytems\n"));
			usage();
		}
	} else {
		/*
		 * The kernel doesn't currently support crc=0,finobt=1
		 * filesystems. If crcs are not enabled and the user has not
		 * explicitly turned finobt on, then silently turn it off to
		 * avoid an unnecessary warning.
		 * If the user explicitly tried to use crc=0,finobt=1,
		 * then issue an error.
		 * The same is also for sparse inodes.
		 */
		if (sb_feat.finobt &&
		    opts[OPT_M].subopt_params[M_FINOBT].seen) {
			fprintf(stderr,
_("finobt not supported without CRC support\n"));
			usage();
		}
		sb_feat.finobt = 0;

		if (sb_feat.spinodes) {
			fprintf(stderr,
_("sparse inodes not supported without CRC support\n"));
			usage();
		}
		sb_feat.spinodes = 0;

		if (sb_feat.rmapbt) {
			fprintf(stderr,
_("rmapbt not supported without CRC support\n"));
			usage();
		}
		sb_feat.rmapbt = false;

		if (sb_feat.reflink) {
			fprintf(stderr,
_("reflink not supported without CRC support\n"));
			usage();
		}
		sb_feat.reflink = false;
	}


	if (sb_feat.rmapbt && xi.rtname) {
		fprintf(stderr,
_("rmapbt not supported with realtime devices\n"));
		usage();
		sb_feat.rmapbt = false;
	}

	if (nsflag || nlflag) {
		if (dirblocksize < blocksize ||
					dirblocksize > XFS_MAX_BLOCKSIZE) {
			fprintf(stderr, _("illegal directory block size %"PRIu64"\n"),
				dirblocksize);
			usage();
		}
	} else {
		if (blocksize < (1 << XFS_MIN_REC_DIRSIZE))
			dirblocklog = XFS_MIN_REC_DIRSIZE;
		else
			dirblocklog = blocklog;
		dirblocksize = 1 << dirblocklog;
	}


	if (dbytes) {
		if (dbytes % XFS_MIN_BLOCKSIZE) {
			fprintf(stderr,
			_("illegal data length %"PRIu64", not a multiple of %d\n"),
				dbytes, XFS_MIN_BLOCKSIZE);
			usage();
		}
		dblocks = (xfs_rfsblock_t)(dbytes >> blocklog);
		if (dbytes % blocksize)
			fprintf(stderr, _("warning: "
	"data length %"PRIu64" not a multiple of %"PRIu64", truncated to %"PRIu64"\n"),
				dbytes, blocksize,
				(uint64_t)(dblocks << blocklog));
	}
	if (ipflag) {
		inodelog = blocklog - libxfs_highbit32(inopblock);
		isize = 1 << inodelog;
	} else if (!ilflag && !isflag) {
		inodelog = sb_feat.crcs_enabled ? XFS_DINODE_DFL_CRC_LOG
						: XFS_DINODE_DFL_LOG;
		isize = 1 << inodelog;
	}
	if (sb_feat.crcs_enabled && inodelog < XFS_DINODE_DFL_CRC_LOG) {
		fprintf(stderr,
		_("Minimum inode size for CRCs is %d bytes\n"),
			1 << XFS_DINODE_DFL_CRC_LOG);
		usage();
	}

	if (logbytes) {
		if (logbytes % XFS_MIN_BLOCKSIZE) {
			fprintf(stderr,
			_("illegal log length %"PRIu64", not a multiple of %d\n"),
				logbytes, XFS_MIN_BLOCKSIZE);
			usage();
		}
		logblocks = (xfs_rfsblock_t)(logbytes >> blocklog);
		if (logbytes % blocksize)
			fprintf(stderr,
	_("warning: log length %"PRIu64" not a multiple of %"PRIu64", truncated to %"PRIu64"\n"),
				logbytes, blocksize,
				(uint64_t)(logblocks << blocklog));
	}
	if (rtbytes) {
		if (rtbytes % XFS_MIN_BLOCKSIZE) {
			fprintf(stderr,
			_("illegal rt length %"PRIu64", not a multiple of %d\n"),
				rtbytes, XFS_MIN_BLOCKSIZE);
			usage();
		}
		rtblocks = (xfs_rfsblock_t)(rtbytes >> blocklog);
		if (rtbytes % blocksize)
			fprintf(stderr,
	_("warning: rt length %"PRIu64" not a multiple of %"PRIu64", truncated to %"PRIu64"\n"),
				rtbytes, blocksize,
				(uint64_t)(rtblocks << blocklog));
	}
	/*
	 * If specified, check rt extent size against its constraints.
	 */
	if (rtextbytes) {
		if (rtextbytes % blocksize) {
			fprintf(stderr,
		_("illegal rt extent size %"PRIu64", not a multiple of %"PRIu64"\n"),
				rtextbytes, blocksize);
			usage();
		}
		rtextblocks = (xfs_extlen_t)(rtextbytes >> blocklog);
	} else {
		/*
		 * If realtime extsize has not been specified by the user,
		 * and the underlying volume is striped, then set rtextblocks
		 * to the stripe width.
		 */
		uint64_t	rswidth;
		uint64_t	rtextbytes;

		if (!norsflag && !xi.risfile && !(!rtbytes && xi.disfile))
			rswidth = ft.rtswidth;
		else
			rswidth = 0;

		/* check that rswidth is a multiple of fs blocksize */
		if (!norsflag && rswidth && !(BBTOB(rswidth) % blocksize)) {
			rswidth = DTOBT(rswidth);
			rtextbytes = rswidth << blocklog;
			if (XFS_MIN_RTEXTSIZE <= rtextbytes &&
			    (rtextbytes <= XFS_MAX_RTEXTSIZE)) {
				rtextblocks = rswidth;
			}
		}
		if (!rtextblocks) {
			rtextblocks = (blocksize < XFS_MIN_RTEXTSIZE) ?
					XFS_MIN_RTEXTSIZE >> blocklog : 1;
		}
	}
	ASSERT(rtextblocks);

	/*
	 * Check some argument sizes against mins, maxes.
	 */
	if (isize > blocksize / XFS_MIN_INODE_PERBLOCK ||
	    isize < XFS_DINODE_MIN_SIZE ||
	    isize > XFS_DINODE_MAX_SIZE) {
		int	maxsz;

		fprintf(stderr, _("illegal inode size %"PRIu64"\n"), isize);
		maxsz = MIN(blocksize / XFS_MIN_INODE_PERBLOCK,
			    XFS_DINODE_MAX_SIZE);
		if (XFS_DINODE_MIN_SIZE == maxsz)
			fprintf(stderr,
			_("allowable inode size with %"PRIu64" byte blocks is %d\n"),
				blocksize, XFS_DINODE_MIN_SIZE);
		else
			fprintf(stderr,
	_("allowable inode size with %"PRIu64" byte blocks is between %d and %d\n"),
				blocksize, XFS_DINODE_MIN_SIZE, maxsz);
		exit(1);
	}

	/* if lsu or lsunit was specified, automatically use v2 logs */
	if ((lsu || lsunit) && sb_feat.log_version == 1) {
		fprintf(stderr,
			_("log stripe unit specified, using v2 logs\n"));
		sb_feat.log_version = 2;
	}

	calc_stripe_factors(dsu, dsw, sectorsize, lsu, lsectorsize,
				&dsunit, &dswidth, &lsunit);

	xi.setblksize = sectorsize;

	/*
	 * Initialize.  This will open the log and rt devices as well.
	 */
	if (!libxfs_init(&xi))
		usage();
	if (!xi.ddev) {
		fprintf(stderr, _("no device name given in argument list\n"));
		usage();
	}

	/*
	 * Ok, Linux only has a 1024-byte resolution on device _size_,
	 * and the sizes below are in basic 512-byte blocks,
	 * so if we have (size % 2), on any partition, we can't get
	 * to the last 512 bytes.  The same issue exists for larger
	 * sector sizes - we cannot write past the last sector.
	 *
	 * So, we reduce the size (in basic blocks) to a perfect
	 * multiple of the sector size, or 1024, whichever is larger.
	 */

	sector_mask = (uint64_t)-1 << (MAX(sectorlog, 10) - BBSHIFT);
	xi.dsize &= sector_mask;
	xi.rtsize &= sector_mask;
	xi.logBBsize &= (uint64_t)-1 << (MAX(lsectorlog, 10) - BBSHIFT);


	/* don't do discards on print-only runs or on files */
	if (discard && !Nflag) {
		if (!xi.disfile)
			discard_blocks(xi.ddev, xi.dsize);
		if (xi.rtdev && !xi.risfile)
			discard_blocks(xi.rtdev, xi.rtsize);
		if (xi.logdev && xi.logdev != xi.ddev && !xi.lisfile)
			discard_blocks(xi.logdev, xi.logBBsize);
	}

	if (!liflag && !ldflag)
		loginternal = xi.logdev == 0;
	if (xi.logname)
		logfile = xi.logname;
	else if (loginternal)
		logfile = _("internal log");
	else if (xi.volname && xi.logdev)
		logfile = _("volume log");
	else if (!ldflag) {
		fprintf(stderr, _("no log subvolume or internal log\n"));
		usage();
	}
	if (xi.rtname)
		rtfile = xi.rtname;
	else
	if (xi.volname && xi.rtdev)
		rtfile = _("volume rt");
	else if (!xi.rtdev)
		rtfile = _("none");
	if (dbytes && xi.dsize > 0 && dblocks > DTOBT(xi.dsize)) {
		fprintf(stderr,
			_("size %s specified for data subvolume is too large, "
			"maximum is %"PRIu64" blocks\n"),
			get_conf_raw(OPT_D, D_SIZE),
			(uint64_t)DTOBT(xi.dsize));
		usage();
	} else if (!dbytes && xi.dsize > 0)
		dblocks = DTOBT(xi.dsize);
	else if (!dbytes) {
		fprintf(stderr, _("can't get size of data subvolume\n"));
		usage();
	}
	if (dblocks < XFS_MIN_DATA_BLOCKS) {
		fprintf(stderr,
	_("size %"PRIu64" of data subvolume is too small, minimum %d blocks\n"),
			dblocks, XFS_MIN_DATA_BLOCKS);
		usage();
	}

	if (loginternal && xi.logdev) {
		fprintf(stderr,
			_("can't have both external and internal logs\n"));
		usage();
	} else if (loginternal && sectorsize != lsectorsize) {
		fprintf(stderr,
	_("data and log sector sizes must be equal for internal logs\n"));
		usage();
	}

	if (xi.dbsize > sectorsize) {
		fprintf(stderr, _(
"Warning: the data subvolume sector size %"PRIu64" is less than the sector size \n\
reported by the device (%u).\n"),
			sectorsize, xi.dbsize);
	}
	if (!loginternal && xi.lbsize > lsectorsize) {
		fprintf(stderr, _(
"Warning: the log subvolume sector size %"PRIu64" is less than the sector size\n\
reported by the device (%u).\n"),
			lsectorsize, xi.lbsize);
	}
	if (rtbytes && xi.rtsize > 0 && xi.rtbsize > sectorsize) {
		fprintf(stderr, _(
"Warning: the realtime subvolume sector size %"PRIu64" is less than the sector size\n\
reported by the device (%u).\n"),
			sectorsize, xi.rtbsize);
	}

	if (rtbytes && xi.rtsize > 0 && rtblocks > DTOBT(xi.rtsize)) {
		fprintf(stderr,
			_("size %s specified for rt subvolume is too large, "
			"maximum is %"PRIu64" blocks\n"),
			get_conf_raw(OPT_R, R_SIZE),
			(uint64_t)DTOBT(xi.rtsize));
		usage();
	} else if (!rtbytes && xi.rtsize > 0)
		rtblocks = DTOBT(xi.rtsize);
	else if (rtbytes && !xi.rtdev) {
		fprintf(stderr,
			_("size specified for non-existent rt subvolume\n"));
		usage();
	}
	if (xi.rtdev) {
		rtextents = rtblocks / rtextblocks;
		nbmblocks = (xfs_extlen_t)howmany(rtextents, NBBY * blocksize);
	} else {
		rtextents = rtblocks = 0;
		nbmblocks = 0;
	}

	if (!nodsflag) {
		if (dsunit) {
			if (ft.dsunit && ft.dsunit != dsunit) {
				fprintf(stderr,
					_("%s: Specified data stripe unit %"PRIu64" "
					"is not the same as the volume stripe "
					"unit %d\n"),
					progname, dsunit, ft.dsunit);
			}
			if (ft.dswidth && ft.dswidth != dswidth) {
				fprintf(stderr,
					_("%s: Specified data stripe width %"PRIu64" "
					"is not the same as the volume stripe "
					"width %d\n"),
					progname, dswidth, ft.dswidth);
			}
		} else {
			dsunit = ft.dsunit;
			dswidth = ft.dswidth;
			nodsflag = 1;
		}
	} /* else dsunit & dswidth can't be set if nodsflag is set */

	if (dasize) {		/* User-specified AG size */
		/*
		 * Check specified agsize is a multiple of blocksize.
		 */
		if (agsize % blocksize) {
			fprintf(stderr,
		_("agsize (%"PRIu64") not a multiple of fs blk size (%"PRIu64")\n"),
				agsize, blocksize);
			usage();
		}
		agsize /= blocksize;
		agcount = dblocks / agsize + (dblocks % agsize != 0);

	} else if (daflag) {	/* User-specified AG count */
		agsize = dblocks / agcount + (dblocks % agcount != 0);
	} else {
		calc_default_ag_geometry(blocklog, dblocks,
				dsunit | dswidth, &agsize, &agcount);
	}

	/*
	 * If dsunit is a multiple of fs blocksize, then check that is a
	 * multiple of the agsize too
	 */
	if (dsunit && !(BBTOB(dsunit) % blocksize) &&
	    dswidth && !(BBTOB(dswidth) % blocksize)) {

		/* convert from 512 byte blocks to fs blocksize */
		dsunit = DTOBT(dsunit);
		dswidth = DTOBT(dswidth);

		/*
		 * agsize is not a multiple of dsunit
		 */
		if ((agsize % dsunit) != 0) {
			/*
			 * Round up to stripe unit boundary. Also make sure
			 * that agsize is still larger than
			 * XFS_AG_MIN_BLOCKS(blocklog)
		 	 */
			tmp_agsize = ((agsize + (dsunit - 1))/ dsunit) * dsunit;
			/*
			 * Round down to stripe unit boundary if rounding up
			 * created an AG size that is larger than the AG max.
			 */
			if (tmp_agsize > XFS_AG_MAX_BLOCKS(blocklog))
				tmp_agsize = ((agsize) / dsunit) * dsunit;

			if ((tmp_agsize >= XFS_AG_MIN_BLOCKS(blocklog)) &&
			    (tmp_agsize <= XFS_AG_MAX_BLOCKS(blocklog))) {
				agsize = tmp_agsize;
				if (!daflag)
					agcount = dblocks/agsize +
						(dblocks % agsize != 0);
				if (dasize)
					fprintf(stderr,
				_("agsize rounded to %"PRIu64", swidth = %"PRIu64"\n"),
						agsize, dswidth);
			} else {
				if (nodsflag) {
					dsunit = dswidth = 0;
				} else {
					/*
					 * agsize is out of bounds, this will
					 * print nice details & exit.
					 */
					validate_ag_geometry(blocklog, dblocks,
							    agsize, agcount);
					exit(1);
				}
			}
		}
		if (dswidth && ((agsize % dswidth) == 0) && (agcount > 1)) {
			/* This is a non-optimal configuration because all AGs
			 * start on the same disk in the stripe.  Changing
			 * the AG size by one sunit will guarantee that this
			 * does not happen.
			 */
			tmp_agsize = agsize - dsunit;
			if (tmp_agsize < XFS_AG_MIN_BLOCKS(blocklog)) {
				tmp_agsize = agsize + dsunit;
				if (dblocks < agsize) {
					/* oh well, nothing to do */
					tmp_agsize = agsize;
				}
			}
			if (daflag || dasize) {
				fprintf(stderr, _(
"Warning: AG size is a multiple of stripe width.  This can cause performance\n\
problems by aligning all AGs on the same disk.  To avoid this, run mkfs with\n\
an AG size that is one stripe unit smaller, for example %"PRIu64".\n"),
					tmp_agsize);
			} else {
				agsize = tmp_agsize;
				agcount = dblocks/agsize + (dblocks % agsize != 0);
				/*
				 * If the last AG is too small, reduce the
				 * filesystem size and drop the blocks.
				 */
				if ( dblocks % agsize != 0 &&
				    (dblocks % agsize <
				    XFS_AG_MIN_BLOCKS(blocklog))) {
					dblocks = (xfs_rfsblock_t)((agcount - 1) * agsize);
					agcount--;
					ASSERT(agcount != 0);
				}
			}
		}
	} else {
		if (nodsflag)
			dsunit = dswidth = 0;
		else {
			fprintf(stderr,
				_("%s: Stripe unit(%"PRIu64") or stripe width(%"PRIu64") is "
				"not a multiple of the block size(%"PRIu64")\n"),
				progname, BBTOB(dsunit), BBTOB(dswidth),
				blocksize);
			exit(1);
		}
	}

	/*
	 * If the last AG is too small, reduce the filesystem size
	 * and drop the blocks.
	 */
	if ( dblocks % agsize != 0 &&
	     (dblocks % agsize < XFS_AG_MIN_BLOCKS(blocklog))) {
		ASSERT(!daflag);
		dblocks = (xfs_rfsblock_t)((agcount - 1) * agsize);
		agcount--;
		ASSERT(agcount != 0);
	}

	validate_ag_geometry(blocklog, dblocks, agsize, agcount);

	if (!imflag)
		imaxpct = calc_default_imaxpct(blocklog, dblocks);

	/*
	 * check that log sunit is modulo fsblksize or default it to dsunit.
	 */

	if (lsunit) {
		/* convert from 512 byte blocks to fs blocks */
		lsunit = DTOBT(lsunit);
	} else if (sb_feat.log_version == 2 && loginternal && dsunit) {
		/* lsunit and dsunit now in fs blocks */
		lsunit = dsunit;
	}

	if (sb_feat.log_version == 2 && (lsunit * blocksize) > 256 * 1024) {
		/* Warn only if specified on commandline */
		if (lsuflag || lsunitflag) {
			fprintf(stderr,
	_("log stripe unit (%"PRIu64" bytes) is too large (maximum is 256KiB)\n"),
				(lsunit * blocksize));
			fprintf(stderr,
	_("log stripe unit adjusted to 32KiB\n"));
		}
		lsunit = (32 * 1024) >> blocklog;
	}

	min_logblocks = max_trans_res(agsize,
				   sb_feat.crcs_enabled, sb_feat.dir_version,
				   sectorlog, blocklog, inodelog, dirblocklog,
				   sb_feat.log_version, lsunit, sb_feat.finobt,
				   sb_feat.rmapbt, sb_feat.reflink);
	ASSERT(min_logblocks);
	min_logblocks = MAX(XFS_MIN_LOG_BLOCKS, min_logblocks);
	if (!logbytes && dblocks >= (1024*1024*1024) >> blocklog)
		min_logblocks = MAX(min_logblocks, XFS_MIN_LOG_BYTES>>blocklog);
	if (logbytes && xi.logBBsize > 0 && logblocks > DTOBT(xi.logBBsize)) {
		fprintf(stderr,
_("size %s specified for log subvolume is too large, maximum is %"PRIu64" blocks\n"),
			get_conf_raw(OPT_L, L_SIZE),
			(uint64_t)DTOBT(xi.logBBsize));
		usage();
	} else if (!logbytes && xi.logBBsize > 0) {
		logblocks = DTOBT(xi.logBBsize);
	} else if (logbytes && !xi.logdev && !loginternal) {
		fprintf(stderr,
			_("size specified for non-existent log subvolume\n"));
		usage();
	} else if (loginternal && logbytes && logblocks >= dblocks) {
		fprintf(stderr, _("size %"PRIu64" too large for internal log\n"),
			logblocks);
		usage();
	} else if (!loginternal && !xi.logdev) {
		logblocks = 0;
	} else if (loginternal && !logbytes) {

		if (dblocks < GIGABYTES(1, blocklog)) {
			/* tiny filesystems get minimum sized logs. */
			logblocks = min_logblocks;
		} else if (dblocks < GIGABYTES(16, blocklog)) {

			/*
			 * For small filesystems, we want to use the
			 * XFS_MIN_LOG_BYTES for filesystems smaller than 16G if
			 * at all possible, ramping up to 128MB at 256GB.
			 */
			logblocks = MIN(XFS_MIN_LOG_BYTES >> blocklog,
					min_logblocks * XFS_DFL_LOG_FACTOR);
		} else {
			/*
			 * With a 2GB max log size, default to maximum size
			 * at 4TB. This keeps the same ratio from the older
			 * max log size of 128M at 256GB fs size. IOWs,
			 * the ratio of fs size to log size is 2048:1.
			 */
			logblocks = (dblocks << blocklog) / 2048;
			logblocks = logblocks >> blocklog;
		}

		/* Ensure the chosen size meets minimum log size requirements */
		logblocks = MAX(min_logblocks, logblocks);

		/* make sure the log fits wholly within an AG */
		if (logblocks >= agsize)
			logblocks = min_logblocks;

		/* and now clamp the size to the maximum supported size */
		logblocks = MIN(logblocks, XFS_MAX_LOG_BLOCKS);
		if ((logblocks << blocklog) > XFS_MAX_LOG_BYTES)
			logblocks = XFS_MAX_LOG_BYTES >> blocklog;

	}
	validate_log_size(logblocks, blocklog, min_logblocks);

	protostring = setup_proto(protofile);
	bsize = 1 << (blocklog - BBSHIFT);
	mp = &mbuf;
	sbp = &mp->m_sb;
	memset(mp, 0, sizeof(xfs_mount_t));
	sbp->sb_blocklog = (__uint8_t)blocklog;
	sbp->sb_sectlog = (__uint8_t)sectorlog;
	sbp->sb_agblklog = (__uint8_t)libxfs_log2_roundup((unsigned int)agsize);
	sbp->sb_agblocks = (xfs_agblock_t)agsize;
	mp->m_blkbb_log = sbp->sb_blocklog - BBSHIFT;
	mp->m_sectbb_log = sbp->sb_sectlog - BBSHIFT;

	/*
	 * sb_versionnum, finobt and rmapbt flags must be set before we use
	 * libxfs_prealloc_blocks().
	 */
	sb_set_features(&mp->m_sb, &sb_feat, sectorsize, lsectorsize, dsunit);


	if (loginternal) {
		/*
		 * Readjust the log size to fit within an AG if it was sized
		 * automatically.
		 */
		if (!logbytes) {
			logblocks = MIN(logblocks,
					libxfs_alloc_ag_max_usable(mp));

			/* revalidate the log size is valid if we changed it */
			validate_log_size(logblocks, blocklog, min_logblocks);
		}
		if (logblocks > agsize - libxfs_prealloc_blocks(mp)) {
			fprintf(stderr,
	_("internal log size %"PRIu64" too large, must fit in allocation group\n"),
				logblocks);
			usage();
		}

		if (laflag) {
			if (logagno >= agcount) {
				fprintf(stderr,
		_("log ag number %d too large, must be less than %"PRIu64"\n"),
					logagno, agcount);
				usage();
			}
		} else
			logagno = (xfs_agnumber_t)(agcount / 2);

		logstart = XFS_AGB_TO_FSB(mp, logagno, libxfs_prealloc_blocks(mp));
		/*
		 * Align the logstart at stripe unit boundary.
		 */
		if (lsunit) {
			logstart = fixup_internal_log_stripe(mp,
					lsflag, logstart, agsize, lsunit,
					&logblocks, blocklog, &lalign);
		} else if (dsunit) {
			logstart = fixup_internal_log_stripe(mp,
					lsflag, logstart, agsize, dsunit,
					&logblocks, blocklog, &lalign);
		}
	} else {
		logstart = 0;
		if (lsunit)
			fixup_log_stripe_unit(lsflag, lsunit,
					&logblocks, blocklog);
	}
	validate_log_size(logblocks, blocklog, min_logblocks);

	if (!qflag || Nflag) {
		printf(_(
		   "meta-data=%-22s isize=%-6lu agcount=%"PRIu64", agsize=%"PRIu64" blks\n"
		   "         =%-22s sectsz=%-5lu attr=%u, projid32bit=%u\n"
		   "         =%-22s crc=%-8u finobt=%u, sparse=%u, rmapbt=%u, reflink=%u\n"
		   "data     =%-22s bsize=%-6lu blocks=%"PRIu64", imaxpct=%"PRIu64"\n"
		   "         =%-22s sunit=%-6lu swidth=%"PRIu64" blks\n"
		   "naming   =version %-14u bsize=%-6lu ascii-ci=%d ftype=%d\n"
		   "log      =%-22s bsize=%-6d blocks=%"PRIu64", version=%d\n"
		   "         =%-22s sectsz=%-5lu sunit=%"PRIu64" blks, lazy-count=%d\n"
		   "realtime =%-22s extsz=%-6d blocks=%"PRIu64", rtextents=%"PRIu64"\n"),
			dfile, isize, agcount, agsize,
			"", sectorsize, sb_feat.attr_version,
				    !sb_feat.projid16bit,
			"", sb_feat.crcs_enabled, sb_feat.finobt, sb_feat.spinodes,
			sb_feat.rmapbt, sb_feat.reflink,
			"", blocksize, dblocks, imaxpct,
			"", dsunit, dswidth,
			sb_feat.dir_version, dirblocksize, sb_feat.nci,
				sb_feat.dirftype,
			logfile, 1 << blocklog, logblocks,
			sb_feat.log_version, "", lsectorsize, lsunit,
				sb_feat.lazy_sb_counters,
			rtfile, rtextblocks << blocklog,
			rtblocks, rtextents);
		if (Nflag)
			exit(0);
	}

	if (label)
		strncpy(sbp->sb_fname, label, sizeof(sbp->sb_fname));
	sbp->sb_magicnum = XFS_SB_MAGIC;
	sbp->sb_blocksize = blocksize;
	sbp->sb_dblocks = dblocks;
	sbp->sb_rblocks = rtblocks;
	sbp->sb_rextents = rtextents;
	platform_uuid_copy(&sbp->sb_uuid, &uuid);
	/* Only in memory; libxfs expects this as if read from disk */
	platform_uuid_copy(&sbp->sb_meta_uuid, &uuid);
	sbp->sb_logstart = logstart;
	sbp->sb_rootino = sbp->sb_rbmino = sbp->sb_rsumino = NULLFSINO;
	sbp->sb_rextsize = rtextblocks;
	sbp->sb_agcount = (xfs_agnumber_t)agcount;
	sbp->sb_rbmblocks = nbmblocks;
	sbp->sb_logblocks = (xfs_extlen_t)logblocks;
	sbp->sb_sectsize = (__uint16_t)sectorsize;
	sbp->sb_inodesize = (__uint16_t)isize;
	sbp->sb_inopblock = (__uint16_t)(blocksize / isize);
	sbp->sb_sectlog = (__uint8_t)sectorlog;
	sbp->sb_inodelog = (__uint8_t)inodelog;
	sbp->sb_inopblog = (__uint8_t)(blocklog - inodelog);
	sbp->sb_rextslog =
		(__uint8_t)(rtextents ?
			libxfs_highbit32((unsigned int)rtextents) : 0);
	sbp->sb_inprogress = 1;	/* mkfs is in progress */
	sbp->sb_imax_pct = imaxpct;
	sbp->sb_icount = 0;
	sbp->sb_ifree = 0;
	sbp->sb_fdblocks = dblocks - agcount * libxfs_prealloc_blocks(mp) -
		(loginternal ? logblocks : 0);
	sbp->sb_frextents = 0;	/* will do a free later */
	sbp->sb_uquotino = sbp->sb_gquotino = sbp->sb_pquotino = 0;
	sbp->sb_qflags = 0;
	sbp->sb_unit = dsunit;
	sbp->sb_width = dswidth;
	sbp->sb_dirblklog = dirblocklog - blocklog;
	if (sb_feat.log_version == 2) {	/* This is stored in bytes */
		lsunit = (lsunit == 0) ? 1 : XFS_FSB_TO_B(mp, lsunit);
		sbp->sb_logsunit = lsunit;
	} else
		sbp->sb_logsunit = 0;
	if (sb_feat.inode_align) {
		int	cluster_size = XFS_INODE_BIG_CLUSTER_SIZE;
		if (sb_feat.crcs_enabled)
			cluster_size *= isize / XFS_DINODE_MIN_SIZE;
		sbp->sb_inoalignmt = cluster_size >> blocklog;
		sb_feat.inode_align = sbp->sb_inoalignmt != 0;
	} else
		sbp->sb_inoalignmt = 0;
	if (lsectorsize != BBSIZE || sectorsize != BBSIZE) {
		sbp->sb_logsectlog = (__uint8_t)lsectorlog;
		sbp->sb_logsectsize = (__uint16_t)lsectorsize;
	} else {
		sbp->sb_logsectlog = 0;
		sbp->sb_logsectsize = 0;
	}

	sb_set_features(&mp->m_sb, &sb_feat, sectorsize, lsectorsize, dsunit);

	if (force_overwrite)
		zero_old_xfs_structures(&xi, sbp);

	/*
	 * Zero out the beginning of the device, to obliterate any old
	 * filesystem signatures out there.  This should take care of
	 * swap (somewhere around the page size), jfs (32k),
	 * ext[2,3] and reiserfs (64k) - and hopefully all else.
	 */
	libxfs_buftarg_init(mp, xi.ddev, xi.logdev, xi.rtdev);
	buf = libxfs_getbuf(mp->m_ddev_targp, 0, BTOBB(WHACK_SIZE));
	memset(XFS_BUF_PTR(buf), 0, WHACK_SIZE);
	libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
	libxfs_purgebuf(buf);

	/* OK, now write the superblock */
	buf = libxfs_getbuf(mp->m_ddev_targp, XFS_SB_DADDR, XFS_FSS_TO_BB(mp, 1));
	buf->b_ops = &xfs_sb_buf_ops;
	memset(XFS_BUF_PTR(buf), 0, sectorsize);
	libxfs_sb_to_disk((void *)XFS_BUF_PTR(buf), sbp);
	libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
	libxfs_purgebuf(buf);

	/*
	 * If the data area is a file, then grow it out to its final size
	 * if needed so that the reads for the end of the device in the mount
	 * code will succeed.
	 */
	if (xi.disfile && xi.dsize * xi.dbsize < dblocks * blocksize) {
		if (ftruncate(xi.dfd, dblocks * blocksize) < 0) {
			fprintf(stderr,
				_("%s: Growing the data section failed\n"),
				progname);
			exit(1);
		}
	}

	/*
	 * Zero out the end of the device, to obliterate any
	 * old MD RAID (or other) metadata at the end of the device.
	 * (MD sb is ~64k from the end, take out a wider swath to be sure)
	 */
	if (!xi.disfile) {
		buf = libxfs_getbuf(mp->m_ddev_targp,
				    (xi.dsize - BTOBB(WHACK_SIZE)),
				    BTOBB(WHACK_SIZE));
		memset(XFS_BUF_PTR(buf), 0, WHACK_SIZE);
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
		libxfs_purgebuf(buf);
	}

	/*
	 * Zero the log....
	 */
	libxfs_log_clear(mp->m_logdev_targp, NULL,
		XFS_FSB_TO_DADDR(mp, logstart),
		(xfs_extlen_t)XFS_FSB_TO_BB(mp, logblocks),
		&sbp->sb_uuid, sb_feat.log_version, lsunit, XLOG_FMT, XLOG_INIT_CYCLE, false);

	mp = libxfs_mount(mp, sbp, xi.ddev, xi.logdev, xi.rtdev, 0);
	if (mp == NULL) {
		fprintf(stderr, _("%s: filesystem failed to initialize\n"),
			progname);
		exit(1);
	}

	/*
	 * XXX: this code is effectively shared with the kernel growfs code.
	 * These initialisations should be pulled into libxfs to keep the
	 * kernel/userspace header initialisation code the same.
	 */
	for (agno = 0; agno < agcount; agno++) {
		struct xfs_agfl	*agfl;
		int		bucket;
		struct xfs_perag *pag = libxfs_perag_get(mp, agno);

		/*
		 * Superblock.
		 */
		buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AG_DADDR(mp, agno, XFS_SB_DADDR),
				XFS_FSS_TO_BB(mp, 1));
		buf->b_ops = &xfs_sb_buf_ops;
		memset(XFS_BUF_PTR(buf), 0, sectorsize);
		libxfs_sb_to_disk((void *)XFS_BUF_PTR(buf), sbp);
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

		/*
		 * AG header block: freespace
		 */
		buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AG_DADDR(mp, agno, XFS_AGF_DADDR(mp)),
				XFS_FSS_TO_BB(mp, 1));
		buf->b_ops = &xfs_agf_buf_ops;
		agf = XFS_BUF_TO_AGF(buf);
		memset(agf, 0, sectorsize);
		if (agno == agcount - 1)
			agsize = dblocks - (xfs_rfsblock_t)(agno * agsize);
		agf->agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
		agf->agf_versionnum = cpu_to_be32(XFS_AGF_VERSION);
		agf->agf_seqno = cpu_to_be32(agno);
		agf->agf_length = cpu_to_be32(agsize);
		agf->agf_roots[XFS_BTNUM_BNOi] = cpu_to_be32(XFS_BNO_BLOCK(mp));
		agf->agf_roots[XFS_BTNUM_CNTi] = cpu_to_be32(XFS_CNT_BLOCK(mp));
		agf->agf_levels[XFS_BTNUM_BNOi] = cpu_to_be32(1);
		agf->agf_levels[XFS_BTNUM_CNTi] = cpu_to_be32(1);
		pag->pagf_levels[XFS_BTNUM_BNOi] = 1;
		pag->pagf_levels[XFS_BTNUM_CNTi] = 1;
		if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
			agf->agf_roots[XFS_BTNUM_RMAPi] =
						cpu_to_be32(XFS_RMAP_BLOCK(mp));
			agf->agf_levels[XFS_BTNUM_RMAPi] = cpu_to_be32(1);
			agf->agf_rmap_blocks = cpu_to_be32(1);
		}
		if (xfs_sb_version_hasreflink(&mp->m_sb)) {
			agf->agf_refcount_root = cpu_to_be32(
					libxfs_refc_block(mp));
			agf->agf_refcount_level = cpu_to_be32(1);
			agf->agf_refcount_blocks = cpu_to_be32(1);
		}
		agf->agf_flfirst = 0;
		agf->agf_fllast = cpu_to_be32(XFS_AGFL_SIZE(mp) - 1);
		agf->agf_flcount = 0;
		nbmblocks = (xfs_extlen_t)(agsize - libxfs_prealloc_blocks(mp));
		agf->agf_freeblks = cpu_to_be32(nbmblocks);
		agf->agf_longest = cpu_to_be32(nbmblocks);
		if (xfs_sb_version_hascrc(&mp->m_sb))
			platform_uuid_copy(&agf->agf_uuid, &mp->m_sb.sb_uuid);

		if (loginternal && agno == logagno) {
			be32_add_cpu(&agf->agf_freeblks, -logblocks);
			agf->agf_longest = cpu_to_be32(agsize -
				XFS_FSB_TO_AGBNO(mp, logstart) - logblocks);
		}
		if (libxfs_alloc_min_freelist(mp, pag) > worst_freelist)
			worst_freelist = libxfs_alloc_min_freelist(mp, pag);
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

		/*
		 * AG freelist header block
		 */
		buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AG_DADDR(mp, agno, XFS_AGFL_DADDR(mp)),
				XFS_FSS_TO_BB(mp, 1));
		buf->b_ops = &xfs_agfl_buf_ops;
		agfl = XFS_BUF_TO_AGFL(buf);
		/* setting to 0xff results in initialisation to NULLAGBLOCK */
		memset(agfl, 0xff, sectorsize);
		if (xfs_sb_version_hascrc(&mp->m_sb)) {
			agfl->agfl_magicnum = cpu_to_be32(XFS_AGFL_MAGIC);
			agfl->agfl_seqno = cpu_to_be32(agno);
			platform_uuid_copy(&agfl->agfl_uuid, &mp->m_sb.sb_uuid);
			for (bucket = 0; bucket < XFS_AGFL_SIZE(mp); bucket++)
				agfl->agfl_bno[bucket] = cpu_to_be32(NULLAGBLOCK);
		}

		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

		/*
		 * AG header block: inodes
		 */
		buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AG_DADDR(mp, agno, XFS_AGI_DADDR(mp)),
				XFS_FSS_TO_BB(mp, 1));
		agi = XFS_BUF_TO_AGI(buf);
		buf->b_ops = &xfs_agi_buf_ops;
		memset(agi, 0, sectorsize);
		agi->agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
		agi->agi_versionnum = cpu_to_be32(XFS_AGI_VERSION);
		agi->agi_seqno = cpu_to_be32(agno);
		agi->agi_length = cpu_to_be32((xfs_agblock_t)agsize);
		agi->agi_count = 0;
		agi->agi_root = cpu_to_be32(XFS_IBT_BLOCK(mp));
		agi->agi_level = cpu_to_be32(1);
		if (sb_feat.finobt) {
			agi->agi_free_root = cpu_to_be32(XFS_FIBT_BLOCK(mp));
			agi->agi_free_level = cpu_to_be32(1);
		}
		agi->agi_freecount = 0;
		agi->agi_newino = cpu_to_be32(NULLAGINO);
		agi->agi_dirino = cpu_to_be32(NULLAGINO);
		if (xfs_sb_version_hascrc(&mp->m_sb))
			platform_uuid_copy(&agi->agi_uuid, &mp->m_sb.sb_uuid);
		for (c = 0; c < XFS_AGI_UNLINKED_BUCKETS; c++)
			agi->agi_unlinked[c] = cpu_to_be32(NULLAGINO);
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

		/*
		 * BNO btree root block
		 */
		buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AGB_TO_DADDR(mp, agno, XFS_BNO_BLOCK(mp)),
				bsize);
		buf->b_ops = &xfs_allocbt_buf_ops;
		block = XFS_BUF_TO_BLOCK(buf);
		memset(block, 0, blocksize);
		libxfs_btree_init_block(mp, buf, XFS_BTNUM_BNO, 0, 1, agno, 0);

		arec = XFS_ALLOC_REC_ADDR(mp, block, 1);
		arec->ar_startblock = cpu_to_be32(libxfs_prealloc_blocks(mp));
		if (loginternal && agno == logagno) {
			if (lalign) {
				/*
				 * Have to insert two records
				 * Insert pad record for stripe align of log
				 */
				arec->ar_blockcount = cpu_to_be32(
					XFS_FSB_TO_AGBNO(mp, logstart) -
					be32_to_cpu(arec->ar_startblock));
				nrec = arec + 1;
				/*
				 * Insert record at start of internal log
				 */
				nrec->ar_startblock = cpu_to_be32(
					be32_to_cpu(arec->ar_startblock) +
					be32_to_cpu(arec->ar_blockcount));
				arec = nrec;
				be16_add_cpu(&block->bb_numrecs, 1);
			}
			/*
			 * Change record start to after the internal log
			 */
			be32_add_cpu(&arec->ar_startblock, logblocks);
		}
		/*
		 * Calculate the record block count and check for the case where
		 * the log might have consumed all available space in the AG. If
		 * so, reset the record count to 0 to avoid exposure of an invalid
		 * record start block.
		 */
		arec->ar_blockcount = cpu_to_be32(agsize -
					be32_to_cpu(arec->ar_startblock));
		if (!arec->ar_blockcount)
			block->bb_numrecs = 0;

		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

		/*
		 * CNT btree root block
		 */
		buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AGB_TO_DADDR(mp, agno, XFS_CNT_BLOCK(mp)),
				bsize);
		buf->b_ops = &xfs_allocbt_buf_ops;
		block = XFS_BUF_TO_BLOCK(buf);
		memset(block, 0, blocksize);
		libxfs_btree_init_block(mp, buf, XFS_BTNUM_CNT, 0, 1, agno, 0);

		arec = XFS_ALLOC_REC_ADDR(mp, block, 1);
		arec->ar_startblock = cpu_to_be32(libxfs_prealloc_blocks(mp));
		if (loginternal && agno == logagno) {
			if (lalign) {
				arec->ar_blockcount = cpu_to_be32(
					XFS_FSB_TO_AGBNO(mp, logstart) -
					be32_to_cpu(arec->ar_startblock));
				nrec = arec + 1;
				nrec->ar_startblock = cpu_to_be32(
					be32_to_cpu(arec->ar_startblock) +
					be32_to_cpu(arec->ar_blockcount));
				arec = nrec;
				be16_add_cpu(&block->bb_numrecs, 1);
			}
			be32_add_cpu(&arec->ar_startblock, logblocks);
		}
		/*
		 * Calculate the record block count and check for the case where
		 * the log might have consumed all available space in the AG. If
		 * so, reset the record count to 0 to avoid exposure of an invalid
		 * record start block.
		 */
		arec->ar_blockcount = cpu_to_be32(agsize -
					be32_to_cpu(arec->ar_startblock));
		if (!arec->ar_blockcount)
			block->bb_numrecs = 0;

		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

		/*
		 * refcount btree root block
		 */
		if (xfs_sb_version_hasreflink(&mp->m_sb)) {
			buf = libxfs_getbuf(mp->m_ddev_targp,
					XFS_AGB_TO_DADDR(mp, agno,
						libxfs_refc_block(mp)),
					bsize);
			buf->b_ops = &xfs_refcountbt_buf_ops;

			block = XFS_BUF_TO_BLOCK(buf);
			memset(block, 0, blocksize);
			libxfs_btree_init_block(mp, buf, XFS_BTNUM_REFC, 0,
						0, agno, 0);

			libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
		}

		/*
		 * INO btree root block
		 */
		buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AGB_TO_DADDR(mp, agno, XFS_IBT_BLOCK(mp)),
				bsize);
		buf->b_ops = &xfs_inobt_buf_ops;
		block = XFS_BUF_TO_BLOCK(buf);
		memset(block, 0, blocksize);
		libxfs_btree_init_block(mp, buf, XFS_BTNUM_INO, 0, 0, agno, 0);
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

		/*
		 * Free INO btree root block
		 */
		if (sb_feat.finobt) {
			buf = libxfs_getbuf(mp->m_ddev_targp,
					XFS_AGB_TO_DADDR(mp, agno, XFS_FIBT_BLOCK(mp)),
					bsize);
			buf->b_ops = &xfs_inobt_buf_ops;
			block = XFS_BUF_TO_BLOCK(buf);
			memset(block, 0, blocksize);
			libxfs_btree_init_block(mp, buf, XFS_BTNUM_FINO, 0, 0, agno, 0);
			libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
		}

		/* RMAP btree root block */
		if (xfs_sb_version_hasrmapbt(&mp->m_sb)) {
			struct xfs_rmap_rec	*rrec;

			buf = libxfs_getbuf(mp->m_ddev_targp,
				XFS_AGB_TO_DADDR(mp, agno, XFS_RMAP_BLOCK(mp)),
				bsize);
			buf->b_ops = &xfs_rmapbt_buf_ops;
			block = XFS_BUF_TO_BLOCK(buf);
			memset(block, 0, blocksize);

			libxfs_btree_init_block(mp, buf, XFS_BTNUM_RMAP, 0, 0, agno, 0);

			/*
			 * mark the AG header regions as static metadata
			 * The BNO btree block is the first block after the
			 * headers, so it's location defines the size of region
			 * the static metadata consumes.
			 */
			rrec = XFS_RMAP_REC_ADDR(block, 1);
			rrec->rm_startblock = 0;
			rrec->rm_blockcount = cpu_to_be32(XFS_BNO_BLOCK(mp));
			rrec->rm_owner = cpu_to_be64(XFS_RMAP_OWN_FS);
			rrec->rm_offset = 0;
			be16_add_cpu(&block->bb_numrecs, 1);

			/* account freespace btree root blocks */
			rrec = XFS_RMAP_REC_ADDR(block, 2);
			rrec->rm_startblock = cpu_to_be32(XFS_BNO_BLOCK(mp));
			rrec->rm_blockcount = cpu_to_be32(2);
			rrec->rm_owner = cpu_to_be64(XFS_RMAP_OWN_AG);
			rrec->rm_offset = 0;
			be16_add_cpu(&block->bb_numrecs, 1);

			/* account inode btree root blocks */
			rrec = XFS_RMAP_REC_ADDR(block, 3);
			rrec->rm_startblock = cpu_to_be32(XFS_IBT_BLOCK(mp));
			rrec->rm_blockcount = cpu_to_be32(XFS_RMAP_BLOCK(mp) -
							XFS_IBT_BLOCK(mp));
			rrec->rm_owner = cpu_to_be64(XFS_RMAP_OWN_INOBT);
			rrec->rm_offset = 0;
			be16_add_cpu(&block->bb_numrecs, 1);

			/* account for rmap btree root */
			rrec = XFS_RMAP_REC_ADDR(block, 4);
			rrec->rm_startblock = cpu_to_be32(XFS_RMAP_BLOCK(mp));
			rrec->rm_blockcount = cpu_to_be32(1);
			rrec->rm_owner = cpu_to_be64(XFS_RMAP_OWN_AG);
			rrec->rm_offset = 0;
			be16_add_cpu(&block->bb_numrecs, 1);

			/* account for refcount btree root */
			if (xfs_sb_version_hasreflink(&mp->m_sb)) {
				rrec = XFS_RMAP_REC_ADDR(block, 5);
				rrec->rm_startblock = cpu_to_be32(
							libxfs_refc_block(mp));
				rrec->rm_blockcount = cpu_to_be32(1);
				rrec->rm_owner = cpu_to_be64(XFS_RMAP_OWN_REFC);
				rrec->rm_offset = 0;
				be16_add_cpu(&block->bb_numrecs, 1);
			}

			/* account for the log space */
			if (loginternal && agno == logagno) {
				rrec = XFS_RMAP_REC_ADDR(block,
					be16_to_cpu(block->bb_numrecs) + 1);
				rrec->rm_startblock = cpu_to_be32(
						XFS_FSB_TO_AGBNO(mp, logstart));
				rrec->rm_blockcount = cpu_to_be32(logblocks);
				rrec->rm_owner = cpu_to_be64(XFS_RMAP_OWN_LOG);
				rrec->rm_offset = 0;
				be16_add_cpu(&block->bb_numrecs, 1);
			}

			libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
		}

		libxfs_perag_put(pag);
	}

	/*
	 * Touch last block, make fs the right size if it's a file.
	 */
	buf = libxfs_getbuf(mp->m_ddev_targp,
		(xfs_daddr_t)XFS_FSB_TO_BB(mp, dblocks - 1LL), bsize);
	memset(XFS_BUF_PTR(buf), 0, blocksize);
	libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

	/*
	 * Make sure we can write the last block in the realtime area.
	 */
	if (mp->m_rtdev_targp->dev && rtblocks > 0) {
		buf = libxfs_getbuf(mp->m_rtdev_targp,
				XFS_FSB_TO_BB(mp, rtblocks - 1LL), bsize);
		memset(XFS_BUF_PTR(buf), 0, blocksize);
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
	}

	/*
	 * BNO, CNT free block list
	 */
	for (agno = 0; agno < agcount; agno++) {
		xfs_alloc_arg_t	args;
		xfs_trans_t	*tp;
		struct xfs_trans_res tres = {0};

		c = libxfs_trans_alloc(mp, &tres, worst_freelist, 0, 0, &tp);
		if (c)
			res_failed(c);

		memset(&args, 0, sizeof(args));
		args.tp = tp;
		args.mp = mp;
		args.agno = agno;
		args.alignment = 1;
		args.pag = libxfs_perag_get(mp,agno);

		libxfs_alloc_fix_freelist(&args, 0);
		libxfs_perag_put(args.pag);
		libxfs_trans_commit(tp);
	}

	/*
	 * Allocate the root inode and anything else in the proto file.
	 */
	parse_proto(mp, &fsx, &protostring);

	/*
	 * Protect ourselves against possible stupidity
	 */
	if (XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino) != 0) {
		fprintf(stderr,
			_("%s: root inode created in AG %u, not AG 0\n"),
			progname, XFS_INO_TO_AGNO(mp, mp->m_sb.sb_rootino));
		exit(1);
	}

	/*
	 * Write out multiple secondary superblocks with rootinode field set
	 */
	if (mp->m_sb.sb_agcount > 1) {
		/*
		 * the last superblock
		 */
		buf = libxfs_readbuf(mp->m_dev,
				XFS_AGB_TO_DADDR(mp, mp->m_sb.sb_agcount-1,
					XFS_SB_DADDR),
				XFS_FSS_TO_BB(mp, 1),
				LIBXFS_EXIT_ON_FAILURE, &xfs_sb_buf_ops);
		XFS_BUF_TO_SBP(buf)->sb_rootino = cpu_to_be64(
							mp->m_sb.sb_rootino);
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
		/*
		 * and one in the middle for luck
		 */
		if (mp->m_sb.sb_agcount > 2) {
			buf = libxfs_readbuf(mp->m_dev,
				XFS_AGB_TO_DADDR(mp, (mp->m_sb.sb_agcount-1)/2,
					XFS_SB_DADDR),
				XFS_FSS_TO_BB(mp, 1),
				LIBXFS_EXIT_ON_FAILURE, &xfs_sb_buf_ops);
			XFS_BUF_TO_SBP(buf)->sb_rootino = cpu_to_be64(
							mp->m_sb.sb_rootino);
			libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
		}
	}

	/*
	 * Dump all inodes and buffers before marking us all done.
	 * Need to drop references to inodes we still hold, first.
	 */
	libxfs_rtmount_destroy(mp);
	libxfs_bcache_purge();

	/*
	 * Mark the filesystem ok.
	 */
	buf = libxfs_getsb(mp, LIBXFS_EXIT_ON_FAILURE);
	(XFS_BUF_TO_SBP(buf))->sb_inprogress = 0;
	libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

	libxfs_umount(mp);
	if (xi.rtdev)
		libxfs_device_close(xi.rtdev);
	if (xi.logdev && xi.logdev != xi.ddev)
		libxfs_device_close(xi.logdev);
	libxfs_device_close(xi.ddev);

	return 0;
}

static void
conflict(
	char		opt,
	char		*tab[],
	int		oldidx,
	int		newidx)
{
	fprintf(stderr, _("Cannot specify both -%c %s and -%c %s\n"),
		opt, tab[oldidx], opt, tab[newidx]);
	usage();
}


static void
illegal(
	const char	*value,
	const char	*opt)
{
	fprintf(stderr, _("Illegal value %s for -%s option\n"), value, opt);
	usage();
}

static int
ispow2(
	unsigned int	i)
{
	return (i & (i - 1)) == 0;
}

static void __attribute__((noreturn))
reqval(
	char		opt,
	char		*tab[],
	int		idx)
{
	fprintf(stderr, _("-%c %s option requires a value\n"), opt, tab[idx]);
	usage();
}

static void
respec(
	char		opt,
	char		*tab[],
	int		idx)
{
	fprintf(stderr, "-%c ", opt);
	if (tab)
		fprintf(stderr, "%s ", tab[idx]);
	fprintf(stderr, _("option respecified\n"));
	usage();
}

static void
unknown(
	char		opt,
	char		*s)
{
	fprintf(stderr, _("unknown option -%c %s\n"), opt, s);
	usage();
}

int
cvtnum(
	unsigned int	blksize,
	unsigned int	sectsize,
	const char	*s,
	uint64_t	*val)
{
	uint64_t	i;
	char		*sp;
	int		c;
	uint64_t	orig_val;

	i = strtoull(s, &sp, 0);
	if (i == 0 && sp == s)
		return -EINVAL;
	if (*sp == '\0') {
		*val = i;
		return 0;
	}
	if (sp[1] != '\0')
		return -EINVAL;

	if (*sp == 'b') {
		if (!blksize) {
			fprintf(stderr,
_("Blocksize must be provided prior to using 'b' suffix.\n"));
			usage();
		} else {
			*val = i * blksize;
			if (*val < i || *val < blksize)
				return -ERANGE;
			return 0;
		}
	}
	if (*sp == 's') {
		if (!sectsize) {
			fprintf(stderr,
_("Sectorsize must be specified prior to using 's' suffix.\n"));
			usage();
		} else {
			*val = i * sectsize;
			if (*val < i || *val < sectsize)
				return -ERANGE;
			return 0;
		}
	}

	c = tolower(*sp);
	orig_val = i;
	switch (c) {
	case 'e':
		i *= 1024LL;
		/* fall through */
	case 'p':
		i *= 1024LL;
		/* fall through */
	case 't':
		i *= 1024LL;
		/* fall through */
	case 'g':
		i *= 1024LL;
		/* fall through */
	case 'm':
		i *= 1024LL;
		/* fall through */
	case 'k':
		*val = i * 1024LL;
		if (*val < orig_val)
			return -ERANGE;
		return 0;
	default:
		break;
	}
	return -EINVAL;
}

static void __attribute__((noreturn))
usage( void )
{
	fprintf(stderr, _("Usage: %s\n\
/* blocksize */		[-b log=n|size=num]\n\
/* metadata */		[-m crc=0|1,finobt=0|1,uuid=xxx,rmapbt=0|1,reflink=0|1]\n\
/* data subvol */	[-d agcount=n,agsize=n,file,name=xxx,size=num,\n\
			    (sunit=value,swidth=value|su=num,sw=num|noalign),\n\
			    sectlog=n|sectsize=num\n\
/* force overwrite */	[-f]\n\
/* inode size */	[-i log=n|perblock=n|size=num,maxpct=n,attr=0|1|2,\n\
			    projid32bit=0|1,sparse=0|1]\n\
/* no discard */	[-K]\n\
/* log subvol */	[-l agnum=n,internal,size=num,logdev=xxx,version=n\n\
			    sunit=value|su=num,sectlog=n|sectsize=num,\n\
			    lazy-count=0|1]\n\
/* label */		[-L label (maximum 12 characters)]\n\
/* naming */		[-n log=n|size=num,version=2|ci,ftype=0|1]\n\
/* no-op info only */	[-N]\n\
/* prototype file */	[-p fname]\n\
/* quiet */		[-q]\n\
/* realtime subvol */	[-r extsize=num,size=num,rtdev=xxx]\n\
/* sectorsize */	[-s log=n|size=num]\n\
/* version */		[-V]\n\
			devicename\n\
<devicename> is required unless -d name=xxx is given.\n\
<num> is xxx (bytes), xxxs (sectors), xxxb (fs blocks), xxxk (xxx KiB),\n\
      xxxm (xxx MiB), xxxg (xxx GiB), xxxt (xxx TiB) or xxxp (xxx PiB).\n\
<value> is xxx (512 byte blocks).\n"),
		progname);
	exit(1);
}
