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

trap 'onerr' EXIT

make_disk() {
	echo "creating test disk and setting up loop device..."
	dd if=/dev/zero bs=1MiB count=1024 of="test.disk"
	parted -s test.disk mktable gpt
	parted test.disk unit B print free
	sudo losetup -d /dev/loop0
	sudo losetup /dev/loop0 test.disk
	sudo parted /dev/loop0 unit B print free
}

test_argparsing() {
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create -1 fat32
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create fat32
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create fat32 "foo bar"
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create fat32 "foo bar" 8MiB
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create fat32 "" 8MiB
}

test_fs() {
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create efi   8 MiB
	yes | sudo ./ded.sh /dev/loop0 create ext4  8 MiB
	yes | sudo ./ded.sh /dev/loop0 create ntfs  8 MiB
	yes | sudo ./ded.sh /dev/loop0 create fat32 8 MiB
	yes | sudo ./ded.sh /dev/loop0 create swap
}

test_holes() {
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create fat32 8 MiB
	yes | sudo ./ded.sh /dev/loop0 create fat32 8 MiB
	yes | sudo ./ded.sh /dev/loop0 create ext4  8 MiB
	yes | sudo ./ded.sh /dev/loop0 create fat32 8 MiB
	yes | sudo ./ded.sh /dev/loop0 create fat32 8 MiB
	yes | sudo ./ded.sh /dev/loop0 create fat32 8 MiB
	yes | sudo ./ded.sh /dev/loop0 remove 2
	yes | sudo ./ded.sh /dev/loop0 remove 5
	# add some strange flags to test recreating them
	sudo parted /dev/loop0 set 3 hidden on
	sudo parted /dev/loop0 set 3 hp-service on
	yes | sudo ./ded.sh /dev/loop0 lshift 3
	# new partition is 2
	yes | sudo ./ded.sh /dev/loop0 resize 2
}

test_resize() {
	# experimentally ~260MiB is the smallest fatresize/libparted wants to work with
	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create fat32 300 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 500 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 350 MiB

	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create efi 300 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 500 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 350 MiB

	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create ext4 100 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 500 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 50 MiB

	yes | sudo ./ded.sh /dev/loop0 wipe
	yes | sudo ./ded.sh /dev/loop0 create ntfs 100 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 500 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1 50 MiB
	yes | sudo ./ded.sh /dev/loop0 resize 1
}

main() {
	if [ "${1}" = "-r" ]; then
		make_disk
	fi

	#test_argparsing
	#test_fs
	test_holes
	test_resize

	sudo parted -s /dev/loop0 unit B print free
	sudo parted -s /dev/loop0 unit MiB print free
	sudo ./ded.sh /dev/loop0
}

main "${@}"
