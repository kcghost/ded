#!/bin/sh
# SPDX-License-Identifier: MIT
# ded.sh
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

# Simplified filesystem-aware partition manager
set -e

NORMAL_PRECISION="4"
# max that doesn't cause errors comparing large numbers
MAX_PRECISION="18"

onerr() {
	code=$?
	if [ ${code} -ne 0 ]; then
		if [ $code -ne 15 ]; then
			echo "Unknown failure occurred!"
		fi
	fi
}

trap 'onerr' EXIT
trap 'exit 15' TERM

fail() {
	echo "${1}" >&2
	kill $$
}

confirm() {
	printf "Proceed? [y/N] "
	read -r yn
	case "${yn}" in
		[yY][eE][sS]|[yY]) ;;
		*)
			trap '' EXIT
			exit 1
	esac
	printf "\n"
}

get_blockdevs() {
	block_devices=""
	while read -r line; do
		if [ "${line}" = "BYT;" ]; then
			read -r line
			parse_diskline "${line}"
			if [ "${block_devices}" = "" ]; then
				block_devices="${p_device}"
			else
				block_devices="${block_devices} ${p_device}"
			fi
		fi
	done << EOF
$(parted -lm)
EOF
}

parse_diskline() {
	# /dev/sdb:1000204886016B:scsi:512:512:gpt:ATA Samsung SSD 860:;
	OIFS="${IFS}"
	IFS=':;'
	# shellcheck disable=SC2086
	set ${1}
	IFS="${OIFS}"
	p_device="$1"
	p_size="${2%B}"
	# shellcheck disable=SC2034
	p_transport="$3"
	# shellcheck disable=SC2034
	p_sector_logical="$4"
	# shellcheck disable=SC2034
	p_sector_physical="$5"
	p_label="$6"
	p_model="$7"
	p_flags="$8"
}

# if comma separated list contains item
contains() {
	OIFS="${IFS}"
	list="${1}"
	item="${2}"
	IFS="${3:-, }"
	# shellcheck disable=SC2086
	for v in ${list}; do
		if [ "${v}" = "${item}" ]; then
			IFS="${OIFS}"
			return 0
		fi
	done
	IFS="${OIFS}"
	return 1
}

parse_partline() {
	# 1:1048576B:1074790399B:1073741824B:fat32::msftdata;
	OIFS="${IFS}"
	IFS=':;'
	# shellcheck disable=SC2086
	set ${1}
	IFS="${OIFS}"
	p_number="$1"
	p_start="${2%B}"
	p_end="${3%B}"
	p_size="${4%B}"
	p_fs="$5"
	p_name="$6"
	p_flags="${7:-}"

	# "type" is a simplified abstraction over fs type and flags
	# ideally a GPT guid, but parted flags are less concrete
	p_type="${p_fs}"
	# simplify linux-swap(new,v1,v0,old) etc situation
	case "${p_type}" in
		*swap*)
			p_type="swap" ;;
	esac
	if contains "${p_flags}" "esp"; then
		p_type="efi"
	fi
	if contains "${p_flags}" "msftres"; then
		p_type="msres"
	fi
	if contains "${p_flags}" "diag"; then
		p_type="msdiag"
	fi
}

floor_mib() {
	bytes="${1}"
	bytes=$(( (bytes / (1024 * 1024)) * (1024 * 1024) ))
	echo "${bytes}"
}

roundup_mib() {
	bytes="${1}"
	if [ "$(( bytes % (1024 * 1024) ))" -gt 0 ]; then
		bytes=$(floor_mib "$(( bytes + (1024 * 1024) ))")
	fi
	echo "${bytes}"
}

