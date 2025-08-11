# ded

`ded` is a simplified partition and filesystem manager.
It aims to provide `gparted`-like functionality but in a CLI tool.

`ded` only allows basic "reasonable" operations for modern desktop/laptop computers.
It is not designed to be a flexible tool, but instead one that can't be used to shoot yourself in the foot.

**WARNING**: This tool has *not* been heavily tested and is still under active development.

Always back up important data before using this or any other partition management tool.

## Usage

```
./ded.sh COMMAND DEVICE
./ded.sh is a simplified partition manager that is filesystem aware.
If no COMMAND is given, partitions are listed for all devices.
If -y is given no confirmation prompts will be given.

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
```

The following example uses a partition table set up by a Windows 11 install on a wiped disk.
Windows 11 sets up a 1GiB EFI partition, which is a good recommended size.
It also aligns each partition start and end to 1MiB, which `ded` also does for each new partition.
The example resizes the Windows install to make room for a Linux dual boot.

```
~ # ded
Byte quantities often approximate.
/dev/sda ( 149 GiB) gpt "ATA INTEL SSDSA2BW16"
  1:    1 MiB - 1025 MiB (   1 GiB)    efi "EFI System partition"
  2: 1025 MiB - 1041 MiB (  16 MiB)  msres "Microsoft reserved partition"
  3: 1041 MiB -  148 GiB ( 147 GiB)   ntfs "Basic data partition"
  4:  148 GiB -  149 GiB ( 642 MiB) msdiag ""
 -1:  149 GiB -  149 GiB (1863 KiB)   free ""

~ # ded resize sda 3 80 GiB
/dev/sda ( 149 GiB) gpt "ATA INTEL SSDSA2BW16"
  1:    1 MiB - 1025 MiB (   1 GiB)    efi "EFI System partition"
  2: 1025 MiB - 1041 MiB (  16 MiB)  msres "Microsoft reserved partition"
  3: 1041 MiB -  148 GiB ( 147 GiB)   ntfs "Basic data partition"
  4:  148 GiB -  149 GiB ( 642 MiB) msdiag ""
 -1:  149 GiB -  149 GiB (1863 KiB)   free ""

Shrinking partition 3 from  147 GiB to   80 GiB
Proceed? [y/N] y

ntfsresize v2022.10.3 (libntfs-3g)
Device name        : /dev/sda3
NTFS volume version: 3.1
Cluster size       : 4096 bytes
Current volume size: 158275203584 bytes (158276 MB)
Current device size: 158275207168 bytes (158276 MB)
New volume size    : 85899342336 bytes (85900 MB)
Checking filesystem consistency ...
100.00 percent completed
Accounting clusters ...
Space in use       : 43901 MB (27.7%)
Collecting resizing constraints ...
Needed relocations : 0 (0 MB)
Schedule chkdsk for NTFS consistency check at Windows boot time ...
Resetting $LogFile ... (this might take a while)
Updating $BadClust file ...
Updating $Bitmap file ...
Updating Boot record ...
Syncing device ...
Successfully resized NTFS on device '/dev/sda3'.
You can go on to shrink the device for example with Linux fdisk.
IMPORTANT: When recreating the partition, make sure that you
  1)  create it at the same disk sector (use sector as the unit!)
  2)  create it with the same partition type (usually 7, HPFS/NTFS)
  3)  do not make it smaller than the new NTFS filesystem size
  4)  set the bootable flag for the partition if it existed before
Otherwise you won't be able to access NTFS or can't boot from the disk!
If you make a mistake and don't have a partition table backup then you
can recover the partition table by TestDisk or Parted's rescue mode.
Warning: Shrinking a partition can cause data loss, are you sure you want to continue?
Yes/No? y                                                                 
Information: You may need to update /etc/fstab.

/dev/sda ( 149 GiB) gpt "ATA INTEL SSDSA2BW16"
  1:    1 MiB - 1025 MiB (   1 GiB)    efi "EFI System partition"
  2: 1025 MiB - 1041 MiB (  16 MiB)  msres "Microsoft reserved partition"
  3: 1041 MiB -   81 GiB (  80 GiB)   ntfs "Basic data partition"
 -1:   81 GiB -  148 GiB (  67 GiB)   free ""
  4:  148 GiB -  149 GiB ( 642 MiB) msdiag ""
 -2:  149 GiB -  149 GiB (1863 KiB)   free ""

Success!

~ # ded create sda -1 ext4 63 GiB
/dev/sda ( 149 GiB) gpt "ATA INTEL SSDSA2BW16"
  1:    1 MiB - 1025 MiB (   1 GiB)    efi "EFI System partition"
  2: 1025 MiB - 1041 MiB (  16 MiB)  msres "Microsoft reserved partition"
  3: 1041 MiB -   81 GiB (  80 GiB)   ntfs "Basic data partition"
 -1:   81 GiB -  148 GiB (  67 GiB)   free ""
  4:  148 GiB -  149 GiB ( 642 MiB) msdiag ""
 -2:  149 GiB -  149 GiB (1863 KiB)   free ""

number: -1
 start:              82961 MiB (~  81 GiB)
   end:       154636648447   B (~ 144 GiB)
  size:                 63 GiB (~  63 GiB)
  type: ext4
fstype: ext4
  name: Linux filesystem data
The next operations will create a new partition and format it.
Proceed? [y/N] y

The next operation will format new partition 5 on block device /dev/sda5.
Warning: label too long; will be truncated to 'Linux filesystem'

/dev/sda ( 149 GiB) gpt "ATA INTEL SSDSA2BW16"
  1:    1 MiB - 1025 MiB (   1 GiB)    efi "EFI System partition"
  2: 1025 MiB - 1041 MiB (  16 MiB)  msres "Microsoft reserved partition"
  3: 1041 MiB -   81 GiB (  80 GiB)   ntfs "Basic data partition"
  5:   81 GiB -  144 GiB (  63 GiB)   ext4 "Linux filesystem data"
 -1:  144 GiB -  148 GiB (4511 MiB)   free ""
  4:  148 GiB -  149 GiB ( 642 MiB) msdiag ""
 -2:  149 GiB -  149 GiB (1863 KiB)   free ""

Success!
~ # ded create sda -1 swap
/dev/sda ( 149 GiB) gpt "ATA INTEL SSDSA2BW16"
  1:    1 MiB - 1025 MiB (   1 GiB)    efi "EFI System partition"
  2: 1025 MiB - 1041 MiB (  16 MiB)  msres "Microsoft reserved partition"
  3: 1041 MiB -   81 GiB (  80 GiB)   ntfs "Basic data partition"
  5:   81 GiB -  144 GiB (  63 GiB)   ext4 "Linux filesystem data"
 -1:  144 GiB -  148 GiB (4511 MiB)   free ""
  4:  148 GiB -  149 GiB ( 642 MiB) msdiag ""
 -2:  149 GiB -  149 GiB (1863 KiB)   free ""

number: -1
 start:             147473 MiB (~ 144 GiB)
   end:       159366774783   B (~ 148 GiB)
  size:               4511 MiB (~4511 MiB)
  type: swap
fstype: linux-swap(v1)
  name: Swap partition
The next operations will create a new partition and format it.
Proceed? [y/N] y

The next operation will format new partition 6 on block device /dev/sda6.
Setting up swapspace version 1, size = 4730122240 bytes
UUID=83ce3bb6-bef1-4c8a-a9d4-4ce8879b9a9e
/dev/sda ( 149 GiB) gpt "ATA INTEL SSDSA2BW16"
  1:    1 MiB - 1025 MiB (   1 GiB)    efi "EFI System partition"
  2: 1025 MiB - 1041 MiB (  16 MiB)  msres "Microsoft reserved partition"
  3: 1041 MiB -   81 GiB (  80 GiB)   ntfs "Basic data partition"
  5:   81 GiB -  144 GiB (  63 GiB)   ext4 "Linux filesystem data"
  6:  144 GiB -  148 GiB (4511 MiB)   swap "Swap partition"
  4:  148 GiB -  149 GiB ( 642 MiB) msdiag ""
 -1:  149 GiB -  149 GiB (1863 KiB)   free ""

Success!
```

