# This file handles installing necessary ubuntu packages when
# using Vagrant.

# x11-utils for xdpyinfo as used in display_option detection.

# dos2unix in case running on a windows host where git has
#  bollocksed the line endings, making script files not
#  executable.

apt-get update
apt-get upgrade
sudo apt-get -y install build-essential nasm libc6-dev-i386 git gdb x11-utils dos2unix
sudo apt-get -y build-dep qemu
sudo apt-get -y install qemu-system-i386