human_bytes() {
	bytes="${1}"
	precision="${2:-${NORMAL_PRECISION}}"
	# make sure numbers do not take greater than n digits
	cutoff=$(printf "%${precision}s" | tr ' ' '9')
	
	postfix="  B"
	if [ "${bytes}" -gt "${cutoff}" ] || [ "$(( bytes % 1024 ))" -eq 0 ] ; then
		bytes=$(( (bytes / 1024) + ((bytes % 1024)>512) ))
		postfix="KiB"
	fi
	if [ "${bytes}" -gt "${cutoff}" ] || [ "$(( bytes % 1024 ))" -eq 0 ] ; then
		bytes=$(( (bytes / 1024) + ((bytes % 1024)>512) ))
		postfix="MiB"
	fi
	if [ "${bytes}" -gt "${cutoff}" ] || [ "$(( bytes % 1024 ))" -eq 0 ] ; then
		bytes=$(( (bytes / 1024) + ((bytes % 1024)>512) ))
		postfix="GiB"
	fi
	if [ "${bytes}" -gt "${cutoff}" ] || [ "$(( bytes % 1024 ))" -eq 0 ] ; then
		bytes=$(( (bytes / 1024) + ((bytes % 1024)>512) ))
		postfix="TiB"
	fi
	if [ "${bytes}" -gt "${cutoff}" ] || [ "$(( bytes % 1024 ))" -eq 0 ] ; then
		bytes=$(( (bytes / 1024) + ((bytes % 1024)>512) ))
		postfix="PiB"
	fi
	printf "% ${precision}s %s" "${bytes}" "${postfix}"
}

parse_bytes() {
	# handle spaces/multiple arguments
	input="$*"
	case "${input}" in
		*.*) fail "Sizes should be whole numbers IEC MiB or larger!" ;;
	esac

	bytes=$(echo "${input}" | tr -d -c "[:digit:]")
	postfix=$(echo "${input}" | tr -d -c "[:alpha:]")
	case "${postfix}" in
		"B") fail "Sizes should be whole numbers IEC MiB or larger!" ;;
		"KiB") fail "Sizes should be whole numbers IEC MiB or larger!" ;;
		"MiB") bytes=$(( bytes * 1024 * 1024 )) ;;
		"GiB") bytes=$(( bytes * 1024 * 1024 * 1024 )) ;;
		"TiB") bytes=$(( bytes * 1024 * 1024 * 1024 * 1024 )) ;;
		"PiB") bytes=$(( bytes * 1024 * 1024 * 1024 * 1024 * 1024 )) ;;
		"") bytes=$(( bytes * 1024 * 1024 )) ;;
		*) fail "Use IEC units only!" ;;
	esac

	echo "${bytes}"
}

disk_hook() { :; }
part_hook() { :; }

parse_device() {
	device="${1}"
	if [ ! -r "${device}" ]; then
		fail "Cannot not read from ${device}!"
	fi
	free_i="0"
	while read -r line; do
		if [ "${line}" = "BYT;" ]; then
			# line directly after BYT is disk information
			read -r line
			parse_diskline "${line}"
			disk_hook
			continue
		fi
		# otherwise partition (or free) entry for disk
		parse_partline "${line}"
		# make numbers relevant for free space sections, normally just "1"
		if [ "${p_type}" = "free" ]; then
			p_number=""
			# if enough size to support a new partition
			if [ "${p_size}" -ge "1048576" ]; then
				free_i=$(( free_i - 1 ))
				p_number="${free_i}"
			fi
		fi
		part_hook
	done << EOF
$(parted -ms "${device}" unit B print free || fail "Could not read from device")
EOF
}

print_device() {
	device="${1}"
	disk_hook() {
		printf "%s (%s) %s \"%s\"\n" \
		"${p_device}" "$(human_bytes "${p_size}")" "${p_label}" "${p_model}"
	}
	part_hook() {
		# don't bother with tiny alignment sections
		if [ "${p_number}" = "" ]; then
			return
		fi
		# opting to just not care about the flags in this context
		# instead flags are absorbed into the partition/fs type
		printf "% 3s: %s - %s (%s) % 6s \"%s\"\n" \
		"${p_number}" \
		"$(human_bytes "${p_start}")" \
		"$(human_bytes "${p_end}")" \
		"$(human_bytes "${p_size}")" \
		"${p_type}" \
		"${p_name}"
	}
	parse_device "${device}"
	printf "\n"
}

