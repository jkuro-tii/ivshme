#!/bin/sh

SOCKET=./server.sock
DEVICE=/dev/ivshmem

if test -e "$SOCKET"; then
  rm "$SOCKET"
fi

if test ! -e "$DEVICE"; then
sudo rmmod kvm_ivshmem ; sudo insmod ../module/kvm_ivshmem.ko; sudo chmod a+rwx /dev/ivshmem
fi

./server server &
sleep 5
echo "Executing 'waypipe -d -s $SOCKET server -- weston-terminal'"
waypipe -d -s "$SOCKET" server -- weston-terminal
