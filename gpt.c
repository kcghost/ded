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
#include <uchar.h>
#include <wchar.h>
#include <locale.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/random.h>
#include <linux/fs.h>
#include <linux/hdreg.h>

char* program_name = "gpt";
int flags = 0;
int first_print = 1;
// minimal size without extra reserved space (that must be zero in current spec)
#define MBR_SZ 512
#define HDR_SZ  92
#define PART_SZ 128
// semi-arbitrary size for buffered read/write
#define BLOCK_SZ 512
// 12 digits can represent 1 PiB in 4096 blocks
#define BLOCKS_DIGITS 12
// longest known type alias "root-loongarch64-verity-sig"
#define TYPE_DIGITS 27

#define getbit(in,bit) ((in >> bit) & 1)
#define setbit(out,bit,val) out = (out & ~((typeof(out))1 << bit)) | ((typeof(out))(val) << bit)

typedef struct {
	int head;
	int sector;
	int cylinder;
} chs;

typedef struct __attribute__((__packed__)) {
	uint8_t  head;
	// the first two high bits are part of a 10-bit cylinder value, the rest is "sector"
	uint8_t  ch_sector;
	// 8 low bits of cylinder
	uint8_t  cl;
} mbr_chs;

typedef struct __attribute__((__packed__)) {
	uint8_t  boot_indicator;
	mbr_chs  start;
	uint8_t  type;
	mbr_chs  end;
	uint32_t start_lba;
	uint32_t size_lba;
} mbr_part;

typedef struct __attribute__((__packed__)) {
	uint8_t   boot_code[440];
	uint32_t  unique_sig;
	uint16_t  unknown;
	mbr_part  part[4];
	uint16_t  signature;
} mbr;
_Static_assert(sizeof(mbr) == MBR_SZ, "bad mbr size!");

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
	uint64_t attr;
	char16_t name[PARTNAME_CHARS];
	// rest of partition entry size and must be zero
} part_entry;
_Static_assert(sizeof(part_entry) == PART_SZ, "bad part_entry size!");

typedef struct {
	char device[PATH_MAX];
	int fd;
	unsigned int lbsz;
	uint64_t last_lba;
	struct hd_geometry geo;
	mbr m;
	gpt_hdr hdr;
	gpt_hdr alt;
	int is_valid_gpt;
	int max_size_digits;
	int part_entries;
	int padding[4];
	int max_entries;
	uint32_t hdr_sz;
	uint32_t part_sz;
	uint8_t id[16];
} gpt_dev;

#define str(token) #token
#define xstr(token) str(token)
#define fail(...) do { fputs("crit: ", stderr); fprintf(stderr, __VA_ARGS__); fputs("\n", stderr); exit(EXIT_FAILURE); } while(0)
#define warn(...) do { fputs("warn: ", stderr); fprintf(stderr, __VA_ARGS__); fputs("\n", stderr); } while(0)
#define wr(condition, msg, code) do { if(condition) { warn(msg "\n"); return code; } } while(0)

int digits(uint64_t i) {
	int digits = 1;
	while((i = i / 10) > 0) { digits++; }
	return digits;
}

chs mtochs(mbr_chs mchs) {
	chs r;
	r.head = mchs.head;
	r.sector = mchs.ch_sector & 0b00111111;
	r.cylinder = ((mchs.ch_sector & 0b11000000)<<2) | mchs.cl;
	return r;
}

mbr_chs chstom(chs c) {
	mbr_chs r;
	r.head = c.head;
	r.ch_sector = c.sector | (c.cylinder>>8);
	r.cl = c.cylinder & 0b11111111;
	return r;
}

