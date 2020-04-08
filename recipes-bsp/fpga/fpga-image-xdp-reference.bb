SUMMARY = "FPGA bitstream for XDP board reference design"
COMPATIBLE_MACHINE = "^xdpzu7"

# Downloads a precompiled bitstream from the TOPIC website

require fpga-image.inc

LICENSE = "GPLv2"
LIC_FILES_CHKSUM = "file://${META_ZYNQ_BASE}/COPYING;md5=751419260aa954499f7abaabaa882bbe"

BOARD_DESIGN_URI = ""

PV = "1"

BOARD_DESIGN_PATH = "${BPN}-${MACHINE}"
TOPICDOWNLOADS_URI ?= "http://topic-downloads.fra1.digitaloceanspaces.com"
BOARD_DESIGN_URI = "file://${HOME}/Downloads/fpga.bit"

PKGV = "${PV}"
S = "${WORKDIR}"
B = "${S}"

# Nothing to build
do_compile() {
    cp ${HOME}/Downloads/fpga.bit ${S}/fpga.bit
}

SRC_URI[xdpzu7.md5sum] = "0b5860a586b4bea70f4c1d44d6aabbc8"
