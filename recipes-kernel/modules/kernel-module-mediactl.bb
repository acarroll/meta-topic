#
# This file was derived from the 'Hello World!' example recipe in the
# Yocto Project Development Manual.
#
SUMMARY = "Kernel module for Topic media ctl"
SECTION = "Test"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

inherit module gitpkgv

SRC_URI = "file://Makefile \ 
	   file://topic-mediactl.c "

S = "${WORKDIR}"


#Start at boot
KERNEL_MODULE_AUTOLOAD += "topic-mediactl"

