#!/bin/sh

SOCKET=./client.sock
DEVICE=/dev/ivshmem
MODDIR=~ghaf/ivshmem/code/module

if test -e $SOCKET; then
  echo "Removing $SOCKET"
  rm "$SOCKET"
fi
if test ! -e "$DEVICE"; then
echo "Loading shared memory module"
sudo rmmod kvm_ivshmem ; sudo insmod $MODDIR/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
fi

echo "Starting waypipe in background"
waypipe  -s "$SOCKET" client &
sudo rmmod kvm_ivshmem ; sudo insmod ../module/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
./server
