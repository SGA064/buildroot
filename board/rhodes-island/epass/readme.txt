Intro
=====

This configuration builds a Buildroot image set for the Rhodes Island
ePass board based on the Allwinner F1C200S.

It fetches the Linux kernel from:

  https://github.com/SGA064/linux-6.18.8.git

using the main branch.

The Linux kernel is built with the in-tree epass_f1c200s_defconfig and
the allwinner/suniv-f1c200s-epass device tree. Buildroot builds the
kernel zImage and Linux DTB separately, then post-image.sh appends the
DTB into the final output/images/zImage used for flashing.

How to build it
===============

  $ make epass_f1c200s_defconfig
  $ make

Result of the build
===================

After building, the main files are available in output/images/:

  u-boot-sunxi-with-spl.bin
  zImage
  suniv-f1c200s-epass.dtb
  rootfs.ubi
  epass-flash.sh

How to flash
============

Put the board in FEL mode and run:

  $ PATH=output/host/bin:$PATH output/images/epass-flash.sh \
      -B output/images/u-boot-sunxi-with-spl.bin \
      -k output/images/zImage \
      -u output/images/rootfs.ubi

The U-Boot DFU layout matches the SPI-NAND partition map in the ePass
device tree:

  128k(spl), 896k(u-boot), 128k(env), 8m(kernel), -(ubi)
