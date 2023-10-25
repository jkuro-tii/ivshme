#!/bin/sh

SOCKET=./server.sock
DEVICE=/dev/ivshmem
MODDIR=~ghaf/ivshmem/code/module

if test -e "$SOCKET"; then
  echo "Removing $SOCKET"
  rm "$SOCKET"
fi

if test ! -e "$DEVICE"; then
echo "Loading shared memory module"
sudo rmmod kvm_ivshmem ; sudo insmod $MODDIR/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
fi

./memsocket -s "$SOCKET"

echo "Executing 'waypipe -d -s $SOCKET server -- weston-terminal'"
waypipe -d -s "$SOCKET" server -- weston-terminal
