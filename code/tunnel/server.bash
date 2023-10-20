#!/bin/sh

SOCKET=./server.sock
DEVICE=/dev/ivshmem
MODDIR=~ghaf/ivshmem/code/module
if test -f "$SOCKET"; then
  rm "$SOCKET"
fi
if test ! -f "$DEVICE"; then
echo "Loading shared memory module"
sudo rmmod kvm_ivshmem ; sudo insmod $MODDIR/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
fi
./server server &
sleep 5
echo "Executing 'waypipe -d -s $SOCKET server -- weston-terminal'"
waypipe -d -s "$SOCKET" server -- weston-terminal
