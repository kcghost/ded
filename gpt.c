/* SPDX-License-Identifier: MIT */
/* gpt.c
 * Copyright (C) 2025 Casey Fitzpatrick <kcghost@gmail.com>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <linux/fs.h>
#include <uchar.h>
#include <wchar.h>
#include <locale.h>
#include <limits.h>

char* program_name = "gpt";
int flags = 0;
// just not handling sizing outside the current spec
#define HDR_SZ  92
#define PART_SZ 128
// 12 digits can represent 1 PiB in 4096 blocks
#define BLOCKS_DIGITS 12
// longest known type alias "root-loongarch64-verity-sig"
#define TYPE_DIGITS 27

// https://uefi.org/specs/UEFI/2.11/05_GUID_Partition_Table_Format.html
typedef struct __attribute__((__packed__)) {
	char     signature[8];
	uint16_t revision_minor;
	uint16_t revision_major;
	uint32_t header_size;
	uint32_t crc;
	uint32_t reserved;
	uint64_t this_lba;
	uint64_t alt_lba;
	uint64_t first_lba;
	uint64_t last_lba;
	uint8_t  disk_guid[16];
	uint64_t ptable_lba;
	uint32_t ptable_entries;
	uint32_t entry_size;
	uint32_t ptable_crc;
	// rest of lba is reserved and must be zero
} gpt_hdr;
_Static_assert(sizeof(gpt_hdr) == HDR_SZ, "bad header size!");

#define PARTNAME_CHARS 36
typedef struct __attribute__((__packed__)) {
	uint8_t  type[16];
	uint8_t  id[16];
	uint64_t start_lba;
	uint64_t end_lba;
	union __attribute__((__packed__)) {
		uint64_t attr;
		struct __attribute__((__packed__)) {
			uint64_t efiflags:3;
			uint64_t reserved:45;
			uint64_t typeflags:16;
		};
		// the last 16 are type id specific.
		// But in practice well known flags dont yet overlap
		struct __attribute__((__packed__)) {
			// common efi attributes
			uint64_t required:1;
			uint64_t no_blockio:1;
			uint64_t legacy_bootable:1;
			uint64_t :45;
			// chromeos
			uint64_t priority:4;        // bits 48-51
			uint64_t tries_remaining:4; // bits 52-55
			uint64_t boot_success:1;    // bit 56
			// not known to be used by any common types
			uint64_t bit57:1;
			uint64_t bit58:1;
			// DPS spec
			uint64_t growfs:1;          // bit 59
			// MS flags, but DPS uses 63 and 60 with similar enough meanings
			uint64_t read_only:1;       // bit 60
			uint64_t shadow_copy:1;     // bit 61
			uint64_t hidden:1;          // bit 62
			uint64_t no_automount:1;    // bit 63
		};
	};
	char16_t name[PARTNAME_CHARS];
	// rest of partition entry size and must be zero
} part_entry;
_Static_assert(sizeof(part_entry) == PART_SZ, "bad part_entry size!");

typedef struct {
	char device[PATH_MAX];
	int fd;
	unsigned int lbsz;
	uint64_t last_lba;
	gpt_hdr hdr;
	int max_label_digits;
	int max_size_digits;
} gpt_dev;

#define str(token) #token
#define xstr(token) str(token)
#define fail(...) do { fputs("crit: ", stderr); fprintf(stderr, __VA_ARGS__); exit(EXIT_FAILURE); } while(0)
#define warn(...) do { fputs("warn: ", stderr); fprintf(stderr, __VA_ARGS__); } while(0)
#define wr(condition, msg, code) do { if(condition) { warn(msg "\n"); return code; } } while(0)

int digits(uint64_t i) {
	int digits = 1;
	while((i = i / 10) > 0) { digits++; }
	return digits;
}

// crc adapted from public domain code: https://web.mit.edu/freebsd/head/sys/libkern/crc32.c
const uint32_t crc32_tab[] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
	0xe963a535, 0x9e6495a3,	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
	0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
	0xf3b97148, 0x84be41de,	0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,	0x14015c4f, 0x63066cd9,
	0xfa0f3d63, 0x8d080df5,	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
	0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,	0x35b5a8fa, 0x42b2986c,
	0xdbbbc9d6, 0xacbcf940,	0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
	0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
	0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,	0x76dc4190, 0x01db7106,
	0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
	0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
	0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
	0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
	0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
	0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
	0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
	0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
	0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
	0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
	0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
	0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
	0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
	0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
	0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
	0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
	0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
	0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
	0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
	0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
	0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
	0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693,
	0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
	0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

uint32_t crc32(uint32_t start, const void *buf, size_t size) {
	const uint8_t *p = buf;
	uint32_t crc;

	crc = start ^ 0xFFFFFFFF;
	while (size--) {
		crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFF;
}

#define UUID_STR_SZ 37
void uuid_str(char* str, uint8_t* bytes) {
	snprintf(
		str, UUID_STR_SZ,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		bytes[3],  bytes[2],  bytes[1],  bytes[0],
		bytes[5],  bytes[4],  bytes[7],  bytes[6],
		bytes[8],  bytes[9],  bytes[10], bytes[11],
		bytes[12], bytes[13], bytes[14], bytes[15]
	);
}

void seekread(int fd, off_t offset, void* buf, size_t count) {
	if(lseek(fd, offset, SEEK_SET) == -1) { fail("seek"); }
	if(read(fd, buf, count) != count) { fail("read"); }
}

#define NOT_GPT -1
#define UNEXPECTED -2
#define CORRUPT -3
#define CORRUPT_PTABLE -4
#define CORRUPT_BACKUP -5

int validate_header(gpt_hdr* hdr, gpt_dev* dev) {
	uint32_t crc;
	part_entry part;
	int len;
	
	wr(strncmp("EFI PART", hdr->signature, 8) != 0, "not GPT!", NOT_GPT);

	crc = hdr->crc;
	hdr->crc = 0;
	wr(crc32(0, hdr, hdr->header_size) != crc, "header integrity check failed!", CORRUPT);
	hdr->crc = crc;
	
	wr(hdr->revision_major != 1 || hdr->revision_minor != 0, "unexpected GPT revision!", UNEXPECTED);
	wr(hdr->header_size != HDR_SZ, "invalid header size", UNEXPECTED);
	wr(hdr->entry_size != PART_SZ, "can't handle large part entry sizes", UNEXPECTED);

	crc = 0;
	for(int i = 0; i < hdr->ptable_entries; i++) {
		seekread(dev->fd, (hdr->ptable_lba * dev->lbsz) + (i * hdr->entry_size), &part, sizeof(part));
		wr(part.reserved != 0, "unexpected partition attributes in reserved field!", UNEXPECTED);
		crc = crc32(crc, &part, PART_SZ);
	}
	wr(crc != hdr->ptable_crc, "corrupted partition table!", CORRUPT_PTABLE);

	return 0;
}

int check_device(gpt_dev* dev) {
	gpt_hdr alt;
	int primary_ret;
	int alt_ret;

	seekread(dev->fd, (dev->last_lba * dev->lbsz), &alt, sizeof(alt));

	primary_ret = validate_header(&(dev->hdr), dev);
	alt_ret = validate_header(&alt, dev);

	if(primary_ret == NOT_GPT && alt_ret == NOT_GPT) {
		return NOT_GPT;
	}
	if(primary_ret != 0 && alt_ret == 0) {
		warn("primary GPT table is faulty. But the backup appears fine, maybe try restoring the backup?\n");
		return primary_ret;
	}
	if(primary_ret == 0 && alt_ret != 0) {
		warn("backup GPT table is faulty. But the primary table appears fine, maybe try rewriting the backup?\n");
		return CORRUPT_BACKUP;
	}
	if(primary_ret != 0 && alt_ret != 0) {
		warn("Both primary and backup tables are faulty!\n");
		return primary_ret;
	}

	wr(dev->hdr.this_lba != 1, "unexpected lba address in primary", UNEXPECTED);
	wr(dev->hdr.alt_lba != dev->last_lba, "unexpected alt lba address in primary", UNEXPECTED);
	wr(alt.this_lba != dev->hdr.alt_lba, "unexpected lba address in alt", UNEXPECTED);
	wr(alt.ptable_crc != dev->hdr.ptable_crc, "backup table has different contents!", UNEXPECTED);

	return 0;
}

int open_device(char* device, gpt_dev* dev)  {
	uint64_t size_bytes;
	wr((dev->fd = open(device, O_RDONLY)) == -1, "could not open device", -1);
	wr((ioctl(dev->fd, BLKSSZGET, &(dev->lbsz))) != 0, "block size", -1);
	wr((ioctl(dev->fd, BLKGETSIZE64, &size_bytes)) != 0, "device size", -1);

	dev->last_lba = (size_bytes / dev->lbsz) - 1;
	dev->max_size_digits = digits(dev->last_lba);
	
	seekread(dev->fd, (1 * dev->lbsz), &(dev->hdr), sizeof(dev->hdr));
	if(dev->hdr.this_lba != 1) { fail("unexpected current lba"); }

	strcpy(dev->device, device);

	return 0;
}

// callback for each non-empty partition entry
void iterate_part(gpt_dev* dev, void (*on_part)(gpt_dev* dev, int num, part_entry* part)) {
	part_entry part;

	for(int part_num = 0; part_num < dev->hdr.ptable_entries; part_num++) {
		seekread(dev->fd, (dev->hdr.ptable_lba * dev->lbsz) + (part_num * dev->hdr.entry_size), &part, sizeof(part));
		// if type id is not all zeroes trigger on_part
		for(int i = 0; i < 16; i++) {
			if(part.type[i] != 0) {
				on_part(dev, part_num, &part);
				break;
			}
		}
	}
}

void c16tolocal(char16_t* in, char* out) {
	mbstate_t ps = {0};
	size_t r;
	while(in[0] != u'\0') {
		if((r = c16rtomb(out, in[0], &ps)) == -1) { fail("could not parse label!"); }
		in++;
		out += r;
	}
	// write final null char
	if((r = c16rtomb(out, in[0], &ps)) == -1) { fail("could not parse label!"); }
}

int validate_device(gpt_dev* dev) {
	int ret;
	ret = check_device(dev);
	switch(ret) {
		case 0:
			break;
		case NOT_GPT:
			wprintf(L"%hs not_gpt\n", dev->device);
			break;
		case UNEXPECTED:
			warn("An unexpected problem occurred validating the partition table on %s.\n"
				"This could indicate a corrupt table. Or just that this program can't handle a new format or edge case.\n", dev->device);
			break;
		case CORRUPT:
		case CORRUPT_PTABLE:
		case CORRUPT_BACKUP:
		default:
			warn("A corruption problem was detected on %s.\n"
				"You may need to restore or rewrite the backup table. Or start a new table.\n", dev->device);
			break;
	}
	return ret;
}

void print_part(gpt_dev* dev, int num, part_entry* part) {
	char type_uuid[UUID_STR_SZ];
	char id_uuid[UUID_STR_SZ];
	// just account for maximum possible length of localized string
	char name[PARTNAME_CHARS * MB_LEN_MAX];
	char typeflags_s[17];
	int comma;
	
	uuid_str(type_uuid, part->type);
	uuid_str(id_uuid, part->id);
	c16tolocal(part->name, name);

	// num uuid start end type common-attr type-attr label
	// TODO: type short name lookup?
#define pcomma do { if(comma) { wprintf(L","); } } while(0)
#define pattr(attr) if(part->attr) { pcomma; wprintf(L"%s", xstr(attr)); comma = 1; }
	wprintf(L"%03d|%s|%0*lu|%0*lu|%s|",
		num, id_uuid,
		dev->max_size_digits,part->start_lba, dev->max_size_digits,part->end_lba,
		type_uuid
	);

	if(flags) {
		comma = 0;
		pattr(required);
		pattr(no_blockio);
		pattr(legacy_bootable);
		wprintf(L"|");

		// TODO: different handling based on type id?
		comma = 0;
		pattr(growfs);
		pattr(read_only);
		pattr(shadow_copy);
		pattr(hidden);
		pattr(no_automount);
	} else {
		for(int i = 0; i < 16; i++) {
			typeflags_s[i] = '0' + ((part->typeflags >> i) & 1);
		}
		typeflags_s[16] = '\0';

		wprintf(L"%c%c%c|%s",
			part->required ? '1': '0',
			part->no_blockio ? '1': '0',
			part->legacy_bootable ? '1': '0',
			typeflags_s
		);
	}
	
	wprintf(L"|%s\n", name);
}

void print_device(char* device) {
	int ret;
	gpt_dev dev = {0};
	char uuid[UUID_STR_SZ];
	
	if(open_device(device, &dev) != 0) { return; }
	if(validate_device(&dev) != 0) { return; }

	uuid_str(uuid, dev.hdr.disk_guid);
	fprintf(stderr, "%-*s|%-36s|%-*s|logical block size\n",
		(int)strlen(device), "path", "disk uuid", dev.max_size_digits,"last");
	wprintf(L"%s|%s|%lu|%u\n", device, uuid, dev.last_lba, dev.lbsz);
	
	// num uuid start end common-attr type type-attr label
	fprintf(stderr, "num|%-36s|%-*s|%-*s|%-36s|cmn|type attributes |label\n",
		"uuid", dev.max_size_digits,"start", dev.max_size_digits,"end", "typeid"
	);
	iterate_part(&dev, print_part);
}

void usage() {
	wprintf(
		L"Usage: %hs [OPTIONS] [DEVICE]\n"
		"\n"
		"Print or modify contents of GPT partition tables.\n"
		"If no DEVICE is provided all disks are listed.\n"
		"\n"
		"OPTIONs:\n"
		"-f      interpret attributes as comma separated flags\n"
		"        notice: type attributes may be wrong depedning on type\n" 
		"\n"
		, program_name);
}

int main(int argc, char* argv[]) {
	// set locale to system default, so we can print "wide" characters with wprintf (for UTF-16 partition label)
	// Once either printf or wprintf is used the other stops working for that stream
	// Use wprintf for stdout, regular printf for stderr
	setlocale(LC_ALL, "");
	
	if(argv[0] != NULL) {
		program_name = argv[0];
		argv++;
	}
	if(argv[0] == NULL) {
		usage();
		return 1;
	}
	while(argv[0] != NULL && argv[0][0] == '-') {
		while(argv[0][1] != '\0') {
			switch(argv[0][1]) {
				case 'h':
					usage();
					return 0;
				case 'f':
					flags = 1;
					break;
				default:
					usage();
					return 1;
			}
			argv[0]++;
		}
		argv++;
	}

	print_device(argv[0]);
	
	return 0;
}
