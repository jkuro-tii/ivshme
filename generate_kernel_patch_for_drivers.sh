#!/usr/bin/env bash

set -euo pipefail

[[ ! -d linux-v6.1 ]] && git clone git@github.com:torvalds/linux.git --branch v6.1 --single-branch linux-v6.1

pushd linux-v6.1
  git reset --hard
  git clean -xdf

  echo "obj-y                           += kvm_ivshmem.o" >> drivers/char/Makefile
  cp -v ../drivers/kvm_ivshmem.c ./drivers/char

  git add .
  git commit -m "Shared memory driver"
  git format-patch -k -1 -o ..
popd
