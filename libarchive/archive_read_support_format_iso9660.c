/*-
 * Copyright (c) 2003-2007 Tim Kientzle
 * Copyright (c) 2009 Andreas Henriksson <andreas@fatal.se>
 * Copyright (c) 2009 Michihiro NAKAJIMA
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"
__FBSDID("$FreeBSD: src/lib/libarchive/archive_read_support_format_iso9660.c,v 1.30 2008/12/06 06:57:45 kientzle Exp $");

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
/* #include <stdint.h> */ /* See archive_platform.h */
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <time.h>
#ifdef HAVE_ZLIB_H
#include <zlib.h>       
#endif

#include "archive.h"
#include "archive_endian.h"
#include "archive_entry.h"
#include "archive_private.h"
#include "archive_read_private.h"
#include "archive_string.h"

/*
 * An overview of ISO 9660 format:
 *
 * Each disk is laid out as follows:
 *   * 32k reserved for private use
 *   * Volume descriptor table.  Each volume descriptor
 *     is 2k and specifies basic format information.
 *     The "Primary Volume Descriptor" (PVD) is defined by the
 *     standard and should always be present; other volume
 *     descriptors include various vendor-specific extensions.
 *   * Files and directories.  Each file/dir is specified by
 *     an "extent" (starting sector and length in bytes).
 *     Dirs are just files with directory records packed one
 *     after another.  The PVD contains a single dir entry
 *     specifying the location of the root directory.  Everything
 *     else follows from there.
 *
 * This module works by first reading the volume descriptors, then
 * building a list of directory entries, sorted by starting
 * sector.  At each step, I look for the earliest dir entry that
 * hasn't yet been read, seek forward to that location and read
 * that entry.  If it's a dir, I slurp in the new dir entries and
 * add them to the heap; if it's a regular file, I return the
 * corresponding archive_entry and wait for the client to request
 * the file body.  This strategy allows us to read most compliant
 * CDs with a single pass through the data, as required by libarchive.
 */
#define	LOGICAL_BLOCK_SIZE	2048
#define	SYSTEM_AREA_BLOCK	16

/* Structure of on-disk primary volume descriptor. */
#define PVD_type_offset 0
#define PVD_type_size 1
#define PVD_id_offset (PVD_type_offset + PVD_type_size)
#define PVD_id_size 5
#define PVD_version_offset (PVD_id_offset + PVD_id_size)
#define PVD_version_size 1
#define PVD_reserved1_offset (PVD_version_offset + PVD_version_size)
#define PVD_reserved1_size 1
#define PVD_system_id_offset (PVD_reserved1_offset + PVD_reserved1_size)
#define PVD_system_id_size 32
#define PVD_volume_id_offset (PVD_system_id_offset + PVD_system_id_size)
#define PVD_volume_id_size 32
#define PVD_reserved2_offset (PVD_volume_id_offset + PVD_volume_id_size)
#define PVD_reserved2_size 8
#define PVD_volume_space_size_offset (PVD_reserved2_offset + PVD_reserved2_size)
#define PVD_volume_space_size_size 8
#define PVD_reserved3_offset (PVD_volume_space_size_offset + PVD_volume_space_size_size)
#define PVD_reserved3_size 32
#define PVD_volume_set_size_offset (PVD_reserved3_offset + PVD_reserved3_size)
#define PVD_volume_set_size_size 4
#define PVD_volume_sequence_number_offset (PVD_volume_set_size_offset + PVD_volume_set_size_size)
#define PVD_volume_sequence_number_size 4
#define PVD_logical_block_size_offset (PVD_volume_sequence_number_offset + PVD_volume_sequence_number_size)
#define PVD_logical_block_size_size 4
#define PVD_path_table_size_offset (PVD_logical_block_size_offset + PVD_logical_block_size_size)
#define PVD_path_table_size_size 8
#define PVD_type_1_path_table_offset (PVD_path_table_size_offset + PVD_path_table_size_size)
#define PVD_type_1_path_table_size 4
#define PVD_opt_type_1_path_table_offset (PVD_type_1_path_table_offset + PVD_type_1_path_table_size)
#define PVD_opt_type_1_path_table_size 4
#define PVD_type_m_path_table_offset (PVD_opt_type_1_path_table_offset + PVD_opt_type_1_path_table_size)
#define PVD_type_m_path_table_size 4
#define PVD_opt_type_m_path_table_offset (PVD_type_m_path_table_offset + PVD_type_m_path_table_size)
#define PVD_opt_type_m_path_table_size 4
#define PVD_root_directory_record_offset (PVD_opt_type_m_path_table_offset + PVD_opt_type_m_path_table_size)
#define PVD_root_directory_record_size 34
#define PVD_volume_set_id_offset (PVD_root_directory_record_offset + PVD_root_directory_record_size)
#define PVD_volume_set_id_size 128
#define PVD_publisher_id_offset (PVD_volume_set_id_offset + PVD_volume_set_id_size)
#define PVD_publisher_id_size 128
#define PVD_preparer_id_offset (PVD_publisher_id_offset + PVD_publisher_id_size)
#define PVD_preparer_id_size 128
#define PVD_application_id_offset (PVD_preparer_id_offset + PVD_preparer_id_size)
#define PVD_application_id_size 128
#define PVD_copyright_file_id_offset (PVD_application_id_offset + PVD_application_id_size)
#define PVD_copyright_file_id_size 37
#define PVD_abstract_file_id_offset (PVD_copyright_file_id_offset + PVD_copyright_file_id_size)
#define PVD_abstract_file_id_size 37
#define PVD_bibliographic_file_id_offset (PVD_abstract_file_id_offset + PVD_abstract_file_id_size)
#define PVD_bibliographic_file_id_size 37
#define PVD_creation_date_offset (PVD_bibliographic_file_id_offset + PVD_bibliographic_file_id_size)
#define PVD_creation_date_size 17
#define PVD_modification_date_offset (PVD_creation_date_offset + PVD_creation_date_size)
#define PVD_modification_date_size 17
#define PVD_expiration_date_offset (PVD_modification_date_offset + PVD_modification_date_size)
#define PVD_expiration_date_size 17
#define PVD_effective_date_offset (PVD_expiration_date_offset + PVD_expiration_date_size)
#define PVD_effective_date_size 17
#define PVD_file_structure_version_offset (PVD_effective_date_offset + PVD_effective_date_size)
#define PVD_file_structure_version_size 1
#define PVD_reserved4_offset (PVD_file_structure_version_offset + PVD_file_structure_version_size)
#define PVD_reserved4_size 1
#define PVD_application_data_offset (PVD_reserved4_offset + PVD_reserved4_size)
#define PVD_application_data_size 512
#define PVD_reserved5_offset (PVD_application_data_offset + PVD_application_data_size)
#define PVD_reserved5_size (2048 - PVD_reserved5_offset)

/* TODO: It would make future maintenance easier to just hardcode the
 * above values.  In particular, ECMA119 states the offsets as part of
 * the standard.  That would eliminate the need for the following check.*/
#if PVD_reserved5_offset != 1395
#error PVD offset and size definitions are wrong.
#endif


/* Structure of optional on-disk supplementary volume descriptor. */
#define SVD_type_offset 0
#define SVD_type_size 1
#define SVD_id_offset (SVD_type_offset + SVD_type_size)
#define SVD_id_size 5
#define SVD_version_offset (SVD_id_offset + SVD_id_size)
#define SVD_version_size 1
/* ... */
#define SVD_reserved1_offset	72
#define SVD_reserved1_size	8
#define SVD_volume_space_size_offset 80
#define SVD_volume_space_size_size 8
#define SVD_escape_sequences_offset (SVD_volume_space_size_offset + SVD_volume_space_size_size)
#define SVD_escape_sequences_size 32
/* ... */
#define SVD_logical_block_size_offset 128
#define SVD_logical_block_size_size 4
/* ... */
#define SVD_root_directory_record_offset 156
#define SVD_root_directory_record_size 34
#define SVD_reserved2_offset	882
#define SVD_reserved2_size	1
#define SVD_reserved3_offset	1395
#define SVD_reserved3_size	653
/* ... */
/* FIXME: validate correctness of last SVD entry offset. */

/* Structure of an on-disk directory record. */
/* Note:  ISO9660 stores each multi-byte integer twice, once in
 * each byte order.  The sizes here are the size of just one
 * of the two integers.  (This is why the offset of a field isn't
 * the same as the offset+size of the previous field.) */
#define DR_length_offset 0
#define DR_length_size 1
#define DR_ext_attr_length_offset 1
#define DR_ext_attr_length_size 1
#define DR_extent_offset 2
#define DR_extent_size 4
#define DR_size_offset 10
#define DR_size_size 4
#define DR_date_offset 18
#define DR_date_size 7
#define DR_flags_offset 25
#define DR_flags_size 1
#define DR_file_unit_size_offset 26
#define DR_file_unit_size_size 1
#define DR_interleave_offset 27
#define DR_interleave_size 1
#define DR_volume_sequence_number_offset 28
#define DR_volume_sequence_number_size 2
#define DR_name_len_offset 32
#define DR_name_len_size 1
#define DR_name_offset 33

#ifdef HAVE_ZLIB_H
static const unsigned char zisofs_magic[8] = {
	0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07
};

struct zisofs {
	/* Set 1 if this file compressed by paged zlib */
	int		 pz;
	int		 pz_log2_bs; /* Log2 of block size */
	uint64_t	 pz_uncompressed_size;

	int		 initialized;
	unsigned char	*uncompressed_buffer;
	size_t		 uncompressed_buffer_size;

