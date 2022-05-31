import ctypes
import ctypes.util
import os
import mmap
import fcntl
import struct
import time

def get_iocode(magic, num, size, dirn):
    lut = {'':0, 'w':1, 'r':2, 'rw':3}
    dirn = lut[dirn]
    op = dirn << 30 | size << 16 | magic << 8 | num
    return op

libc = ctypes.CDLL(ctypes.util.find_library('c'))

pagesize = libc.getpagesize()
bufsize = 2*pagesize

fd = os.open("/dev/mymiscdev", os.O_RDWR)
mm = mmap.mmap(fd, bufsize, offset=0*pagesize) 

def read_reg(addr):
    fmt = struct.Struct("iII")
    op = get_iocode(0xA5, 2, fmt.size, 'rw')
    arg = bytearray(fmt.size)
    fmt.pack_into(arg, 0, 0, addr, 0)
    fcntl.ioctl(fd, op, arg)
    return struct.unpack_from("I", arg, 8)[0]

def write_reg(addr, val):
    fmt = struct.Struct("iII")
    op = get_iocode(0xA5, 2, fmt.size, 'rw')
    arg = bytearray(fmt.size)
    fmt.pack_into(arg, 0, 1, addr, val)
    fcntl.ioctl(fd, op, arg)

def do_dma(write, offset, length):
    fmt = struct.Struct("iII")
    op = get_iocode(0xA5, 3, fmt.size, 'w')
    arg = fmt.pack(write, offset, length)
    fcntl.ioctl(fd, op, arg)
    cnt = 0
    while 1:
        # poll for completion
        dmacmd = read_reg(0x98)
        cnt += 1
        if dmacmd & 1 == 0:
            break
        time.sleep(0.01)
    return cnt

print(hex(read_reg(0)))

# check bit inversion
write_reg(4, 0xAAAA5555)
print(hex(read_reg(4)))
write_reg(4, 0x5555AAAA)
print(hex(read_reg(4)))

# compute factorial
write_reg(8, 10)
while 1:
    # poll for completion
    status = read_reg(0x20)
    if status & 1 == 0:
        break
print(read_reg(8))

# test dma
length = 256
# load stuff into bounce buffer
for idx in range(length):
    mm[idx] = idx
# write to device
cnt = do_dma(1, 0, length)
print(f'polled {cnt} times for dma write')
# read from device
offset = 4096
cnt = do_dma(0, offset, length)
print(f'polled {cnt} times for dma read')
# check bounce buffer
for idx in range(length):
    assert mm[offset + idx] == idx


