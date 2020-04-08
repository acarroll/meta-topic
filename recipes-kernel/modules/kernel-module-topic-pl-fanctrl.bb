SUMMARY = "Kernel driver module for PL fan controller"
LICENSE = "GPLv3"
LIC_FILES_CHKSUM = "file://COPYING;md5=9eef91148a9b14ec7f9df333daebc746"

MODULE = "${@d.getVar('PN', True).split('-', 2)[2]}"

inherit module gitpkgv

PV = "4+${SRCPV}"
PKGV = "4+${GITPKGV}"
S = "${WORKDIR}/git"

TOPIC_INTERNAL_URI = "git://repo-products.topic.nl"
SRC_URI = "${TOPIC_INTERNAL_URI}/${BPN}"

SRCREV = "ca1e84464aba47e37a8cec426f5ff7c8bba7be4f"

# Automatically load at boot
KERNEL_MODULE_AUTOLOAD = "${MODULE}"