	uint32_t	 pz_offset;
	unsigned char	 header[16];
	size_t		 header_avail;
	int		 header_passed;
	unsigned char	*block_pointers;
	size_t		 block_pointers_alloc;
	size_t		 block_pointers_size;
	size_t		 block_pointers_avail;
	size_t		 block_off;
	uint32_t	 block_avail;

	z_stream	 stream;
	int		 stream_valid;
};
#else
struct zisofs {
	/* Set 1 if this file compressed by paged zlib */
	int		 pz;
};
#endif

/* In-memory storage for a directory record. */
struct file_info {
	struct file_info	*parent;
	int		 refcount;
	int		 subdirs;
	uint64_t	 offset;	/* Offset on disk.		*/
	uint64_t	 size;		/* File size in bytes.		*/
	uint32_t	 ce_offset;	/* Offset of CE.		*/
	uint32_t	 ce_size;	/* Size of CE.			*/
	time_t		 birthtime;	/* File created time.		*/
	time_t		 mtime;		/* File last modified time.	*/
	time_t		 atime;		/* File last accessed time.	*/
	time_t		 ctime;		/* File attribute change time.	*/
	uint64_t	 rdev;		/* Device number.		*/
	mode_t		 mode;
	uid_t		 uid;
	gid_t		 gid;
	int64_t		 number;
	int		 nlinks;
	struct archive_string name; /* Pathname */
	char		 name_continues; /* Non-zero if name continues */
	struct archive_string symlink;
	char		 symlink_continues; /* Non-zero if link continues */
	/* Set 1 if this file compressed by paged zlib(zisofs) */
	int		 pz;
	int		 pz_log2_bs; /* Log2 of block size */
	uint64_t	 pz_uncompressed_size;
};


struct iso9660 {
	int	magic;
#define ISO9660_MAGIC   0x96609660

	int opt_support_joliet;
	int opt_support_rockridge;

	struct archive_string pathname;
	char	seenRockridge;	/* Set true if RR extensions are used. */
	char	seenSUSP;	/* Set true if SUSP is beging used. */
	char	seenJoliet;

	unsigned char	suspOffset;
	struct {
		struct read_ce_req {
			uint64_t	 offset;/* Offset of CE on disk. */
			struct file_info *file;
		}		*reqs;
		int		 cnt;
		int		 allocated;
	}	read_ce_req;
	int64_t		previous_number;
	uint64_t	previous_size;
	struct archive_string previous_pathname;

	/* TODO: Make this a heap for fast inserts and deletions. */
	struct file_info **pending_files;
	int	pending_files_allocated;
	int	pending_files_used;
	struct cache_files {
		struct file_info	**files;
		int			 allocated;
		int			 count;
		int			 index;
	}	cache_files;

	uint64_t current_position;
	ssize_t	logical_block_size;
	uint64_t volume_size; /* Total size of volume in bytes. */
	int32_t  volume_block;/* Total size of volume in logical blocks. */

	struct vd {
		int		location;	/* Location of Extent.	*/
		uint32_t	size;
	} primary, joliet;

	off_t	entry_sparse_offset;
	int64_t	entry_bytes_remaining;
	struct zisofs	 entry_zisofs;
};

static void	add_entry(struct iso9660 *iso9660, struct file_info *file);
static int	archive_read_format_iso9660_bid(struct archive_read *);
static int	archive_read_format_iso9660_options(struct archive_read *,
		    const char *, const char *);
static int	archive_read_format_iso9660_cleanup(struct archive_read *);
static int	archive_read_format_iso9660_read_data(struct archive_read *,
		    const void **, size_t *, off_t *);
static int	archive_read_format_iso9660_read_data_skip(struct archive_read *);
static int	archive_read_format_iso9660_read_header(struct archive_read *,
		    struct archive_entry *);
static const char *build_pathname(struct archive_string *, struct file_info *);
#if DEBUG
static void	dump_isodirrec(FILE *, const unsigned char *isodirrec);
#endif
static time_t	time_from_tm(struct tm *);
static time_t	isodate17(const unsigned char *);
static time_t	isodate7(const unsigned char *);
static int	isBootRecord(struct iso9660 *iso9660,
		    const unsigned char *h);
static int	isVolumePartition(struct iso9660 *iso9660,
		    const unsigned char *h);
static int	isVDSetTerminator(struct iso9660 *iso9660,
		    const unsigned char *h);
static int	isJolietSVD(struct iso9660 *, const unsigned char *);
static int	isPVD(struct iso9660 *, const unsigned char *);
static struct file_info *next_cache_entry(struct iso9660 *iso9660);
static struct file_info *next_entry(struct iso9660 *);
static int	next_entry_seek(struct archive_read *a, struct iso9660 *iso9660,
		    struct file_info **pfile);
static struct file_info *
		parse_file_info(struct archive_read *a,
		    struct file_info *parent, const unsigned char *isodirrec);
static void	parse_rockridge(struct iso9660 *iso9660,
		    struct file_info *file, const unsigned char *start,
		    const unsigned char *end);
static int	register_CE(struct iso9660 *iso9660, int32_t location,
		    struct file_info *file);
static int	read_CE(struct archive_read *a, struct iso9660 *iso9660);
static void	parse_rockridge_NM1(struct file_info *,
		    const unsigned char *, int);
static void	parse_rockridge_SL1(struct file_info *,
		    const unsigned char *, int);
static void	parse_rockridge_TF1(struct file_info *,
		    const unsigned char *, int);
static void	parse_rockridge_ZF1(struct file_info *,
		    const unsigned char *, int);
static void	release_file(struct iso9660 *, struct file_info *);
static unsigned	toi(const void *p, int n);

int
archive_read_support_format_iso9660(struct archive *_a)
{
	struct archive_read *a = (struct archive_read *)_a;
	struct iso9660 *iso9660;
	int r;

	iso9660 = (struct iso9660 *)malloc(sizeof(*iso9660));
	if (iso9660 == NULL) {
		archive_set_error(&a->archive, ENOMEM, "Can't allocate iso9660 data");
		return (ARCHIVE_FATAL);
	}
	memset(iso9660, 0, sizeof(*iso9660));
	iso9660->magic = ISO9660_MAGIC;
	/* Enable to support Joliet extensions by default.	*/
	iso9660->opt_support_joliet = 1;
	/* Enable to support Rock Ridge extensions by default.	*/
	iso9660->opt_support_rockridge = 1;

	r = __archive_read_register_format(a,
	    iso9660,
	    "iso9660",
	    archive_read_format_iso9660_bid,
	    archive_read_format_iso9660_options,
	    archive_read_format_iso9660_read_header,
	    archive_read_format_iso9660_read_data,
	    archive_read_format_iso9660_read_data_skip,
	    archive_read_format_iso9660_cleanup);

	if (r != ARCHIVE_OK) {
		free(iso9660);
		return (r);
	}
	return (ARCHIVE_OK);
}


static int
archive_read_format_iso9660_bid(struct archive_read *a)
{
	struct iso9660 *iso9660;
	ssize_t bytes_read;
	const void *h;
	const unsigned char *p;
	int seenTerminator;

	iso9660 = (struct iso9660 *)(a->format->data);

	/*
	 * Skip the first 32k (reserved area) and get the first
	 * 8 sectors of the volume descriptor table.  Of course,
	 * if the I/O layer gives us more, we'll take it.
	 */
#define RESERVED_AREA	(SYSTEM_AREA_BLOCK * LOGICAL_BLOCK_SIZE)
	h = __archive_read_ahead(a,
	    RESERVED_AREA + 8 * LOGICAL_BLOCK_SIZE,
	    &bytes_read);
	if (h == NULL)
	    return (-1);
	p = (const unsigned char *)h;

	/* Skip the reserved area. */
	bytes_read -= RESERVED_AREA;
	p += RESERVED_AREA;

	/* Check each volume descriptor. */
	seenTerminator = 0;
	for (; bytes_read > LOGICAL_BLOCK_SIZE;
	    bytes_read -= LOGICAL_BLOCK_SIZE, p += LOGICAL_BLOCK_SIZE) {
		int bid = 0;

		/* Do not handle undefined Volume Descriptor Type. */
		if (p[0] >= 4 && p[0] <= 254)
			return (0);
		/* Standard Identifier must be "CD001" */
		if (memcmp(p + 1, "CD001", 5) != 0)
			return (0);
		bid += isPVD(iso9660, p);
		bid += isJolietSVD(iso9660, p);
		bid += isBootRecord(iso9660, p);
		bid += isVolumePartition(iso9660, p);
		if (bid == 0) {
			if (isVDSetTerminator(iso9660, p)) {
				seenTerminator = 1;
				break;
			}
			return (0);
		}
	}
	/*
	 * ISO 9660 format must have Primary Volume Descriptor and
	 * Volume Descriptor Set Terminator.
	 */
	if (seenTerminator && iso9660->primary.location > 16)
		return (48);

	/* We didn't find a valid PVD; return a bid of zero. */
	return (0);
}

static int
archive_read_format_iso9660_options(struct archive_read *a,
		const char *key, const char *val)
{
	struct iso9660 *iso9660;

	iso9660 = (struct iso9660 *)(a->format->data);

	if (strcmp(key, "joliet") == 0) {
		if (val == NULL || strcmp(val, "off") == 0 ||
				strcmp(val, "ignore") == 0 ||
				strcmp(val, "disable") == 0 ||
				strcmp(val, "0") == 0)
			iso9660->opt_support_joliet = 0;
		else
			iso9660->opt_support_joliet = 1;
		return (ARCHIVE_OK);
	}
	if (strcmp(key, "rockridge") == 0 ||
	    strcmp(key, "Rockridge") == 0) {
		iso9660->opt_support_rockridge = val != NULL;
		return (ARCHIVE_OK);
	}

	/* Note: The "warn" return is just to inform the options
	 * supervisor that we didn't handle it.  It will generate
	 * a suitable error if noone used this option. */
	return (ARCHIVE_WARN);
}

