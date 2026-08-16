#!/bin/sh
set -eu

sudo rmmod hd60prodrv
dmesg | grep hd60prodrv | tail -20
