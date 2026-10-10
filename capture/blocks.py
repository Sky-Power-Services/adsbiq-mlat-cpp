# SPDX-License-Identifier: AGPL-3.0-or-later
"""Independent bounded zlib or checksummed LZ4 blocks around unchanged MLAT record envelopes.

Reader inflates at most8MiB, checks declared length/EOF/CRC, and only permits
record-sized read requests. No whole-file decode or unbounded decompression.
"""
import struct
import zlib

HEADER = struct.Struct('<4sII')
MAX_PLAIN = 8*1024*1024
MAX_COMPRESSED = 9*1024*1024
MAX_CHUNK_PLAIN = 64*1024*1024
MAX_TOTAL_PLAIN = 8*1024*1024*1024
FORMAT = 'mlatcap-v1-zlib-blocks'
LZ4_FORMAT = 'mlatcap-v1-lz4-blocks'
CHECKSUM = struct.Struct('<I')


def encode_block(data, codec="zlib", lz4_acceleration=1):
    if type(data) is not bytes or not 0 < len(data) <= MAX_PLAIN:
        raise ValueError('block input bound')
    if codec == 'lz4':
        from .lz4_codec import compress
        compressed = compress(data, lz4_acceleration)
    elif codec == 'zlib':
        compressed = zlib.compress(data, 1)
    else:
        raise ValueError('unknown block codec')
    if len(compressed) > MAX_COMPRESSED:
        raise ValueError('compressed block bound')
    if codec == 'lz4':
        return HEADER.pack(b'MCL1', len(data), len(compressed)) + CHECKSUM.pack(zlib.crc32(data)) + compressed
    return HEADER.pack(b'MCZ1', len(data), len(compressed)) + compressed


class BlockReader:
    def __init__(self, stream, max_plain=MAX_CHUNK_PLAIN, codec=None):
        if type(max_plain) is not int or not 0 < max_plain <= MAX_CHUNK_PLAIN:
            raise ValueError("logical chunk bound")
        if codec not in (None, 'zlib', 'lz4'):
            raise ValueError('unknown block codec')
        self.codec = codec
        self.max_plain = max_plain
        self.stream = stream
        self.buffer = b''
        self.offset = 0
        self.total_plain = 0
        self.eof = False

    def _next(self):
        header = self.stream.read(HEADER.size)
        if not header:
            self.eof = True
            return False
        if len(header) != HEADER.size:
            raise ValueError('truncated block header')
        magic, plain_size, compressed_size = HEADER.unpack(header)
        if magic not in (b'MCZ1', b'MCL1') or not 0 < plain_size <= MAX_PLAIN or not 0 < compressed_size <= MAX_COMPRESSED:
            raise ValueError('block header bounds')
        if self.total_plain + plain_size > self.max_plain:
            raise ValueError('logical chunk budget exceeded')
        codec = 'lz4' if magic == b'MCL1' else 'zlib'
        if self.codec is not None and self.codec != codec:
            raise ValueError('manifest block codec mismatch')
        expected_crc = None
        if codec == 'lz4':
            checksum = self.stream.read(CHECKSUM.size)
            if len(checksum) != CHECKSUM.size:
                raise ValueError('truncated block checksum')
            expected_crc, = CHECKSUM.unpack(checksum)
        compressed = self.stream.read(compressed_size)
        if len(compressed) != compressed_size:
            raise ValueError('truncated compressed block')
        if codec == 'lz4':
            from .lz4_codec import decompress
            plain = decompress(compressed, plain_size)
            if zlib.crc32(plain) != expected_crc:
                raise ValueError('block checksum mismatch')
        else:
            decoder = zlib.decompressobj()
            plain = decoder.decompress(compressed, plain_size+1)
            if (len(plain) != plain_size or not decoder.eof or decoder.unconsumed_tail
                    or decoder.unused_data):
                raise ValueError('compressed length or framing mismatch')
        self.buffer = plain
        self.offset = 0
        self.total_plain += plain_size
        return True

    def read(self, size):
        if type(size) is not int or not 0 <= size <= 65536:
            raise ValueError('streaming read bound')
        output = bytearray()
        while len(output) < size:
            if self.offset == len(self.buffer):
                self.buffer = b''
                if self.eof or not self._next():
                    break
            amount = min(size-len(output), len(self.buffer)-self.offset)
            output.extend(self.buffer[self.offset:self.offset+amount])
            self.offset += amount
        return bytes(output)