static int
isBootRecord(struct iso9660 *iso9660, const unsigned char *h)
{

	/* Type of the Volume Descriptor Boot Record must be 0. */
	if (h[0] != 0)
		return (0);

	/* Volume Descriptor Version must be 1. */
	if (h[6] != 1)
		return (0);

	return (1);
}

static int
isVolumePartition(struct iso9660 *iso9660, const unsigned char *h)
{
	int32_t location;

	/* Type of the Volume Partition Descriptor must be 3. */
	if (h[0] != 3)
		return (0);

	/* Volume Descriptor Version must be 1. */
	if (h[6] != 1)
		return (0);
	/* Unused Field */
	if (h[7] != 0)
		return (0);

	location = archive_le32dec(h + 72);
	if (location <= SYSTEM_AREA_BLOCK ||
	    location >= iso9660->volume_block)
		return (0);
	if (location != archive_be32dec(h + 76))
		return (0);

	return (1);
}

static int
isVDSetTerminator(struct iso9660 *iso9660, const unsigned char *h)
{
	int i;

	/* Type of the Volume Descriptor Set Terminator must be 255. */
	if (h[0] != 255)
		return (0);

	/* Volume Descriptor Version must be 1. */
	if (h[6] != 1)
		return (0);

	/* Reserved field must be 0. */
	for (i = 7; i < 2048; ++i)
		if (h[i] != 0)
			return (0);

	return (1);
}

static int
isJolietSVD(struct iso9660 *iso9660, const unsigned char *h)
{
	const unsigned char *p;
	int i;

	/* Type 2 means it's a SVD. */
	if (h[SVD_type_offset] != 2)
		return (0);

	/* ID must be "CD001" */
	if (memcmp(h + SVD_id_offset, "CD001", 5) != 0)
		return (0);

	/* Reserved field must be 0. */
	for (i = 0; i < SVD_reserved1_size; ++i)
		if (h[SVD_reserved1_offset + i] != 0)
			return (0);
	for (i = 0; i < SVD_reserved2_size; ++i)
		if (h[SVD_reserved2_offset + i] != 0)
			return (0);
	for (i = 0; i < SVD_reserved3_size; ++i)
		if (h[SVD_reserved3_offset + i] != 0)
			return (0);

	/* FIXME: do more validations according to joliet spec. */

	/* check if this SVD contains joliet extension! */
	p = h + SVD_escape_sequences_offset;
	/* N.B. Joliet spec says p[1] == '\\', but.... */
	if (p[0] == '%' && p[1] == '/') {
		int level = 0;

		if (p[2] == '@')
			level = 1;
		else if (p[2] == 'C')
			level = 2;
		else if (p[2] == 'E')
			level = 3;
		else /* not joliet */
			return (0);

		iso9660->seenJoliet = level;

	} else /* not joliet */
		return (0);

	iso9660->logical_block_size = toi(h + SVD_logical_block_size_offset, 2);
	if (iso9660->logical_block_size <= 0)
		return (0);

	iso9660->volume_block =
	    archive_le32dec(h + SVD_volume_space_size_offset);
	if (iso9660->volume_block <= SYSTEM_AREA_BLOCK+4)
		return (0);
	iso9660->volume_size = iso9660->logical_block_size
	    * (uint64_t)iso9660->volume_block;

	/* Read Root Directory Record in Volume Descriptor. */
	p = h + SVD_root_directory_record_offset;
	if (p[DR_length_offset] != 34)
		return (0);
	iso9660->joliet.location = archive_le32dec(p + DR_extent_offset);
	iso9660->joliet.size = archive_le32dec(p + DR_size_offset);

	return (48);
}

static int
isPVD(struct iso9660 *iso9660, const unsigned char *h)
{
	const unsigned char *p;
	int i;

	/* Type of the Primary Volume Descriptor must be 1. */
	if (h[PVD_type_offset] != 1)
		return (0);

	/* ID must be "CD001" */
	if (memcmp(h + PVD_id_offset, "CD001", 5) != 0)
		return (0);

	/* PVD version must be 1. */
	if (h[PVD_version_offset] != 1)
		return (0);

	/* Reserved field must be 0. */
	if (h[PVD_reserved1_offset] != 0)
		return (0);

	/* Reserved field must be 0. */
	for (i = 0; i < PVD_reserved2_size; ++i)
		if (h[PVD_reserved2_offset + i] != 0)
			return (0);

	/* Reserved field must be 0. */
	for (i = 0; i < PVD_reserved3_size; ++i)
		if (h[PVD_reserved3_offset + i] != 0)
			return (0);

	/* Logical block size must be > 0. */
	/* I've looked at Ecma 119 and can't find any stronger
	 * restriction on this field. */
	iso9660->logical_block_size = toi(h + PVD_logical_block_size_offset, 2);
	if (iso9660->logical_block_size <= 0)
		return (0);

	iso9660->volume_block =
	    archive_le32dec(h + PVD_volume_space_size_offset);
	if (iso9660->volume_block <= SYSTEM_AREA_BLOCK+4)
		return (0);
	iso9660->volume_size = iso9660->logical_block_size
	    * (uint64_t)iso9660->volume_block;

	/* File structure version must be 1 for ISO9660/ECMA119. */
	if (h[PVD_file_structure_version_offset] != 1)
		return (0);


	/* Reserved field must be 0. */
	for (i = 0; i < PVD_reserved4_size; ++i)
		if (h[PVD_reserved4_offset + i] != 0)
			return (0);

	/* Reserved field must be 0. */
	for (i = 0; i < PVD_reserved5_size; ++i)
		if (h[PVD_reserved5_offset + i] != 0)
			return (0);

	/* XXX TODO: Check other values for sanity; reject more
	 * malformed PVDs. XXX */

	/* Read Root Directory Record in Volume Descriptor. */
	p = h + PVD_root_directory_record_offset;
	if (p[DR_length_offset] != 34)
		return (0);
	iso9660->primary.location = archive_le32dec(p + DR_extent_offset);
	iso9660->primary.size = archive_le32dec(p + DR_size_offset);

	return (48);
}

static int
read_children(struct archive_read *a, struct file_info *parent)
{
	struct iso9660 *iso9660;

	iso9660 = (struct iso9660 *)(a->format->data);
	while (iso9660->entry_bytes_remaining > 0) {
		const void *block;
		const unsigned char *p;
		ssize_t step = iso9660->logical_block_size;
		if (step > iso9660->entry_bytes_remaining)
			step = iso9660->entry_bytes_remaining;
		block = __archive_read_ahead(a, step, NULL);
		if (block == NULL) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Failed to read full block when scanning "
			    "ISO9660 directory list");
			return (ARCHIVE_FATAL);
		}
		__archive_read_consume(a, step);
		iso9660->current_position += step;
		iso9660->entry_bytes_remaining -= step;
		for (p = (const unsigned char *)block;
		     *p != 0 && p < (const unsigned char *)block + step;
		     p += *p) {
			struct file_info *child;

			/* N.B.: these special directory identifiers
			 * are 8 bit "values" even on a
			 * Joliet CD with UCS-2 (16bit) encoding.
			 */

			/* Skip '.' entry. */
			if (*(p + DR_name_len_offset) == 1
			    && *(p + DR_name_offset) == '\0')
				continue;
			/* Skip '..' entry. */
			if (*(p + DR_name_len_offset) == 1
			    && *(p + DR_name_offset) == '\001')
				continue;
			child = parse_file_info(a, parent, p);
			if (child == NULL)
				return (ARCHIVE_FATAL);
			add_entry(iso9660, child);
		}
	}
	return (ARCHIVE_OK);
}

static int
archive_read_format_iso9660_read_header(struct archive_read *a,
    struct archive_entry *entry)
{
	struct iso9660 *iso9660;
	struct file_info *file;
	int r;

	iso9660 = (struct iso9660 *)(a->format->data);

	if (!a->archive.archive_format) {
		a->archive.archive_format = ARCHIVE_FORMAT_ISO9660;
		a->archive.archive_format_name = "ISO9660";
	}

	if (iso9660->current_position == 0) {
		int64_t skipsize;
		struct vd *vd;
		const void *block;
		char seenJoliet;

		vd = &(iso9660->primary);
		if (!iso9660->opt_support_joliet)
			iso9660->seenJoliet = 0;
		if (iso9660->seenJoliet &&
			vd->location > iso9660->joliet.location)
			/* This condition is unlikely; by way of caution. */
			vd = &(iso9660->joliet);

		skipsize = LOGICAL_BLOCK_SIZE * vd->location;
		skipsize = __archive_read_skip(a, skipsize);
		if (skipsize < 0)
			return ((int)skipsize);
		iso9660->current_position = skipsize;

		block = __archive_read_ahead(a, vd->size, NULL);
		if (block == NULL) {
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_MISC,
			    "Failed to read full block when scanning "
			    "ISO9660 directory list");
			return (ARCHIVE_FATAL);
		}

