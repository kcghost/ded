#!/bin/sh
# SPDX-License-Identifier: MIT-0
# test.sh
# Copyright (C) 2025 Casey Fitzpatrick <kcghost@gmail.com>
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
set -e

onerr() {
	[ $? -eq 0 ] && exit
	echo "Failure occurred!"
}

ded() {
	sudo ./ded.sh "$@"
}

trap 'onerr' EXIT

make_disk() {
	echo "creating test disk and setting up loop device..."
	dd if=/dev/zero bs=1MiB count=1024 of="test.disk"
	parted -s test.disk mktable gpt
	parted test.disk unit B print free
	sudo losetup -d /dev/loop0 || true
	sudo losetup /dev/loop0 test.disk
	sudo parted /dev/loop0 unit B print free
}

test_argparsing() {
	ded -y wipe loop0
	ded -y create loop0 -1 fat
	ded -y wipe loop0
	ded -y create loop0 fat
	ded -y wipe loop0
	ded -y create loop0 fat "foo bar"
	ded -y wipe loop0
	ded -y create loop0 fat "foo bar" 8MiB
	ded -y wipe loop0
	ded -y create loop0 fat "" 8MiB
}

test_fs() {
	ded -y wipe loop0
	ded -y create loop0 efi   8 MiB
	ded -y create loop0 ext4  8 MiB
	ded -y create loop0 ntfs  8 MiB
	ded -y create loop0 fat32 8 MiB
	ded -y create loop0 swap
}

test_holes() {
	ded -y wipe loop0
	ded -y create loop0 fat32 8 MiB
	ded -y create loop0 fat32 8 MiB
	ded -y create loop0 ext4  8 MiB
	ded -y create loop0 fat32 8 MiB
	ded -y create loop0 fat32 8 MiB
	ded -y create loop0 fat32 8 MiB
	ded -y remove loop0 2
	ded -y remove loop0 5
	# add some strange flags to test recreating them
	sudo parted loop0 set 3 hidden on
	sudo parted loop0 set 3 hp-service on
	ded-y lshift loop0 3
	# new partition is 2
	ded -y resize loop0 2
}

test_resize() {
	# experimentally ~260MiB is the smallest fatresize/libparted wants to work with
	ded -y wipe loop0
	ded -y create loop0 fat32 300 MiB
	ded -y resize loop0 1 500 MiB
	ded -y resize loop0 1 350 MiB

	ded -y wipe loop0
	ded -y create loop0 efi 300 MiB
	ded -y resize loop0 1 500 MiB
	ded -y resize loop0 1 350 MiB

	ded -y wipe loop0
	ded -y create loop0 ext4 100 MiB
	ded -y resize loop0 1 500 MiB
	ded -y resize loop0 1 50 MiB

	ded -y wipe loop0
	ded -y create loop0 ntfs 100 MiB
	ded -y resize loop0 1 500 MiB
	ded -y resize loop0 1 50 MiB
	ded -y resize loop0 1
}

main() {
	if [ "${1}" = "-r" ]; then
		make_disk
	fi

	test_argparsing
	#test_fs
	#test_holes
	#test_resize

	sudo parted -s /dev/loop0 unit B print free
	sudo parted -s /dev/loop0 unit MiB print free
	sudo ./ded.sh print loop0
}

main "${@}"
