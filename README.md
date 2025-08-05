# ded

`ded` is a simplified partition and filesystem manager.
It aims to provide `gparted`-like functionality but in a CLI tool.

`ded` only allows basic "reasonable" operations for modern desktop/laptop computers.
It is not designed to be a flexible tool, but instead one that can't be used to shoot yourself in the foot.

**WARNING**: This tool has *not* been heavily tested and is still under active development.

Always back up important data before using this or any other partition management tool.

## Usage

```
ded is a simplified partition manager that is filesystem aware.
If no arguments are given, partitions are listed for all devices.
If no COMMAND is given, partitions are listed for DEVICE.

COMMANDs:
create [NUMBER] TYPE [NAME] [SIZE] create partition/filesystem in NUMBER
resize NUMBER [SIZE]               shrink or grow partition/filesystem NUMBER
remove NUMBER                      remove partition NUMBER
lshift NUMBER                      shift NUMBER to preceding empty space
wipe                               start a new gpt partition table

Negative NUMBERs denote free space large enough for new partitions.
If omitted NUMBER defaults to the first available free space (-1).

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
mke2fs, mkdosfs, mkntfs
```

```
sudo ded /dev/sda wipe
sudo ded /dev/sda create efi  1GiB
sudo ded /dev/sda create ext4 100GiB
sudo ded /dev/sda create swap 4GiB
sudo ded /dev/sda create ntfs
```

## Install

`ded` is just a (mostly) POSIX shell script:
```
sudo make install
# or
sudo install -Dm755 ded.sh /usr/bin/ded
```

You'll also need the following external tools available in most package managers:
* `parted`
* `resize2fs`
* `fatresize`
* `ntfsresize`
* `mke2fs`
* `mkdosfs`
* `mkntfs`

## Why

Most CLI partition editor tools are "literal" in that they only edit the partition table itself.
They never touch the fileystems themselves on those partitions.
They always almost always leave the disk in a non-sane in-between state.

Formatting, resizing, and even moving the filesystems themselves are left as separate operations to be done with separate tools.
When done manually this is an incredibly error-prone process.
For example when resizing a partition it matters whether you are shrinking or growing the partition, whether you should edit the partition table itself first or last, and what units and arguments the different tools use.
Screw it up and you can easily install Ubuntu directly in the middle of a Windows partition.

`gparted` is usually the recommended tool for managing a disk in a filesystem-aware way.
But `gparted` requires a GUI interface, so it isn't appropriate for headless or rescue systems.
`gparted` is billed as a "frontend" for `parted`, but `parted` really just manages the partition table literally.
It doesn't do anything close to what `gparted` does.

`ded` depends on `parted` to manage the partition table itself, but also uses filesystem
formatting and resizing tools to provide a complete interface for basic operations.

## Known issues

### FAT resizing

Be careful when resizing FAT32 partitions. `fatresize` can typically only handle partitions larger than about 260 MiB, sometimes they need to be larger than 512 MiB. `ded` might fail the resize operation and a grow would leave the partition table in a non-sane state.

This is especially annoying as EFI partitions (which are typically FAT32) are often just 100 MiB in size.
The recommended size for a new EFI partition is 1 GiB. But growing it would fail.

It would be great if there was a better resizer utility available, but I don't believe one exists.
(If you know of one file an issue and suggest it!).
`dosfstools` doesn't contain a resizer.
`parted` used to support resizing FAT and HFS fileystems on its own, but dropped support for it.
`fatresize` really just exposes the old functionality kept in the `parted` source code as a library.

## Copyright

Released under the [MIT License](LICENSE).