		/*
		 * While reading Root Directory, flag seenJoliet
		 * must be zero to avoid converting special name
		 * 0x00(Current Directory) and next byte to UCS2.
		 */
		seenJoliet = iso9660->seenJoliet;/* Save flag. */
		iso9660->seenJoliet = 0;
		file = parse_file_info(a, NULL, block);
		if (file == NULL)
			return (ARCHIVE_FATAL);
		iso9660->seenJoliet = seenJoliet;
		if (vd == &(iso9660->primary) && iso9660->seenRockridge
		    && iso9660->seenJoliet)
			/*
			 * If iso image has RockRidge and Joliet,
			 * we use RockRidge Extensions.
			 */
			iso9660->seenJoliet = 0;
		if (vd == &(iso9660->primary) && !iso9660->seenRockridge
		    && iso9660->seenJoliet) {
			/* Switch reading data from primary to joliet. */ 
			release_file(iso9660, file);
			vd = &(iso9660->joliet);
			skipsize = LOGICAL_BLOCK_SIZE * vd->location;
			skipsize -= LOGICAL_BLOCK_SIZE *
			    iso9660->primary.location;
			skipsize = __archive_read_skip(a, skipsize);
			if (skipsize < 0)
				return ((int)skipsize);
			iso9660->current_position += skipsize;

			block = __archive_read_ahead(a, vd->size, NULL);
			if (block == NULL) {
				archive_set_error(&a->archive,
				    ARCHIVE_ERRNO_MISC,
				    "Failed to read full block when scanning "
				    "ISO9660 directory list");
				return (ARCHIVE_FATAL);
			}
			seenJoliet = iso9660->seenJoliet;/* Save flag. */
			iso9660->seenJoliet = 0;
			file = parse_file_info(a, NULL, block);
			if (file == NULL)
				return (ARCHIVE_FATAL);
			iso9660->seenJoliet = seenJoliet;
		}
		/* Store the root directory in the pending list. */
		add_entry(iso9660, file);
		if (iso9660->seenRockridge) {
			a->archive.archive_format =
			    ARCHIVE_FORMAT_ISO9660_ROCKRIDGE;
			a->archive.archive_format_name =
			    "ISO9660 with Rockridge extensions";
		}
	}

	/* Get the next entry that appears after the current offset. */
	r = next_entry_seek(a, iso9660, &file);
	if (r != ARCHIVE_OK) {
		release_file(iso9660, file);
		return (r);
	}

	iso9660->entry_bytes_remaining = file->size;
	iso9660->entry_sparse_offset = 0; /* Offset for sparse-file-aware clients. */

	if (file->offset + file->size > iso9660->volume_size) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "File is beyond end-of-media: %s", file->name);
		iso9660->entry_bytes_remaining = 0;
		iso9660->entry_sparse_offset = 0;
		release_file(iso9660, file);
		return (ARCHIVE_WARN);
	}

	/* Set up the entry structure with information about this entry. */
	archive_entry_set_mode(entry, file->mode);
	archive_entry_set_uid(entry, file->uid);
	archive_entry_set_gid(entry, file->gid);
	archive_entry_set_nlink(entry, file->nlinks);
	archive_entry_set_birthtime(entry, file->birthtime, 0);
	archive_entry_set_mtime(entry, file->mtime, 0);
	archive_entry_set_ctime(entry, file->ctime, 0);
	archive_entry_set_atime(entry, file->atime, 0);
	/* N.B.: Rock Ridge supports 64-bit device numbers. */
	archive_entry_set_rdev(entry, (dev_t)file->rdev);
	archive_entry_set_size(entry, iso9660->entry_bytes_remaining);
	archive_string_empty(&iso9660->pathname);
	archive_entry_set_pathname(entry,
	    build_pathname(&iso9660->pathname, file));
	if (file->symlink.s != NULL)
		archive_entry_copy_symlink(entry, file->symlink.s);

	/* Note: If the input isn't seekable, we can't rewind to
	 * return the same body again, so if the next entry refers to
	 * the same data, we have to return it as a hardlink to the
	 * original entry. */
	if (file->number != -1 &&
	    file->number == iso9660->previous_number
	    && file->size == iso9660->previous_size) {
		archive_entry_set_hardlink(entry,
		    iso9660->previous_pathname.s);
		archive_entry_unset_size(entry);
		iso9660->entry_bytes_remaining = 0;
		iso9660->entry_sparse_offset = 0;
		release_file(iso9660, file);
		return (ARCHIVE_OK);
	}

	/* Except for the hardlink case above, if the offset of the
	 * next entry is before our current position, we can't seek
	 * backwards to extract it, so issue a warning.  Note that
	 * this can only happen if this entry was added to the heap
	 * after we passed this offset, that is, only if the directory
	 * mentioning this entry is later than the body of the entry.
	 * Such layouts are very unusual; most ISO9660 writers lay out
	 * and record all directory information first, then store
	 * all file bodies. */
	/* TODO: Someday, libarchive's I/O core will support optional
	 * seeking.  When that day comes, this code should attempt to
	 * seek and only return the error if the seek fails.  That
	 * will give us support for whacky ISO images that require
	 * seeking while retaining the ability to read almost all ISO
	 * images in a streaming fashion. */
	if (file->offset < iso9660->current_position) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Ignoring out-of-order file @%x (%s) %jd < %jd",
		    file,
		    iso9660->pathname.s,
		    file->offset, iso9660->current_position);
		iso9660->entry_bytes_remaining = 0;
		iso9660->entry_sparse_offset = 0;
		release_file(iso9660, file);
		return (ARCHIVE_WARN);
	}

	/* Initialize zisofs variables. */
	iso9660->entry_zisofs.pz = file->pz;
	if (file->pz) {
#ifdef HAVE_ZLIB_H
		struct zisofs  *zisofs;

		zisofs = &iso9660->entry_zisofs;
		zisofs->initialized = 0;
		zisofs->pz_log2_bs = file->pz_log2_bs;
		zisofs->pz_uncompressed_size = file->pz_uncompressed_size;
		zisofs->pz_offset = 0;
		zisofs->header_avail = 0;
		zisofs->header_passed = 0;
		zisofs->block_pointers_avail = 0;
#endif
		archive_entry_set_size(entry, file->pz_uncompressed_size);
	}

	iso9660->previous_size = file->size;
	iso9660->previous_number = file->number;
	archive_strcpy(&iso9660->previous_pathname, iso9660->pathname.s);

	/* If this is a directory, read in all of the entries right now. */
	if (archive_entry_filetype(entry) == AE_IFDIR) {
		r = read_children(a, file);
		if (r != ARCHIVE_OK) {
			release_file(iso9660, file);
			return (ARCHIVE_FATAL);
		}
		/* Overwrite nlinks by proper link number which is
		 * calculated from number of sub directories. */
		archive_entry_set_nlink(entry, 2 + file->subdirs);
	}

	release_file(iso9660, file);
	return (ARCHIVE_OK);
}

static int
archive_read_format_iso9660_read_data_skip(struct archive_read *a)
{
	/* Because read_next_header always does an explicit skip
	 * to the next entry, we don't need to do anything here. */
	(void)a; /* UNUSED */
	return (ARCHIVE_OK);
}

#ifdef HAVE_ZLIB_H