printall() {
	get_blockdevs
	if [ "${block_devices}" = "" ]; then
		fail "Could not find any devices! (You might need sudo)"
	fi

	printf "Byte quantities approximated to %s digits.\n" "${NORMAL_PRECISION}"
	for bd in ${block_devices}; do
		print_device "${bd}"
	done
}

# get start/end for a given partition or section number
get_section() {
	wanted_number="${1}"
	r_start="nothing"
	disk_hook() { :; }
	part_hook() {
		if [ "${p_number}" = "${wanted_number}" ]; then
			r_start="${p_start}"
			r_end="${p_end}"
			r_size="${p_size}"
			r_name="${p_name}"
			r_fs="${p_fs}"
			r_flags="${p_flags}"
			r_type="${p_type}"
		fi
	}
	parse_device "${device}"
	if [ "${r_start}" = "nothing" ]; then
		fail "Could not find partition number ${wanted_number}!"
	fi
}

# get partition/section number for a given start
get_part() {
	wanted_start="${1}"
	r_part="nothing"
	disk_hook() { :; }
	part_hook() {
		if [ "${wanted_start}" -ge "${p_start}" ] && [ "${wanted_start}" -le "${p_end}" ]; then
			r_part="${p_number}"
		fi
	}
	parse_device "${device}"
	if [ "${r_part}" = "nothing" ]; then
		fail "Could not find new partition!?"
	fi
}

assert_exists() {
	command -v "${1}" >/dev/null || fail "Need ${1} in PATH!"
}

get_partdevice() {
	device="${1}"
	partnum="${2}"
	r_partdevice="${device}${partnum}"

	if [ ! -b "${r_partdevice}" ]; then
		# sometimes a "p" is use, such as in loop devices
		r_partdevice="${device}p${partnum}"
		if [ ! -b "${r_partdevice}" ]; then
			fail "Could not find ${partnum} for device ${device}!"
		fi
	fi
}

