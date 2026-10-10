# SPDX-License-Identifier: AGPL-3.0-or-later
"""Lossless bounded rate-report rows; retain key case/width/order and number type."""
import struct
from .schema import hexstr, number


def encode_body(rows):
    if type(rows) not in (list, tuple) or len(rows)>2048:raise ValueError('rate count')
    result=bytearray(struct.pack('<H',len(rows)));seen=set()
    for row in rows:
        if type(row) not in (tuple,list) or len(row)!=2:raise ValueError('rate row')
        key,value=row;hexstr(key,range(1,7));number(value)
        if key in seen:raise ValueError('duplicate rate key')
        seen.add(key);result.append(len(key));result.extend(key.encode('ascii'))
        result.extend(b'\0'+struct.pack('<q',value) if type(value) is int else b'\1'+struct.pack('<d',value))
    return bytes(result)


def decode_body(body):
    if len(body)<2:raise ValueError('short rate header')
    count,=struct.unpack_from('<H',body)
    if count>2048:raise ValueError('rate count')
    pos=2;rows=[];seen=set()
    for _ in range(count):
        if pos>=len(body):raise ValueError('short rate row')
        size=body[pos];pos+=1
        if not 1<=size<=6 or pos+size+9>len(body):raise ValueError('rate row bounds')
        key=body[pos:pos+size].decode('ascii');hexstr(key,range(1,7));pos+=size
        if key in seen:raise ValueError('duplicate rate key')
        seen.add(key);tag=body[pos];pos+=1
        if tag not in (0,1):raise ValueError('rate number tag')
        value,=struct.unpack_from('<q' if tag==0 else '<d',body,pos);number(value);pos+=8
        rows.append([key,value])
    if pos!=len(body):raise ValueError('extra rate bytes')
    return rows
