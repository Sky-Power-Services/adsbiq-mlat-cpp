# SPDX-License-Identifier: AGPL-3.0-or-later
"""Bounded choice population plus lossless selection indices, not RNG changes."""
import struct
from .randomness import validate, validate_key
from .schema import number

HEADER=struct.Struct('<BBHH')
STREAMS=('tracker','coordinator')
OPS=('sample','shuffle')


def key_bytes(key):
    key=validate_key(key)
    if key[0]=='aircraft':return b'\0'+struct.pack('<I',key[1])
    output=bytearray(b'\1'+struct.pack('<II',key[1],key[2]))
    for value in key[3:]:
        output += b'\0'+struct.pack('<q',value) if type(value) is int else b'\1'+struct.pack('<d',value)
    return bytes(output)


def encode_body(value):
    validate(value)
    if value['op'] not in OPS:raise ValueError('binary choice operation')
    population=value['population'];selected=value['selected']
    body=bytearray(HEADER.pack(STREAMS.index(value['stream']),OPS.index(value['op']),len(population),len(selected)))
    mapping={tuple(key):i for i,key in enumerate(population)}
    encoded=[key_bytes(key) for key in population]
    for key in encoded:body += key
    for key in selected:
        index=mapping[tuple(key)]
        if key_bytes(key)!=encoded[index]:raise ValueError('choice key representation mismatch')
        body += struct.pack('<H',index)
    if len(body)>65536-33:raise ValueError('choice record limit')
    return bytes(body)


def decode_body(body):
    if len(body)<HEADER.size:raise ValueError('short choice header')
    stream,op,count,selected_count=HEADER.unpack_from(body)
    if stream>1 or op>1 or count>4096 or selected_count>count:raise ValueError('choice header bounds')
    offset=HEADER.size;population=[]
    for _ in range(count):
        if offset>=len(body):raise ValueError('short choice key')
        tag=body[offset];size=5 if tag==0 else 27
        if tag not in (0,1) or offset+size>len(body):raise ValueError('choice key bounds')
        if tag==0:key=['aircraft',struct.unpack_from('<I',body,offset+1)[0]]
        else:
            receiver,icao=struct.unpack_from('<II',body,offset+1)
            key=['pair',receiver,icao]
            for pos in (offset+9,offset+18):
                if body[pos] not in (0,1):raise ValueError('choice numeric tag')
                key.append(number(struct.unpack_from('<q' if body[pos]==0 else '<d',body,pos+1)[0]))
        validate_key(key);population.append(key);offset+=size
    if offset+selected_count*2!=len(body):raise ValueError('choice selection size')
    selected=[]
    for index, in struct.iter_unpack('<H',body[offset:]):
        if index>=count:raise ValueError('choice index bounds')
        selected.append(list(population[index]))
    value=dict(stream=STREAMS[stream],op=OPS[op],population=population,selected=selected)
    validate(value)
    return value