create_cmd() {
	[ $# -gt 0 ] || (print_help && exit 1)

	target_num="-1"
	if [ $# -gt 0 ]; then
		first_char=$(printf "%.1s" "${1}")
		# negative or digit is NUMBER
		case "${first_char}" in
			'-'|[0-9])
				target_num="${1}"
				shift
				;;
		esac
	fi
	get_section "${target_num}"

	[ $# -gt 0 ] || (print_help && exit 1)
	target_type="${1}"; shift
	fs_type="${target_type}"
	default_name="Unnamed partition"
	case "${target_type}" in
		"ext4")
			default_name="Linux filesystem data"
			assert_exists "mkfs.ext4"
			;;
		"fat32")
			default_name="Basic data partition"
			fs_type="fat32"
			assert_exists "mkfs.vfat"
			;;
		"efi")
			default_name="EFI System partition"
			fs_type="fat32"
			assert_exists "mkfs.vfat"
			;;
		"ntfs")
			default_name="Basic data partition"
			assert_exists "mkfs.ntfs"
			;;
		"swap")
			default_name="Swap partition"
			fs_type="linux-swap(v1)"
			assert_exists "mkswap"
			;;
		*)
			fail "Unsupported TYPE!" ;;
	esac

	target_name="${default_name}"
	if [ $# -gt 0 ]; then
		first_char=$(printf "%.1s" "${1}")
		# accept either empty string or a string starting with a non-number as NAME
		case "${first_char}" in
			''|[!0-9])
				target_name="${1}"
				shift
				;;
		esac
	fi

	target_size=$(parse_bytes "${@}")
	target_start=$(roundup_mib "${r_start}")
	if [ "${target_size}" -eq "0" ]; then
		target_size=$(floor_mib "$(( (r_end - target_start ) + 1))")
	fi
	# correct alignment, at least according to checkpartitionsalignment.sh
	target_end=$(( target_start + target_size - 1 ))
	
	case "${fs_type}" in
		"fat32")
			if [ "${target_size}" -lt "$(( 512 * 1024 * 1024 ))" ]; then
				fail "Creating FAT32 filesystems less than 512 MiB is not supported!"
			fi
			;;
	esac

	print_device "${device}"

	printf "number: %s\n" "${target_num}"
	printf " start: %s (~%s)\n" "$(human_bytes "${target_start}" "${MAX_PRECISION}")" "$(human_bytes "${target_start}")"
	printf "   end: %s (~%s)\n" "$(human_bytes "${target_end}" "${MAX_PRECISION}")" "$(human_bytes "${target_end}")"
	printf "  size: %s (~%s)\n" "$(human_bytes "${target_size}" "${MAX_PRECISION}")" "$(human_bytes "${target_size}")"
	printf "  type: %s\n" "${target_type}"
	printf "fstype: %s\n" "${fs_type}"
	printf "  name: %s\n" "${target_name}"
	
	if [ "${target_num}" -gt 0 ]; then
		printf "WARNING! The next operations will remove and reformat existing partition %s!\n" "${target_num}"
		confirm
		parted -s "${device}" rm "${target_num}" || fail "Failed to remove partition ${target_num}"
	else
		printf "The next operations will create a new partition and format it.\n"
		confirm
	fi
	
	parted -s "${device}" unit B mkpart \""${target_name}"\" "${fs_type}" "${target_start}" "${target_end}" || fail "Failed to create partition!"
	# """
	
	sync
	partprobe
	get_part "${target_start}"
	get_partdevice "${device}" "${r_part}"
	printf "The next operation will format new partition %s on block device %s.\n" "${r_part}" "${r_partdevice}"

	case "${target_type}" in
		"ext4")
			yes 2>/dev/null | mkfs.ext4 -q -L "${target_name}" "${r_partdevice}" || fail "Failed to format partition!" ;;
		"fat32")
			mkfs.vfat -n "${target_name}" "${r_partdevice}" || fail "Failed to format partition!" ;;
		"efi")
			# turn off basic data flag. want: boot, esp, no_automount
			parted -s "${device}" set "${r_part}" msftdata off || fail "Failed to set flag"
			# seems that "boot" and "esp" are aliases for the same thing
			parted -s "${device}" set "${r_part}" esp on || fail "Failed to set flag"
			parted -s "${device}" set "${r_part}" no_automount on || fail "Failed to set flag"
			mkfs.vfat -n "${target_name}" "${r_partdevice}" || fail "Failed to format partition!"
			;;
		"ntfs")
			mkfs.ntfs -L "${target_name}" "${r_partdevice}" || fail "Failed to format partition!"
			;;
		"swap")
			mkswap -L "${target_name}" "${r_partdevice}" || fail "Failed to format partition!"
			;;
	esac

	sync
	partprobe
	print_device "${device}"

	echo "Success!"
}

resize_fs() {
	partdevice="${1}"
	type="${2}"
	size="${3}"
	in_mib=$(( size / 1024 / 1024 ))
	
	case "${type}" in
		"ext4")
			assert_exists "resize2fs"
			# resize2fs doesn't take bytes. units are powers of two, M==MiB
			resize2fs "${partdevice}" "${in_mib}M" || fail "Failed to resize ext4!"
			;;
		"fat32")
			assert_exists "fatresize"
			if [ "${size}" -lt "$(( 512 * 1024 * 1024 ))" ]; then
				fail "Resizing fat32 filesystems less than 512 MiB is not supported!"
			fi
			fatresize -q -f -s "${in_mib}Mi" "${partdevice}" || fail "Failed to resize fat32!"
			;;
		"ntfs")
			assert_exists "ntfsresize"
			# units are in SI 1000, avoid them and use bytes
			yes 2>/dev/null | ntfsresize -f -s "${size}" "${partdevice}" || fail "Failed to resize ntfs!"
			;;
		*)
			fail "Unsupported TYPE!" ;;
	esac
}

