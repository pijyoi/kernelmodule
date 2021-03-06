from __future__ import print_function

import ctypes
import ctypes.util
import os
import mmap
import fcntl
import struct

def get_iocode(magic, num, size, dirn):
    lut = {'':0, 'w':1, 'r':2, 'rw':3}
    dirn = lut[dirn]
    op = dirn << 30 | size << 16 | magic << 8 | num
    return op

libc = ctypes.CDLL(ctypes.util.find_library('c'))

pagesize = libc.getpagesize()
bufsize = 32768

fd = os.open("/dev/mymiscdev", os.O_RDWR)
mm = mmap.mmap(fd, bufsize, offset=1*pagesize) 

memptr = ctypes.c_void_p()
libc.posix_memalign(ctypes.byref(memptr), 128, bufsize)

print(hex(memptr.value), bufsize)

fmt = struct.Struct("PL")
op = get_iocode(0xA5, 1, fmt.size, 'r')
arg = fmt.pack(memptr.value, bufsize)
fcntl.ioctl(fd, op, arg)

print('attempting read')
data = os.read(fd, 32)