void bitstring(uint64_t in, int bits, char* out) {
	for(int i = 0; i < bits; i++) {
		out[i] = getbit(in, bits-1-i) ? '1' : '0';
	}
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
	while(size--) {
		crc = crc32_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFF;
}

// without needing a buffer predict the crc32 for a given number of blank bytes
uint32_t crc32_zero(uint32_t start, size_t size) {
	uint32_t crc;

	crc = start ^ 0xFFFFFFFF;
	while(size--) {
		crc = crc32_tab[crc & 0xFF] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFF;
}

#define UUID_STR_SZ 37
void uuid_str(char* str, uint8_t* bytes) {
	// the first 3 sections are little-endian for...reasons? reasons.
	snprintf(
		str, UUID_STR_SZ,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		bytes[3],  bytes[2],  bytes[1],  bytes[0],
		bytes[5],  bytes[4],
		bytes[7],  bytes[6],
		bytes[8],  bytes[9],  bytes[10], bytes[11],
		bytes[12], bytes[13], bytes[14], bytes[15]
	);
}

void parse_uuid(char* in, uint8_t* dst) {
	if(sscanf(in,
		"%02hhx%02hhx%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx-%02hhx%02hhx%02hhx%02hhx%02hhx%02hhx",
		dst+3,  dst+2,  dst+1,  dst+0,
		dst+5,  dst+4,
		dst+7,  dst+6,
		dst+8,  dst+9,  dst+10, dst+11,
		dst+12, dst+13, dst+14, dst+15
	) != 16) {
		fail("could not parse UUID!");
	}
}

int not_zero(uint8_t* buf, size_t sz) {
	for(size_t i = 0; i< sz; i++) {
		if(buf[i] != 0) {
			return 1;
		}
	}
	return 0;
}

void gen_guid4(uint8_t* dst) {
	// RFC4122 version 4 (random guid), but this explains guids much better than the RFC: https://guid.one/guid/make
	// Almost all GUIDs in practical use for EFI are version 4, even very early ones like ms-basic and linux-generic
	// Though it is neat you can tell the esp guid was generated at exactly 1999-04-21T19:24:01.5625	
	// grub introduced a "bios" one that is just the bytes "Hah!IdontNeedEFI", and is not compliant at all
	// nobody *really* cares, but ideally it should be RFC4122 compliant.
	if(getrandom(dst, 16, 0) != 16) { fail("could not get random bytes!"); }
	dst[6] = dst[6] & 0x0f | 0x40;
	dst[8] = dst[8] & 0x3f | 0x80;
}

void seekread(int fd, off_t offset, void* buf, size_t count) {
	if(lseek(fd, offset, SEEK_SET) == -1) { fail("seek"); }
	if(read(fd, buf, count) != count) { fail("read"); }
}

// if not zero return -1
int seekread_zero(int fd, off_t offset, size_t count) {
	uint8_t buf[BLOCK_SZ] = {0};

	if(lseek(fd, offset, SEEK_SET) == -1) { fail("seek"); }
	while(count > BLOCK_SZ) {
		count = count - BLOCK_SZ;
		if(read(fd, buf, BLOCK_SZ) != BLOCK_SZ) { perror(""); fail("read"); }
		if(not_zero(buf, BLOCK_SZ)) { return -1; }
	}
	if(count) {
		if(read(fd, buf, count) != count) { perror(""); fail("read"); }
		if(not_zero(buf, count)) { return -1; }
	}
	
	return 0;
}

void seekwrite(int fd, off_t offset, void* buf, size_t count) {
	if(lseek(fd, offset, SEEK_SET) == -1) { fail("seek"); }
	if(write(fd, buf, count) != count) { perror(""); fail("write"); }
}

void seekwrite_zero(int fd, off_t offset, size_t count) {
	uint8_t buf[BLOCK_SZ] = {0};

	if(lseek(fd, offset, SEEK_SET) == -1) { fail("seek"); }
	while(count > BLOCK_SZ) {
		count = count - BLOCK_SZ;
		if(write(fd, buf, BLOCK_SZ) != BLOCK_SZ) { perror(""); fail("write"); }
	}
	if(count) {
		if(write(fd, buf, count) != count) { perror(""); fail("write"); }
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
	if(c16rtomb(out, in[0], &ps) == -1) { fail("could not parse label!"); }
}

void localtoc16(char* in, char16_t* out, size_t len) {
	mbstate_t ps = {0};
	size_t r;
	char16_t* end = out + len;
	
	while(in[0] != '\0') {
		r = mbrtoc16(out, in, 1, &ps);
		switch(r) {
			case -1:
				fail("could not parse label!");
			case -2:
				in++;
				break;
			case -3:
				out++;
				break;
			case 1:
				in++;
				out++;
				break;
			default:
				fail("unexpected parsing error!");
		}
		if(out > end) { fail("label too long!"); }
	}
	// write final null char
	if(mbrtoc16(out, in, 1, &ps) == -1) { fail("could not parse label!"); }
}

#define NOT_GPT -1
#define UNEXPECTED -2
#define CORRUPT -3
#define CORRUPT_PTABLE -4
#define CORRUPT_BACKUP -5

int validate_header(gpt_hdr* hdr, gpt_dev* dev, uint64_t lba) {
	uint32_t reported_crc;
	uint32_t calc_crc;
	part_entry part;
	int len;
	
	if(strncmp("EFI PART", hdr->signature, 8) != 0) { return NOT_GPT; }
	wr(hdr->header_size < HDR_SZ || hdr->header_size > dev->lbsz, "illegal header size!", UNEXPECTED);
	wr(hdr->revision_major != 1 || hdr->revision_minor != 0, "unexpected GPT revision!", UNEXPECTED);
	
	reported_crc = hdr->crc;
	hdr->crc = 0;
	calc_crc = crc32(0, hdr, HDR_SZ);
	// the header can be bigger than HDR_SZ, but the extra space *must* be zeroed
	if(hdr->header_size > HDR_SZ) {
		calc_crc = crc32_zero(calc_crc, hdr->header_size - HDR_SZ);
	}
	wr(seekread_zero(dev->fd, (lba * dev->lbsz) + HDR_SZ, hdr->header_size - HDR_SZ) != 0, "reserved part of header not zero!", UNEXPECTED);
	wr(calc_crc != reported_crc, "header integrity check failed!", CORRUPT);
	hdr->crc = reported_crc;
	wr(hdr->entry_size * hdr->ptable_entries < (16*1024), "partition table too small!", UNEXPECTED);

	// it might not be practical, but any power of two greater than 128 is legal
	if(hdr->entry_size < 128 || (hdr->entry_size & (hdr->entry_size - 1)) != 0) {
		warn("illegal partition entry size!");
		return UNEXPECTED;
	}

	calc_crc = 0;
	for(int i = 0; i < hdr->ptable_entries; i++) {
		seekread(dev->fd, (hdr->ptable_lba * dev->lbsz) + (i * hdr->entry_size), &part, sizeof(part));
		wr((part.attr & 0b0000000000000000111111111111111111111111111111111111111111111000)!= 0, "unexpected partition attributes in reserved field!", UNEXPECTED);
		calc_crc = crc32(calc_crc, &part, PART_SZ);
		if(not_zero(part.type, 16)) {
			dev->part_entries++;
		}
		// each entry may be bigger than 128, but the extra space *must* be zeroed
		if(hdr->entry_size > PART_SZ) {
			calc_crc = crc32_zero(calc_crc, hdr->entry_size - PART_SZ);
			wr(seekread_zero(dev->fd,
				(hdr->ptable_lba * dev->lbsz) + 
				(i * hdr->entry_size) + PART_SZ,
				hdr->entry_size - PART_SZ) != 0,
			"reserved part of part entry not zero!", UNEXPECTED);
		}
	}
	wr(calc_crc != hdr->ptable_crc, "corrupted partition table!", CORRUPT_PTABLE);

	return 0;
}

// populate hdr and validate the device is actually GPT
int check_device(gpt_dev* dev) {
	int primary_ret;
	int alt_ret;
	uint32_t calc_crc;

	primary_ret = validate_header(&(dev->hdr), dev, 1);
	alt_ret = validate_header(&(dev->alt), dev, dev->last_lba);

	if(primary_ret == NOT_GPT && alt_ret == NOT_GPT) {
		return NOT_GPT;
	}
	if(primary_ret != 0 && alt_ret == 0) {
		warn("primary GPT table is faulty. But the backup appears fine, maybe try restoring the backup?");
		return primary_ret;
	}
	if(primary_ret == 0 && alt_ret != 0) {
		warn("backup GPT table is faulty. But the primary table appears fine, maybe try rewriting the backup?");
		return CORRUPT_BACKUP;
	}
	if(primary_ret != 0 && alt_ret != 0) {
		warn("Both primary and backup tables are faulty!");
		return primary_ret;
	}

	wr(dev->hdr.this_lba != 1, "unexpected lba address in primary", UNEXPECTED);
	wr(dev->hdr.alt_lba != dev->last_lba, "unexpected alt lba address in primary", UNEXPECTED);
	wr(dev->alt.this_lba != dev->hdr.alt_lba, "unexpected lba address in alt", UNEXPECTED);
	wr(dev->alt.ptable_crc != dev->hdr.ptable_crc, "backup table has different contents!", UNEXPECTED);

	wr(dev->hdr.ptable_lba <= 1 || dev->hdr.ptable_lba >= dev->hdr.first_lba, "bad ptable address in primary!", UNEXPECTED);
	wr(dev->alt.ptable_lba >= dev->last_lba || dev->alt.ptable_lba <= dev->alt.last_lba, "bad ptable address in backup!", UNEXPECTED);

	// check that disk guid and other fields are exactly the same as primary
	dev->alt.this_lba = 1;
	dev->alt.alt_lba = dev->last_lba;
	dev->alt.ptable_lba = dev->hdr.ptable_lba;
	dev->alt.crc = 0;
	calc_crc = crc32(0, &(dev->alt), HDR_SZ);
	if(dev->alt.header_size > HDR_SZ) {
		calc_crc = crc32_zero(calc_crc, dev->alt.header_size - HDR_SZ);
	}
	wr(calc_crc != dev->hdr.crc, "primary and backup headers dont have the same contents", UNEXPECTED);
	
	dev->is_valid_gpt = 1;
	return 0;
}

void ensure_valid(gpt_dev* dev) {
	if(dev->is_valid_gpt == 0) {
		check_device(dev);
	}

	if(dev->is_valid_gpt == 0) {
		fail("not a valid gpt device! need to fix first!");
	}
}

int open_device(char* device, gpt_dev* dev, int rflag)  {
	uint64_t size_bytes;
	wr((dev->fd = open(device, rflag)) == -1, "could not open device", -1);
	if(ioctl(dev->fd, BLKSSZGET, &(dev->lbsz)) != 0) {
		warn("%s not a block device, assuming 512 is the logical block size", device);
		dev->lbsz = 512;
	}
	if(ioctl(dev->fd, BLKGETSIZE64, &size_bytes) != 0) {
		// might just be a file rather than a block device
		wr((size_bytes = lseek(dev->fd, 0, SEEK_END)) == -1, "could not get device size!", -1);
	}

	if(ioctl(dev->fd, HDIO_GETGEO, &(dev->geo)) != 0) {
		warn("could not read geometry for %s, assuming traditional max values for hpc and spt", device);
		dev->geo.heads = 255;
		dev->geo.sectors = 63;
	}

	dev->last_lba = (size_bytes / dev->lbsz) - 1;
	dev->max_size_digits = digits(dev->last_lba);

	dev->max_entries = 128; // default

	strcpy(dev->device, device);

	// read entire mbr, primary gpt header, and backup gpt header
	// partitions are read and checked on the fly
	// none of these are necessarily valid at this point though
	seekread(dev->fd, 0, &(dev->m), MBR_SZ);
	seekread(dev->fd, (1 * dev->lbsz), &(dev->hdr), HDR_SZ);
	seekread(dev->fd, (dev->last_lba * dev->lbsz), &(dev->alt), HDR_SZ);

	return 0;
}

// callback for each non-empty partition entry
void iterate_part(gpt_dev* dev, void (*on_part)(gpt_dev* dev, int num, part_entry* part)) {
	part_entry part;

	for(int part_num = 0; part_num < dev->hdr.ptable_entries; part_num++) {
		seekread(dev->fd, (dev->hdr.ptable_lba * dev->lbsz) + (part_num * dev->hdr.entry_size), &part, sizeof(part));
		// if type id is not all zeroes trigger on_part
		if(not_zero(part.type, sizeof(part.type))) {
			on_part(dev, part_num + 1, &part);
		}
	}
}


int validate_device(gpt_dev* dev) {
	int ret;
	ret = check_device(dev);
	switch(ret) {
		case 0:
			break;
		case NOT_GPT:
			warn("%s does not have gpt table.", dev->device);
			break;
		case UNEXPECTED:
			warn("An unexpected problem occurred validating the partition table on %s."
				"This could indicate a corrupt table. Or just that this program can't handle a new format or edge case.", dev->device);
			break;
		case CORRUPT:
		case CORRUPT_PTABLE:
		case CORRUPT_BACKUP:
		default:
			warn("A corruption problem was detected on %s."
				"You may need to restore or rewrite the backup table. Or start a new table.", dev->device);
			break;
	}
	return ret;
}

void print_part(gpt_dev* dev, int num, part_entry* part) {
	char type_uuid[UUID_STR_SZ];
	char id_uuid[UUID_STR_SZ];
	// just account for maximum possible length of localized string
	char name[PARTNAME_CHARS * MB_LEN_MAX];
	char cmn_bits[3+1] = {0};
	char type_bits[16+1] = {0};
	
	uuid_str(type_uuid, part->type);
	uuid_str(id_uuid, part->id);
	c16tolocal(part->name, name);

	bitstring(part->attr >> 48, 16, type_bits);
	bitstring(part->attr, 3, cmn_bits);

	// num uuid start end type type-attr common-attr label
	wprintf(L"%03d|%s|%0*lu|%0*lu|%s|%s|%s|%s\n",
		num, id_uuid,
		dev->max_size_digits,part->start_lba, dev->max_size_digits,part->end_lba,
		type_uuid,
		type_bits,
		cmn_bits,
		name
	);
}

void print_device(gpt_dev* dev) {
	int ret;
	mbr m = {0};
	chs start;
	chs end;
	char uuid[UUID_STR_SZ];

	// print separator breaks after first print
	if(first_print) {
		first_print = 0;
	} else {
		fprintf(stderr, "\n");
	}

	fprintf(stderr,
		"dsk|%-*s|lbsz|%-*s|hpc|spt|cyls |start sector\n",
		(int)strlen(dev->device),"path",
		dev->max_size_digits, "last lb"
	);
	wprintf(L"dsk|%s|%04u|%0*u|%03u|%03u|%05u|%lu\n",
		dev->device,
		dev->lbsz,
		dev->max_size_digits,dev->last_lba,
		dev->geo.heads,
		dev->geo.sectors,
		dev->geo.cylinders,
		dev->geo.start
	);

	if(dev->m.signature == 0xaa55) {
		fprintf(stderr,"mbr|uniq sig|code crc|unknown\n");
		wprintf(L"mbr|%08x|%08x|%04x\n",
			m.unique_sig,
			crc32(0, m.boot_code, sizeof(m.boot_code)),
			m.unknown
		);
		fprintf(stderr,"mN|os|start     |size      |shd|ss|scyl|ehd|es|ecyl\n");
		for(int i = 0; i < 4; i++) {
			start = mtochs(m.part[i].start);
			end = mtochs(m.part[i].end);
			wprintf(L"m%d|%02x|%010u|%010u|%03u|%02u|%04u|%03u|%02u|%04u\n",
				i,
				m.part[i].type,
				m.part[i].start_lba,
				m.part[i].size_lba,
				start.head,
				start.sector,
				start.cylinder,
				end.head,
				end.sector,
				end.cylinder
			);
		}
	}

	if(validate_device(dev) != 0) { return; }

	uuid_str(uuid, dev->hdr.disk_guid);
	fprintf(stderr,
		"gpt|%-36s|%-*s|%-*s|max entries\n",
		"disk uuid",
		dev->max_size_digits,"fst avl",
		dev->max_size_digits,"lst avl"
	);
	wprintf(L"gpt|%s|%0*lu|%0*lu|%u\n",
		uuid,
		dev->max_size_digits, dev->hdr.first_lba, dev->max_size_digits,dev->hdr.last_lba,
		dev->hdr.ptable_entries
	);
	
	if(dev->part_entries) {
		// num uuid start end common-attr type type-attr label
		fprintf(stderr, "num|%-36s|%-*s|%-*s|%-36s|type attributes |cmn|label\n",
			"partuuid",
			dev->max_size_digits,"start", dev->max_size_digits,"end", "type"
		);
		iterate_part(dev, print_part);
	}
}

void print_devices() {
	FILE* parts;
	unsigned int major;
	unsigned int minor;
	uint64_t blocks;
	char name[NAME_MAX];
	char path[PATH_MAX];
	
	gpt_dev dev = {0};

	if((parts = fopen("/proc/partitions", "r")) == NULL) { fail("could not read /proc/partitions!"); }
	// throw away header
	fgets(name, NAME_MAX, parts);
	fgets(name, NAME_MAX, parts);

	while(!ferror(parts) && !feof(parts)) {
		if(fscanf(parts, "%u %u %lu %s", &major, &minor, &blocks, name) == 4) {
			snprintf(path, PATH_MAX, "/sys/block/%s", name);
			if(access(path, F_OK) == 0) {
				snprintf(path, PATH_MAX, "/dev/%s", name);
				if(open_device(path, &dev, O_RDONLY) != 0) {
					continue;
				}
				print_device(&dev);
				close(dev.fd);
			}
		}
	}
	fclose(parts);
}

void write_mbr(gpt_dev* dev) {
	mbr m = {0};
	uint16_t cylinder;
	chs end;

	m.part[0].type = 0xee; // GPT protective
	m.part[0].start_lba = 1;
	m.part[0].size_lba = dev->last_lba > UINT32_MAX ? UINT32_MAX : (uint32_t)dev->last_lba;
	m.part[0].start.ch_sector = 2; // sector == lba % spt + 1, lba is 1.

	// https://en.wikipedia.org/wiki/Logical_block_addressing#CHS_conversion
	// max cylinder in this addressing is 2^10-1. lba can be too large to represent
	if(dev->last_lba >= (1024 * (dev->geo.heads * dev->geo.sectors))) {
		// if too large use max values
		end.cylinder = 1023;
		end.head = 255;
		end.sector = 63;
	} else {
		end.cylinder = dev->last_lba / (dev->geo.heads * dev->geo.sectors);
		end.head = (dev->last_lba / dev->geo.sectors) % dev->geo.heads;
		end.sector = (dev->last_lba % dev->geo.sectors) + 1;
	}
	m.part[0].end = chstom(end);
	m.signature = 0xaa55;

	memcpy(&(dev->m), &m, MBR_SZ);
	seekwrite(dev->fd, 0, &m, MBR_SZ);
}

void write_gpt(gpt_dev* dev) {
	gpt_hdr h = {0};
	int table_sz; // in blocks

	strncpy(h.signature,"EFI PART", 8); // size prevents null terminator, that's okay
	h.revision_major = 1;
	h.revision_minor = 0;
	// bigger up to block size is legal, but is reserved and *must* be zero
	h.header_size = HDR_SZ;
	if(dev->hdr_sz > HDR_SZ) {
		h.header_size = dev->hdr_sz;
	}
	// must be 128*2n. But currently anything after 128 is reserved and must be zero.
	// not really practical, but technically legal
	h.entry_size = PART_SZ;
	if(dev->part_sz > PART_SZ) {
		h.entry_size = dev->part_sz;
	}
	
	h.this_lba = 1;
	h.alt_lba = dev->last_lba; // last lba of the whole device, not "last usable"
	// req: ptable_lba > 1 and ptable_lba < first_lba - and likewise reversed for alt
	// which implies you can add as much "padding" as you want before and after both tables
	// its weird. but its easy enough to support and its fun.
	h.ptable_lba = 2 + dev->padding[0]; // normally just 2
	// must be enough so that the table is at least 16KiB large
	h.ptable_entries = dev->max_entries; // normally 128
	// normally 32 (128*128/512==32)
	table_sz = ((h.ptable_entries * h.entry_size) + dev->lbsz - 1) / dev->lbsz;
	h.first_lba = h.ptable_lba + table_sz + dev->padding[1];
	h.last_lba = h.alt_lba - 1 - dev->padding[3] - table_sz - dev->padding[2];

	if(not_zero(dev->id, 16)) {
		memcpy(h.disk_guid, dev->id, 16);
	} else {
		gen_guid4(h.disk_guid);
	}
	
	h.ptable_crc = crc32_zero(0, h.ptable_entries * h.entry_size);
	h.crc = crc32(0, &h, HDR_SZ);
	if(dev->hdr_sz > HDR_SZ) {
		h.crc = crc32_zero(h.crc, dev->hdr_sz - HDR_SZ);
		// just zero out whole header space and overwrite
		seekwrite_zero(dev->fd, 1 * dev->lbsz, dev->hdr_sz);
	}
	seekwrite_zero(dev->fd, h.ptable_lba * dev->lbsz, h.ptable_entries * h.entry_size);
	seekwrite(dev->fd, 1 * dev->lbsz, &h, HDR_SZ);
	memcpy(&(dev->hdr), &h, HDR_SZ);

	h.crc = 0;
	h.this_lba = dev->last_lba;
	h.alt_lba = 1;
	h.ptable_lba = h.last_lba + 1 + dev->padding[2];
	h.crc = crc32(0, &h, HDR_SZ);
	if(dev->hdr_sz > HDR_SZ) {
		h.crc = crc32_zero(h.crc, dev->hdr_sz - HDR_SZ);
		// just zero out whole header space and overwrite
		seekwrite_zero(dev->fd, dev->last_lba * dev->lbsz, dev->hdr_sz);
	}
	seekwrite_zero(dev->fd, h.ptable_lba * dev->lbsz, h.ptable_entries * h.entry_size);
	seekwrite(dev->fd, dev->last_lba * dev->lbsz, &h, HDR_SZ);
	memcpy(&(dev->alt), &h, HDR_SZ);

	fprintf(stderr, "wrote new GPT header and table\n");
}

// recalculate headers and rewrite them
void rewrite_headers(gpt_dev* dev) {
	uint32_t calc_crc;

	dev->hdr.crc = 0;
	calc_crc = crc32(0, &(dev->hdr), HDR_SZ);
	if(dev->hdr.header_size > HDR_SZ) {
		calc_crc = crc32_zero(calc_crc, dev->hdr.header_size - HDR_SZ);
	}
	dev->hdr.crc = calc_crc;

	dev->alt.crc = 0;
	calc_crc = crc32(0, &(dev->alt), HDR_SZ);
	if(dev->alt.header_size > HDR_SZ) {
		calc_crc = crc32_zero(calc_crc, dev->alt.header_size - HDR_SZ);
	}
	dev->alt.crc = calc_crc;

	seekwrite(dev->fd, 1 * dev->lbsz,             &(dev->hdr), HDR_SZ);
	seekwrite(dev->fd, dev->last_lba * dev->lbsz, &(dev->alt), HDR_SZ);
}

uint32_t calc_ptable(gpt_dev* dev) {
	uint32_t calc_crc = 0;
	part_entry part;

	for(int i = 0; i < dev->hdr.ptable_entries; i++) {
		seekread(dev->fd, (dev->hdr.ptable_lba * dev->lbsz) + (i * dev->hdr.entry_size), &part, sizeof(part));
		calc_crc = crc32(calc_crc, &part, PART_SZ);

		if(dev->hdr.entry_size > PART_SZ) {
			calc_crc = crc32_zero(calc_crc, dev->hdr.entry_size- PART_SZ);
		}
	}
	return calc_crc;
}

void relabel_gpt(gpt_dev* dev) {
	ensure_valid(dev);

	if(not_zero(dev->id, 16)) {
		memcpy(dev->hdr.disk_guid, dev->id, 16);
	} else {
		gen_guid4(dev->hdr.disk_guid);
	}
	memcpy(dev->alt.disk_guid, dev->hdr.disk_guid, 16);

	rewrite_headers(dev);
}

void set_entry(gpt_dev* dev, char* argv[]) {
	uint32_t num;
	part_entry part;

	for(int i = 0; i < 8; i++) {
		if(argv[i] == NULL) { fail("not enough arguments for set!"); }
	}
	
	ensure_valid(dev);
	
	num = strtol(argv[0], NULL, 10);
	num = num - 1;
	if(num > dev->hdr.ptable_entries) { fail("entry does not exist!"); }
	seekread(dev->fd, (dev->hdr.ptable_lba * dev->lbsz) + (num * dev->hdr.entry_size), &part, PART_SZ);
	
	if(argv[1][0] == '-') {
		if(!not_zero(part.id, 16)) {
			gen_guid4(part.id);
		}
	} else {
		parse_uuid(argv[1], part.id);
	}

	part.start_lba = strtol(argv[2], NULL, 10);
	if(part.start_lba < dev->hdr.first_lba) { fail("start too early!"); }
	part.end_lba = strtol(argv[3], NULL, 10);
	if(part.end_lba < part.start_lba) { fail("end too early!"); }
	if(part.end_lba > dev->hdr.last_lba) { fail("end too late!"); }

	if(argv[4][0] == '-') {
		if(!not_zero(part.type, 16)) {
			fail("new entry needs a type!");
		}
	} else {
		parse_uuid(argv[4], part.type);
	}

	for(int i = 0; i < 16; i++) {
		if(argv[5][i] == 0) { break; }
		if(argv[5][i] == '-') { continue; }
		setbit(part.attr, (15-i) + 48, argv[5][i] == '1');
	}
	for(int i = 0; i < 3; i++) {
		if(argv[6][i] == 0) { break; }
		if(argv[6][i] == '-') { continue; }
		setbit(part.attr, (2-i), argv[6][i] == '1');
	}

	localtoc16(argv[7], part.name, PARTNAME_CHARS);

	seekwrite(dev->fd, (dev->hdr.ptable_lba * dev->lbsz) + (num * dev->hdr.entry_size), &part, PART_SZ);
	seekwrite(dev->fd, (dev->alt.ptable_lba * dev->lbsz) + (num * dev->alt.entry_size), &part, PART_SZ);
	
	dev->alt.ptable_crc = dev->hdr.ptable_crc = calc_ptable(dev);
	rewrite_headers(dev);

	fprintf(stderr, "wrote partition entry %u\n", num+1);
}

void usage() {
	wprintf(L""
		"%hs [-f]\n"
		"%hs [DEVICE] [COMMANDS]\n"
		"\n"
		"Print or modify contents of GPT partition tables.\n"
		"\n"
		"If no DEVICE is provided all known devices are printed.\n"
		"COMMANDS are processed in the order given. Will print if none provided.\n"
		"\n"
		"WARNING: This is a raw editing tool primarily to be used by scripts.\n"
		"Most commands are performed with no sanity checks or confirmations.\n"
		"\n"
		"COMMANDS:\n"
		"-L LBSZ    Override logical block size (normally reported or 512)\n"
		"           useful if DEVICE is a file\n"
		"-B BLOCK   Override last block of DEVICE (total size in blocks - 1)\n"
		"-G HPC SPT Override geometry: heads per cylinder(255), sectors per track(63)\n"
		"           used in building protective MBR\n"
		"-N MAX     Use MAX entries when building(-g) or rebuilding(-r) a GPT table. Defaults to 128.\n"
		"           Each entry is 128 bytes. Be careful the table itself won't overlap the first partition.\n"
		"           Typically 128*128/512 is 32 contiguous blocks. Past MBR and GPT header the first available is lba 34.\n"
		"           Assuming p1 start==1MiB,lbsz==512 you could have (1048576-(512*2))/128==8184 entries.\n"
		"-U UUID    Use specific UUID when building(-g) or rebuilding(-r) a GPT table."
		"-P A B C D Add padding around part tables(in number of blocks) when building GPT table (-g).\n"
		"           before primary table (after lba 1), after primary table,\n"
		"           before backup table, after backup table(before last header).\n"
		"-R H P     Use custom header and part entry sizing when building a GPT table (-g).\n"
		"           H<=lbsz. P must be a power of 2 and >128. The extra reserved space must be zero.\n"
		"-p         Print\n"
		"-m         Build and write a new protective MBR\n"
		"-g         Build and write new blank GPT table (wipes all partitions!)\n"
		"-r         Relabel an existing table with -U UUID, or a new random one if not provided.\n"
		"-s NUM PARTID START END TYPEID TYPEATTR CMNATTR LABEL\n"
		"           Set NUM partition entry. PARTID may be '-' to generate. Bits in ATTR may be '-' to skip existing flags.\n"
		"\n"
		, program_name, program_name);
}

int main(int argc, char* argv[]) {
	int cmd_processed = 0;
	gpt_dev dev = {0};

	// set locale to system default, so we can print "wide" characters with wprintf (for UTF-16 partition label)
	// Once either printf or wprintf is used the other stops working for that stream
	// Use wprintf for stdout, regular printf for stderr
	setlocale(LC_ALL, "");
	
	if(argv[0] != NULL) {
		program_name = argv[0];
		argv++;
	}

	if(argv[0] != NULL && argv[0][0] != '-') {
		if(open_device(argv[0], &dev, O_RDWR) != 0) { fail("could not open device!"); }
		argv++;
	} else {
		// no device provided. only handle print options
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
next_printopt:
			argv++;
		}

		print_devices();
		return 0;
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
				case 'L':
					if(argv[1] == NULL) { fail("need argument!"); }
					dev.lbsz = atoi(argv[1]);
					warn("overriding logical block size to %u", dev.lbsz);
					argv += 1;
					goto next_cmd;
				case 'G':
					if(argv[1] == NULL || argv[2] == NULL) { fail("need arguments!"); }
					dev.geo.heads = atoi(argv[1]);
					dev.geo.sectors = atoi(argv[2]);
					warn("overriding geometry hpc:%u spt:%u", dev.geo.heads, dev.geo.sectors);
					argv += 2;
					goto next_cmd;
				case 'B':
					if(argv[1] == NULL) { fail("need argument!"); }
					dev.last_lba = strtol(argv[1], NULL, 10);
					dev.max_size_digits = digits(dev.last_lba);
					warn("overriding last lba to %lu", dev.last_lba);
					argv += 1;
					goto next_cmd;
				case 'N':
					if(argv[1] == NULL) { fail("need argument!"); }
					dev.max_entries = atoi(argv[1]);
					argv += 1;
					goto next_cmd;
				case 'U':
					if(argv[1] == NULL) { fail("need argument!"); }
					parse_uuid(argv[1], dev.id);
					argv += 1;
					goto next_cmd;
				case 'P':
					if(argv[1] == NULL || argv[2] == NULL || argv[3] == NULL || argv[4] == NULL) { fail("need arguments!"); }
					dev.padding[0] = atoi(argv[1]);
					dev.padding[1] = atoi(argv[2]);
					dev.padding[2] = atoi(argv[3]);
					dev.padding[3] = atoi(argv[4]);
					argv += 4;
					goto next_cmd;
				case 'R':
					if(argv[1] == NULL || argv[2] == NULL) { fail("need arguments!"); }
					dev.hdr_sz = atoi(argv[1]);
					dev.part_sz = atoi(argv[2]);
					if(dev.hdr_sz < HDR_SZ || dev.hdr_sz > dev.lbsz) { fail("invalid header size!"); }
					if(dev.part_sz < 128 || ((dev.part_sz & dev.part_sz - 1) != 0)) { fail("invalid part size!"); }
					argv += 2;
					goto next_cmd;
				case 'p':
					cmd_processed = 1;
					print_device(&dev);
					break;
				case 'm':
					cmd_processed = 1;
					write_mbr(&dev);
					break;
				case 'g':
					cmd_processed = 1;
					write_gpt(&dev);
					break;
				case 'r':
					cmd_processed = 1;
					relabel_gpt(&dev);
					break;
				case 's':
					cmd_processed = 1;
					set_entry(&dev, argv+1);
					argv += 8;
					goto next_cmd;
				default:
					usage();
					return 1;
			}
			argv[0]++;
		}
next_cmd:
		argv++;
	}

	if(!cmd_processed) {
		print_device(&dev);
	}

	close(dev.fd);
	return 0;
}