static int
zisofs_read_data(struct archive_read *a,
    const void **buff, size_t *size, off_t *offset)
{
	struct iso9660 *iso9660;
	struct zisofs  *zisofs;
	const unsigned char *p;
	size_t avail;
	ssize_t bytes_read;
	size_t uncompressed_size;
	int r;

	iso9660 = (struct iso9660 *)(a->format->data);
	zisofs = &iso9660->entry_zisofs;

	p = __archive_read_ahead(a, 1, &bytes_read);
	if (bytes_read <= 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
		    "Truncated zisofs file body");
		return (ARCHIVE_FATAL);
	}
	if (bytes_read > iso9660->entry_bytes_remaining)
		bytes_read = iso9660->entry_bytes_remaining;
	avail = bytes_read;
	uncompressed_size = 0;

	if (!zisofs->initialized) {
		size_t ceil, xsize;

		/* Allocate block pointers buffer. */
		ceil = (zisofs->pz_uncompressed_size +
			(1LL << zisofs->pz_log2_bs) - 1)
			>> zisofs->pz_log2_bs;
		xsize = (ceil + 1) * 4;
		if (zisofs->block_pointers_alloc < xsize) {
			size_t alloc;

			if (zisofs->block_pointers != NULL)
				free(zisofs->block_pointers);
			alloc = ((xsize >> 10) + 1) << 10;
			zisofs->block_pointers = malloc(alloc);
			if (zisofs->block_pointers == NULL) {
				archive_set_error(&a->archive, ENOMEM,
				    "No memory for zisofs decompression");
				return (ARCHIVE_FATAL);
			}
			zisofs->block_pointers_alloc = alloc;
		}
		zisofs->block_pointers_size = xsize;

		/* Allocate uncompressed data buffer. */
		xsize = 1UL << zisofs->pz_log2_bs;
		if (zisofs->uncompressed_buffer_size < xsize) {
			if (zisofs->uncompressed_buffer != NULL)
				free(zisofs->uncompressed_buffer);
			zisofs->uncompressed_buffer = malloc(xsize);
			if (zisofs->uncompressed_buffer == NULL) {
				archive_set_error(&a->archive, ENOMEM,
				    "No memory for zisofs decompression");
				return (ARCHIVE_FATAL);
			}
		}
		zisofs->uncompressed_buffer_size = xsize;

		/*
		 * Read the file header, and check the magic code of zisofs.
		 */
		if (zisofs->header_avail < sizeof(zisofs->header)) {
			xsize = sizeof(zisofs->header) - zisofs->header_avail;
			if (avail < xsize)
				xsize = avail;
			memcpy(zisofs->header + zisofs->header_avail, p, xsize);
			zisofs->header_avail += xsize;
			avail -= xsize;
			p += xsize;
		}
		if (!zisofs->header_passed &&
		    zisofs->header_avail == sizeof(zisofs->header)) {
			int err = 0;

			if (memcmp(zisofs->header, zisofs_magic,
			    sizeof(zisofs_magic)) != 0)
				err = 1;
			if (archive_le32dec(zisofs->header + 8)
			    != zisofs->pz_uncompressed_size)
				err = 1;
			if (zisofs->header[12] != 4)
				err = 1;
			if (zisofs->header[13] != zisofs->pz_log2_bs)
				err = 1;
			if (err) {
				archive_set_error(&a->archive,
				    ARCHIVE_ERRNO_FILE_FORMAT,
				    "Illegal zisofs file body");
				return (ARCHIVE_FATAL);
			}
			zisofs->header_passed = 1;
		}
		/*
		 * Read block pointers.
		 */
		if (zisofs->header_passed &&
		    zisofs->block_pointers_avail < zisofs->block_pointers_size) {
			xsize = zisofs->block_pointers_size
			    - zisofs->block_pointers_avail;
			if (avail < xsize)
				xsize = avail;
			memcpy(zisofs->block_pointers
			    + zisofs->block_pointers_avail, p, xsize);
			zisofs->block_pointers_avail += xsize;
			avail -= xsize;
			p += xsize;
		    	if (zisofs->block_pointers_avail
			    == zisofs->block_pointers_size) {
				/* We've got all block pointers and initialize
				 * related variables.	*/
				zisofs->block_off = 0;
				zisofs->block_avail = 0;
				/* Complete a initialization */
				zisofs->initialized = 1;
			}
		}

		if (!zisofs->initialized)
			goto next_data; /* We need more datas. */
	}

	/*
	 * Get block offsets from block pointers.
	 */
	if (zisofs->block_avail == 0) {
		uint32_t bst, bed;

		if (zisofs->block_off + 4 >= zisofs->block_pointers_size) {
			/* There isn't a pair of offsets. */
			archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
			    "Illegal zisofs block pointers");
			return (ARCHIVE_FATAL);
		}
		bst = archive_le32dec(zisofs->block_pointers + zisofs->block_off);
		if (bst != zisofs->pz_offset + (bytes_read - avail)) {
			/* TODO: Should we seek offset of current file by bst ? */
			archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
			    "Illegal zisofs block pointers(cannot seek)");
			return (ARCHIVE_FATAL);
		}
		bed = archive_le32dec(
		    zisofs->block_pointers + zisofs->block_off + 4);
		if (bed < bst) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
			    "Illegal zisofs block pointers");
			return (ARCHIVE_FATAL);
		}
		zisofs->block_avail = bed - bst;
		zisofs->block_off += 4;

		/* Initialize compression library for new block. */
		if (zisofs->stream_valid)
			r = inflateReset(&zisofs->stream);
		else
			r = inflateInit(&zisofs->stream);
		if (r != Z_OK) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Can't initialize zisofs decompression.");
			return (ARCHIVE_FATAL);
		}
		zisofs->stream_valid = 1;
		zisofs->stream.total_in = 0;
		zisofs->stream.total_out = 0;
	}

	/*
	 * Make uncompressed datas.
	 */
	if (zisofs->block_avail == 0) {
		memset(zisofs->uncompressed_buffer, 0,
		    zisofs->uncompressed_buffer_size);
		uncompressed_size = zisofs->uncompressed_buffer_size;
	} else {
		zisofs->stream.next_in = (Bytef *)(uintptr_t)(const void *)p;
		if (avail > zisofs->block_avail)
			zisofs->stream.avail_in = zisofs->block_avail;
		else
			zisofs->stream.avail_in = avail;
		zisofs->stream.next_out = zisofs->uncompressed_buffer;
		zisofs->stream.avail_out = zisofs->uncompressed_buffer_size;

		r = inflate(&zisofs->stream, 0);
		switch (r) {
		case Z_OK: /* Decompressor made some progress.*/
		case Z_STREAM_END: /* Found end of stream. */
			break;
		default:
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "zisofs decompression failed (%d)", r);
			return (ARCHIVE_FATAL);
		}
		uncompressed_size =
		    zisofs->uncompressed_buffer_size - zisofs->stream.avail_out;
		avail -= zisofs->stream.next_in - p;
		zisofs->block_avail -= zisofs->stream.next_in - p;
	}
next_data:
	bytes_read -= avail;
	*buff = zisofs->uncompressed_buffer;
	*size = uncompressed_size;
	*offset = iso9660->entry_sparse_offset;
	iso9660->entry_sparse_offset += uncompressed_size;
	iso9660->entry_bytes_remaining -= bytes_read;
	iso9660->current_position += bytes_read;
	zisofs->pz_offset += bytes_read;
	__archive_read_consume(a, bytes_read);

	return (ARCHIVE_OK);
}

#else /* HAVE_ZLIB_H */

static int
zisofs_read_data(struct archive_read *a,
    const void **buff, size_t *size, off_t *offset)
{

	(void)buff;/* UNUSED */
	(void)size;/* UNUSED */
	(void)offset;/* UNUSED */
	archive_set_error(&a->archive, ARCHIVE_ERRNO_FILE_FORMAT,
	    "zisofs is not supported on this platform.");
	return (ARCHIVE_FATAL);
}

#endif /* HAVE_ZLIB_H */

static int
archive_read_format_iso9660_read_data(struct archive_read *a,
    const void **buff, size_t *size, off_t *offset)
{
	ssize_t bytes_read;
	struct iso9660 *iso9660;

	iso9660 = (struct iso9660 *)(a->format->data);
	if (iso9660->entry_bytes_remaining <= 0) {
		*buff = NULL;
		*size = 0;
		*offset = iso9660->entry_sparse_offset;
		return (ARCHIVE_EOF);
	}
	if (iso9660->entry_zisofs.pz)
		return (zisofs_read_data(a, buff, size, offset));

	*buff = __archive_read_ahead(a, 1, &bytes_read);
	if (bytes_read == 0)
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Truncated input file");
	if (*buff == NULL)
		return (ARCHIVE_FATAL);
	if (bytes_read > iso9660->entry_bytes_remaining)
		bytes_read = iso9660->entry_bytes_remaining;
	*size = bytes_read;
	*offset = iso9660->entry_sparse_offset;
	iso9660->entry_sparse_offset += bytes_read;
	iso9660->entry_bytes_remaining -= bytes_read;
	iso9660->current_position += bytes_read;
	__archive_read_consume(a, bytes_read);
	return (ARCHIVE_OK);
}

static int
archive_read_format_iso9660_cleanup(struct archive_read *a)
{
	struct iso9660 *iso9660;
	struct file_info *file;
	int r = ARCHIVE_OK;

	iso9660 = (struct iso9660 *)(a->format->data);
	while (iso9660->cache_files.index < iso9660->cache_files.count)
		release_file(iso9660,
		    iso9660->cache_files.files[iso9660->cache_files.index++]);
	free(iso9660->cache_files.files);
	while ((file = next_entry(iso9660)) != NULL)
		release_file(iso9660, file);
	free(iso9660->read_ce_req.reqs);
	archive_string_free(&iso9660->pathname);
	archive_string_free(&iso9660->previous_pathname);
	if (iso9660->pending_files)
		free(iso9660->pending_files);
#ifdef HAVE_ZLIB_H
	free(iso9660->entry_zisofs.uncompressed_buffer);
	free(iso9660->entry_zisofs.block_pointers);
	if (iso9660->entry_zisofs.stream_valid) {
		if (inflateEnd(&iso9660->entry_zisofs.stream) != Z_OK) {
			archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
			    "Failed to clean up zlib decompressor");
			r = ARCHIVE_FATAL;
		}
	}
#endif
	free(iso9660);
	(a->format->data) = NULL;
	return (r);
}

/*
 * This routine parses a single ISO directory record, makes sense
 * of any extensions, and stores the result in memory.
 */
