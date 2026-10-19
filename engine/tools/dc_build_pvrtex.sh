#!/usr/bin/env bash
# Builds KallistiOS's texture encoder (kos/utils/pvrtex) for the converter's
# VQ textures (tools/n64/level_dc.py vq_chain), with DreamSDK's MinGW. Its
# files are opened in binary mode first: as shipped it writes them in text
# mode, which on Windows turns each 0x0A byte into 0x0D 0x0A.
#
#   C:/DreamSDK/usr/bin/bash.exe <full path of tools/dc_build_pvrtex.sh>
set -e
export PATH="/mingw64/bin:/usr/bin:$PATH"
cd /opt/toolchains/dc/kos/utils/pvrtex
sed -i 's/fopen(\([a-z]*\), "w")/fopen(\1, "wb")/; s/fopen(\([a-z]*\), "r")/fopen(\1, "rb")/' \
	file_common.c file_dctex.c file_pvr.c file_tex.c
export KOS_BASE=/opt/toolchains/dc/kos
make CC=gcc CXX=g++
ls -l pvrtex.exe
