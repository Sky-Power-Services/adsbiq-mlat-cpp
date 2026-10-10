# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded ctypes adapter to the existing Linux liblz4 stable block API.

CDLL releases the GIL during compression/decompression. Input bytes and output
buffers remain owned by the calling writer/reader for the entire native call.
No downloads, subprocesses, global installs or platform DLL searches.
"""
import ctypes
import sys

MAX_INPUT = 8*1024*1024
MAX_OUTPUT = 9*1024*1024
_library = None


def library():
    global _library
    if _library is None:
        if sys.platform != 'linux':
            raise RuntimeError('LZ4 capture requires the qualified Linux runtime')
        value = ctypes.CDLL('liblz4.so.1')
        value.LZ4_compressBound.argtypes = [ctypes.c_int]
        value.LZ4_compressBound.restype = ctypes.c_int
        value.LZ4_compress_default.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        value.LZ4_compress_default.restype = ctypes.c_int
        value.LZ4_compress_fast.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        value.LZ4_compress_fast.restype = ctypes.c_int
        value.LZ4_decompress_safe.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        value.LZ4_decompress_safe.restype = ctypes.c_int
        value.LZ4_versionNumber.argtypes = []
        value.LZ4_versionNumber.restype = ctypes.c_int
        if value.LZ4_versionNumber() < 10900:
            raise RuntimeError('unsupported LZ4 runtime')
        _library = value
    return _library


def compress(data, acceleration=1):
    if type(data) is not bytes or not 0 < len(data) <= MAX_INPUT:
        raise ValueError('LZ4 input bound')
    if type(acceleration) is not int or not 1 <= acceleration <= 16:
        raise ValueError('LZ4 acceleration bound')
    lib = library()
    bound = lib.LZ4_compressBound(len(data))
    if not 0 < bound <= MAX_OUTPUT:
        raise ValueError('LZ4 output bound')
    output = ctypes.create_string_buffer(bound)
    size = lib.LZ4_compress_fast(data, output, len(data), bound, acceleration)
    if not 0 < size <= bound:
        raise ValueError('LZ4 compression failed')
    return ctypes.string_at(output, size)


def decompress(data, expected):
    if (type(data) is not bytes or not 0 < len(data) <= MAX_OUTPUT
            or type(expected) is not int or not 0 < expected <= MAX_INPUT):
        raise ValueError('LZ4 decode bound')
    output = ctypes.create_string_buffer(expected)
    size = library().LZ4_decompress_safe(data, output, len(data), expected)
    if size != expected:
        raise ValueError('LZ4 decoded length or framing mismatch')
    return output.raw