static struct file_info *
parse_file_info(struct archive_read *a, struct file_info *parent,
    const unsigned char *isodirrec)
{
	struct iso9660 *iso9660;
	struct file_info *file;
	size_t name_len;
	const unsigned char *rr_start, *rr_end;
	const unsigned char *p;
	size_t dr_len;
	int32_t location;
	int flags;

	iso9660 = (struct iso9660 *)(a->format->data);

	dr_len = (size_t)isodirrec[DR_length_offset];
	name_len = (size_t)isodirrec[DR_name_len_offset];
	location = archive_le32dec(isodirrec + DR_extent_offset);
	/* Sanity check that dr_len needs at least 34. */
	if (dr_len < 34) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Invalid length of directory record");
		return (NULL);
	}
	/* Sanity check that name_len doesn't exceed dr_len. */
	if (dr_len - 33 < name_len || name_len == 0) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Invalid length of file identifier");
		return (NULL);
	}
	/* Sanity check that location doesn't exceed volume block.
	 * Don't check lower limit of location; it's possibility
	 * the location has negative value when file type is symbolic
	 * link or file size is zero. As far as I know latest mkisofs
	 * do that.
	 */
	if (location >= iso9660->volume_block) {
		archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Invalid location of extent of file");
		return (NULL);
	}

	/* Create a new file entry and copy data from the ISO dir record. */
	file = (struct file_info *)malloc(sizeof(*file));
	if (file == NULL) {
		archive_set_error(&a->archive, ENOMEM,
		    "No memory for file entry");
		return (NULL);
	}
	memset(file, 0, sizeof(*file));
	file->parent = parent;
	if (parent != NULL)
		parent->refcount++;
	file->offset = iso9660->logical_block_size * (uint64_t)location;
	file->size = toi(isodirrec + DR_size_offset, DR_size_size);
	file->mtime = isodate7(isodirrec + DR_date_offset);
	file->ctime = file->atime = file->mtime;

	p = isodirrec + DR_name_offset;
	/* Rockridge extensions (if any) follow name.  Compute this
	 * before fidgeting the name_len below. */
	rr_start = p + name_len + (name_len & 1 ? 0 : 1);
	rr_end = isodirrec + dr_len;

	if (iso9660->seenJoliet) {
		/* Joliet names are max 64 chars (128 bytes) according to spec,
		 * but genisoimage/mkisofs allows recording longer Joliet
		 * names which are 103 UCS2 characters(206 bytes) by their
		 * option '-joliet-long'.
		 */
		wchar_t wbuff[103+1], *wp;
		const unsigned char *c;

		if (name_len > 206)
			name_len = 206;
		/* convert BE UTF-16 to wchar_t */
		for (c = p, wp = wbuff;
				c < (p + name_len) &&
				wp < (wbuff + sizeof(wbuff)/sizeof(*wbuff) - 1);
				c += 2) {
			*wp++ = (((255 & (int)c[0]) << 8) | (255 & (int)c[1]));
		}
		*wp = L'\0';

#if 0 /* untested code, is it at all useful on Joliet? */
		/* trim trailing first version and dot from filename.
		 *
		 * Remember we where in UTF-16BE land!
		 * SEPARATOR 1 (.) and SEPARATOR 2 (;) are both
		 * 16 bits big endian characters on Joliet.
		 *
		 * TODO: sanitize filename?
		 *       Joliet allows any UCS-2 char except:
		 *       *, /, :, ;, ? and \.
		 */
		/* Chop off trailing ';1' from files. */
		if (*(wp-2) == ';' && *(wp-1) == '1') {
			wp-=2;
			*wp = L'\0';
		}

		/* Chop off trailing '.' from filenames. */
		if (*(wp-1) == '.')
			*(--wp) = L'\0';
#endif

		/* store the result in the file name field. */
		archive_strappend_w_utf8(&file->name, wbuff);
	} else {
		/* Chop off trailing ';1' from files. */
		if (name_len > 2 && p[name_len - 2] == ';' &&
				p[name_len - 1] == '1')
			name_len -= 2;
		/* Chop off trailing '.' from filenames. */
		if (name_len > 1 && p[name_len - 1] == '.')
			--name_len;

		archive_strncpy(&file->name, (const char *)p, name_len);
	}

	flags = isodirrec[DR_flags_offset];
	if (flags & 0x02)
		file->mode = AE_IFDIR | 0700;
	else
		file->mode = AE_IFREG | 0400;
	/*
	 * Use location for file number.
	 * File number is treated as inode number to find out harlink
	 * target. If Rockridge extensions is being used, file number
	 * will be overwritten by FILE SERIAL NUMBER of RRIP "PX"
	 * extension.
	 * NOTE: Older mkisofs did not record that FILE SERIAL NUMBER
	 * in ISO images.
	 */
	if (file->size == 0)
		/* If file->size is zero, its location points wrong place.
		 * Dot not use it for file number. */
		file->number = -1;
	else
		file->number = (int64_t)(uint32_t)location;

	/* Rockridge extensions overwrite information from above. */
	if (iso9660->opt_support_rockridge) {
		if (parent == NULL && rr_end - rr_start >= 7) {
			p = rr_start;
			if (p[0] == 'S' && p[1] == 'P'
			    && p[2] == 7 && p[3] == 1
			    && p[4] == 0xBE && p[5] == 0xEF) {
				/*
				 * SP extension stores the suspOffset
				 * (Number of bytes to skip between
				 * filename and SUSP records.)
				 * It is mandatory by the SUSP standard
				 * (IEEE 1281).
				 *
				 * It allows SUSP to coexist with
				 * non-SUSP uses of the System
				 * Use Area by placing non-SUSP data
				 * before SUSP data.
				 *
				 * SP extension must be in the root
				 * directory entry, disable all SUSP
				 * processing if not found.
				 */
				iso9660->suspOffset = p[6];
				iso9660->seenSUSP = 1;
				rr_start += 7;
			}
		}
		if (iso9660->seenSUSP) {
			file->name_continues = 0;
			file->symlink_continues = 0;
			rr_start += iso9660->suspOffset;
			parse_rockridge(iso9660, file, rr_start, rr_end);
		} else
			/* If there isn't SUSP, disable parsing
			 * rock ridge extensions. */
			iso9660->opt_support_rockridge = 0;
	}

	/* Tell file's parent how many children that parent has. */
	if (parent != NULL && (flags & 0x02))
		parent->subdirs++;

#if DEBUG
	/* DEBUGGING: Warn about attributes I don't yet fully support. */
	if ((flags & ~0x02) != 0) {
		fprintf(stderr, "\n ** Unrecognized flag: ");
		dump_isodirrec(stderr, isodirrec);
		fprintf(stderr, "\n");
	} else if (toi(isodirrec + DR_volume_sequence_number_offset, 2) != 1) {
		fprintf(stderr, "\n ** Unrecognized sequence number: ");
		dump_isodirrec(stderr, isodirrec);
		fprintf(stderr, "\n");
	} else if (*(isodirrec + DR_file_unit_size_offset) != 0) {
		fprintf(stderr, "\n ** Unexpected file unit size: ");
		dump_isodirrec(stderr, isodirrec);
		fprintf(stderr, "\n");
	} else if (*(isodirrec + DR_interleave_offset) != 0) {
		fprintf(stderr, "\n ** Unexpected interleave: ");
		dump_isodirrec(stderr, isodirrec);
		fprintf(stderr, "\n");
	} else if (*(isodirrec + DR_ext_attr_length_offset) != 0) {
		fprintf(stderr, "\n ** Unexpected extended attribute length: ");
		dump_isodirrec(stderr, isodirrec);
		fprintf(stderr, "\n");
	}
#endif
	return (file);
}

static void
add_entry(struct iso9660 *iso9660, struct file_info *file)
{
	uint64_t file_offset, parent_offset;
	int hole, parent;

	/* Expand our pending files list as necessary. */
	if (iso9660->pending_files_used >= iso9660->pending_files_allocated) {
		struct file_info **new_pending_files;
		int new_size = iso9660->pending_files_allocated * 2;

		if (iso9660->pending_files_allocated < 1024)
			new_size = 1024;
		/* Overflow might keep us from growing the list. */
		if (new_size <= iso9660->pending_files_allocated)
			__archive_errx(1, "Out of memory");
		new_pending_files = (struct file_info **)malloc(new_size * sizeof(new_pending_files[0]));
		if (new_pending_files == NULL)
			__archive_errx(1, "Out of memory");
		memcpy(new_pending_files, iso9660->pending_files,
		    iso9660->pending_files_allocated * sizeof(new_pending_files[0]));
		if (iso9660->pending_files != NULL)
			free(iso9660->pending_files);
		iso9660->pending_files = new_pending_files;
		iso9660->pending_files_allocated = new_size;
	}

	file_offset = file->offset + file->size;

	/*
	 * Start with hole at end, walk it up tree to find insertion point.
	 */
	hole = iso9660->pending_files_used++;
	while (hole > 0) {
		parent = (hole - 1)/2;
		parent_offset = iso9660->pending_files[parent]->offset
		    + iso9660->pending_files[parent]->size;
		if (file_offset >= parent_offset) {
			iso9660->pending_files[hole] = file;
			return;
		}
		// Move parent into hole <==> move hole up tree.
		iso9660->pending_files[hole] = iso9660->pending_files[parent];
		hole = parent;
	}
	iso9660->pending_files[0] = file;
}

static void
parse_rockridge(struct iso9660 *iso9660, struct file_info *file,
    const unsigned char *p, const unsigned char *end)
{

	while (p + 4 < end  /* Enough space for another entry. */
	    && p[0] >= 'A' && p[0] <= 'Z' /* Sanity-check 1st char of name. */
	    && p[1] >= 'A' && p[1] <= 'Z' /* Sanity-check 2nd char of name. */
	    && p[2] >= 4 /* Sanity-check length. */
	    && p + p[2] <= end) { /* Sanity-check length. */
		const unsigned char *data = p + 4;
		int data_length = p[2] - 4;
		int version = p[3];

		/*
		 * Yes, each 'if' here does test p[0] again.
		 * Otherwise, the fall-through handling to catch
		 * unsupported extensions doesn't work.
		 */
		switch(p[0]) {
		case 'C':
			if (p[0] == 'C' && p[1] == 'E') {
				if (version == 1 && data_length == 24) {
					/*
					 * CE extension comprises:
					 *   8 byte sector containing extension
					 *   8 byte offset w/in above sector
					 *   8 byte length of continuation
					 */
					int32_t location =
					    archive_le32dec(data);
					file->ce_offset =
					    archive_le32dec(data+8);
					file->ce_size =
					    archive_le32dec(data+16);
					register_CE(iso9660, location, file);
				}
				break;
			}
			/* FALLTHROUGH */
		case 'N':
			if (p[0] == 'N' && p[1] == 'M') {
				if (version == 1) {
					parse_rockridge_NM1(file,
					    data, data_length);
					iso9660->seenRockridge = 1;
				}
				break;
			}
			/* FALLTHROUGH */
		case 'P':
			if (p[0] == 'P' && p[1] == 'D') {
				/*
				 * PD extension is padding;
				 * contents are always ignored.
				 */
				break;
			}
			if (p[0] == 'P' && p[1] == 'N') {
				if (version == 1 && data_length == 16) {
					file->rdev = toi(data,4);
					file->rdev <<= 32;
					file->rdev |= toi(data + 8, 4);
					iso9660->seenRockridge = 1;
				}
				break;
			}
			if (p[0] == 'P' && p[1] == 'X') {
				/*
				 * PX extension comprises:
				 *   8 bytes for mode,
				 *   8 bytes for nlinks,
				 *   8 bytes for uid,
				 *   8 bytes for gid,
				 *   8 bytes for inode.
				 */
				if (version == 1) {
					if (data_length >= 8)
						file->mode
						    = toi(data, 4);
					if (data_length >= 16)
						file->nlinks
						    = toi(data + 8, 4);
					if (data_length >= 24)
						file->uid
						    = toi(data + 16, 4);
					if (data_length >= 32)
						file->gid
						    = toi(data + 24, 4);
					if (data_length >= 40)
						file->number
						    = toi(data + 32, 4);
					iso9660->seenRockridge = 1;
				}
				break;
			}
			/* FALLTHROUGH */
		case 'R':
			if (p[0] == 'R' && p[1] == 'R' && version == 1) {
				/*
				 * RR extension comprises:
				 *    one byte flag value
				 * This extension is obsolete,
				 * so contents are always ignored.
				 */
				break;
			}
			/* FALLTHROUGH */
		case 'S':
			if (p[0] == 'S' && p[1] == 'L') {
				if (version == 1) {
					parse_rockridge_SL1(file,
					    data, data_length);
					iso9660->seenRockridge = 1;
				}
				break;
			}
			if (p[0] == 'S' && p[1] == 'T'
			    && data_length == 0 && version == 1) {
				/*
				 * ST extension marks end of this
				 * block of SUSP entries.
				 *
				 * It allows SUSP to coexist with
				 * non-SUSP uses of the System
				 * Use Area by placing non-SUSP data
				 * after SUSP data.
				 */
				iso9660->seenSUSP = 0;
				iso9660->seenRockridge = 0;
				return;
			}
		case 'T':
			if (p[0] == 'T' && p[1] == 'F') {
				if (version == 1) {
					parse_rockridge_TF1(file,
					    data, data_length);
					iso9660->seenRockridge = 1;
				}
				break;
			}
			/* FALLTHROUGH */
		case 'Z':
			if (p[0] == 'Z' && p[1] == 'F') {
				if (version == 1)
					parse_rockridge_ZF1(file,
					    data, data_length);
				break;
			}
			/* FALLTHROUGH */
		default:
			/* The FALLTHROUGHs above leave us here for
			 * any unsupported extension. */
			break;
		}



		p += p[2];
	}
}

