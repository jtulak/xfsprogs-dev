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

#define MAX_OPTS	16
#define MAX_SUBOPTS	16
#define SUBOPT_NEEDS_VAL	(-1LL)
#define MAX_CONFLICTS	32
#define LAST_CONFLICT	(-1)

#define OPT_B		0
#define B_LOG		0
#define B_SIZE		1

#define OPT_D		1
#define D_AGCOUNT	0
#define D_FILE		1
#define D_NAME		2
#define D_SIZE		3
#define D_SUNIT 	4
#define D_SWIDTH	5
#define D_AGSIZE	6
#define D_SU		7
#define D_SW		8
#define D_SECTLOG	9
#define D_SECTSIZE	10
#define D_NOALIGN	11
#define D_RTINHERIT	12
#define D_PROJINHERIT	13
#define D_EXTSZINHERIT	14


#define OPT_I		2
#define I_ALIGN 	0
#define I_LOG		1
#define I_MAXPCT	2
#define I_PERBLOCK	3
#define I_SIZE		4
#define I_ATTR		5
#define I_PROJID32BIT	6
#define I_SPINODES	7

#define OPT_L		3
#define L_AGNUM 	0
#define L_INTERNAL	1
#define L_SIZE		2
#define L_VERSION	3
#define L_SUNIT 	4
#define L_SU		5
#define L_DEV		6
#define L_SECTLOG	7
#define L_SECTSIZE	8
#define L_FILE		9
#define L_NAME		10
#define L_LAZYSBCNTR	11


#define OPT_N		4
#define N_LOG		0
#define N_SIZE		1
#define N_VERSION	2
#define N_FTYPE 	3

#define OPT_R		5
#define R_EXTSIZE	0
#define R_SIZE		1
#define R_DEV		2
#define R_FILE		3
#define R_NAME		4
#define R_NOALIGN	5

#define OPT_S		6
#define S_LOG		0
#define S_SECTLOG	1
#define S_SIZE		2
#define S_SECTSIZE	3

