#!/bin/sh
# SPDX-License-Identifier: MIT
# gpt.sh
# Copyright (C) 2025 Casey Fitzpatrick <kcghost@gmail.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so.
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# Experimental shell based GPT partition table parser
set -e

fail() {
	echo "${1}" >&2
	exit 1
}

# replacement for `basename` without subshell
getbase() {
	r="${1##*/}"
	if [ -n "${2}" ]; then
		r="${r%"${2}"}"
	fi
}

# extract unsigned integer from file
# getnum <addr> <size> <file>
getnum() {
	# get rid of annoying od spaces by converting straight into arithmetic
	r=$(( $(od -An "-j${1}" "-N${2}" -t "u${2}" "${3}") ))
	# technically size=8 on 64 bit system could flip the sign bit
	if [ "${r}" -lt 0 ]; then fail "can't handle really big numbers!"; fi
}

# extract printable string from file
# getstr <addr> <size> <file>
getstr() {
	r=$(dd status=none if="${3}" bs=1 skip="${1}" count="${2}" | tr -cd "[:alnum:][:blank:][:punct:]")
}

# extract little-endian uuid from bytes in a file
# getuuid <addr> <file>
getuuid() {
	# split bytes to rearrange them
	# shellcheck disable=SC2046
	set -- $(od -An "-j${1}" "-N16" -t "x1" "${2}")
	r="${4}${3}${2}${1}-${6}${5}-${8}${7}-${9}${10}-${11}${12}${13}${14}${15}${16}"
}

genuuid() {
	read -r r </proc/sys/kernel/random/uuid
}

main() {
	device="${1}"; shift
	if [ -b "/dev/${device}" ]; then device="/dev/${device}"; fi
	if [ ! -b "${device}" ]; then fail "no such device"; fi

	getbase "${device}"; dev="${r}"
	read -r block_sz <"/sys/class/block/${dev}/queue/logical_block_size"

	# https://en.wikipedia.org/wiki/GUID_Partition_Table
	# LBA 1 is the primary GPT header, so just 1 block_sz
	getstr "${block_sz}" 8 "${device}"; magic="${r}"
	if [ "${magic}" != "EFI PART" ]; then
		fail "Device does not have a valid GPT table!"
	fi
	getuuid "$(( block_sz + 56 ))" "${device}"; disk_uuid="${r}"
	getnum "$(( block_sz + 72 ))" 8 "${device}"; part_lba="${r}" # usually just 2
	getnum "$(( block_sz + 80 ))" 4 "${device}"; max_entries="${r}"
	getnum "$(( block_sz + 84 ))" 4 "${device}"; part_sz="${r}"
	getnum "$(( block_sz + 88 ))" 4 "${device}"; crc32="${r}"
	getnum "$(( block_sz + 32 ))" 4 "${device}"; backup="${r}"
	#getnum "$(( (block_sz * backup) + 72 ))" 8 "${device}"; part_lba="${r}"
	echo "${device} ${disk_uuid} ${crc32} ${part_sz} ${max_entries} ${backup}"

	#// RFC4122: Version 4, Variant 10x
	#uuidData[8] = (0x80 | (uuidData[8] & 0x3f));
	#uuidData[6] = (0x40 | (uuidData[6] & 0xf));
	#echo "${uuid}"; exit 0
	
	part_num=1
	type_unused="00000000-0000-0000-0000-000000000000"
	until [ "${part_num}" -gt "${max_entries}" ]; do
		part_entry="$(( (part_lba * block_sz) + ((part_num-1) * part_sz) ))"
		getuuid "$(( part_entry + 0 ))" "${device}"; type="${r}"
		if [ "${type}" != "${type_unused}" ]; then
			getuuid "$(( part_entry + 16 ))" "${device}"; uid="${r}"
			getnum "$(( part_entry + 32 ))" 8 "${device}"; first="${r}"
			getnum "$(( part_entry + 40 ))" 8 "${device}"; last="${r}"
			# only 3 bits of common attributes defined at this time
			getnum "$(( part_entry + 48 ))" 1 "${device}"; attr="${r}"
			req=$(( (attr & 1 ) > 0 ))
			ign=$(( (attr & 2 ) > 0 ))
			leg=$(( (attr & 4 ) > 0 ))
			# 16 bits of type-specific attributes
			getnum "$(( part_entry + 54 ))" 2 "${device}"; p_attr="${r}"
			getstr "$(( part_entry + 56 ))" 72 "${device}"; name="${r}"

			printf "%03d %s %s %016x %016x %d%d%d %04x %s\n" \
			"${part_num}" "${type}" "${uid}" "${first}" "${last}" \
			"${req}" "${ign}" "${leg}" "${p_attr}" "${name}"
		fi
		part_num=$(( part_num + 1 ))
	done
}

main "${@}"
#getnum 72 8 header
#echo "${r}"

t() {
	dd if=/dev/sda bs=1 skip=1024 count=16384 of=ptable

	dd bs=512 if=/dev/sdb skip=2 count=32 of=ptable1
	dd bs=512 if=/dev/sdb skip=1953525135 count=32 of=ptable2
}