static int
register_CE(struct iso9660 *iso9660, int32_t location,
    struct file_info *file)
{
	struct read_ce_req *p;
	uint64_t offset;
	int i;

	offset = ((uint64_t)location) * (uint64_t)iso9660->logical_block_size;
	if (((file->mode & AE_IFMT) == AE_IFREG &&
	    offset >= file->offset) ||
	    offset < iso9660->current_position) {
		/*archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
		    "Invalid location in RRIP \"CE\"");*/
		return (ARCHIVE_FATAL);
	}
	if (iso9660->read_ce_req.cnt + 1 > iso9660->read_ce_req.allocated) {
		int alloc = iso9660->read_ce_req.allocated;

		if (alloc == 0)
			alloc = 8;
		else
			alloc *= 2;
		p = malloc(alloc * sizeof(*p));
		if (p == NULL)
			__archive_errx(1, "Out of memory");
		iso9660->read_ce_req.allocated = alloc;
		if (iso9660->read_ce_req.reqs != NULL) {
			memcpy(p, iso9660->read_ce_req.reqs,
			    iso9660->read_ce_req.cnt * sizeof(*p));
			free(iso9660->read_ce_req.reqs);
		}
		iso9660->read_ce_req.reqs = p;
	}
	for (i = 0; i < iso9660->read_ce_req.cnt; i++) {
		if (iso9660->read_ce_req.reqs[i].offset > offset) {
			p = &iso9660->read_ce_req.reqs[i];
			memmove(p+1, p,
			    (iso9660->read_ce_req.cnt -i) * sizeof(*p));
			p->offset = offset;
			p->file = file;
			iso9660->read_ce_req.cnt++;
			return (ARCHIVE_OK);
		}
	}
	p = &iso9660->read_ce_req.reqs[iso9660->read_ce_req.cnt];
	p->offset = offset;
	p->file = file;
	iso9660->read_ce_req.cnt++;
	return (ARCHIVE_OK);
}

static int
read_CE(struct archive_read *a, struct iso9660 *iso9660)
{
	struct read_ce_req *ce;
	const unsigned char *b, *p, *end;
	size_t step;

	/* Read RRIP "CE" System Use Entry. */
	while (iso9660->read_ce_req.cnt &&
	    iso9660->read_ce_req.reqs[0].offset ==
	    iso9660->current_position) {
		step = iso9660->logical_block_size;
		b = __archive_read_ahead(a, step, NULL);
		if (b == NULL) {
			archive_set_error(&a->archive,
			    ARCHIVE_ERRNO_MISC,
			    "Failed to read full block when scanning "
			    "ISO9660 directory list");
			return (ARCHIVE_FATAL);
		}
		ce = iso9660->read_ce_req.reqs;
		do {
			p = b + ce->file->ce_offset;
			end = p + ce->file->ce_size;
			parse_rockridge(iso9660, ce->file, p, end);
			memmove(ce, ce+1, sizeof(*ce) *
			    (iso9660->read_ce_req.cnt -1));
			iso9660->read_ce_req.cnt--;
		} while (iso9660->read_ce_req.cnt &&
		    ce->offset == iso9660->current_position);
		__archive_read_consume(a, step);
		iso9660->current_position += step;
	}
	return (ARCHIVE_OK);
}

static void
parse_rockridge_NM1(struct file_info *file,
		    const unsigned char *data, int data_length)
{
	if (!file->name_continues)
		archive_string_empty(&file->name);
	file->name_continues = 0;
	if (data_length < 1)
		return;
	/*
	 * NM version 1 extension comprises:
	 *   1 byte flag, value is one of:
	 *     = 0: remainder is name
	 *     = 1: remainder is name, next NM entry continues name
	 *     = 2: "."
	 *     = 4: ".."
	 *     = 32: Implementation specific
	 *     All other values are reserved.
	 */
	switch(data[0]) {
	case 0:
		if (data_length < 2)
			return;
		archive_strncat(&file->name, (const char *)data + 1, data_length - 1);
		break;
	case 1:
		if (data_length < 2)
			return;
		archive_strncat(&file->name, (const char *)data + 1, data_length - 1);
		file->name_continues = 1;
		break;
	case 2:
		archive_strcat(&file->name, ".");
		break;
	case 4:
		archive_strcat(&file->name, "..");
		break;
	default:
		return;
	}

}

static void
parse_rockridge_TF1(struct file_info *file, const unsigned char *data,
    int data_length)
{
	char flag;
	/*
	 * TF extension comprises:
	 *   one byte flag
	 *   create time (optional)
	 *   modify time (optional)
	 *   access time (optional)
	 *   attribute time (optional)
	 *  Time format and presence of fields
	 *  is controlled by flag bits.
	 */
	if (data_length < 1)
		return;
	flag = data[0];
	++data;
	--data_length;
	if (flag & 0x80) {
		/* Use 17-byte time format. */
		if ((flag & 1) && data_length >= 17) {
			/* Create time. */
			file->birthtime = isodate17(data);
			data += 17;
			data_length -= 17;
		}
		if ((flag & 2) && data_length >= 17) {
			/* Modify time. */
			file->mtime = isodate17(data);
			data += 17;
			data_length -= 17;
		}
		if ((flag & 4) && data_length >= 17) {
			/* Access time. */
			file->atime = isodate17(data);
			data += 17;
			data_length -= 17;
		}
		if ((flag & 8) && data_length >= 17) {
			/* Attribute change time. */
			file->ctime = isodate17(data);
			data += 17;
			data_length -= 17;
		}
	} else {
		/* Use 7-byte time format. */
		if ((flag & 1) && data_length >= 7) {
			/* Create time. */
			file->birthtime = isodate17(data);
			data += 7;
			data_length -= 7;
		}
		if ((flag & 2) && data_length >= 7) {
			/* Modify time. */
			file->mtime = isodate7(data);
			data += 7;
			data_length -= 7;
		}
		if ((flag & 4) && data_length >= 7) {
			/* Access time. */
			file->atime = isodate7(data);
			data += 7;
			data_length -= 7;
		}
		if ((flag & 8) && data_length >= 7) {
			/* Attribute change time. */
			file->ctime = isodate7(data);
			data += 7;
			data_length -= 7;
		}
	}
}

static void
parse_rockridge_SL1(struct file_info *file, const unsigned char *data,
    int data_length)
{
	const char *separator = "";

	if (!file->symlink_continues || file->symlink.length < 1)
		archive_string_empty(&file->symlink);
	else if (!file->symlink_continues &&
	    file->symlink.s[file->symlink.length - 1] != '/')
		separator = "/";
	file->symlink_continues = 0;

	/*
	 * Defined flag values:
	 *  0: This is the last SL record for this symbolic link
	 *  1: this symbolic link field continues in next SL entry
	 *  All other values are reserved.
	 */
	if (data_length < 1)
		return;
	switch(*data) {
	case 0:
		break;
	case 1:
		file->symlink_continues = 1;
		break;
	default:
		return;
	}
	++data;  /* Skip flag byte. */
	--data_length;

	/*
	 * SL extension body stores "components".
	 * Basically, this is a complicated way of storing
	 * a POSIX path.  It also interferes with using
	 * symlinks for storing non-path data. <sigh>
	 *
	 * Each component is 2 bytes (flag and length)
	 * possibly followed by name data.
	 */
	while (data_length >= 2) {
		unsigned char flag = *data++;
		unsigned char nlen = *data++;
		data_length -= 2;

		archive_strcat(&file->symlink, separator);
		separator = "/";

		switch(flag) {
		case 0: /* Usual case, this is text. */
			if (data_length < nlen)
				return;
			archive_strncat(&file->symlink,
			    (const char *)data, nlen);
			break;
		case 0x01: /* Text continues in next component. */
			if (data_length < nlen)
				return;
			archive_strncat(&file->symlink,
			    (const char *)data, nlen);
			separator = "";
			break;
		case 0x02: /* Current dir. */
			archive_strcat(&file->symlink, ".");
			break;
		case 0x04: /* Parent dir. */
			archive_strcat(&file->symlink, "..");
			break;
		case 0x08: /* Root of filesystem. */
			archive_strcat(&file->symlink, "/");
			separator = "";
			break;
		case 0x10: /* Undefined (historically "volume root" */
			archive_string_empty(&file->symlink);
			archive_strcat(&file->symlink, "ROOT");
			break;
		case 0x20: /* Undefined (historically "hostname") */
			archive_strcat(&file->symlink, "hostname");
			break;
		default:
			/* TODO: issue a warning ? */
			return;
		}
		data += nlen;
		data_length -= nlen;
	}
}

