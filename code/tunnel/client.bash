#!/bin/sh

SOCKET=./client.sock
DEVICE=/dev/ivshmem
MODDIR=~ghaf/ivshmem/code/module

if test -f "$SOCKET"; then
  rm "$SOCKET"
fi
if test ! -f "$DEVICE"; then
echo "Loading shared memory module"
sudo rmmod kvm_ivshmem ; sudo insmod $MODDIR/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
fi

echo "Starting waypipe in background"
waypipe  -s "$SOCKET" client &
sudo rmmod kvm_ivshmem ; sudo insmod ../module/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
./server