### Recommended partition sizes

Here are some recommended sizing guidelines:

| Type  | Purpose         | Size      |
|-------|-----------------|-----------|
| efi   | System boot     | 1 GiB     |
| fat32 | Removable media | >=512 MiB |
| ntfs  | Windows         | >=64 GiB  |
| ext4  | Ubuntu          | >=25 GiB  |
| swap  | Normal          | 1-4 GiB   |
| swap  | Hibernation     | 1.5x RAM  |

In general I recommend only considering either whole numbers of GiB or "the rest of it" when considering partition sizes.

[Archwiki recommends a 1 GiB EFI partition](https://wiki.archlinux.org/title/EFI_system_partition#Create_the_partition). That is plenty of space to do [some advanced things](https://wiki.archlinux.org/title/Unified_kernel_image) with it as well as avoid any quirks having to do with < 512 MiB sizing. It also matches what the Windows 11 installer default appears to be. Any more than 1 GiB though is probably a significant waste of space.

Red Hat provides the following table of [swap space recommendations](https://docs.redhat.com/en/documentation/red_hat_enterprise_linux/8/html/managing_storage_devices/getting-started-with-swap_managing-storage-devices).
It is a [matter of some debate](https://askubuntu.com/questions/49109/i-have-16gb-ram-do-i-need-32gb-swap), but I would say almost any modern system needs *some* amount of swap space *probably at least 1 GiB*, but almost definitely no more than 4 GiB.
You need to scale the swap space with RAM if you wish to support hibernation. But the common recommendation especially for system with large amounts of RAM is to just forget about hibernation.


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
* `mkfs.ext4`
* `mkfs.vfat`
* `mkfs.ntfs`

## Why

Most CLI partition editor tools are "literal" in that they only edit the partition table itself.
They never touch the fileystems themselves on those partitions.
They always almost always leave the disk in a non-sane in-between state.

Formatting, resizing, and even moving the filesystems themselves are left as separate operations to be done with separate tools.
When done manually this is an incredibly error-prone process.
For example when resizing a partition it matters whether you are shrinking or growing the partition, whether you should edit the partition table itself first or last, and what units and arguments the different tools use.
Screw it up and you can easily install Ubuntu directly in the middle of a Windows partition. (ask me how I know!)

`gparted` is usually the recommended tool for managing a disk in a filesystem-aware way.
But `gparted` requires a GUI interface, so it isn't appropriate for headless or rescue systems.
`gparted` is billed as a "frontend" for `parted`, but `parted` really just manages the partition table literally.
It doesn't do anything close to what `gparted` does.

`ded` depends on `parted` to manage the partition table itself, but also uses filesystem
formatting and resizing tools to provide a complete interface for basic operations.

## Known issues

### FAT resizing

[Microsoft doesn't officially support FAT32 filesystems smaller than 512 MiB](https://support.microsoft.com/en-us/topic/description-of-default-cluster-sizes-for-fat32-file-system-905ea1b1-5c4e-a03f-3863-e4846a878d31).
[dosfstools also defaults to using FAT32 as opposed to FAT16 when the size is 512 MiB or larger](https://github.com/dosfstools/dosfstools/blob/289a48b9cb5b3c589391d28aa2515c325c932c7a/src/mkfs.fat.c#L644).
In practice they [can be smaller](https://aeb.win.tue.nl/linux/fs/fat/fatgen103.pdf), but smaller FAT filesystems [can cause compatibility issues](https://wiki.archlinux.org/title/EFI_system_partition#Create_the_partition).
Some tools/firmwares/operating systems can't work with FAT32 filesystems less than 512 MiB in size.
Other times the lower limit is said to be 260 MiB, or other times 32 MiB.

To avoid issues `ded` doesn't allow creating FAT32 filesystems less than 512 MiB in size.
That includes FAT32 filesystems used to create an EFI partition.
Problematically however many operating system installers of years past have used [smaller sizes for the FAT32 formatted EFI partition, going as small as 100 MiB.](https://www.ctrl.blog/entry/esp-size-guide.html).

I would recommend resizing or re-creating these to be 1 GiB in size if the opportunity arises.
But `ded` does not offer much help with this at this time.
`fatresize` depends on code from `parted` which appears to have a minimimum size restriction at the 260 MiB mark.
Older builds of `fatresize` simply refuse to work with FAT32 filesystems smaller than 512 MiB.

I'm not really sure how best to handle these pre-existing partitions.
It might be worthwhile to [backup the filesystem](https://www.fsarchiver.org/), re-create a larger one and restore the backup.
Though *probably* if you have free space after the EFI partition you can also afford to just wipe it and start from scratch.

## Copyright

Released under the [MIT License](LICENSE).