static void
parse_rockridge_ZF1(struct file_info *file, const unsigned char *data,
    int data_length)
{

	if (data[0] == 0x70 && data[1] == 0x7a && data_length == 12) {
		/* paged zlib */
		file->pz = 1;
		file->pz_log2_bs = data[3];
		file->pz_uncompressed_size = archive_le32dec(&data[4]);
	}
}


static void
release_file(struct iso9660 *iso9660, struct file_info *file)
{
	struct file_info *parent;

	if (file == NULL)
		return;

	if (file->refcount == 0) {
		parent = file->parent;
		archive_string_free(&file->name);
		archive_string_free(&file->symlink);
		free(file);
		if (parent != NULL) {
			parent->refcount--;
			release_file(iso9660, parent);
		}
	}
}

static int
next_entry_seek(struct archive_read *a, struct iso9660 *iso9660,
    struct file_info **pfile)
{
	struct file_info *file;
	uint64_t offset;

	*pfile = file = next_cache_entry(iso9660);
	if (file == NULL)
		return (ARCHIVE_EOF);

	/* Read data which recorded by RRIP "CE" extension. */
	if (read_CE(a, iso9660) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

	/* Don't waste time seeking for zero-length bodies. */
	if (file->size == 0)
		file->offset = iso9660->current_position;

	offset = file->offset;

	/* Seek forward to the start of the entry. */
	if (iso9660->current_position < offset) {
		off_t step = offset - iso9660->current_position;
		off_t bytes_read;
		bytes_read = __archive_read_skip(a, step);
		if (bytes_read < 0)
			return (bytes_read);
		iso9660->current_position = offset;
	}

	/* We found body of file; handle it now. */
	return (ARCHIVE_OK);
}

static struct file_info *
next_cache_entry(struct iso9660 *iso9660)
{
	struct cache_files *cf = &(iso9660->cache_files);
	struct file_info *file;

	if (cf->index < cf->count)
		return (cf->files[cf->index++]);

	file = next_entry(iso9660);
	if (file == NULL)
		return (NULL);

	if ((file->mode & AE_IFMT) != AE_IFREG)
		return (file);

	/* Collect files which has the same file serial number. */
	cf->index = cf->count = 0;
	while (file != NULL && file->number != -1 &&
	    iso9660->pending_files_used > 0 &&
	    file->number == iso9660->pending_files[0]->number) {
		if (cf->count + 1 >= cf->allocated) {
			struct file_info **pp;
			int size;

			if (cf->allocated == 0)
				size = 32;
			else
				size = cf->allocated << 1;
			pp = malloc(size);
			if (pp == NULL)
				__archive_errx(1, "Out of memory");
			if (cf->count)
				memcpy(pp, cf->files,
				    cf->count * sizeof(cf->files[0]));
			free(cf->files);
			cf->files = pp;
		}
		cf->files[cf->count++] = file;
		file = next_entry(iso9660);
	}
	if (cf->count) {
		int i;

		cf->files[cf->count++] = file;
		/* cf->count is the same as number of hardlink,
		 * so much so that it overwrite nlinks by value
		 * of cf->count. */
		for (i = 0; i < cf->count; i++)
			cf->files[i]->nlinks = cf->count;
		return (cf->files[cf->index++]);
	} else
		return (file);
}

static struct file_info *
next_entry(struct iso9660 *iso9660)
{
	uint64_t a_offset, b_offset, c_offset;
	int a, b, c;
	struct file_info *r, *tmp;

	if (iso9660->pending_files_used < 1)
		return (NULL);

	/*
	 * The first file in the list is the earliest; we'll return this.
	 */
	r = iso9660->pending_files[0];

	/*
	 * Move the last item in the heap to the root of the tree
	 */
	iso9660->pending_files[0]
	    = iso9660->pending_files[--iso9660->pending_files_used];

	/*
	 * Rebalance the heap.
	 */
	a = 0; // Starting element and its offset
	a_offset = iso9660->pending_files[a]->offset
	    + iso9660->pending_files[a]->size;
	for (;;) {
		b = a + a + 1; // First child
		if (b >= iso9660->pending_files_used)
			return (r);
		b_offset = iso9660->pending_files[b]->offset
		    + iso9660->pending_files[b]->size;
		c = b + 1; // Use second child if it is smaller.
		if (c < iso9660->pending_files_used) {
			c_offset = iso9660->pending_files[c]->offset
			    + iso9660->pending_files[c]->size;
			if (c_offset < b_offset) {
				b = c;
				b_offset = c_offset;
			}
		}
		if (a_offset <= b_offset)
			return (r);
		tmp = iso9660->pending_files[a];
		iso9660->pending_files[a] = iso9660->pending_files[b];
		iso9660->pending_files[b] = tmp;
		a = b;
	}
}

static unsigned int
toi(const void *p, int n)
{
	const unsigned char *v = (const unsigned char *)p;
	if (n > 1)
		return v[0] + 256 * toi(v + 1, n - 1);
	if (n == 1)
		return v[0];
	return (0);
}

static time_t
isodate7(const unsigned char *v)
{
	struct tm tm;
	int offset;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = v[0];
	tm.tm_mon = v[1] - 1;
	tm.tm_mday = v[2];
	tm.tm_hour = v[3];
	tm.tm_min = v[4];
	tm.tm_sec = v[5];
	/* v[6] is the signed timezone offset, in 1/4-hour increments. */
	offset = ((const signed char *)v)[6];
	if (offset > -48 && offset < 52) {
		tm.tm_hour -= offset / 4;
		tm.tm_min -= (offset % 4) * 15;
	}
	return (time_from_tm(&tm));
}

static time_t
isodate17(const unsigned char *v)
{
	struct tm tm;
	int offset;
	memset(&tm, 0, sizeof(tm));
	tm.tm_year = (v[0] - '0') * 1000 + (v[1] - '0') * 100
	    + (v[2] - '0') * 10 + (v[3] - '0')
	    - 1900;
	tm.tm_mon = (v[4] - '0') * 10 + (v[5] - '0');
	tm.tm_mday = (v[6] - '0') * 10 + (v[7] - '0');
	tm.tm_hour = (v[8] - '0') * 10 + (v[9] - '0');
	tm.tm_min = (v[10] - '0') * 10 + (v[11] - '0');
	tm.tm_sec = (v[12] - '0') * 10 + (v[13] - '0');
	/* v[16] is the signed timezone offset, in 1/4-hour increments. */
	offset = ((const signed char *)v)[16];
	if (offset > -48 && offset < 52) {
		tm.tm_hour -= offset / 4;
		tm.tm_min -= (offset % 4) * 15;
	}
	return (time_from_tm(&tm));
}

static time_t
time_from_tm(struct tm *t)
{
#if HAVE_TIMEGM
	/* Use platform timegm() if available. */
	return (timegm(t));
#else
	/* Else use direct calculation using POSIX assumptions. */
	/* First, fix up tm_yday based on the year/month/day. */
	mktime(t);
	/* Then we can compute timegm() from first principles. */
	return (t->tm_sec + t->tm_min * 60 + t->tm_hour * 3600
	    + t->tm_yday * 86400 + (t->tm_year - 70) * 31536000
	    + ((t->tm_year - 69) / 4) * 86400 -
	    ((t->tm_year - 1) / 100) * 86400
	    + ((t->tm_year + 299) / 400) * 86400);
#endif
}

static const char *
build_pathname(struct archive_string *as, struct file_info *file)
{
	if (file->parent != NULL && archive_strlen(&file->parent->name) > 0) {
		build_pathname(as, file->parent);
		archive_strcat(as, "/");
	}
	if (archive_strlen(&file->name) == 0)
		archive_strcat(as, ".");
	else
		archive_string_concat(as, &file->name);
	return (as->s);
}

#if DEBUG
static void
dump_isodirrec(FILE *out, const unsigned char *isodirrec)
{
	fprintf(out, " l %d,",
	    toi(isodirrec + DR_length_offset, DR_length_size));
	fprintf(out, " a %d,",
	    toi(isodirrec + DR_ext_attr_length_offset, DR_ext_attr_length_size));
	fprintf(out, " ext 0x%x,",
	    toi(isodirrec + DR_extent_offset, DR_extent_size));
	fprintf(out, " s %d,",
	    toi(isodirrec + DR_size_offset, DR_extent_size));
	fprintf(out, " f 0x%02x,",
	    toi(isodirrec + DR_flags_offset, DR_flags_size));
	fprintf(out, " u %d,",
	    toi(isodirrec + DR_file_unit_size_offset, DR_file_unit_size_size));
	fprintf(out, " ilv %d,",
	    toi(isodirrec + DR_interleave_offset, DR_interleave_size));
	fprintf(out, " seq %d,",
	    toi(isodirrec + DR_volume_sequence_number_offset, DR_volume_sequence_number_size));
	fprintf(out, " nl %d:",
	    toi(isodirrec + DR_name_len_offset, DR_name_len_size));
	fprintf(out, " `%.*s'",
	    toi(isodirrec + DR_name_len_offset, DR_name_len_size), isodirrec + DR_name_offset);
}
#endif