#define OPT_M		7
#define M_CRC		0
#define M_FINOBT	1
#define M_UUID		2
#define M_RMAPBT	3
#define M_REFLINK	4

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
 *     Accepts the .index values of the conflicting subopt as .opt (e.g. OPT_D)
 *     and .subopt (e.g. D_FILE). If .test_values is true, then the conflict
 *     is raised only when the "remote" suboption .value is equal to
 *     .invalid_value field and the "current" suboption has .value equal to
 *     .at_value.
 *     If .test_values is false, a conflict is raised when the suboption appears
 *     on the CLI, no matter its value. The field .message contains an optional
 *     explanatory string for the user. This string can't be translated here,
 *     so it has to be enveloped with _() when printed.
 *     The last member of this list has to be {LAST_CONFLICT}.
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
		struct subopt_conflict {
			int		opt;
			int		subopt;
			bool		test_values;
			uint64_t	invalid_value;
			uint64_t	at_value;
			const char	*message;
		} 		conflicts [MAX_CONFLICTS];
		uint64_t	minval;
		uint64_t	maxval;
		uint64_t	flagval;
		const char	*raw_input;
		uint64_t	value;
	}		subopt_params[MAX_SUBOPTS];
} opts[MAX_OPTS] = {
	{
		.index = OPT_B,
		.name = 'b',
		.subopts = {
			"log",
			"size",
			NULL
		},
		.subopt_params = {
			{ .index = B_LOG,
			  .conflicts = {
				{.opt = OPT_B,
				 .subopt = B_SIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_MIN_BLOCKSIZE_LOG,
			  .maxval = XFS_MAX_BLOCKSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = B_SIZE,
			  .convert = true,
			  .is_power_2 = true,
			  .conflicts = {
				{.opt = OPT_B,
				 .subopt = B_LOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_MIN_BLOCKSIZE,
			  .maxval = XFS_MAX_BLOCKSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
		},
	},
	{
		.index = OPT_D,
		.name = 'd',
		.subopts = {
			"agcount",
			"file",
			"name",
			"size",
			"sunit",
			"swidth",
			"agsize",
			"su",
			"sw",
			"sectlog",
			"sectsize",
			"noalign",
			"rtinherit",
			"projinherit",
			"extszinherit",
			NULL
		},
		.subopt_params = {
			{ .index = D_AGCOUNT,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_AGSIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 1,
			  .maxval = XFS_MAX_AGNUMBER,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_FILE,
			  .conflicts = { {LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = D_NAME,
			  .conflicts = { {LAST_CONFLICT} },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SIZE,
			  .conflicts = { {LAST_CONFLICT} },
			  .convert = true,
			  .minval = XFS_AG_MIN_BYTES,
			  .maxval = LLONG_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SUNIT,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_NOALIGN,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SU,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SW,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SWIDTH,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_NOALIGN,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SU,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SW,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_AGSIZE,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_AGCOUNT,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .minval = XFS_AG_MIN_BYTES,
			  .maxval = XFS_AG_MAX_BYTES,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SU,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_NOALIGN,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SUNIT,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SWIDTH,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SW,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_NOALIGN,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SUNIT,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SWIDTH,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SECTLOG,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_SECTSIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_SECTSIZE,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_SECTLOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_NOALIGN,
			  .conflicts = {
				{.opt = OPT_D,
				 .subopt = D_SU,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SW,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SUNIT,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_D,
				 .subopt = D_SWIDTH,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = D_RTINHERIT,
			  .conflicts = { {LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = D_PROJINHERIT,
			  .conflicts = { {LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = D_EXTSZINHERIT,
			  .conflicts = { {LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
		},
	},
	{
		.index = OPT_I,
		.name = 'i',
		.subopts = {
			"align",
			"log",
			"maxpct",
			"perblock",
			"size",
			"attr",
			"projid32bit",
			"sparse",
			NULL
		},
		.subopt_params = {
			{ .index = I_ALIGN,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("Inodes always aligned for CRC enabled filesytems.")},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = I_LOG,
			  .conflicts = {
				{.opt = OPT_I,
				 .subopt = I_PERBLOCK,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_I,
				 .subopt = I_SIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_DINODE_MIN_LOG,
			  .maxval = XFS_DINODE_MAX_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_MAXPCT,
			  .conflicts = { {LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 100,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_PERBLOCK,
			  .conflicts = {
				{.opt = OPT_I,
				 .subopt = I_LOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_I,
				 .subopt = I_SIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .is_power_2 = true,
			  .minval = XFS_MIN_INODE_PERBLOCK,
			  .maxval = XFS_MAX_BLOCKSIZE / XFS_DINODE_MIN_SIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_SIZE,
			  .conflicts = {
				{.opt = OPT_I,
				 .subopt = I_PERBLOCK,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_I,
				 .subopt = I_LOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .is_power_2 = true,
			  .minval = XFS_DINODE_MIN_SIZE,
			  .maxval = XFS_DINODE_MAX_SIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_ATTR,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 1,
				 .message =
		N_("V2 attribute format always enabled on CRC enabled filesytems.")},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 2,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = I_PROJID32BIT,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("32 bit Project IDs always enabled on CRC enabled filesytems.")},
				{LAST_CONFLICT} },

			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = I_SPINODES,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("Sparse inodes not supported without CRC support.")},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
		},
	},
	{
		.index = OPT_L,
		.name = 'l',
		.subopts = {
			"agnum",
			"internal",
			"size",
			"version",
			"sunit",
			"su",
			"logdev",
			"sectlog",
			"sectsize",
			"file",
			"name",
			"lazy-count",
			NULL
		},
		.subopt_params = {
			{ .index = L_AGNUM,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_DEV,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_L,
				 .subopt = L_NAME,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = UINT_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_INTERNAL,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_FILE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_L,
				 .subopt = L_DEV,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_L,
				 .subopt = L_NAME,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			  .value = 1,
			},
			{ .index = L_SIZE,
			  .conflicts = { {LAST_CONFLICT} },
			  .convert = true,
			  .minval = 2 * 1024 * 1024LL,	/* XXX: XFS_MIN_LOG_BYTES */
			  .maxval = XFS_MAX_LOG_BYTES,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_VERSION,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 1,
				 .message =
		N_("V2 logs are required for CRC enabled filesystems.")},
				{LAST_CONFLICT} },
			  .minval = 1,
			  .maxval = 2,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SUNIT,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_SU,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 1,
			  .maxval = BTOBB(XLOG_MAX_RECORD_BSIZE),
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SU,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_SUNIT,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .minval = BBTOB(1),
			  .maxval = XLOG_MAX_RECORD_BSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_DEV,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_AGNUM,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_L,
				 .subopt = L_INTERNAL,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SECTLOG,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_SECTSIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_SECTSIZE,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_SECTLOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_FILE,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_INTERNAL,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = L_NAME,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_AGNUM,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_L,
				 .subopt = L_INTERNAL,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = L_LAZYSBCNTR,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("Lazy superblock counted always enabled for CRC enabled filesytems.")},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
		},
	},
	{
		.index = OPT_N,
		.name = 'n',
		.subopts = {
			"log",
			"size",
			"version",
			"ftype",
		NULL,
		},
		.subopt_params = {
			{ .index = N_LOG,
			  .conflicts = {
				{.opt = OPT_N,
				 .subopt = N_SIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_MIN_REC_DIRSIZE,
			  .maxval = XFS_MAX_BLOCKSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = N_SIZE,
			  .conflicts = {
				{.opt = OPT_N,
				 .subopt = N_LOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = 1 << XFS_MIN_REC_DIRSIZE,
			  .maxval = XFS_MAX_BLOCKSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = N_VERSION,
			  .conflicts = { {LAST_CONFLICT} },
			  .minval = 2,
			  .maxval = 2,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = N_FTYPE,
			  .conflicts = {  {.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("Cannot disable ftype with crcs enabled.")},
					  {LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
		},
	},
	{
		.index = OPT_R,
		.name = 'r',
		.subopts = {
			"extsize",
			"size",
			"rtdev",
			"file",
			"name",
			"noalign",
			NULL
		},
		.subopt_params = {
			{ .index = R_EXTSIZE,
			  .conflicts = { {LAST_CONFLICT} },
			  .convert = true,
			  .minval = XFS_MIN_RTEXTSIZE,
			  .maxval = XFS_MAX_RTEXTSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_SIZE,
			  .conflicts = { {LAST_CONFLICT} },
			  .convert = true,
			  .minval = 0,
			  .maxval = LLONG_MAX,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_DEV,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_RMAPBT,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				 .message =
		N_("rmapbt not supported without CRC support.")},
				{LAST_CONFLICT} },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_FILE,
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			  .conflicts = { {LAST_CONFLICT} },
			},
			{ .index = R_NAME,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_RMAPBT,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				 .message =
		N_("rmapbt not supported without CRC support.")},
				{LAST_CONFLICT} },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = R_NOALIGN,
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			  .conflicts = { {LAST_CONFLICT} },
			},
		},
	},
	{
		.index = OPT_S,
		.name = 's',
		.subopts = {
			"log",
			"sectlog",
			"size",
			"sectsize",
			NULL
		},
		.subopt_params = {
			{ .index = S_LOG,
			  .conflicts = {
				{.opt = OPT_S,
				 .subopt = S_SIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_S,
				 .subopt = S_SECTSIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = S_SECTLOG,
			  .conflicts = {
				{.opt = OPT_S,
				 .subopt = S_SIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_S,
				 .subopt = S_SECTSIZE,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .minval = XFS_MIN_SECTORSIZE_LOG,
			  .maxval = XFS_MAX_SECTORSIZE_LOG,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = S_SIZE,
			  .conflicts = {
				{.opt = OPT_S,
				 .subopt = S_LOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_S,
				 .subopt = S_SECTLOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = S_SECTSIZE,
			  .conflicts = {
				{.opt = OPT_S,
				 .subopt = S_LOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{.opt = OPT_S,
				 .subopt = S_SECTLOG,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				},
				{LAST_CONFLICT} },
			  .convert = true,
			  .is_power_2 = true,
			  .minval = XFS_MIN_SECTORSIZE,
			  .maxval = XFS_MAX_SECTORSIZE,
			  .flagval = SUBOPT_NEEDS_VAL,
			},
		},
	},
	{
		.index = OPT_M,
		.name = 'm',
		.subopts = {
			"crc",
			"finobt",
			"uuid",
			"rmapbt",
			"reflink",
			NULL
		},
		.subopt_params = {
			{ .index = M_CRC,
			  .conflicts = {
				{.opt = OPT_L,
				 .subopt = L_VERSION,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 1,
				 .message =
		N_("V2 logs are required for CRC enabled filesystems.")},
				{.opt = OPT_I,
				 .subopt = I_ALIGN,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("Inodes always aligned for CRC enabled filesytems.")},
				{.opt = OPT_I,
				 .subopt = I_PROJID32BIT,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("32 bit Project IDs always enabled on CRC enabled filesytems.")},
			{.opt = OPT_I,
				 .subopt = I_ATTR,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 1,
				 .message =
		N_("V2 attribute format always enabled on CRC enabled filesytems.")},
			{.opt = OPT_L,
				 .subopt = L_LAZYSBCNTR,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("Lazy superblock counted always enabled for CRC enabled filesytems.")},
				{.opt = OPT_M,
				 .subopt = M_FINOBT,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("Finobt not supported without CRC support.")},
				{.opt = OPT_M,
				 .subopt = M_RMAPBT,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("rmapbt not supported without CRC support.")},
				{.opt = OPT_M,
				 .subopt = M_REFLINK,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("reflink not supported without CRC support.")},
				{.opt = OPT_I,
				 .subopt = I_SPINODES,
				 .test_values = true,
				 .invalid_value = 1,
				 .at_value = 0,
				 .message =
		N_("Sparse inodes not supported without CRC support.")},
				{.opt = OPT_N,
				 .subopt = N_FTYPE,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("Cannot disable ftype with crcs enabled.")},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = M_FINOBT,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("Finobt not supported without CRC support\n")},
				{LAST_CONFLICT} },
			  .minval = 0,
			  .maxval = 1,
			  .flagval = 1,
			},
			{ .index = M_UUID,
			  .conflicts = { {LAST_CONFLICT} },
			  .flagval = SUBOPT_NEEDS_VAL,
			},
			{ .index = M_RMAPBT,
			.conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("rmapbt not supported without CRC support.")},
				{.opt = OPT_R,
				 .subopt = R_NAME,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				 .message =
		N_("rmapbt not supported with realtime devices.")},
				{.opt = OPT_R,
				 .subopt = R_DEV,
				 .test_values = false,
				 .invalid_value = 0,
				 .at_value = 0,
				 .message =
		N_("rmapbt not supported with realtime devices.")},
				{LAST_CONFLICT} },
			.minval = 0,
			.maxval = 1,
			.flagval = 0,
			},
			{ .index = M_REFLINK,
			  .conflicts = {
				{.opt = OPT_M,
				 .subopt = M_CRC,
				 .test_values = true,
				 .invalid_value = 0,
				 .at_value = 1,
				 .message =
		N_("reflink not supported without CRC support.")},
				{LAST_CONFLICT} },
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

static void print_conflict_struct(
	struct opt_params *opt,
	struct subopt_param *subopt,
	struct subopt_conflict *conflict);

/*
 * Use this macro before we have superblock and mount structure
 */
#define	DTOBT(d) \
	((xfs_rfsblock_t)((d) >> (get_conf_val(OPT_B, B_LOG) - BBSHIFT)))

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
 * Convert L_SU to L_SUNIT for 512 bytes blocks and check validity of the
 * values.
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

	/* verify if L_SU/L_SUNIT is a multiple block size */
	if (lsu % get_conf_val(OPT_B, B_SIZE) != 0) {
		fprintf(stderr,
_("log stripe unit (%d) must be a multiple of the block size (%"PRIu64")\n"),
		lsu, get_conf_val(OPT_B, B_SIZE));
		exit(1);
	}
	if ((BBTOB(*lsunit) % get_conf_val(OPT_B, B_SIZE) != 0)) {
		fprintf(stderr,
_("log stripe unit (%"PRIu64") must be a multiple of the block size (%"PRIu64")\n"),
		BBTOB(*lsunit), get_conf_val(OPT_B, B_SIZE));
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
	if ((logblocks << get_conf_val(OPT_B, B_LOG)) > XFS_MAX_LOG_BYTES) {
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
	 * If D_AGCOUNT is too large, make it smaller.
	 */
	if (get_conf_val(OPT_D, D_AGCOUNT) > XFS_MAX_AGNUMBER + 1) {
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
	struct opt_params	*opt,
	int			index,
	bool			str_seen)
{
	struct subopt_param	*sp = &opt->subopt_params[index];
	int			i;

	if (sp->index != index) {
		fprintf(stderr,
	("Developer screwed up option parsing (%d/%d)! Please report!\n"),
			sp->index, index);
		reqval(opt->name, (char **)opt->subopts, index);
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
			respec(opt->name, (char **)opt->subopts, index);
		sp->seen = true;
	} else {
		if (sp->str_seen)
			respec(opt->name, (char **)opt->subopts, index);
		sp->str_seen = true;
	}

	/* check for conflicts with the option */
	for (i = 0; i < MAX_CONFLICTS; i++) {
		struct subopt_conflict conflict_opt = sp->conflicts[i];

		if (conflict_opt.opt == LAST_CONFLICT)
			break;
		if (conflict_opt.test_values)
			break;
		if (opts[conflict_opt.opt].subopt_params[conflict_opt.subopt].seen ||
		    opts[conflict_opt.opt].subopt_params[conflict_opt.subopt].str_seen) {
			print_conflict_struct(opt, sp, &conflict_opt);
		}
	}
}

/*
 * Check for conflict values between options.
 */
static void
check_opt_value(
	struct opt_params	*opt,
	int			index,
	long long 		value)
{
	struct subopt_param	*sp = &opt->subopt_params[index];
	int			i;

	/* check for conflicts with the option */
	for (i = 0; i < MAX_CONFLICTS; i++) {
		struct subopt_conflict conflict_opt = sp->conflicts[i];

		if (conflict_opt.opt == LAST_CONFLICT)
			break;
		if (!conflict_opt.test_values)
			break;
		if ((opts[conflict_opt.opt].subopt_params[conflict_opt.subopt].seen ||
		     opts[conflict_opt.opt].subopt_params[conflict_opt.subopt].str_seen) &&
		    opts[conflict_opt.opt].subopt_params[conflict_opt.subopt].value
				== conflict_opt.invalid_value &&
		    value == conflict_opt.at_value) {
			print_conflict_struct(opt, sp, &conflict_opt);
		}
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
		ret = cvtnum(get_conf_val(OPT_B, B_SIZE),
			   get_conf_val(OPT_D, D_SECTSIZE), str, &c);
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

	check_opt_value(opts, index, c);

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
	xfs_agf_t		*agf;
	xfs_agi_t		*agi;
	xfs_agnumber_t		agno;
	xfs_alloc_rec_t		*arec;
	struct xfs_btree_block	*block;
	bool			blflag;
	bool			bsflag;
	uint64_t		bsize;
	xfs_buf_t		*buf;
	uint64_t		c;
	bool			daflag;
	uint64_t		dasize;
	xfs_rfsblock_t		dblocks;
	char			*dfile;
	bool			force_overwrite;
	struct fsxattr		fsx;
	bool			ilflag;
	bool			imflag;
	bool			ipflag;
	bool			isflag;
	char			*label = NULL;
	bool			laflag;
	uint64_t		lalign;
	bool			ldflag;
	bool			liflag;
	xfs_rfsblock_t		logblocks;
	char			*logfile;
	xfs_fsblock_t		logstart;
	bool			lvflag;
	bool			lsflag;
	bool			lsuflag;
	bool			lsunitflag;
	bool			lslflag;
	bool			lssflag;
	uint64_t		min_logblocks;
	xfs_mount_t		*mp;
	xfs_mount_t		mbuf;
	xfs_extlen_t		nbmblocks;
	bool			nlflag;
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
	xfs_extlen_t		rtextblocks;
	xfs_rtblock_t		rtextents;
	char			*rtfile;
	xfs_sb_t		*sbp;
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
	dasize = dblocks = 0;
	daflag = ilflag = imflag = ipflag = isflag = false;
	liflag = laflag = lsflag = lsuflag = lsunitflag = ldflag = lvflag = false;
	logblocks = rtblocks = rtextblocks = 0;
	Nflag = nlflag = nsflag = nvflag = false;
	qflag = false;
	dfile = logfile = rtfile = NULL;
	protofile = NULL;
	lalign = 0;
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
			uint64_t tmp;
			while (*p != '\0') {
				char	**subopts =
						(char **)opts[OPT_B].subopts;
				char	*value;

				switch (getsubopt(&p, subopts, &value)) {
				case B_LOG:
					tmp = parse_conf_val(OPT_B, B_LOG,
							     value);
					set_conf_val(OPT_B, B_SIZE, 1 << tmp);
					blflag = 1;
					break;
				case B_SIZE:
					tmp = parse_conf_val(OPT_B, B_SIZE,
							     value);
					set_conf_val(OPT_B, B_LOG,
						libxfs_highbit32(tmp));
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
				uint64_t tmp;


				switch (getsubopt(&p, subopts, &value)) {
				case D_AGCOUNT:
					parse_conf_val(OPT_D, D_AGCOUNT,
						       value);
					daflag = 1;
					break;
				case D_AGSIZE:
					parse_conf_val(OPT_D, D_AGSIZE, value);
					dasize = 1;
					break;
				case D_FILE:
					xi.disfile = parse_conf_val(OPT_D,
								    D_FILE,
								    value);
					break;
				case D_NAME:
					xi.dname = getstr(value, &opts[OPT_D],
								D_NAME);
					set_conf_val(OPT_D, D_NAME,  1);
					break;
				case D_SIZE:
					parse_conf_val(OPT_D, D_SIZE, value);
					break;
				case D_SUNIT:
					parse_conf_val(OPT_D, D_SUNIT, value);
					break;
				case D_SWIDTH:
					parse_conf_val(OPT_D, D_SWIDTH, value);
					break;
				case D_SU:
					parse_conf_val(OPT_D, D_SU, value);
					break;
				case D_SW:
					parse_conf_val(OPT_D, D_SW, value);
					break;
				case D_NOALIGN:
					parse_conf_val(OPT_D, D_NOALIGN,
						       value);
					break;
				case D_SECTLOG:
					/*
					 * we can set sector size both as
					 * -d sectsize/sectlog and as -s option.
					 * Thus we need to fill the same value
					 * into multiple options to make sure
					 * that we are doing the parsing right.
					 * Only D_SECTSIZE/D_SECTLOG is used
					 * later in the code, though.
					 */
					tmp = parse_conf_val(OPT_D, D_SECTLOG,
							     value);

					set_conf_val(OPT_S, S_LOG, tmp);
					set_conf_val(OPT_S, S_SECTLOG, tmp);
					set_conf_val(OPT_D, D_SECTSIZE,
						     1 << tmp);
					set_conf_val(OPT_S, S_SECTSIZE,
						     1 << tmp);
					set_conf_val(OPT_S, S_SIZE,
						     1 << tmp);
					slflag = 1;
					break;
				case D_SECTSIZE:
					/*
					 * we can set sector size both as
					 * -d sectsize/sectlog and as -s option.
					 * Thus we need to fill the same value
					 * into multiple options to make sure
					 * that we are doing the parsing right.
					 * Only D_SECTSIZE/D_SECTLOG is used
					 * later in the code, though.
					 */
					tmp = parse_conf_val(OPT_D, D_SECTSIZE,
							     value);

					set_conf_val(OPT_S, S_SIZE, tmp);
					set_conf_val(OPT_S, S_SECTSIZE, tmp);
					set_conf_val(OPT_D, D_SECTLOG,
						libxfs_highbit32(tmp));
					set_conf_val(OPT_S, S_LOG,
						libxfs_highbit32(tmp));
					set_conf_val(OPT_S, S_SECTLOG,
						libxfs_highbit32(tmp));

					ssflag = 1;
					break;
				case D_RTINHERIT:
					c = parse_conf_val(OPT_D, D_RTINHERIT,
							   value);
					if (c)
						fsx.fsx_xflags |=
							XFS_DIFLAG_RTINHERIT;
					break;
				case D_PROJINHERIT:
					fsx.fsx_projid =
						parse_conf_val(OPT_D,
							       D_PROJINHERIT,
							       value);
					fsx.fsx_xflags |=
						XFS_DIFLAG_PROJINHERIT;
					break;
				case D_EXTSZINHERIT:
					fsx.fsx_extsize =
						parse_conf_val(OPT_D,
							       D_EXTSZINHERIT,
							       value);
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
				uint64_t tmp;

				switch (getsubopt(&p, subopts, &value)) {
				case I_ALIGN:
					sb_feat.inode_align =
						parse_conf_val(OPT_I, I_ALIGN,
							       value);
					break;
				case I_LOG:
					tmp = parse_conf_val(OPT_I, I_LOG,
							     value);
					set_conf_val(OPT_I, I_SIZE, 1 << tmp);
					ilflag = 1;
					break;
				case I_MAXPCT:
					parse_conf_val(OPT_I, I_MAXPCT, value);
					imflag = 1;
					break;
				case I_PERBLOCK:
					parse_conf_val(OPT_I, I_PERBLOCK,
						       value);
					ipflag = 1;
					break;
				case I_SIZE:
					tmp = parse_conf_val(OPT_I, I_SIZE,
							       value);
					set_conf_val(OPT_I, I_LOG,
						     libxfs_highbit32(tmp));
					isflag = 1;
					break;
				case I_ATTR:
					sb_feat.attr_version =
						parse_conf_val(OPT_I, I_ATTR,
							       value);
					break;
				case I_PROJID32BIT:
					sb_feat.projid16bit =
						!getnum(value, &opts[OPT_I],
							I_PROJID32BIT);
					set_conf_val(OPT_I, I_PROJID32BIT,
						     sb_feat.projid16bit);
					break;
				case I_SPINODES:
					sb_feat.spinodes =
						parse_conf_val(OPT_I,
							       I_SPINODES,
							       value);
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
					parse_conf_val(OPT_L, L_AGNUM, value);
					laflag = 1;
					break;
				case L_FILE:
					xi.lisfile = parse_conf_val(OPT_L,
								    L_FILE,
								    value);
					break;
				case L_INTERNAL:
					parse_conf_val(OPT_L, L_INTERNAL,
						       value);
					liflag = 1;
					break;
				case L_SU:
					parse_conf_val(OPT_L, L_SU, value);
					lsuflag = 1;
					break;
				case L_SUNIT:
					parse_conf_val(OPT_L, L_SUNIT, value);
					lsunitflag = 1;
					break;
				case L_NAME:
				case L_DEV:
					logfile = getstr(value, &opts[OPT_L],
								L_NAME);
					xi.logname = logfile;
					ldflag = 1;
					set_conf_val(OPT_L, L_INTERNAL, 0);
					set_conf_val(OPT_L, L_NAME, 1);
					set_conf_val(OPT_L, L_DEV, 1);
					break;
				case L_VERSION:
					sb_feat.log_version =
						parse_conf_val(OPT_L,
							       L_VERSION,
							       value);
					lvflag = 1;
					set_conf_val(OPT_L, L_VERSION,
						     sb_feat.log_version);
					break;
				case L_SIZE:
					parse_conf_val(OPT_L, L_SIZE, value);
					break;
				case L_SECTLOG:
					tmp = parse_conf_val(OPT_L,
								    L_SECTLOG,
								    value);
					set_conf_val(OPT_L, L_SECTSIZE,
						     1 << tmp);
					lslflag = 1;
					break;
				case L_SECTSIZE:
					tmp =
						parse_conf_val(OPT_L,
							       L_SECTSIZE,
							       value);
					set_conf_val(OPT_L, L_SECTLOG,
						     libxfs_highbit32(tmp));
					lssflag = 1;
					break;
				case L_LAZYSBCNTR:
					sb_feat.lazy_sb_counters =
						parse_conf_val(OPT_L,
							       L_LAZYSBCNTR,
							       value);
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
						parse_conf_val(OPT_M, M_CRC,
							       value);
					if (sb_feat.crcs_enabled)
						sb_feat.dirftype = true;
					break;
				case M_FINOBT:
					sb_feat.finobt =
						parse_conf_val(OPT_M, M_FINOBT,
							       value);
					set_conf_val(OPT_M, M_FINOBT,
						     sb_feat.finobt);
					break;
				case M_UUID:
					if (!value || *value == '\0')
						reqval('m', subopts, M_UUID);
					if (platform_uuid_parse(value, &uuid))
						illegal(optarg, "m uuid");
					set_conf_val(OPT_M, M_UUID, 1);
					break;
				case M_RMAPBT:
					sb_feat.rmapbt =
						parse_conf_val(OPT_M, M_RMAPBT,
							       value);
					break;
				case M_REFLINK:
					sb_feat.reflink =
						parse_conf_val(OPT_M,
							       M_REFLINK,
							       value);
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
				uint64_t tmp;

				switch (getsubopt(&p, subopts, &value)) {
				case N_LOG:
					tmp = parse_conf_val(OPT_N,
								     N_LOG,
								     value);
					set_conf_val(OPT_N, N_SIZE, 1 << tmp);
					nlflag = 1;
					break;
				case N_SIZE:
					tmp = parse_conf_val(OPT_N,
								      N_SIZE,
								      value);
					set_conf_val(OPT_N, N_LOG,
						     libxfs_highbit32(tmp));
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
					set_conf_val(OPT_N, N_VERSION,
						     sb_feat.dir_version);
					break;
				case N_FTYPE:
					sb_feat.dirftype =
						parse_conf_val(OPT_N, N_FTYPE,
							       value);
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
					parse_conf_val(OPT_R, R_EXTSIZE,
						       value);
					break;
				case R_FILE:
					xi.risfile = parse_conf_val(OPT_R,
								    R_FILE,
								    value);
					break;
				case R_NAME:
				case R_DEV:
					xi.rtname = getstr(value, &opts[OPT_R],
							   R_NAME);
					set_conf_val(OPT_R, R_NAME, 1);
					set_conf_val(OPT_R, R_DEV, 1);
					break;
				case R_SIZE:
					parse_conf_val(OPT_R, R_SIZE, value);
					break;
				case R_NOALIGN:
					parse_conf_val(OPT_R, R_NOALIGN,
						       value);
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
				uint64_t tmp;
				/*
				 * we can set sector size both as
				 * -d sectsize/sectlog and as -s option.
				 * Thus we need to fill the same value
				 * into multiple options to make sure
				 * that we are doing the parsing right.
				 * Only D_SECTSIZE/D_SECTLOG is used
				 * later in the code, though.
				 */

				switch (getsubopt(&p, subopts, &value)) {
				case S_LOG:
				case S_SECTLOG:
					if (lssflag)
						conflict('s', subopts,
							 S_SECTSIZE,
							 S_SECTLOG);
					tmp = parse_conf_val(OPT_S,
								   S_SECTLOG,
								   value);

					set_conf_val(OPT_S, S_LOG, tmp);
					set_conf_val(OPT_D, D_SECTLOG, tmp);
					set_conf_val(OPT_L, L_SECTLOG, tmp);
					set_conf_val(OPT_D, D_SECTSIZE,
						     1 << tmp);
					set_conf_val(OPT_S, S_SIZE, 1 << tmp);
					set_conf_val(OPT_S, S_SECTSIZE,
						     1 << tmp);

					set_conf_val(OPT_L, L_SECTSIZE, tmp);
					lslflag = slflag = 1;
					break;
				case S_SIZE:
				case S_SECTSIZE:
					if (lslflag)
						conflict('s', subopts,
							 S_SECTLOG,
							 S_SECTSIZE);
					tmp = parse_conf_val(OPT_S,
								    S_SECTSIZE,
								    value);

					set_conf_val(OPT_S, S_SIZE, tmp);
					set_conf_val(OPT_D, D_SECTSIZE, tmp);
					set_conf_val(OPT_L, L_SECTSIZE, tmp);
					set_conf_val(OPT_D, D_SECTLOG,
						libxfs_highbit32(tmp));
					set_conf_val(OPT_S, S_LOG,
						libxfs_highbit32(tmp));
					set_conf_val(OPT_S, S_SECTLOG,
						libxfs_highbit32(tmp));

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
	 * Blocksize and D_SECTSIZE first, other things depend on them
	 * For RAID4/5/6 we want to align sector size and block size,
	 * so we need to start with the device geometry extraction too.
	 */
	if (!blflag && !bsflag) {
		set_conf_val(OPT_B, B_LOG, XFS_DFL_BLOCKSIZE_LOG);
		set_conf_val(OPT_B, B_SIZE, 1 << XFS_DFL_BLOCKSIZE_LOG);
	}
	if (get_conf_val(OPT_B, B_SIZE) < XFS_MIN_BLOCKSIZE ||
	    get_conf_val(OPT_B, B_SIZE) > XFS_MAX_BLOCKSIZE) {
		fprintf(stderr, _("illegal block size %"PRIu64"\n"),
			get_conf_val(OPT_B, B_SIZE));
		usage();
	}
	if (sb_feat.crcs_enabled &&
	    get_conf_val(OPT_B, B_SIZE) < XFS_MIN_CRC_BLOCKSIZE) {
		fprintf(stderr,
_("Minimum block size for CRC enabled filesystems is %d bytes.\n"),
			XFS_MIN_CRC_BLOCKSIZE);
		usage();
	}

	/*
	 * If user explicitly stated -m crc=1 -n ftype=0, an error was already
	 * issued. But if -n ftype=0 was stated alone, then it is a conflict
	 * with a default value for crc enabled and has to be detected here.
	 */
	if (sb_feat.crcs_enabled && !sb_feat.dirftype) {
		fprintf(stderr, _("cannot disable ftype with crcs enabled\n"));
		usage();
	}
	if (!slflag && !ssflag) {
		set_conf_val(OPT_D, D_SECTLOG, XFS_MIN_SECTORSIZE_LOG);
		set_conf_val(OPT_D, D_SECTSIZE, XFS_MIN_SECTORSIZE);
	}
	if (!lslflag && !lssflag) {
		set_conf_val(OPT_L, L_SECTLOG, get_conf_val(OPT_D, D_SECTLOG));
		set_conf_val(OPT_L, L_SECTSIZE,
			     get_conf_val(OPT_D, D_SECTSIZE));
	}

	/*
	 * Before anything else, verify that we are correctly operating on
	 * files or block devices and set the control parameters correctly.
	 * Explicitly disable direct IO for image files so we don't error out on
	 * sector size mismatches between the new filesystem and the underlying
	 * host filesystem.
	 */
	check_device_type(dfile, &xi.disfile, !get_conf_val(OPT_D, D_SIZE),
			  !dfile,
			  Nflag ? NULL : &xi.dcreat, force_overwrite, "d");
	if (!get_conf_val(OPT_L, L_INTERNAL))
		check_device_type(xi.logname, &xi.lisfile,
				  !get_conf_val(OPT_L, L_SIZE),
				  !xi.logname, Nflag ? NULL : &xi.lcreat,
				  force_overwrite, "l");
	if (xi.rtname)
		check_device_type(xi.rtname,
				  &xi.risfile,
				  !get_conf_val(OPT_R, R_SIZE),
				  !xi.rtname,
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

		set_conf_val(OPT_D, D_SECTSIZE,
			     ft.psectorsize ? ft.psectorsize :
					      XFS_MIN_SECTORSIZE);

		if ((get_conf_val(OPT_B, B_SIZE) <
		     get_conf_val(OPT_D, D_SECTSIZE)) &&
		    (get_conf_val(OPT_B, B_SIZE) >= ft.lsectorsize)) {
			fprintf(stderr,
_("specified blocksize %"PRIu64" is less than device physical sector size %d\n"),
				get_conf_val(OPT_B, B_SIZE), ft.psectorsize);
			fprintf(stderr,
_("switching to logical sector size %d\n"),
				ft.lsectorsize);
			set_conf_val(OPT_D, D_SECTSIZE,
				     ft.lsectorsize ? ft.lsectorsize :
						      XFS_MIN_SECTORSIZE);
		}
	}

	if (!ssflag) {
		set_conf_val(OPT_D, D_SECTLOG,
			     libxfs_highbit32(get_conf_val(OPT_D, D_SECTSIZE)));
		if (get_conf_val(OPT_L, L_INTERNAL)) {
			set_conf_val(OPT_L, L_SECTSIZE,
				     get_conf_val(OPT_D, D_SECTSIZE));
			set_conf_val(OPT_L, L_SECTLOG,
				     get_conf_val(OPT_D, D_SECTLOG));
		}
	}

	if (get_conf_val(OPT_D, D_SECTSIZE) < XFS_MIN_SECTORSIZE ||
	    get_conf_val(OPT_D, D_SECTSIZE) > XFS_MAX_SECTORSIZE ||
	    get_conf_val(OPT_D, D_SECTSIZE) > get_conf_val(OPT_B, B_SIZE)) {
		if (ssflag)
			fprintf(stderr, _("illegal sector size %"PRIu64"\n"),
				get_conf_val(OPT_D, D_SECTSIZE));
		else
			fprintf(stderr,
_("block size %"PRIu64" cannot be smaller than logical sector size %d\n"),
				get_conf_val(OPT_B, B_SIZE), ft.lsectorsize);
		usage();
	}
	if (get_conf_val(OPT_D, D_SECTSIZE) < ft.lsectorsize) {
		fprintf(stderr, _("illegal sector size %"PRIu64"; hw sector is %d\n"),
			get_conf_val(OPT_D, D_SECTSIZE), ft.lsectorsize);
		usage();
	}
	if (get_conf_val(OPT_L, L_SECTSIZE) < XFS_MIN_SECTORSIZE ||
	    get_conf_val(OPT_L, L_SECTSIZE) > XFS_MAX_SECTORSIZE ||
	    get_conf_val(OPT_L, L_SECTSIZE) > get_conf_val(OPT_B, B_SIZE)) {
		fprintf(stderr, _("illegal log sector size %"PRIu64"\n"),
			get_conf_val(OPT_L, L_SECTSIZE));
		usage();
	} else if (get_conf_val(OPT_L, L_SECTSIZE) > XFS_MIN_SECTORSIZE &&
		   !get_conf_val(OPT_L, L_SU) &&
		   !get_conf_val(OPT_L, L_SUNIT)) {
		set_conf_val(OPT_L, L_SU, get_conf_val(OPT_B, B_SIZE));
		sb_feat.log_version = 2;
	}

	/*
	 * Now we have blocks and sector sizes set up, check parameters that are
	 * no longer optional for CRC enabled filesystems.  Catch them up front
	 * here before doing anything else.
	 */
	if (sb_feat.crcs_enabled) {
		/* minimum inode size is 512 bytes, ipflag checked later */
		if ((isflag || ilflag) &&
		    get_conf_val(OPT_I, I_LOG) < XFS_DINODE_DFL_CRC_LOG) {
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
		if (get_conf_val(OPT_N, N_SIZE) <
		    get_conf_val(OPT_B, B_SIZE) ||
		    get_conf_val(OPT_N, N_SIZE) > XFS_MAX_BLOCKSIZE) {
			fprintf(stderr, _("illegal directory block size %"PRIu64"\n"),
				get_conf_val(OPT_N, N_SIZE));
			usage();
		}
	} else {
		if (get_conf_val(OPT_B, B_SIZE) < (1 << XFS_MIN_REC_DIRSIZE))
			set_conf_val(OPT_N, N_LOG, XFS_MIN_REC_DIRSIZE);
		else
			set_conf_val(OPT_N, N_LOG, get_conf_val(OPT_B, B_LOG));
		set_conf_val(OPT_N, N_SIZE, 1 << get_conf_val(OPT_N, N_LOG));
	}


	if (get_conf_val(OPT_D, D_SIZE)) {
		if (get_conf_val(OPT_D, D_SIZE) % XFS_MIN_BLOCKSIZE) {
			fprintf(stderr,
			_("illegal data length %"PRIu64", not a multiple of %d\n"),
				get_conf_val(OPT_D, D_SIZE), XFS_MIN_BLOCKSIZE);
			usage();
		}
		dblocks = (xfs_rfsblock_t)(get_conf_val(OPT_D, D_SIZE) >>
					   get_conf_val(OPT_B, B_LOG));
		if (get_conf_val(OPT_D, D_SIZE) % get_conf_val(OPT_B, B_SIZE))
			fprintf(stderr, _("warning: "
	"data length %"PRIu64" not a multiple of %"PRIu64", truncated to %"PRIu64"\n"),
				get_conf_val(OPT_D, D_SIZE),
				get_conf_val(OPT_B, B_SIZE),
				(uint64_t)(dblocks <<
					   get_conf_val(OPT_B, B_LOG)));
	}
	if (ipflag) {
		set_conf_val(OPT_I, I_LOG, get_conf_val(OPT_B, B_LOG) -
			libxfs_highbit32(get_conf_val(OPT_I, I_PERBLOCK)));
		set_conf_val(OPT_I, I_SIZE, 1 << get_conf_val(OPT_I, I_LOG));
	} else if (!ilflag && !isflag) {
		set_conf_val(OPT_I, I_LOG,
			     sb_feat.crcs_enabled ? XFS_DINODE_DFL_CRC_LOG
						: XFS_DINODE_DFL_LOG);
		set_conf_val(OPT_I, I_SIZE, 1 << get_conf_val(OPT_I, I_LOG));
	}
	if (sb_feat.crcs_enabled && get_conf_val(OPT_I, I_LOG) <
	    XFS_DINODE_DFL_CRC_LOG) {
		fprintf(stderr,
		_("Minimum inode size for CRCs is %d bytes\n"),
			1 << XFS_DINODE_DFL_CRC_LOG);
		usage();
	}

	if (get_conf_val(OPT_L, L_SIZE)) {
		if (get_conf_val(OPT_L, L_SIZE) % XFS_MIN_BLOCKSIZE) {
			fprintf(stderr,
			_("illegal log length %"PRIu64", not a multiple of %d\n"),
				get_conf_val(OPT_L, L_SIZE), XFS_MIN_BLOCKSIZE);
			usage();
		}
		logblocks = (xfs_rfsblock_t)(get_conf_val(OPT_L, L_SIZE) >>
					     get_conf_val(OPT_B, B_LOG));
		if (get_conf_val(OPT_L, L_SIZE) % get_conf_val(OPT_B, B_SIZE))
			fprintf(stderr,
	_("warning: log length %"PRIu64" not a multiple of %"PRIu64", truncated to %"PRIu64"\n"),
				get_conf_val(OPT_L, L_SIZE),
				get_conf_val(OPT_B, B_SIZE),
				(uint64_t)(logblocks <<
					   get_conf_val(OPT_B, B_LOG)));
	}
	if (get_conf_val(OPT_R, R_SIZE)) {
		if (get_conf_val(OPT_R, R_SIZE) % XFS_MIN_BLOCKSIZE) {
			fprintf(stderr,
			_("illegal rt length %"PRIu64", not a multiple of %d\n"),
				get_conf_val(OPT_R, R_SIZE), XFS_MIN_BLOCKSIZE);
			usage();
		}
		rtblocks = (xfs_rfsblock_t)(get_conf_val(OPT_R, R_SIZE) >>
					    get_conf_val(OPT_B, B_LOG));
		if (get_conf_val(OPT_R, R_SIZE) % get_conf_val(OPT_B, B_SIZE))
			fprintf(stderr,
	_("warning: rt length %"PRIu64" not a multiple of %"PRIu64", truncated to %"PRIu64"\n"),
				get_conf_val(OPT_R, R_SIZE),
				get_conf_val(OPT_B, B_SIZE),
				(uint64_t)(rtblocks <<
					   get_conf_val(OPT_B, B_LOG)));
	}
	/*
	 * If specified, check rt extent size against its constraints.
	 */
	if (get_conf_val(OPT_R, R_EXTSIZE)) {
		if (get_conf_val(OPT_R, R_EXTSIZE) %
		    get_conf_val(OPT_B, B_SIZE)) {
			fprintf(stderr,
		_("illegal rt extent size %"PRIu64", not a multiple of %"PRIu64"\n"),
				get_conf_val(OPT_R, R_EXTSIZE),
				get_conf_val(OPT_B, B_SIZE));
			usage();
		}
		rtextblocks = (xfs_extlen_t)(get_conf_val(OPT_R, R_EXTSIZE) >>
					     get_conf_val(OPT_B, B_LOG));
	} else {
		/*
		 * If realtime extsize has not been specified by the user,
		 * and the underlying volume is striped, then set rtextblocks
		 * to the stripe width.
		 */
		uint64_t	rswidth;

		if (!get_conf_val(OPT_R, R_NOALIGN) && !xi.risfile &&
		    !(!get_conf_val(OPT_R, R_SIZE) && xi.disfile))
			rswidth = ft.rtswidth;
		else
			rswidth = 0;

		/* check that rswidth is a multiple of fs B_SIZE */
		if (!get_conf_val(OPT_R, R_NOALIGN) && rswidth &&
		    !(BBTOB(rswidth) % get_conf_val(OPT_B, B_SIZE))) {
			rswidth = DTOBT(rswidth);
			set_conf_val(OPT_R, R_EXTSIZE,
				     rswidth << get_conf_val(OPT_B, B_LOG));
			if (XFS_MIN_RTEXTSIZE <=
			    get_conf_val(OPT_R, R_EXTSIZE) &&
			    (get_conf_val(OPT_R, R_EXTSIZE) <=
			     XFS_MAX_RTEXTSIZE)) {
				rtextblocks = rswidth;
			}
		}
		if (!rtextblocks) {
			rtextblocks = (get_conf_val(OPT_B, B_SIZE) <
				       XFS_MIN_RTEXTSIZE) ?
				       XFS_MIN_RTEXTSIZE >>
				       get_conf_val(OPT_B, B_LOG) : 1;
		}
	}
	ASSERT(rtextblocks);

	/*
	 * Check some argument sizes against mins, maxes.
	 */
	if (get_conf_val(OPT_I, I_SIZE) >
	    get_conf_val(OPT_B, B_SIZE) / XFS_MIN_INODE_PERBLOCK ||
	    get_conf_val(OPT_I, I_SIZE) < XFS_DINODE_MIN_SIZE ||
	    get_conf_val(OPT_I, I_SIZE) > XFS_DINODE_MAX_SIZE) {
		int	maxsz;

		fprintf(stderr, _("illegal inode size %"PRIu64"\n"),
			get_conf_val(OPT_I, I_SIZE));
		maxsz = MIN(get_conf_val(OPT_B, B_SIZE) /
			    XFS_MIN_INODE_PERBLOCK,
			    XFS_DINODE_MAX_SIZE);
		if (XFS_DINODE_MIN_SIZE == maxsz)
			fprintf(stderr,
			_("allowable inode size with %"PRIu64" byte blocks is %d\n"),
				get_conf_val(OPT_B, B_SIZE),
				XFS_DINODE_MIN_SIZE);
		else
			fprintf(stderr,
	_("allowable inode size with %"PRIu64" byte blocks is between %d and %d\n"),
				get_conf_val(OPT_B, B_SIZE),
				XFS_DINODE_MIN_SIZE, maxsz);
		exit(1);
	}

	/* if L_SU or L_SUNIT was specified, automatically use v2 logs */
	if ((get_conf_val(OPT_L, L_SU) ||
	     get_conf_val(OPT_L, L_SUNIT)) && sb_feat.log_version == 1) {
		fprintf(stderr,
			_("log stripe unit specified, using v2 logs\n"));
		sb_feat.log_version = 2;
	}

	uint64_t dsunit = get_conf_val(OPT_D, D_SUNIT);
	uint64_t dswidth = get_conf_val(OPT_D, D_SWIDTH);
	uint64_t lsunit = get_conf_val(OPT_L, L_SUNIT);

	calc_stripe_factors(get_conf_val(OPT_D, D_SU),
			    get_conf_val(OPT_D, D_SW),
			    get_conf_val(OPT_D, D_SECTSIZE),
			    get_conf_val(OPT_L, L_SU),
			    get_conf_val(OPT_L, L_SECTSIZE),
			    &dsunit, &dswidth, &lsunit);

	set_conf_val(OPT_D, D_SUNIT, dsunit);
	set_conf_val(OPT_D, D_SWIDTH, dswidth);
	set_conf_val(OPT_L, L_SUNIT, lsunit);

	xi.setblksize = get_conf_val(OPT_D, D_SECTSIZE);

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

	sector_mask = (uint64_t)-1 <<
		(MAX(get_conf_val(OPT_D, D_SECTLOG), 10) - BBSHIFT);
	xi.dsize &= sector_mask;
	xi.rtsize &= sector_mask;
	xi.logBBsize &= (uint64_t)-1 << (MAX(get_conf_val(OPT_L, L_SECTLOG),
					     10) - BBSHIFT);


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
		set_conf_val(OPT_L, L_INTERNAL, xi.logdev == 0);
	if (xi.logname)
		logfile = xi.logname;
	else if (get_conf_val(OPT_L, L_INTERNAL))
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
	if (get_conf_val(OPT_D, D_SIZE) &&
	    xi.dsize > 0 && dblocks > DTOBT(xi.dsize)) {
		fprintf(stderr,
			_("size %s specified for data subvolume is too large, "
			"maximum is %"PRIu64" blocks\n"),
			get_conf_raw(OPT_D, D_SIZE),
			(uint64_t)DTOBT(xi.dsize));
		usage();
	} else if (!get_conf_val(OPT_D, D_SIZE) && xi.dsize > 0)
		dblocks = DTOBT(xi.dsize);
	else if (!get_conf_val(OPT_D, D_SIZE)) {
		fprintf(stderr, _("can't get size of data subvolume\n"));
		usage();
	}
	if (dblocks < XFS_MIN_DATA_BLOCKS) {
		fprintf(stderr,
	_("size %"PRIu64" of data subvolume is too small, minimum %d blocks\n"),
			dblocks, XFS_MIN_DATA_BLOCKS);
		usage();
	}

	if (get_conf_val(OPT_L, L_INTERNAL) && xi.logdev) {
		fprintf(stderr,
			_("can't have both external and internal logs\n"));
		usage();
	} else if (get_conf_val(OPT_L, L_INTERNAL) &&
		   get_conf_val(OPT_D, D_SECTSIZE) !=
		   get_conf_val(OPT_L, L_SECTSIZE)) {
		fprintf(stderr,
	_("data and log sector sizes must be equal for internal logs\n"));
		usage();
	}

	if (xi.dbsize > get_conf_val(OPT_D, D_SECTSIZE)) {
		fprintf(stderr, _(
"Warning: the data subvolume sector size %"PRIu64" is less than the sector size \n\
reported by the device (%u).\n"),
			get_conf_val(OPT_D, D_SECTSIZE), xi.dbsize);
	}
	if (!get_conf_val(OPT_L, L_INTERNAL) &&
	    xi.lbsize > get_conf_val(OPT_L, L_SECTSIZE)) {
		fprintf(stderr, _(
"Warning: the log subvolume sector size %"PRIu64" is less than the sector size\n\
reported by the device (%u).\n"),
			get_conf_val(OPT_L, L_SECTSIZE), xi.lbsize);
	}
	if (get_conf_val(OPT_R, R_SIZE) && xi.rtsize > 0 &&
	    xi.rtbsize > get_conf_val(OPT_D, D_SECTSIZE)) {
		fprintf(stderr, _(
"Warning: the realtime subvolume sector size %"PRIu64" is less than the sector size\n\
reported by the device (%u).\n"),
			get_conf_val(OPT_D, D_SECTSIZE), xi.rtbsize);
	}

	if (get_conf_val(OPT_R, R_SIZE) && xi.rtsize > 0 &&
	    rtblocks > DTOBT(xi.rtsize)) {
		fprintf(stderr,
			_("size %s specified for rt subvolume is too large, "
			"maximum is %"PRIu64" blocks\n"),
			get_conf_raw(OPT_R, R_SIZE),
			(uint64_t)DTOBT(xi.rtsize));
		usage();
	} else if (!get_conf_val(OPT_R, R_SIZE) && xi.rtsize > 0)
		rtblocks = DTOBT(xi.rtsize);
	else if (get_conf_val(OPT_R, R_SIZE) && !xi.rtdev) {
		fprintf(stderr,
			_("size specified for non-existent rt subvolume\n"));
		usage();
	}
	if (xi.rtdev) {
		rtextents = rtblocks / rtextblocks;
		nbmblocks = (xfs_extlen_t)howmany(rtextents, NBBY *
						  get_conf_val(OPT_B, B_SIZE));
	} else {
		rtextents = rtblocks = 0;
		nbmblocks = 0;
	}

	if (!get_conf_val(OPT_D, D_NOALIGN)) {
		if (get_conf_val(OPT_D, D_SUNIT)) {
			if (ft.dsunit &&
			    ft.dsunit != get_conf_val(OPT_D, D_SUNIT)) {
				fprintf(stderr,
					_("%s: Specified data stripe unit %"PRIu64" "
					"is not the same as the volume stripe "
					"unit %d\n"),
					progname,
					get_conf_val(OPT_D, D_SUNIT),
					ft.dsunit);
			}
			if (ft.dswidth &&
			    ft.dswidth != get_conf_val(OPT_D, D_SWIDTH)) {
				fprintf(stderr,
					_("%s: Specified data stripe width %"PRIu64" "
					"is not the same as the volume stripe "
					"width %d\n"),
					progname,
					get_conf_val(OPT_D, D_SWIDTH),
					ft.dswidth);
			}
		} else {
			set_conf_val(OPT_D, D_SUNIT, ft.dsunit);
			set_conf_val(OPT_D, D_SWIDTH, ft.dswidth);
			set_conf_val(OPT_D, D_NOALIGN, 1);
		}
	} /* else D_SUNIT & D_SWIDTH can't be set if D_NOALIGN is set */

	if (dasize) {		/* User-specified AG size */
		/*
		 * Check specified D_AGSIZE is a multiple of B_SIZE.
		 */
		if (get_conf_val(OPT_D, D_AGSIZE) %
		    get_conf_val(OPT_B, B_SIZE)) {
			fprintf(stderr,
		_("agsize (%"PRIu64") not a multiple of fs blk size (%"PRIu64")\n"),
				get_conf_val(OPT_D, D_AGSIZE),
				get_conf_val(OPT_B, B_SIZE));
			usage();
		}
		set_conf_val(OPT_D, D_AGSIZE,
			     get_conf_val(OPT_D, D_AGSIZE) /
			     get_conf_val(OPT_B, B_SIZE));
		set_conf_val(OPT_D, D_AGCOUNT,
			     dblocks / get_conf_val(OPT_D, D_AGSIZE) +
			     (dblocks % get_conf_val(OPT_D, D_AGSIZE) != 0));

	} else if (daflag) {	/* User-specified AG count */
		set_conf_val(OPT_D, D_AGSIZE,
			     dblocks / get_conf_val(OPT_D, D_AGCOUNT) +
			     (dblocks % get_conf_val(OPT_D, D_AGCOUNT) != 0));
	} else {
		uint64_t agcount = get_conf_val(OPT_D, D_AGCOUNT);
		uint64_t agsize = get_conf_val(OPT_D, D_AGSIZE);

		calc_default_ag_geometry(get_conf_val(OPT_B, B_LOG),
					 dblocks, get_conf_val(OPT_D, D_SUNIT)
					 | get_conf_val(OPT_D, D_SWIDTH),
					 &agsize, &agcount);
		set_conf_val(OPT_D, D_AGCOUNT, agcount);
		set_conf_val(OPT_D, D_AGSIZE, agsize);
	}

	/*
	 * If D_SUNIT is a multiple of fs B_SIZE,
	 * then check that is a multiple of the D_AGSIZE too
	 */
	if (get_conf_val(OPT_D, D_SUNIT) &&
	    !(BBTOB(get_conf_val(OPT_D, D_SUNIT)) %
	      get_conf_val(OPT_B, B_SIZE)) &&
	    get_conf_val(OPT_D, D_SWIDTH) &&
	    !(BBTOB(get_conf_val(OPT_D, D_SWIDTH)) %
	      get_conf_val(OPT_B, B_SIZE))) {

		/* convert from 512 byte blocks to fs B_SIZE
		 */
		set_conf_val(OPT_D, D_SUNIT,
			     DTOBT(get_conf_val(OPT_D, D_SUNIT)));
		set_conf_val(OPT_D, D_SWIDTH,
			     DTOBT(get_conf_val(OPT_D, D_SWIDTH)));

		/*
		 * D_AGSIZE is not a multiple of D_SUNIT
		 */
		if ((get_conf_val(OPT_D, D_AGSIZE) %
		     get_conf_val(OPT_D, D_SUNIT)) != 0) {
			/*
			 * Round up to stripe unit boundary. Also make sure
			 * that D_AGSIZE is still larger than
			 * XFS_AG_MIN_BLOCKS(get_conf_val(OPT_B, B_LOG))
		 	 */
			tmp_agsize = ((get_conf_val(OPT_D, D_AGSIZE) +
				       (get_conf_val(OPT_D, D_SUNIT) - 1)) /
				      get_conf_val(OPT_D, D_SUNIT)) *
				get_conf_val(OPT_D, D_SUNIT);
			/*
			 * Round down to stripe unit boundary if rounding up
			 * created an AG size that is larger than the AG max.
			 */
			if (tmp_agsize >
			    XFS_AG_MAX_BLOCKS(get_conf_val(OPT_B, B_LOG)))
				tmp_agsize = ((get_conf_val(OPT_D, D_AGSIZE)) /
					      get_conf_val(OPT_D, D_SUNIT)) *
					get_conf_val(OPT_D, D_SUNIT);

			if ((tmp_agsize >=
			     XFS_AG_MIN_BLOCKS(get_conf_val(OPT_B, B_LOG))) &&
			    (tmp_agsize <=
			     XFS_AG_MAX_BLOCKS(get_conf_val(OPT_B, B_LOG)))) {
				set_conf_val(OPT_D, D_AGSIZE, tmp_agsize);
				if (!daflag)
					set_conf_val(OPT_D, D_AGCOUNT,
						dblocks /
						get_conf_val(OPT_D, D_AGSIZE) +
						(dblocks %
						 get_conf_val(OPT_D, D_AGSIZE)
						!= 0));
				if (dasize)
					fprintf(stderr,
				_("agsize rounded to %"PRIu64", swidth = %"PRIu64"\n"),
						get_conf_val(OPT_D, D_AGSIZE),
						get_conf_val(OPT_D, D_SWIDTH));
			} else {
				if (get_conf_val(OPT_D, D_NOALIGN)) {
					set_conf_val(OPT_D, D_SUNIT, 0);
					set_conf_val(OPT_D, D_SWIDTH, 0);
				} else {
					/*
					 * D_AGSIZE is out of bounds, this will
					 * print nice details & exit.
					 */
					validate_ag_geometry(
						get_conf_val(OPT_B, B_LOG),
						dblocks,
						get_conf_val(OPT_D, D_AGSIZE),
						get_conf_val(OPT_D, D_AGCOUNT));
					exit(1);
				}
			}
		}
		if (get_conf_val(OPT_D, D_SWIDTH) &&
		    ((get_conf_val(OPT_D, D_AGSIZE) %
		      get_conf_val(OPT_D, D_SWIDTH)) == 0) &&
		    (get_conf_val(OPT_D, D_AGCOUNT) > 1)) {
			/* This is a non-optimal configuration because all AGs
			 * start on the same disk in the stripe.  Changing
			 * the AG size by one sunit will guarantee that this
			 * does not happen.
			 */
			tmp_agsize = get_conf_val(OPT_D, D_AGSIZE) -
				get_conf_val(OPT_D, D_SUNIT);
			if (tmp_agsize <
			    XFS_AG_MIN_BLOCKS(get_conf_val(OPT_B, B_LOG))) {
				tmp_agsize = get_conf_val(OPT_D, D_AGSIZE) +
					get_conf_val(OPT_D, D_SUNIT);
				if (dblocks < get_conf_val(OPT_D, D_AGSIZE)) {
					/* oh well, nothing to do */
					tmp_agsize =
						get_conf_val(OPT_D, D_AGSIZE);
				}
			}
			if (daflag || dasize) {
				fprintf(stderr, _(
"Warning: AG size is a multiple of stripe width.  This can cause performance\n\
problems by aligning all AGs on the same disk.  To avoid this, run mkfs with\n\
an AG size that is one stripe unit smaller, for example %"PRIu64".\n"),
					tmp_agsize);
			} else {
				set_conf_val(OPT_D, D_AGSIZE, tmp_agsize);
				set_conf_val(OPT_D, D_AGCOUNT,
					dblocks/get_conf_val(OPT_D, D_AGSIZE) +
					     (dblocks %
					      get_conf_val(OPT_D, D_AGSIZE)
					      != 0));
				/*
				 * If the last AG is too small, reduce the
				 * filesystem size and drop the blocks.
				 */
				if (dblocks % get_conf_val(OPT_D, D_AGSIZE)
				    != 0 &&
				    (dblocks % get_conf_val(OPT_D, D_AGSIZE) <
				    XFS_AG_MIN_BLOCKS(
					get_conf_val(OPT_B, B_LOG)))) {

					dblocks = (xfs_rfsblock_t)(
						(get_conf_val(OPT_D, D_AGCOUNT)
						 - 1) *
						get_conf_val(OPT_D, D_AGSIZE));
					set_conf_val(OPT_D, D_AGCOUNT,
						get_conf_val(OPT_D, D_AGCOUNT)
						- 1);
					ASSERT(get_conf_val(OPT_D, D_AGCOUNT)
					       != 0);
				}
			}
		}
	} else {
		if (get_conf_val(OPT_D, D_NOALIGN)) {
			set_conf_val(OPT_D, D_SWIDTH, 0);
			set_conf_val(OPT_D, D_SUNIT, 0);
		} else {
			fprintf(stderr,
				_("%s: Stripe unit(%"PRIu64") or stripe width(%"PRIu64") is "
				"not a multiple of the block size(%"PRIu64")\n"),
				progname,
				BBTOB(get_conf_val(OPT_D, D_SUNIT)),
				BBTOB(get_conf_val(OPT_D, D_SWIDTH)),
				get_conf_val(OPT_B, B_SIZE));
			exit(1);
		}
	}

	/*
	 * If the last AG is too small, reduce the filesystem size
	 * and drop the blocks.
	 */
	if (dblocks % get_conf_val(OPT_D, D_AGSIZE) != 0 &&
	     (dblocks % get_conf_val(OPT_D, D_AGSIZE) <
	      XFS_AG_MIN_BLOCKS(get_conf_val(OPT_B, B_LOG)))) {
		ASSERT(!daflag);
		dblocks = (xfs_rfsblock_t)(
				(get_conf_val(OPT_D, D_AGCOUNT) - 1) *
				get_conf_val(OPT_D, D_AGSIZE));
		set_conf_val(OPT_D, D_AGCOUNT,
			     get_conf_val(OPT_D, D_AGCOUNT) - 1);
		ASSERT(get_conf_val(OPT_D, D_AGCOUNT) != 0);
	}

	validate_ag_geometry(get_conf_val(OPT_B, B_LOG),
			     dblocks,
			     get_conf_val(OPT_D, D_AGSIZE),
			     get_conf_val(OPT_D, D_AGCOUNT));

	if (!imflag)
		set_conf_val(OPT_I, I_MAXPCT,
			     calc_default_imaxpct(get_conf_val(OPT_B, B_LOG),
					       dblocks));

	/*
	 * check that log sunit is modulo fsblksize or default it to D_SUNIT.
	 */

	if (get_conf_val(OPT_L, L_SUNIT)) {
		/* convert from 512 byte blocks to fs blocks */
		set_conf_val(OPT_L, L_SUNIT,
			     DTOBT(get_conf_val(OPT_L, L_SUNIT)));
	} else if (sb_feat.log_version == 2 &&
		   get_conf_val(OPT_L, L_INTERNAL) &&
		   get_conf_val(OPT_D, D_SUNIT)) {
		/* L_SUNIT and D_SUNIT now in fs blocks */
		set_conf_val(OPT_L, L_SUNIT, get_conf_val(OPT_D, D_SUNIT));
	}

	if (sb_feat.log_version == 2 &&
	    (get_conf_val(OPT_L, L_SUNIT) *
	     get_conf_val(OPT_B, B_SIZE)) > 256 * 1024) {
		/* Warn only if specified on commandline */
		if (lsuflag || lsunitflag) {
			fprintf(stderr,
	_("log stripe unit (%"PRIu64" bytes) is too large (maximum is 256KiB)\n"),
				(get_conf_val(OPT_L, L_SUNIT) *
				 get_conf_val(OPT_B, B_SIZE)));
			fprintf(stderr,
	_("log stripe unit adjusted to 32KiB\n"));
		}
		set_conf_val(OPT_L, L_SUNIT,
			     (32 * 1024) >> get_conf_val(OPT_B, B_LOG));
	}

	min_logblocks = max_trans_res(get_conf_val(OPT_D, D_AGSIZE),
				   sb_feat.crcs_enabled, sb_feat.dir_version,
				   get_conf_val(OPT_D, D_SECTLOG),
				   get_conf_val(OPT_B, B_LOG),
				   get_conf_val(OPT_I, I_LOG),
				   get_conf_val(OPT_N, N_LOG),
				   sb_feat.log_version,
				   get_conf_val(OPT_L, L_SUNIT),
				   sb_feat.finobt,
				   sb_feat.rmapbt, sb_feat.reflink);
	ASSERT(min_logblocks);
	min_logblocks = MAX(XFS_MIN_LOG_BLOCKS, min_logblocks);
	if (!get_conf_val(OPT_L, L_SIZE) &&
	    dblocks >= (1024*1024*1024) >> get_conf_val(OPT_B, B_LOG))
		min_logblocks = MAX(min_logblocks,
				    XFS_MIN_LOG_BYTES >>
				    get_conf_val(OPT_B, B_LOG));
	if (get_conf_val(OPT_L, L_SIZE) && xi.logBBsize > 0 &&
	    logblocks > DTOBT(xi.logBBsize)) {
		fprintf(stderr,
_("size %s specified for log subvolume is too large, maximum is %"PRIu64" blocks\n"),
			get_conf_raw(OPT_L, L_SIZE),
			(uint64_t)DTOBT(xi.logBBsize));
		usage();
	} else if (!get_conf_val(OPT_L, L_SIZE) && xi.logBBsize > 0) {
		logblocks = DTOBT(xi.logBBsize);
	} else if (get_conf_val(OPT_L, L_SIZE) && !xi.logdev &&
		   !get_conf_val(OPT_L, L_INTERNAL)) {
		fprintf(stderr,
			_("size specified for non-existent log subvolume\n"));
		usage();
	} else if (get_conf_val(OPT_L, L_INTERNAL) &&
		   get_conf_val(OPT_L, L_SIZE) && logblocks >= dblocks) {
		fprintf(stderr, _("size %"PRIu64" too large for internal log\n"),
			logblocks);
		usage();
	} else if (!get_conf_val(OPT_L, L_INTERNAL) && !xi.logdev) {
		logblocks = 0;
	} else if (get_conf_val(OPT_L, L_INTERNAL) &&
		   !get_conf_val(OPT_L, L_SIZE)) {

		if (dblocks < GIGABYTES(1, get_conf_val(OPT_B, B_LOG))) {
			/* tiny filesystems get minimum sized logs. */
			logblocks = min_logblocks;
		} else if (dblocks <
			   GIGABYTES(16, get_conf_val(OPT_B, B_LOG))) {

			/*
			 * For small filesystems, we want to use the
			 * XFS_MIN_LOG_BYTES for filesystems smaller than 16G if
			 * at all possible, ramping up to 128MB at 256GB.
			 */
			logblocks = MIN(XFS_MIN_LOG_BYTES >>
					get_conf_val(OPT_B, B_LOG),
					min_logblocks * XFS_DFL_LOG_FACTOR);
		} else {
			/*
			 * With a 2GB max log size, default to maximum size
			 * at 4TB. This keeps the same ratio from the older
			 * max log size of 128M at 256GB fs size. IOWs,
			 * the ratio of fs size to log size is 2048:1.
			 */
			logblocks = (dblocks <<
				     get_conf_val(OPT_B, B_LOG)) / 2048;
			logblocks = logblocks >> get_conf_val(OPT_B, B_LOG);
		}

		/* Ensure the chosen size meets minimum log size requirements */
		logblocks = MAX(min_logblocks, logblocks);

		/* make sure the log fits wholly within an AG */
		if (logblocks >= get_conf_val(OPT_D, D_AGSIZE))
			logblocks = min_logblocks;

		/* and now clamp the size to the maximum supported size */
		logblocks = MIN(logblocks, XFS_MAX_LOG_BLOCKS);
		if ((logblocks << get_conf_val(OPT_B, B_LOG)) >
		    XFS_MAX_LOG_BYTES)
			logblocks = XFS_MAX_LOG_BYTES >>
				get_conf_val(OPT_B, B_LOG);

	}
	validate_log_size(logblocks, get_conf_val(OPT_B, B_LOG), min_logblocks);

	protostring = setup_proto(protofile);
	bsize = 1 << (get_conf_val(OPT_B, B_LOG) - BBSHIFT);
	mp = &mbuf;
	sbp = &mp->m_sb;
	memset(mp, 0, sizeof(xfs_mount_t));
	sbp->sb_blocklog = (__uint8_t)get_conf_val(OPT_B, B_LOG);
	sbp->sb_sectlog = (__uint8_t)get_conf_val(OPT_D, D_SECTLOG);
	sbp->sb_agblklog = (__uint8_t)libxfs_log2_roundup(
				(unsigned int)get_conf_val(OPT_D, D_AGSIZE));
	sbp->sb_agblocks = (xfs_agblock_t)get_conf_val(OPT_D, D_AGSIZE);
	mp->m_blkbb_log = sbp->sb_blocklog - BBSHIFT;
	mp->m_sectbb_log = sbp->sb_sectlog - BBSHIFT;

	/*
	 * sb_versionnum, finobt and rmapbt flags must be set before we use
	 * libxfs_prealloc_blocks().
	 */
	sb_set_features(&mp->m_sb, &sb_feat,
			get_conf_val(OPT_D, D_SECTSIZE),
			get_conf_val(OPT_L, L_SECTSIZE),
			get_conf_val(OPT_D, D_SUNIT));


	if (get_conf_val(OPT_L, L_INTERNAL)) {
		/*
		 * Readjust the log size to fit within an AG if it was sized
		 * automatically.
		 */
		if (!get_conf_val(OPT_L, L_SIZE)) {
			logblocks = MIN(logblocks,
					libxfs_alloc_ag_max_usable(mp));

			/* revalidate the log size is valid if we changed it */
			validate_log_size(logblocks,
					  get_conf_val(OPT_B, B_LOG),
					  min_logblocks);
		}
		if (logblocks > get_conf_val(OPT_D, D_AGSIZE) -
		    libxfs_prealloc_blocks(mp)) {
			fprintf(stderr,
	_("internal log size %"PRIu64" too large, must fit in allocation group\n"),
				logblocks);
			usage();
		}

		if (laflag) {
			if (get_conf_val(OPT_L, L_AGNUM) >=
			    get_conf_val(OPT_D, D_AGCOUNT)) {
				fprintf(stderr,
	_("log ag number %"PRIu64" too large, must be less than %"PRIu64"\n"),
					get_conf_val(OPT_L, L_AGNUM),
					get_conf_val(OPT_D, D_AGCOUNT));
				usage();
			}
		} else
			set_conf_val(OPT_L, L_AGNUM, (xfs_agnumber_t)(
					get_conf_val(OPT_D, D_AGCOUNT) / 2));

		logstart = XFS_AGB_TO_FSB(mp, get_conf_val(OPT_L, L_AGNUM),
					  libxfs_prealloc_blocks(mp));
		/*
		 * Align the logstart at stripe unit boundary.
		 */
		if (get_conf_val(OPT_L, L_SUNIT)) {
			logstart = fixup_internal_log_stripe(mp,
					lsflag, logstart,
					get_conf_val(OPT_D, D_AGSIZE),
					get_conf_val(OPT_L, L_SUNIT),
					&logblocks,
					get_conf_val(OPT_B, B_LOG), &lalign);
		} else if (get_conf_val(OPT_D, D_SUNIT)) {
			logstart = fixup_internal_log_stripe(mp,
					lsflag, logstart,
					get_conf_val(OPT_D, D_AGSIZE),
					get_conf_val(OPT_D, D_SUNIT),
					&logblocks,
					get_conf_val(OPT_B, B_LOG), &lalign);
		}
	} else {
		logstart = 0;
		if (get_conf_val(OPT_L, L_SUNIT))
			fixup_log_stripe_unit(lsflag,
					      get_conf_val(OPT_L, L_SUNIT),
					&logblocks, get_conf_val(OPT_B, B_LOG));
	}
	validate_log_size(logblocks, get_conf_val(OPT_B, B_LOG), min_logblocks);

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
			dfile, get_conf_val(OPT_I, I_SIZE),
			get_conf_val(OPT_D, D_AGCOUNT),
			get_conf_val(OPT_D, D_AGSIZE),
			"", get_conf_val(OPT_D, D_SECTSIZE),
			sb_feat.attr_version,
				    !sb_feat.projid16bit,
			"", sb_feat.crcs_enabled, sb_feat.finobt, sb_feat.spinodes,
			sb_feat.rmapbt, sb_feat.reflink,
			"", get_conf_val(OPT_B, B_SIZE),
			dblocks,
			get_conf_val(OPT_I, I_MAXPCT),
			"", get_conf_val(OPT_D, D_SUNIT),
			get_conf_val(OPT_D, D_SWIDTH),
			sb_feat.dir_version,
			get_conf_val(OPT_N, N_SIZE), sb_feat.nci,
				sb_feat.dirftype,
			logfile, 1 << get_conf_val(OPT_B, B_LOG), logblocks,
			sb_feat.log_version, "",
			get_conf_val(OPT_L, L_SECTSIZE),
			get_conf_val(OPT_L, L_SUNIT),
				sb_feat.lazy_sb_counters,
			rtfile, rtextblocks << get_conf_val(OPT_B, B_LOG),
			rtblocks, rtextents);
		if (Nflag)
			exit(0);
	}

	if (label)
		strncpy(sbp->sb_fname, label, sizeof(sbp->sb_fname));
	sbp->sb_magicnum = XFS_SB_MAGIC;
	sbp->sb_blocksize = get_conf_val(OPT_B, B_SIZE);
	sbp->sb_dblocks = dblocks;
	sbp->sb_rblocks = rtblocks;
	sbp->sb_rextents = rtextents;
	platform_uuid_copy(&sbp->sb_uuid, &uuid);
	/* Only in memory; libxfs expects this as if read from disk */
	platform_uuid_copy(&sbp->sb_meta_uuid, &uuid);
	sbp->sb_logstart = logstart;
	sbp->sb_rootino = sbp->sb_rbmino = sbp->sb_rsumino = NULLFSINO;
	sbp->sb_rextsize = rtextblocks;
	sbp->sb_agcount = (xfs_agnumber_t)get_conf_val(OPT_D, D_AGCOUNT);
	sbp->sb_rbmblocks = nbmblocks;
	sbp->sb_logblocks = (xfs_extlen_t)logblocks;
	sbp->sb_sectsize = (__uint16_t)get_conf_val(OPT_D, D_SECTSIZE);
	sbp->sb_inodesize = (__uint16_t)get_conf_val(OPT_I, I_SIZE);
	sbp->sb_inopblock = (__uint16_t)(get_conf_val(OPT_B, B_SIZE) /
					 get_conf_val(OPT_I, I_SIZE));
	sbp->sb_sectlog = (__uint8_t)get_conf_val(OPT_D, D_SECTLOG);
	sbp->sb_inodelog = (__uint8_t)get_conf_val(OPT_I, I_LOG);
	sbp->sb_inopblog = (__uint8_t)(get_conf_val(OPT_B, B_LOG) -
				       get_conf_val(OPT_I, I_LOG));
	sbp->sb_rextslog =
		(__uint8_t)(rtextents ?
			libxfs_highbit32((unsigned int)rtextents) : 0);
	sbp->sb_inprogress = 1;	/* mkfs is in progress */
	sbp->sb_imax_pct = get_conf_val(OPT_I, I_MAXPCT);
	sbp->sb_icount = 0;
	sbp->sb_ifree = 0;
	sbp->sb_fdblocks = dblocks -
		get_conf_val(OPT_D, D_AGCOUNT) * libxfs_prealloc_blocks(mp) -
		(get_conf_val(OPT_L, L_INTERNAL) ? logblocks : 0);
	sbp->sb_frextents = 0;	/* will do a free later */
	sbp->sb_uquotino = sbp->sb_gquotino = sbp->sb_pquotino = 0;
	sbp->sb_qflags = 0;
	sbp->sb_unit = get_conf_val(OPT_D, D_SUNIT);
	sbp->sb_width = get_conf_val(OPT_D, D_SWIDTH);
	sbp->sb_dirblklog = get_conf_val(OPT_N, N_LOG) -
		get_conf_val(OPT_B, B_LOG);
	if (sb_feat.log_version == 2) {	/* This is stored in bytes */
		set_conf_val(OPT_L, L_SUNIT,
			     (get_conf_val(OPT_L, L_SUNIT) == 0) ?
			     1 : XFS_FSB_TO_B(mp,
					      get_conf_val(OPT_L, L_SUNIT)));
		sbp->sb_logsunit = get_conf_val(OPT_L, L_SUNIT);
	} else
		sbp->sb_logsunit = 0;
	if (sb_feat.inode_align) {
		int	cluster_size = XFS_INODE_BIG_CLUSTER_SIZE;
		if (sb_feat.crcs_enabled)
			cluster_size *= get_conf_val(OPT_I, I_SIZE) /
				XFS_DINODE_MIN_SIZE;
		sbp->sb_inoalignmt = cluster_size >> get_conf_val(OPT_B, B_LOG);
		sb_feat.inode_align = sbp->sb_inoalignmt != 0;
	} else
		sbp->sb_inoalignmt = 0;
	if (get_conf_val(OPT_L, L_SECTSIZE) != BBSIZE ||
	    get_conf_val(OPT_D, D_SECTSIZE) != BBSIZE) {
		sbp->sb_logsectlog = (__uint8_t)get_conf_val(OPT_L, L_SECTLOG);
		sbp->sb_logsectsize =
			(__uint16_t)get_conf_val(OPT_L, L_SECTSIZE);
	} else {
		sbp->sb_logsectlog = 0;
		sbp->sb_logsectsize = 0;
	}

	sb_set_features(&mp->m_sb, &sb_feat,
			get_conf_val(OPT_D, D_SECTSIZE),
			get_conf_val(OPT_L, L_SECTSIZE),
			get_conf_val(OPT_D, D_SUNIT));

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
	memset(XFS_BUF_PTR(buf), 0, get_conf_val(OPT_D, D_SECTSIZE));
	libxfs_sb_to_disk((void *)XFS_BUF_PTR(buf), sbp);
	libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
	libxfs_purgebuf(buf);

	/*
	 * If the data area is a file, then grow it out to its final size
	 * if needed so that the reads for the end of the device in the mount
	 * code will succeed.
	 */
	if (xi.disfile &&
	    xi.dsize * xi.dbsize < dblocks * get_conf_val(OPT_B, B_SIZE)) {
		if (ftruncate(xi.dfd,
			     dblocks * get_conf_val(OPT_B, B_SIZE)) < 0) {
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
		&sbp->sb_uuid, sb_feat.log_version,
		get_conf_val(OPT_L, L_SUNIT), XLOG_FMT, XLOG_INIT_CYCLE, false);

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
	for (agno = 0; agno < get_conf_val(OPT_D, D_AGCOUNT); agno++) {
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
		memset(XFS_BUF_PTR(buf), 0, get_conf_val(OPT_D, D_SECTSIZE));
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
		memset(agf, 0, get_conf_val(OPT_D, D_SECTSIZE));
		if (agno == get_conf_val(OPT_D, D_AGCOUNT) - 1)
			set_conf_val(OPT_D, D_AGSIZE,
				dblocks -
				(xfs_rfsblock_t)(agno *
					get_conf_val(OPT_D, D_AGSIZE)));

		agf->agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
		agf->agf_versionnum = cpu_to_be32(XFS_AGF_VERSION);
		agf->agf_seqno = cpu_to_be32(agno);
		agf->agf_length = cpu_to_be32(get_conf_val(OPT_D, D_AGSIZE));
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
		nbmblocks = (xfs_extlen_t)(get_conf_val(OPT_D, D_AGSIZE) -
					   libxfs_prealloc_blocks(mp));
		agf->agf_freeblks = cpu_to_be32(nbmblocks);
		agf->agf_longest = cpu_to_be32(nbmblocks);
		if (xfs_sb_version_hascrc(&mp->m_sb))
			platform_uuid_copy(&agf->agf_uuid, &mp->m_sb.sb_uuid);

		if (get_conf_val(OPT_L, L_INTERNAL) &&
		    agno == get_conf_val(OPT_L, L_AGNUM)) {
			be32_add_cpu(&agf->agf_freeblks, -logblocks);
			agf->agf_longest =
				cpu_to_be32(get_conf_val(OPT_D, D_AGSIZE) -
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
		memset(agfl, 0xff, get_conf_val(OPT_D, D_SECTSIZE));
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
		memset(agi, 0, get_conf_val(OPT_D, D_SECTSIZE));
		agi->agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
		agi->agi_versionnum = cpu_to_be32(XFS_AGI_VERSION);
		agi->agi_seqno = cpu_to_be32(agno);
		agi->agi_length =
			cpu_to_be32(
				(xfs_agblock_t)get_conf_val(OPT_D, D_AGSIZE));
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
		memset(block, 0, get_conf_val(OPT_B, B_SIZE));
		libxfs_btree_init_block(mp, buf, XFS_BTNUM_BNO, 0, 1, agno, 0);

		arec = XFS_ALLOC_REC_ADDR(mp, block, 1);
		arec->ar_startblock = cpu_to_be32(libxfs_prealloc_blocks(mp));
		if (get_conf_val(OPT_L, L_INTERNAL) &&
		    agno == get_conf_val(OPT_L, L_AGNUM)) {
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
		arec->ar_blockcount =
			cpu_to_be32(get_conf_val(OPT_D, D_AGSIZE) -
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
		memset(block, 0, get_conf_val(OPT_B, B_SIZE));
		libxfs_btree_init_block(mp, buf, XFS_BTNUM_CNT, 0, 1, agno, 0);

		arec = XFS_ALLOC_REC_ADDR(mp, block, 1);
		arec->ar_startblock = cpu_to_be32(libxfs_prealloc_blocks(mp));
		if (get_conf_val(OPT_L, L_INTERNAL) &&
		    agno == get_conf_val(OPT_L, L_AGNUM)) {
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
		arec->ar_blockcount =
			cpu_to_be32(get_conf_val(OPT_D, D_AGSIZE) -
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
			memset(block, 0, get_conf_val(OPT_B, B_SIZE));
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
		memset(block, 0, get_conf_val(OPT_B, B_SIZE));
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
			memset(block, 0, get_conf_val(OPT_B, B_SIZE));
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
			memset(block, 0, get_conf_val(OPT_B, B_SIZE));

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
			if (get_conf_val(OPT_L, L_INTERNAL) && agno ==
			    get_conf_val(OPT_L, L_AGNUM)) {
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
	memset(XFS_BUF_PTR(buf), 0, get_conf_val(OPT_B, B_SIZE));
	libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);

	/*
	 * Make sure we can write the last block in the realtime area.
	 */
	if (mp->m_rtdev_targp->dev && rtblocks > 0) {
		buf = libxfs_getbuf(mp->m_rtdev_targp,
				XFS_FSB_TO_BB(mp, rtblocks - 1LL), bsize);
		memset(XFS_BUF_PTR(buf), 0, get_conf_val(OPT_B, B_SIZE));
		libxfs_writebuf(buf, LIBXFS_EXIT_ON_FAILURE);
	}

	/*
	 * BNO, CNT free block list
	 */
	for (agno = 0; agno < get_conf_val(OPT_D, D_AGCOUNT); agno++) {
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


/*
 * Print everything related to a conflict for the user
 * and show up the usage. That will terminate the program.
 */
static void
print_conflict_struct(
	struct opt_params	*opt,
	struct subopt_param	*subopt,
	struct subopt_conflict	*conflict)
{
	fprintf(stderr, _("Cannot specify both -%c %s and -%c %s"),
		opt->name,
		opt->subopts[subopt->index],
		opts[conflict->opt].name,
		opts[conflict->opt].subopts[conflict->subopt]);
	if(conflict->message) {
		fprintf(stderr, ": %s",
			_(conflict->message));
	}
	fprintf(stderr, "\n");
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