resize_cmd() {
	[ $# -gt 0 ] || (print_help && exit 1)
	target_num="${1}"
	shift
	wanted_size=$(parse_bytes "${@}")

	get_section "${target_num}"
	target_fs="${r_fs}"
	current_end="${r_end}"
	current_size="${r_size}"
	get_partdevice "${device}" "${target_num}"

	# get next section details
	get_part "$(( r_end + 1 ))"
	get_section "${r_part}"
	next_type="${r_type}"
	next_size="${r_size}"

	if [ "${wanted_size}" -eq "0" ]; then
		wanted_size="$(floor_mib $(( current_size + next_size )))"
	fi

	print_device "${device}"

	if [ "${wanted_size}" -gt "${current_size}" ]; then
		if [ "${next_type}" != "free" ]; then
			fail "Need free space after!"
		fi
		target_end=$(( current_end + (wanted_size-current_size) ))

		printf "Growing partition %s from %s to %s\n" "${target_num}" "$(human_bytes "${current_size}")" "$(human_bytes "${wanted_size}")"
		confirm
		
		parted -s "${device}" unit B resizepart "${target_num}" "${target_end}" || fail "Failed to resize partition!"
		sync
		partprobe
		# TODO: undo partition change on fail
		resize_fs "${r_partdevice}" "${target_fs}" "${wanted_size}"
		
		print_device "${device}"
	elif [ "${current_size}" -gt "${wanted_size}" ]; then
		target_end=$(( current_end - (current_size-wanted_size) ))
		printf "Shrinking partition %s from %s to %s\n" "${target_num}" "$(human_bytes "${current_size}")" "$(human_bytes "${wanted_size}")"
		confirm

		resize_fs "${r_partdevice}" "${target_fs}" "${wanted_size}"
		# parted -s doesn't work for shrinking partitions "to be safe"
		# use undocumented option "---pretend-input-tty" to work around with "yes"
		# could also work around by removing and re-creating?
		yes 2>/dev/null | parted ---pretend-input-tty "${device}" unit B resizepart "${target_num}" "${target_end}" || fail "Failed to resize partition!"
		print_device "${device}"
	else
		fail "Partition ${target_num} is already that size!"
	fi

	echo "Success!"
}

rm_cmd() {
	[ $# -gt 0 ] || (print_help && exit 1)
	target_num="${1}"

	print_device "${device}"
	printf "WARNING! The next operation will remove partition %s on %s!\n" "${target_num}" "${device}"
	confirm

	parted -s "${device}" rm "${target_num}" || fail "Failed to remove partition ${target_num}"

	print_device "${device}"
	echo "Success!"
}

lshift_cmd() {
	[ $# -gt 0 ] || (print_help && exit 1)
	from="${1}"
	get_section "${from}"
	from_start="${r_start}"
	from_size="${r_size}"
	from_fs="${r_fs}"
	from_name="${r_name}"
	from_flags="${r_flags}"
	in_mib=$(( from_size / 1024 / 1024 ))
	
	get_part "$(( r_start - 1 ))"
	to="${r_part}"
	get_section "${to}"
	to_start="${r_start}"
	to_type="${r_type}"
	new_end=$(( to_start + from_size - 1 ))

	if [ "${to_type}" != "free" ]; then
		fail "Must be shifted into free space!"
	fi

	print_device "${device}"
	printf "The next operation will move partition %s data to %s and re-create the partition.\n" "${from}" "${to}"
	# TODO: move partition without re-creating it
	printf "WARNING: The partition may get a new number. (The partition table entry will be re-created.)\n"
	printf "WARNING: This is the sketchiest possible thing you could do. Backup anything important!\n"
	confirm

	printf "Copying data... (may take awhile!)\n"
	dd \
	conv=fsync,notrunc \
	bs=1048576 \
	iflag=skip_bytes,fullblock \
	oflag=seek_bytes \
	"seek=${to_start}" \
	"skip=${from_start}" \
	"count=${in_mib}" \
	"if=${device}" \
	"of=${device}" || fail "Failed to copy partition data!"

	# parted doesn't support resizing "to the left" afaik. Remove and recreate partition
	parted -s "${device}" rm "${from}" || fail "Failed to remove existing partition!"
	parted -s "${device}" unit B mkpart \""${from_name}"\" "${from_fs}" "${to_start}" "${new_end}" || fail "Failed to re-create partition!"
	# """
	get_part "${to_start}"
	OIFS="${IFS}"
	IFS=' ,'
	# shellcheck disable=SC2086
	for flag in ${from_flags}; do
		parted -s "${device}" set "${r_part}" "${flag}" on || fail "Failed to set flag on new partition!"
	done
	IFS="${OIFS}"

	print_device "${device}"
	echo "Success!"
}

wipe_cmd() {
	print_device "${device}"
	printf "WARNING! The next operation will destroy all partitions on %s!\n" "${device}"
	confirm
	parted -s "${device}" mktable gpt || fail "Failed to create GPT table!"

	print_device "${device}"
	echo "Success!"
}

# TODO: move, backup, restore, renumber(sort?)
print_help() {
	cat << EOF
${0} COMMAND DEVICE
${0} is a simplified partition manager that is filesystem aware.
If no COMMAND is given, partition are listed for all devices

COMMANDs:
print  [DEV]                        print partition summary for DEV
create DEV [NUM] TYPE [NAME] [SIZE] create new partition/filesystem at NUMB
resize DEV NUM [SIZE]               shrink/grow partition/filesystem NUM
remove DEV NUM                      remove partition NUM
lshift DEV NUM                      shift NUM to preceding empty space
wipe                                start a new gpt partition table

Negative NUMs denote free space large enough for new partitions.
If omitted NUM defaults to the first available free space (-1).

Supported TYPEs: ext4, fat32, efi, ntfs, swap
efi is a fat32 filesystem with the esp, boot, and no_automount flags set.
swap is a linux-swap(v1) filesystem with the swap flag set.

SIZE given in whole MiB/GiB/TiB/PiB. Omitting uses next available space.
All new partitions start and end points are automatically aligned to 1 MiB.

Resizing can grow forward into next free space, but not previous space.
To use previous space "to the left" use the "lshift" command.

If provided NAME must not start with a number (would be interpreted as SIZE).
If NAME is not provided a generic default name will be used according to TYPE.
The same NAME is used to label both the partition and filesystem if supported.

Required external commands for full functionality:
parted
resize2fs, fatresize, ntfsresize
mkfs.ext4, mkfs.vfat, mkfs.ntfs
EOF
}

main() {
	if [ $# -eq 0 ]; then
		printall && exit 0
	fi
	if [ "${1}" = "-h" ]; then
		print_help && exit 0
	fi
	if [ "${1}" = "-y" ]; then
		shift
		confirm() {
			echo "-y was given, proceeding automatically..."
		}
	fi

	command="${1}"; shift
	if [ "${command}" = "print" ]; then
		if [ $# -eq 0 ]; then
			printall && exit 0
		fi
	fi

	if [ $# -eq 0 ]; then
		print_help && exit 0
	fi
	device="${1}"; shift
	if [ -b "/dev/${device}" ]; then
		device="/dev/${device}"
	fi
	if [ ! -b "${device}" ]; then
		print_help && exit 1
	fi

	case "${command}" in
		"print")
			printf "Byte quantities approximated to %s digits.\n" "${NORMAL_PRECISION}"
			print_device "${device}"
			;;
		"create") create_cmd "${@}";;
		"resize") resize_cmd "${@}";;
		"remove") rm_cmd "${@}";;
		"lshift") lshift_cmd "${@}";;
		"wipe") wipe_cmd "${@}";;
		*) print_help && exit 1;;
	esac
}

case ${0##*/} in
	dash|-dash|bash|-bash|ksh|-ksh|sh|-sh) return 0;;
	*) main "${@}"
esac
