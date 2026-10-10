/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Experimental Linux CPython ingress encoder, NOT production qualified.
 * Allocates one <=65536-byte immutable record (sync/MLAT <=77 bytes). Never touches files or releases
 * the GIL; only allowlisted fields are copied; ignored control payloads are never read.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#ifdef Py_GIL_DISABLED
#error "Capture queue requires a GIL-enabled CPython build"
#endif
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define KEY_COUNT 16
#define MAX_RECORD 65536
#define AIR_COUNT 20
typedef struct {
    PyObject *keys[KEY_COUNT];PyObject *air[AIR_COUNT];PyObject *emit_names[6];PyObject *status_names[12];PyObject *input_names[4];PyObject *air_allowed,*queue_type,*air_tag,*pair_tag;
    /* Borrowed values, dereferenced ONLY after live exact dictionary identity
     * and globally unique CPython 3.12 mutation version both match. */
    PyObject *input_dict,*input_queue,*input_begin,*input_deadline;
    uint64_t input_version;
} State;
_Static_assert(sizeof(double)==8,"capture requires IEEE-sized double");
static void put64(unsigned char *p,uint64_t n){for(int i=0;i<8;++i){p[i]=(unsigned char)n;n>>=8;}}
static void put32(unsigned char *p,uint32_t n){for(int i=0;i<4;++i){p[i]=(unsigned char)n;n>>=8;}}
static int tick(PyObject *value,unsigned char *out){
    if(!value||(!PyLong_CheckExact(value)&&!PyFloat_CheckExact(value)))return 0;
    double n;
    if(PyLong_CheckExact(value)){
        long long integer=PyLong_AsLongLong(value);
        if(PyErr_Occurred()||integer>1000000000000000000LL||integer< -1000000000000000000LL)return 0;
        n=(double)integer;
    }else{n=PyFloat_AS_DOUBLE(value);if(!isfinite(n)||fabs(n)>1e18)return 0;}
    uint64_t bits;memcpy(&bits,&n,8);put64(out,bits);return 1;
}
/* Zero means invalid; valid digits are one-based. Immutable256-entry table
 * avoids three range branches per nibble on the dominant sync input path. */
static const unsigned char hex_digit[256]={
    ['0']=1,['1']=2,['2']=3,['3']=4,['4']=5,['5']=6,['6']=7,['7']=8,['8']=9,['9']=10,
    ['a']=11,['b']=12,['c']=13,['d']=14,['e']=15,['f']=16,
    ['A']=11,['B']=12,['C']=13,['D']=14,['E']=15,['F']=16};
static int nibble(unsigned char c){return (int)hex_digit[c]-1;}
static int hexbytes(PyObject *value,unsigned char *out,int sync){
    if(!value||!PyUnicode_CheckExact(value))return 0;
    Py_ssize_t length=PyUnicode_GET_LENGTH(value);
    if(length!=28&&(sync||length!=14))return 0;
    if(!PyUnicode_IS_ASCII(value))return 0;
    const unsigned char *text=PyUnicode_1BYTE_DATA(value);
    for(Py_ssize_t i=0;i<length;i+=2){int a=nibble(text[i]),b=nibble(text[i+1]);if(a<0||b<0)return 0;out[i/2]=(unsigned char)((a<<4)|b);}
    return (int)(length/2);
}
static PyObject *invalid(void){PyErr_Clear();PyErr_SetString(PyExc_ValueError,"invalid capture ingress");return NULL;}
static int finish_header(unsigned char *bytes,unsigned size,int kind,uint64_t seq,uint64_t begin,uint32_t id,uint64_t mono){
    struct timespec utc;
    if(mono<begin||clock_gettime(CLOCK_REALTIME,&utc)){invalid();return 0;}
    put32(bytes,size);put64(bytes+4,seq);put64(bytes+12,mono-begin);
    put64(bytes+20,(uint64_t)utc.tv_sec*1000000000ULL+(uint64_t)utc.tv_nsec);
    put32(bytes+28,id);bytes[32]=(unsigned char)kind;
    return 1;
}
static PyObject *finish(unsigned char *bytes,unsigned size,int kind,uint64_t seq,uint64_t begin,uint32_t id,uint64_t mono){
    if(!finish_header(bytes,size,kind,seq,begin,id,mono))return NULL;
    return PyBytes_FromStringAndSize((const char *)bytes,33+size);
}
static int append(unsigned char *bytes,unsigned *used,const char *text,unsigned size){
    if(size>MAX_RECORD-*used)return 0;
    memcpy(bytes+*used,text,size);*used+=size;return 1;
}
static int address(unsigned char *bytes,unsigned *used,PyObject *value){
    if(!PyUnicode_CheckExact(value)||!PyUnicode_IS_ASCII(value))return 0;
    Py_ssize_t size=PyUnicode_GET_LENGTH(value);if(size<1||size>6)return 0;
    const unsigned char *text=PyUnicode_1BYTE_DATA(value);
    /* Validate and copy this <=6-byte ASCII field in one pass. One checked
     * reservation replaces three append checks/copies; case/width unchanged. */
    if(*used>MAX_RECORD||(unsigned)size+2>MAX_RECORD-*used)return 0;
    unsigned offset=*used;bytes[offset++]='"';
    for(Py_ssize_t i=0;i<size;++i){unsigned char c=text[i];if(nibble(c)<0)return 0;bytes[offset++]=c;}
    bytes[offset++]='"';*used=offset;return 1;
}
/* Separate frame keeps the bounded 64KiB scratch off the sync/MLAT hot path. */
static PyObject *metadata(State *state,PyObject *message,uint64_t seq,uint64_t begin,uint32_t id,uint64_t mono){
    int key;PyObject *value=NULL;
    for(key=8;key<KEY_COUNT;++key){value=PyDict_GetItemWithError(message,state->keys[key]);if(PyErr_Occurred())return invalid();if(value)break;}
    if(!value){Py_RETURN_NONE;} /* Unknown/quine: Python allowlist invalidates. */
    unsigned char bytes[MAX_RECORD];unsigned used=33;int kind=key-4;
    if(kind==4||kind==5){
        if(!PyList_CheckExact(value)||PyList_GET_SIZE(value)>4096)return invalid();
        if(!append(bytes,&used,"[",1))return invalid();
        for(Py_ssize_t i=0;i<PyList_GET_SIZE(value);++i){
            if((i&&!append(bytes,&used,",",1))||!address(bytes,&used,PyList_GET_ITEM(value,i)))return invalid();
        }
        if(!append(bytes,&used,"]",1))return invalid();
    }else if(kind==11){
        if(!PyDict_CheckExact(value)||PyDict_Size(value)>2048)return invalid();
        unsigned count=(unsigned)PyDict_Size(value);kind=30;
        bytes[used++]=(unsigned char)count;bytes[used++]=(unsigned char)(count>>8);
        Py_ssize_t position=0;PyObject *icao,*rate;
        while(PyDict_Next(value,&position,&icao,&rate)){
            unsigned char checked[8];if(!tick(rate,checked))return invalid();
            if(!PyUnicode_CheckExact(icao)||!PyUnicode_IS_ASCII(icao))return invalid();
            Py_ssize_t size=PyUnicode_GET_LENGTH(icao);if(size<1||size>6)return invalid();
            const unsigned char *key=PyUnicode_1BYTE_DATA(icao);
            for(Py_ssize_t i=0;i<size;++i)if(nibble(key[i])<0)return invalid();
            unsigned char row[16];row[0]=(unsigned char)size;memcpy(row+1,key,(size_t)size);
            if(PyLong_CheckExact(rate)){row[1+size]=0;put64(row+2+size,(uint64_t)PyLong_AsLongLong(rate));}
            else{row[1+size]=1;memcpy(row+2+size,checked,8);}
            if(!append(bytes,&used,(const char *)row,(unsigned)size+10))return invalid();
        }
    }else{
        if(!append(bytes,&used,"null",4))return invalid();
    }
    return finish(bytes,used-33,kind,seq,begin,id,mono);
}
/* Shared raw core receives checked numeric metadata directly. Fused recording
 * no longer allocates/reparses sequence and generation Python integers. */
static PyObject *encode_input_raw(State *state,uint64_t seq,uint64_t begin,uint32_t id,PyObject *message,double deadline,unsigned char *inline_bytes,unsigned *inline_size){
    struct timespec mono;
    if(clock_gettime(CLOCK_MONOTONIC,&mono))return invalid();
    if(deadline>0&&(double)mono.tv_sec+(double)mono.tv_nsec*1e-9>=deadline){Py_RETURN_FALSE;}
    uint64_t monotonic=(uint64_t)mono.tv_sec*1000000000ULL+(uint64_t)mono.tv_nsec;
    unsigned char bytes[77];unsigned size;int kind;
    PyObject *value=PyDict_GetItemWithError(message,state->keys[0]);
    if(value){
        if(!PyDict_CheckExact(value)||!tick(PyDict_GetItemWithError(value,state->keys[2]),bytes+33)||
           !tick(PyDict_GetItemWithError(value,state->keys[3]),bytes+41)||
           !hexbytes(PyDict_GetItemWithError(value,state->keys[4]),bytes+49,1)||
           !hexbytes(PyDict_GetItemWithError(value,state->keys[5]),bytes+63,1))return invalid();
        size=44;kind=2;
    }else if((value=PyDict_GetItemWithError(message,state->keys[1]))){
        if(!PyDict_CheckExact(value)||!tick(PyDict_GetItemWithError(value,state->keys[6]),bytes+33))return invalid();
        int length=hexbytes(PyDict_GetItemWithError(value,state->keys[7]),bytes+41,0);if(!length)return invalid();
        size=8+(unsigned)length;kind=3;
    }else{return metadata(state,message,seq,begin,(uint32_t)id,monotonic);}
    if(inline_bytes){
        if(!finish_header(bytes,size,kind,seq,begin,id,monotonic))return NULL;
        *inline_size=size+33;memcpy(inline_bytes,bytes,*inline_size);Py_RETURN_TRUE;
    }
    return finish(bytes,size,kind,seq,begin,(uint32_t)id,monotonic);
}
static PyObject *encode_input(PyObject *self,PyObject *const *args,Py_ssize_t count){
    State *state=PyModule_GetState(self);
    if(count!=4&&count!=5){PyErr_SetString(PyExc_TypeError,"capture encoder requires four or five arguments");return NULL;}
    PyObject *sequence=args[0],*start=args[1],*receiver=args[2],*message=args[3];
    if(!PyLong_CheckExact(sequence)||!PyLong_CheckExact(start)||!PyLong_CheckExact(receiver)||!PyDict_CheckExact(message))return invalid();
    uint64_t seq=PyLong_AsUnsignedLongLong(sequence),begin=PyLong_AsUnsignedLongLong(start);
    unsigned long long id=PyLong_AsUnsignedLongLong(receiver);
    if(PyErr_Occurred()||id>UINT32_MAX)return invalid();
    double deadline=0;
    if(count==5){
        if(!PyFloat_CheckExact(args[4]))return invalid();
        deadline=PyFloat_AS_DOUBLE(args[4]);if(!isfinite(deadline)||deadline<=0)return invalid();
    }
    return encode_input_raw(state,seq,begin,(uint32_t)id,message,deadline,NULL,NULL);
}
/* Same exact allowlist as status._python_aircraft, without Python per-field
 * calls. Only immutable scalar references are retained. UID mapping executes
 * the supplied checked callback; no raw source UID leaves this function. */
static PyObject *aircraft_row(PyObject *self,PyObject *const *args,Py_ssize_t count){
    if(count!=2){PyErr_SetString(PyExc_TypeError,"aircraft row requires two arguments");return NULL;}
    State *state=PyModule_GetState(self);PyObject *value=args[0],*map=args[1];
    if(!PyDict_CheckExact(value)||(!PyCallable_Check(map)&&!PyDict_CheckExact(map))||PyDict_Size(value)<13||PyDict_Size(value)>AIR_COUNT)return invalid();
    if(PyDict_CheckExact(map)&&PyDict_Size(map)>4096)return invalid();
    for(int i=0;i<13;++i){
        if(!PyDict_GetItemWithError(value,state->air[i]))return invalid();
    }
    PyObject *result=PyDict_New();if(!result)return NULL;
    Py_ssize_t pos=0;PyObject *key,*item,*tracking=NULL;
    while(PyDict_Next(value,&pos,&key,&item)){
        if(!PyUnicode_CheckExact(key)||PySet_Contains(state->air_allowed,key)!=1)goto bad;
        int is_icao=PyObject_RichCompareBool(key,state->air[0],Py_EQ);
        int is_tracking=PyObject_RichCompareBool(key,state->air[19],Py_EQ);
        if(is_icao<0||is_tracking<0)goto bad;
        if(is_icao){
            if(!PyUnicode_CheckExact(item)||!PyUnicode_IS_ASCII(item)||PyUnicode_GET_LENGTH(item)!=6)goto bad;
            const unsigned char *text=PyUnicode_1BYTE_DATA(item);
            for(int i=0;i<6;++i)if(nibble(text[i])<0)goto bad;
        }else if(is_tracking){
            if(!PyList_CheckExact(item)&&!PyTuple_CheckExact(item))goto bad;
            Py_ssize_t size=PySequence_Size(item);if(size<0||size>4096)goto bad;
            tracking=item;continue;
        }else{
            unsigned char checked[8];if(!tick(item,checked))goto bad;
        }
        if(PyDict_SetItem(result,key,item)<0)goto bad;
    }
    /* Finish dictionary iteration before invoking any Python callback. Own a
     * bounded tuple snapshot so reentrant callbacks cannot invalidate borrowed
     * list items, change loop bounds or free keys while we use them. */
    if(tracking){
        PyObject *source=PySequence_Tuple(tracking);if(!source)goto bad;
        Py_ssize_t size=PyTuple_GET_SIZE(source);
        PyObject *mapped=PyTuple_New(size);if(!mapped){Py_DECREF(source);goto bad;}
        for(Py_ssize_t i=0;i<size;++i){
            PyObject *uid=PyTuple_GET_ITEM(source,i),*identity;
            if(PyDict_CheckExact(map)){
                if(!PyLong_CheckExact(uid)){Py_DECREF(source);Py_DECREF(mapped);goto bad;}
                identity=PyDict_GetItemWithError(map,uid);Py_XINCREF(identity);
            }else identity=PyObject_CallOneArg(map,uid);
            if(!identity){Py_DECREF(source);Py_DECREF(mapped);goto bad;}
            long long n=PyLong_CheckExact(identity)?PyLong_AsLongLong(identity):0;
            if(PyErr_Occurred()||n<1||n>65536){Py_DECREF(identity);Py_DECREF(source);Py_DECREF(mapped);goto bad;}
            PyTuple_SET_ITEM(mapped,i,identity);
        }
        Py_DECREF(source);
        int ok=PyDict_SetItem(result,state->air[19],mapped);Py_DECREF(mapped);
        if(ok<0)goto bad;
    }
    return result;
bad:
    Py_DECREF(result);return invalid();
}
/* Appended event code 28. Body: snapshot u64, six ASCII ICAO bytes, presence
 * mask u32, each present numeric as (tag u8, int64/float64), optional bounded
 * u32 count and mapped u32 receiver IDs. No number precision/type is lost. */
/* Batch-only path: caller checked every map key/value and row key is exact.
 * No callbacks/GIL release, no sanitized dict/tuple materialization. Preserve
 * all original scalar checks and binary field order/type/sign bit. */
static int aircraft_body_direct(State *state,PyObject *key,PyObject *row,PyObject *map,uint64_t snapshot,unsigned char *bytes,unsigned *used){
    Py_ssize_t size=PyDict_Size(row);if(size<13||size>AIR_COUNT)return 0;
    Py_ssize_t pos=0;PyObject *name,*value;
    while(PyDict_Next(row,&pos,&name,&value))if(!PyUnicode_CheckExact(name)||PySet_Contains(state->air_allowed,name)!=1)return 0;
    PyObject *icao=PyDict_GetItemWithError(row,state->air[0]);
    if(!icao||!PyUnicode_CheckExact(icao)||!PyUnicode_IS_ASCII(icao)||PyUnicode_GET_LENGTH(icao)!=6||PyUnicode_Compare(key,icao)!=0)return 0;
    const unsigned char *text=PyUnicode_1BYTE_DATA(icao);
    for(int i=0;i<6;++i)if(nibble(text[i])<0)return 0;
    put64(bytes+33,snapshot);memcpy(bytes+41,text,6);*used=51;uint32_t mask=0;
    for(int i=1;i<19;++i){
        value=PyDict_GetItemWithError(row,state->air[i]);
        if(!value){if(i<13)return 0;continue;}
        unsigned char checked[8];if(!tick(value,checked))return 0;
        mask|=1U<<(i-1);
        if(PyLong_CheckExact(value)){bytes[(*used)++]=0;put64(bytes+*used,(uint64_t)PyLong_AsLongLong(value));}
        else{bytes[(*used)++]=1;memcpy(bytes+*used,checked,8);}
        *used+=8;
    }
    PyObject *tracking=PyDict_GetItemWithError(row,state->air[19]);
    if(tracking){
        if(!PyTuple_CheckExact(tracking)&&!PyList_CheckExact(tracking))return 0;
        Py_ssize_t n=PySequence_Size(tracking);if(n>4096)return 0;
        mask|=1U<<18;put32(bytes+*used,(uint32_t)n);*used+=4;
        for(Py_ssize_t i=0;i<n;++i){
            PyObject *uid=PyTuple_CheckExact(tracking)?PyTuple_GET_ITEM(tracking,i):PyList_GET_ITEM(tracking,i);
            if(!PyLong_CheckExact(uid))return 0;
            PyObject *identity=PyDict_GetItemWithError(map,uid);if(!identity)return 0;
            /* map entries were exact integer identities1..65536 before batch. */
            put32(bytes+*used,(uint32_t)PyLong_AsUnsignedLong(identity));*used+=4;
        }
    }
    put32(bytes+47,mask);return !PyErr_Occurred();
}
static PyObject *encode_aircraft_impl(PyObject *self,PyObject *const *args,Py_ssize_t count,int direct){
    if(count!=7){PyErr_SetString(PyExc_TypeError,"aircraft encoder requires seven arguments");return NULL;}
    if(!PyLong_CheckExact(args[0])||!PyLong_CheckExact(args[1])||!PyLong_CheckExact(args[2])||
       !PyUnicode_CheckExact(args[3])||!PyUnicode_IS_ASCII(args[3])||PyUnicode_GET_LENGTH(args[3])!=6||
       !PyFloat_CheckExact(args[6]))return invalid();
    uint64_t seq=PyLong_AsUnsignedLongLong(args[0]),begin=PyLong_AsUnsignedLongLong(args[1]);
    uint64_t snapshot=PyLong_AsUnsignedLongLong(args[2]);
    double deadline=PyFloat_AS_DOUBLE(args[6]);
    if(PyErr_Occurred()||!snapshot||!isfinite(deadline)||deadline<=0)return invalid();
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return invalid();
    if((double)now.tv_sec+(double)now.tv_nsec*1e-9>=deadline){Py_RETURN_FALSE;}
    uint64_t mono=(uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;
    if(direct){
        unsigned char bytes[MAX_RECORD];unsigned used;
        if(!aircraft_body_direct(PyModule_GetState(self),args[3],args[4],args[5],snapshot,bytes,&used))return invalid();
        return finish(bytes,used-33,28,seq,begin,0,mono);
    }
    PyObject *rowargs[]={args[4],args[5]};
    PyObject *row=aircraft_row(self,rowargs,2);if(!row)return NULL;
    State *state=PyModule_GetState(self);
    if(PyObject_RichCompareBool(args[3],PyDict_GetItemWithError(row,state->air[0]),Py_EQ)!=1){Py_DECREF(row);return invalid();}
    unsigned char bytes[MAX_RECORD];unsigned used=51;uint32_t mask=0;
    put64(bytes+33,snapshot);memcpy(bytes+41,PyUnicode_1BYTE_DATA(args[3]),6);
    for(int i=1;i<19;++i){
        PyObject *value=PyDict_GetItemWithError(row,state->air[i]);if(!value)continue;
        mask|=1U<<(i-1);
        if(PyLong_CheckExact(value)){
            bytes[used++]=0;put64(bytes+used,(uint64_t)PyLong_AsLongLong(value));
        }else{
            bytes[used++]=1;double n=PyFloat_AS_DOUBLE(value);uint64_t bits;
            memcpy(&bits,&n,8);put64(bytes+used,bits);
        }
        used+=8;
    }
    PyObject *tracking=PyDict_GetItemWithError(row,state->air[19]);
    if(tracking){
        mask|=1U<<18;Py_ssize_t size=PyTuple_GET_SIZE(tracking);
        put32(bytes+used,(uint32_t)size);used+=4;
        for(Py_ssize_t i=0;i<size;++i){
            put32(bytes+used,(uint32_t)PyLong_AsUnsignedLong(PyTuple_GET_ITEM(tracking,i)));used+=4;
        }
    }
    put32(bytes+47,mask);Py_DECREF(row);
    if(PyErr_Occurred()||used>MAX_RECORD)return invalid();
    return finish(bytes,used-33,28,seq,begin,0,mono);
}
static PyObject *encode_aircraft(PyObject *self,PyObject *const *args,Py_ssize_t count){return encode_aircraft_impl(self,args,count,0);}
/* GIL-serialized byte-bounded queue. Operations never release the GIL or call
 * user code: exact bytes only. No OS lock, no producer wait, no partial counters.
 * At least33bytes per entry also bounds list/object overhead independently. */
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000 && !defined(MLAT_DISABLE_IDENTITY_CACHE)
#define IDENTITY_CACHE_SIZE 4096
typedef struct {PyObject *map,*receiver;uint64_t version;uint32_t generation;} IdentityCacheEntry;
/* CPython3.12 is the pinned deployment runtime. The deprecated version field
 * is deliberately isolated here; other runtimes use the original lookup.
 * Zero disables caching. Borrowed pointers are ONLY compared, never dereferenced
 * after a dictionary change; the live exact dictionary owns each tuple/receiver.
 * No strong references/cycles, callbacks, GIL release or unbounded allocation. */
static uint64_t dictionary_version(PyObject *dictionary){
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    uint64_t version=((PyDictObject *)dictionary)->ma_version_tag;
#pragma GCC diagnostic pop
    return version;
}
#endif
typedef struct {
    PyObject_HEAD
    unsigned char *buffer,*sizes;
    Py_ssize_t count;
    Py_ssize_t limit,record_limit,queued,peak;
    uint64_t submitted;
    int active;
#ifdef IDENTITY_CACHE_SIZE
    IdentityCacheEntry identities[IDENTITY_CACHE_SIZE];
#endif
} CaptureQueue;
static int queue_init(CaptureQueue *q,PyObject *args,PyObject *kwargs){
    if(kwargs&&PyDict_Size(kwargs)){PyErr_SetString(PyExc_TypeError,"queue expects positional limits");return -1;}
    Py_ssize_t limit,record;
    if(q->buffer){PyErr_SetString(PyExc_ValueError,"queue already initialized");return -1;}
    if(!PyArg_ParseTuple(args,"nn",&limit,&record))return -1;
    if(record<256||record>65536||limit<record||limit>8*1024*1024){PyErr_SetString(PyExc_ValueError,"queue bounds");return -1;}
    /* Fixed byte arena plus <=4/33 bytes of boundary metadata per payload byte.
     * Queue accepts opaque bytes; never infer boundaries from untrusted headers. */
    q->buffer=PyMem_Malloc((size_t)limit);q->sizes=PyMem_Malloc((size_t)(limit/33)*4);
    if(!q->buffer||!q->sizes){PyMem_Free(q->buffer);PyMem_Free(q->sizes);q->buffer=q->sizes=NULL;PyErr_NoMemory();return -1;}
    q->limit=limit;q->record_limit=record;q->active=1;return 0;
}
static void queue_free(CaptureQueue *q){PyTypeObject *type=Py_TYPE(q);PyMem_Free(q->buffer);PyMem_Free(q->sizes);type->tp_free((PyObject *)q);Py_DECREF(type);}
static PyObject *queue_put_raw(CaptureQueue *q,const char *record,Py_ssize_t size){
    if(!q->active)return PyLong_FromLong(0);
    if(size>q->record_limit)return PyLong_FromLong(3);
    if(size>q->limit-q->queued)return PyLong_FromLong(2);
    if(q->submitted==UINT64_MAX){PyErr_SetString(PyExc_OverflowError,"capture sequence limit");return NULL;}
    memcpy(q->buffer+q->queued,record,(size_t)size);put32(q->sizes+4*q->count,(uint32_t)size);++q->count;
    q->queued+=size;if(q->queued>q->peak)q->peak=q->queued;++q->submitted;
    return PyLong_FromLong(1);
}
static PyObject *queue_put(CaptureQueue *q,PyObject *record){
    if(!q->active)return PyLong_FromLong(0);
    if(!PyBytes_CheckExact(record)||PyBytes_GET_SIZE(record)<33){PyErr_SetString(PyExc_ValueError,"queue requires encoded bytes");return NULL;}
    return queue_put_raw(q,PyBytes_AS_STRING(record),PyBytes_GET_SIZE(record));
}
static PyObject *queue_take(CaptureQueue *q,PyObject *unused){
    (void)unused;
    if(!q->buffer){PyErr_SetString(PyExc_ValueError,"uninitialized queue");return NULL;}
    /* Allocate GC-tracked container BEFORE reading queue state. Following bytes
     * allocations/list growth cannot invoke Python GC callbacks/reenter queue. */
    PyObject *result=PyList_New(0);if(!result)return NULL;
    Py_ssize_t offset=0;
    for(Py_ssize_t i=0;i<q->count;++i){
        const unsigned char *p=q->sizes+4*i;uint32_t size=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        PyObject *record=PyBytes_FromStringAndSize((const char *)q->buffer+offset,size);
        if(!record){Py_DECREF(result);return NULL;}
        int ok=PyList_Append(result,record);Py_DECREF(record);if(ok<0){Py_DECREF(result);return NULL;}
        offset+=size;
    }
    q->queued=q->count=0;return result;
}
static PyObject *queue_take_packed(CaptureQueue *q,PyObject *unused){
    (void)unused;if(!q->buffer){PyErr_SetString(PyExc_ValueError,"uninitialized queue");return NULL;}
    PyObject *result=PyTuple_New(2);if(!result)return NULL;
    PyObject *data=PyBytes_FromStringAndSize((const char *)q->buffer,q->queued);
    PyObject *sizes=data?PyBytes_FromStringAndSize((const char *)q->sizes,4*q->count):NULL;
    if(!data||!sizes){Py_XDECREF(data);Py_XDECREF(sizes);Py_DECREF(result);return NULL;}
    PyTuple_SET_ITEM(result,0,data);PyTuple_SET_ITEM(result,1,sizes);q->queued=q->count=0;return result;
}
/* One GIL-held boundary: active receiver identity, encode, bounded enqueue.
 * Returns queue status0..3,4 deadline,5 unsupported input,6 lifecycle mismatch.
 * Never trusts a receiver's attributes or permits a recycled object identity. */
static PyObject *record_input(PyObject *self,PyObject *const *args,Py_ssize_t count){
    if(count!=6){PyErr_SetString(PyExc_TypeError,"native input requires six arguments");return NULL;}
    State *state=PyModule_GetState(self);
    if(!Py_IS_TYPE(args[0],(PyTypeObject *)state->queue_type))return invalid();
    CaptureQueue *queue=(CaptureQueue *)args[0];
    if(!queue->active)return PyLong_FromLong(0);
    if(!queue->buffer||!PyDict_CheckExact(args[1])||PyDict_Size(args[1])>4096)return invalid();
    unsigned long id=0;
#ifdef IDENTITY_CACHE_SIZE
    uint64_t version=dictionary_version(args[1]);
    uintptr_t pointer=(uintptr_t)args[4];
    IdentityCacheEntry *cached=&queue->identities[((pointer>>4)^(pointer>>16))&(IDENTITY_CACHE_SIZE-1)];
    if(version&&cached->version==version&&cached->map==args[1]&&cached->receiver==args[4])id=cached->generation;
    if(!id){
#endif
    PyObject *key=PyLong_FromVoidPtr(args[4]);if(!key)return NULL;
    PyObject *entry=PyDict_GetItemWithError(args[1],key);Py_DECREF(key);
    if(PyErr_Occurred())return NULL;
    if(!entry||!PyTuple_CheckExact(entry)||PyTuple_GET_SIZE(entry)!=2||PyTuple_GET_ITEM(entry,0)!=args[4])return PyLong_FromLong(6);
    PyObject *generation=PyTuple_GET_ITEM(entry,1);
    if(!PyLong_CheckExact(generation))return PyLong_FromLong(6);
    id=PyLong_AsUnsignedLong(generation);
    if(PyErr_Occurred()||id<1||id>65536){PyErr_Clear();return PyLong_FromLong(6);}
#ifdef IDENTITY_CACHE_SIZE
    if(version){cached->map=args[1];cached->receiver=args[4];cached->version=version;cached->generation=(uint32_t)id;}
    }
#endif
    if(!PyLong_CheckExact(args[2])||!PyFloat_CheckExact(args[3])||!PyDict_CheckExact(args[5]))return invalid();
    uint64_t begin=PyLong_AsUnsignedLongLong(args[2]);double deadline=PyFloat_AS_DOUBLE(args[3]);
    if(PyErr_Occurred()||!isfinite(deadline)||deadline<=0)return invalid();
    unsigned char inline_bytes[77];unsigned inline_size=0;
    PyObject *record=encode_input_raw(state,queue->submitted,begin,(uint32_t)id,args[5],deadline,inline_bytes,&inline_size);
    if(!record)return NULL;
    if(record==Py_False){Py_DECREF(record);return PyLong_FromLong(4);}
    if(record==Py_None){Py_DECREF(record);return PyLong_FromLong(5);}
    if(record==Py_True){Py_DECREF(record);return queue_put_raw(queue,(const char *)inline_bytes,inline_size);}
    PyObject *status=queue_put(queue,record);Py_DECREF(record);return status;
}
/* Bound with functools.partial, whose GC traversal owns the context/callback
 * cycle. Context is the exact Recorder's live __dict__, not cached deadlines.
 * Success stays native; every other status uses the existing Python fail-open
 * policy. No producer IO, no new allocation proportional to traffic. */
static PyObject *input_context(PyObject *self,PyObject *const *args,Py_ssize_t count){
    if(count!=5){PyErr_SetString(PyExc_TypeError,"input context requires five arguments");return NULL;}
    if(!PyDict_CheckExact(args[0])||!PyDict_CheckExact(args[1])||!PyCallable_Check(args[2]))return invalid();
    State *state=PyModule_GetState(self);PyObject *result=NULL;long status=-1;
#if defined(IDENTITY_CACHE_SIZE) && !defined(MLAT_DISABLE_CONTEXT_CACHE)
    uint64_t version=dictionary_version(args[0]);
    if(version&&state->input_dict==args[0]&&state->input_version==version){
        /* Pin before record_input can perform any callback-capable lookup.
         * No Python execution/GC allocation occurs before these increments. */
        PyObject *call[6]={Py_NewRef(state->input_queue),args[1],
            Py_NewRef(state->input_begin),Py_NewRef(state->input_deadline),args[3],args[4]};
        result=record_input(self,call,6);
        Py_DECREF(call[0]);Py_DECREF(call[2]);Py_DECREF(call[3]);
        if(result){status=PyLong_AsLong(result);Py_DECREF(result);}
        goto input_result;
    }
#endif
    PyObject *active=PyDict_GetItemWithError(args[0],state->input_names[0]);
    if(active==Py_False)status=0;
    else if(active==Py_True){
        /* Even an exact dict may contain callback-capable keys. Own each
         * value before another lookup can remove it and release its storage. */
        PyObject *call[6]={NULL,args[1],NULL,NULL,args[3],args[4]};
        call[0]=Py_XNewRef(PyDict_GetItemWithError(args[0],state->input_names[1]));
        if(!PyErr_Occurred())call[2]=Py_XNewRef(PyDict_GetItemWithError(args[0],state->input_names[2]));
        if(!PyErr_Occurred())call[3]=Py_XNewRef(PyDict_GetItemWithError(args[0],state->input_names[3]));
#if defined(IDENTITY_CACHE_SIZE) && !defined(MLAT_DISABLE_CONTEXT_CACHE)
        if(call[0]&&call[2]&&call[3]&&version&&version==dictionary_version(args[0])&&
           Py_IS_TYPE(call[0],(PyTypeObject *)state->queue_type)&&PyLong_CheckExact(call[2])&&
           PyFloat_CheckExact(call[3])&&PyDict_Size(args[0])<=256){
            /* Public API may receive custom comparison keys. Never cache such
             * dictionaries: repeated lookups must retain callback semantics. */
            Py_ssize_t pos=0;PyObject *key,*value;int plain=1;
            while(PyDict_Next(args[0],&pos,&key,&value)){
                if(!PyUnicode_CheckExact(key)){plain=0;break;}
            }
            if(plain){
                state->input_dict=args[0];state->input_version=version;
                state->input_queue=call[0];state->input_begin=call[2];state->input_deadline=call[3];
            }
        }
#endif
        if(call[0]&&call[2]&&call[3])result=record_input(self,call,6);
        Py_XDECREF(call[0]);Py_XDECREF(call[2]);Py_XDECREF(call[3]);
        if(result){status=PyLong_AsLong(result);Py_DECREF(result);}
    }
#if defined(IDENTITY_CACHE_SIZE) && !defined(MLAT_DISABLE_CONTEXT_CACHE)
input_result:
#endif
    if(PyErr_Occurred()){
        if(!PyErr_ExceptionMatches(PyExc_Exception))return NULL;
        PyErr_Clear();status=-1;
    }
    if(status==1){Py_RETURN_TRUE;}
    PyObject *code=PyLong_FromLong(status);if(!code)return NULL;
    PyObject *fallback[3]={code,args[3],args[4]};
    result=PyObject_Vectorcall(args[2],fallback,3,NULL);Py_DECREF(code);return result;
}
/* At most64 rows per call: no entire snapshot copy, no unbounded GIL-held
 * traversal. Mapping has exact builtin keys/values, so no Python callbacks can
 * mutate rows or the queue while this batch is encoded. */
static PyObject *record_aircraft_batch(PyObject *self,PyObject *const *args,Py_ssize_t count){
    if(count!=6){PyErr_SetString(PyExc_TypeError,"aircraft batch requires six arguments");return NULL;}
    State *state=PyModule_GetState(self);
    if(!Py_IS_TYPE(args[0],(PyTypeObject *)state->queue_type))return invalid();
    CaptureQueue *queue=(CaptureQueue *)args[0];
    if(!queue->active)return PyLong_FromLong(0);
    if(!queue->buffer||(!PyTuple_CheckExact(args[4])&&!PyList_CheckExact(args[4]))||
       !PyDict_CheckExact(args[5])||PyDict_Size(args[5])>4096)return invalid();
    Py_ssize_t size=PySequence_Size(args[4]);if(size<1||size>64)return invalid();
    Py_ssize_t pos=0;PyObject *key,*value;
    while(PyDict_Next(args[5],&pos,&key,&value)){
        if(!PyLong_CheckExact(key)||!PyLong_CheckExact(value))return invalid();
        long long uid=PyLong_AsLongLong(key),identity=PyLong_AsLongLong(value);
        if(PyErr_Occurred()||uid<0||identity<1||identity>65536)return invalid();
    }
    for(Py_ssize_t i=0;i<size;++i){
        PyObject *pair=PyList_CheckExact(args[4])?PyList_GET_ITEM(args[4],i):PyTuple_GET_ITEM(args[4],i);
        if(!PyTuple_CheckExact(pair)||PyTuple_GET_SIZE(pair)!=2)return invalid();
        PyObject *row=PyTuple_GET_ITEM(pair,1);
        if(!PyDict_CheckExact(row)||!PyUnicode_CheckExact(PyTuple_GET_ITEM(pair,0)))return invalid();
        Py_ssize_t rowpos=0;PyObject *rowkey,*rowvalue;
        while(PyDict_Next(row,&rowpos,&rowkey,&rowvalue)){
            if(!PyUnicode_CheckExact(rowkey))return invalid();
        }
        PyObject *sequence=PyLong_FromUnsignedLongLong(queue->submitted);if(!sequence)return NULL;
        PyObject *fields[]={sequence,args[1],args[3],PyTuple_GET_ITEM(pair,0),PyTuple_GET_ITEM(pair,1),args[5],args[2]};
#ifdef MLAT_LEGACY_AIRCRAFT_BATCH
        PyObject *record=encode_aircraft_impl(self,fields,7,0);Py_DECREF(sequence);
#else
        PyObject *record=encode_aircraft_impl(self,fields,7,1);Py_DECREF(sequence);
#endif
        if(!record)return NULL;
        if(record==Py_False){Py_DECREF(record);return PyLong_FromLong(4);}
        PyObject *status=queue_put(queue,record);Py_DECREF(record);
        if(!status)return NULL;
        if(PyLong_AsLong(status)!=1)return status;
        Py_DECREF(status);
    }
    return PyLong_FromLong(1);
}
/* Construct the same immutable allowlisted RNG keys as randomness.item_key.
 * Snapshot bounded source references before any attribute/identity callback. */
static PyObject *random_keys(PyObject *self,PyObject *const *args,Py_ssize_t count){
    if(count!=2){PyErr_SetString(PyExc_TypeError,"random keys requires two arguments");return NULL;}
    if(!PyTuple_CheckExact(args[0])&&!PyList_CheckExact(args[0]))return invalid();
    Py_ssize_t size=PySequence_Size(args[0]);if(size<0||size>4096)return invalid();
    PyObject *source=PySequence_Tuple(args[0]);if(!source)return NULL;
    PyObject *result=PyTuple_New(size);if(!result){Py_DECREF(source);return NULL;}
    State *state=PyModule_GetState(self);
    for(Py_ssize_t i=0;i<size;++i){
        PyObject *item=PyTuple_GET_ITEM(source,i),*aircraft=item,*identity=NULL,*address=NULL,*key=NULL;
        int pair=PyTuple_CheckExact(item);
        if(pair){
            if(PyTuple_GET_SIZE(item)!=4)goto bad_item;
            identity=PyObject_CallOneArg(args[1],PyTuple_GET_ITEM(item,1));if(!identity)goto bad_item;
            aircraft=PyTuple_GET_ITEM(item,2);
        }
        address=PyObject_GetAttr(aircraft,state->air[0]);if(!address)goto bad_item;
        if(pair){
            unsigned char checked[8];
            if(!tick(PyTuple_GET_ITEM(item,0),checked)||!tick(PyTuple_GET_ITEM(item,3),checked))goto bad_item;
            if(!PyLong_CheckExact(identity))goto bad_item;
            long long id=PyLong_AsLongLong(identity);if(PyErr_Occurred()||id<1||id>65536)goto bad_item;
        }
        if(!PyLong_CheckExact(address))goto bad_item;
        long long icao=PyLong_AsLongLong(address);if(PyErr_Occurred()||icao<0||icao>0xffffff)goto bad_item;
        key=pair?PyTuple_Pack(5,state->pair_tag,identity,address,PyTuple_GET_ITEM(item,0),PyTuple_GET_ITEM(item,3)):
                 PyTuple_Pack(2,state->air_tag,address);
        if(!key)goto bad_item;
        Py_XDECREF(identity);Py_DECREF(address);PyTuple_SET_ITEM(result,i,key);continue;
bad_item:
        Py_XDECREF(identity);Py_XDECREF(address);Py_XDECREF(key);
        Py_DECREF(source);Py_DECREF(result);return invalid();
    }
    Py_DECREF(source);return result;
}
static PyObject *client_row(PyObject *self,PyObject *value){
    if(!PyDict_CheckExact(value))return invalid();
    State *state=PyModule_GetState(self);PyObject *result=PyDict_New();if(!result)return NULL;
    /* Only these ten fields are looked up. Never enumerate/copy private extras. */
    for(int i=0;i<10;++i){
        PyObject *item=PyDict_GetItemWithError(value,state->status_names[i]);if(!item)goto bad;
        if(i<7){unsigned char checked[8];if(!tick(item,checked))goto bad;}
        else if(i==7){if(!PyBool_Check(item))goto bad;}
        else{
            if(!PyList_CheckExact(item)&&!PyTuple_CheckExact(item))goto bad;
            Py_ssize_t size=PySequence_Size(item);if(size<0||size>4096)goto bad;
            PyObject *copy=PyTuple_New(size);if(!copy)goto bad;
            for(Py_ssize_t j=0;j<size;++j){
                PyObject *text=PyList_CheckExact(item)?PyList_GET_ITEM(item,j):PyTuple_GET_ITEM(item,j);
                if(!PyUnicode_CheckExact(text)||!PyUnicode_IS_ASCII(text)||PyUnicode_GET_LENGTH(text)!=6){Py_DECREF(copy);goto bad;}
                const unsigned char *raw=PyUnicode_1BYTE_DATA(text);
                for(int k=0;k<6;++k)if(nibble(raw[k])<0){Py_DECREF(copy);goto bad;}
                PyTuple_SET_ITEM(copy,j,Py_NewRef(text));
            }
            int ok=PyDict_SetItem(result,state->status_names[i],copy);Py_DECREF(copy);
            if(ok<0)goto bad;
            continue;
        }
        if(PyDict_SetItem(result,state->status_names[i],item)<0)goto bad;
    }
    return result;
bad:Py_DECREF(result);return invalid();
}
/* At most27 bytes; exact primitive types prevent hash/comparison callbacks. */
static unsigned random_key_bytes(PyObject *key,unsigned char out[27]){
    if(!PyTuple_CheckExact(key))return 0;
    Py_ssize_t size=PyTuple_GET_SIZE(key);if(size!=2&&size!=5)return 0;
    PyObject *tag=PyTuple_GET_ITEM(key,0);if(!PyUnicode_CheckExact(tag))return 0;
    int pair=size==5;
    if(PyUnicode_CompareWithASCIIString(tag,pair?"pair":"aircraft")!=0)return 0;
    PyObject *address=PyTuple_GET_ITEM(key,pair?2:1);if(!PyLong_CheckExact(address))return 0;
    unsigned long icao=PyLong_AsUnsignedLong(address);if(PyErr_Occurred()||icao>0xffffff)return 0;
    out[0]=(unsigned char)pair;
    if(!pair){put32(out+1,(uint32_t)icao);return 5;}
    PyObject *receiver=PyTuple_GET_ITEM(key,1);if(!PyLong_CheckExact(receiver))return 0;
    unsigned long id=PyLong_AsUnsignedLong(receiver);if(PyErr_Occurred()||id<1||id>65536)return 0;
    put32(out+1,(uint32_t)id);put32(out+5,(uint32_t)icao);
    for(int i=0;i<2;++i){
        PyObject *value=PyTuple_GET_ITEM(key,3+i);unsigned offset=9+9*(unsigned)i;
        if(!tick(value,out+offset+1))return 0;
        if(PyLong_CheckExact(value)){out[offset]=0;put64(out+offset+1,(uint64_t)PyLong_AsLongLong(value));}
        else out[offset]=1;
    }
    return 27;
}
/* Aircraft-only identities are exact 24-bit integers. A bounded half-full
 * native table avoids a Python dict plus one integer allocation per aircraft.
 * Pair identities retain Python numeric equality/duplicate semantics below. */
#ifndef MLAT_LEGACY_RANDOM_TABLE
static int aircraft_choice(unsigned char *bytes,unsigned *used,PyObject *population,PyObject *selected){
    Py_ssize_t n=PyTuple_GET_SIZE(population),m=PyTuple_GET_SIZE(selected);
    uint16_t table[8192]={0};unsigned char chosen[512]={0};
    for(Py_ssize_t i=0;i<n;++i){
        unsigned char data[27];if(random_key_bytes(PyTuple_GET_ITEM(population,i),data)!=5)return 0;
        uint32_t key=(uint32_t)data[1]|(uint32_t)data[2]<<8|(uint32_t)data[3]<<16;
        unsigned slot=(key*UINT32_C(2654435761))>>19;
        while(table[slot]){
            unsigned position=(unsigned)table[slot]-1;
            if(!memcmp(bytes+39+position*5,data,5))return 0;
            slot=(slot+1)&8191;
        }
        table[slot]=(uint16_t)(i+1);
        if(!append(bytes,used,(const char *)data,5))return 0;
    }
    for(Py_ssize_t i=0;i<m;++i){
        unsigned char data[27];if(random_key_bytes(PyTuple_GET_ITEM(selected,i),data)!=5)return 0;
        uint32_t key=(uint32_t)data[1]|(uint32_t)data[2]<<8|(uint32_t)data[3]<<16;
        unsigned slot=(key*UINT32_C(2654435761))>>19;
        while(table[slot]&&memcmp(bytes+39+((unsigned)table[slot]-1)*5,data,5))slot=(slot+1)&8191;
        if(!table[slot])return 0;
        unsigned position=(unsigned)table[slot]-1,byte=position/8,mask=1U<<(position%8);
        if(chosen[byte]&mask)return 0;
        chosen[byte]|=(unsigned char)mask;
        unsigned char encoded[2]={(unsigned char)position,(unsigned char)(position>>8)};
        if(!append(bytes,used,(const char *)encoded,2))return 0;
    }
    return 1;
}
#endif
static PyObject *encode_random_choice(PyObject *self,PyObject *const *args,Py_ssize_t count){
    (void)self;
    if(count!=7){PyErr_SetString(PyExc_TypeError,"choice encoder requires seven arguments");return NULL;}
    if(!PyLong_CheckExact(args[0])||!PyLong_CheckExact(args[1])||!PyUnicode_CheckExact(args[2])||
       !PyUnicode_CheckExact(args[3])||!PyTuple_CheckExact(args[4])||!PyTuple_CheckExact(args[5])||
       !PyFloat_CheckExact(args[6]))return invalid();
    uint64_t seq=PyLong_AsUnsignedLongLong(args[0]),begin=PyLong_AsUnsignedLongLong(args[1]);
    double deadline=PyFloat_AS_DOUBLE(args[6]);if(PyErr_Occurred()||!isfinite(deadline)||deadline<=0)return invalid();
    int stream,op;
    if(PyUnicode_CompareWithASCIIString(args[2],"tracker")==0)stream=0;
    else if(PyUnicode_CompareWithASCIIString(args[2],"coordinator")==0)stream=1;else return invalid();
    if(PyUnicode_CompareWithASCIIString(args[3],"sample")==0)op=0;
    else if(PyUnicode_CompareWithASCIIString(args[3],"shuffle")==0)op=1;else return invalid();
    Py_ssize_t n=PyTuple_GET_SIZE(args[4]),selected=PyTuple_GET_SIZE(args[5]);
    if(n>4096||selected>n||(op&&selected!=n))return invalid();
    struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now))return invalid();
    if((double)now.tv_sec+(double)now.tv_nsec*1e-9>=deadline){Py_RETURN_FALSE;}
    uint64_t mono=(uint64_t)now.tv_sec*1000000000ULL+(uint64_t)now.tv_nsec;
    unsigned char bytes[MAX_RECORD];unsigned used=39;
    bytes[33]=(unsigned char)stream;bytes[34]=(unsigned char)op;
    bytes[35]=(unsigned char)n;bytes[36]=(unsigned char)(n>>8);
    bytes[37]=(unsigned char)selected;bytes[38]=(unsigned char)(selected>>8);
#ifndef MLAT_LEGACY_RANDOM_TABLE
    int aircraft_only=1;
    for(Py_ssize_t i=0;i<n;++i){
        PyObject *key=PyTuple_GET_ITEM(args[4],i);
        if(!PyTuple_CheckExact(key)||PyTuple_GET_SIZE(key)!=2){aircraft_only=0;break;}
    }
    if(aircraft_only){
        if(!aircraft_choice(bytes,&used,args[4],args[5]))return invalid();
        return finish(bytes,used-33,29,seq,begin,0,mono);
    }
#endif
    /* Keep encoded population offsets and selected membership in bounded
     * native scratch. Reuse bytes already validated above rather than encode
     * each selected population key twice or allocate a Python index set. */
    unsigned offsets[4096];unsigned char chosen[512]={0};
    PyObject *mapping=PyDict_New();
    if(!mapping)goto bad;
    for(Py_ssize_t i=0;i<n;++i){
        PyObject *key=PyTuple_GET_ITEM(args[4],i);unsigned char data[27];
        offsets[i]=used;
        unsigned size=random_key_bytes(key,data);if(!size||!append(bytes,&used,(const char *)data,size))goto bad;
        PyObject *index=PyLong_FromSsize_t(i);if(!index)goto bad;
        int ok=PyDict_SetItem(mapping,key,index);Py_DECREF(index);if(ok<0||PyDict_Size(mapping)!=i+1)goto bad;
    }
    for(Py_ssize_t i=0;i<selected;++i){
        PyObject *key=PyTuple_GET_ITEM(args[5],i);unsigned char data[27];
        unsigned size=random_key_bytes(key,data);if(!size)goto bad;
        PyObject *index=PyDict_GetItemWithError(mapping,key);if(!index)goto bad;
        Py_ssize_t position=PyLong_AsSsize_t(index);
        if(position<0||position>=n)goto bad;
        const unsigned char *original=bytes+offsets[position];
        unsigned original_size=original[0]?27:5;
        unsigned byte=(unsigned)position/8,mask=1U<<((unsigned)position%8);
        if(size!=original_size||memcmp(data,original,size)||(chosen[byte]&mask))goto bad;
        chosen[byte]|=(unsigned char)mask;
        unsigned char encoded[2]={(unsigned char)position,(unsigned char)(position>>8)};
        if(!append(bytes,&used,(const char *)encoded,2))goto bad;
    }
    Py_DECREF(mapping);return finish(bytes,used-33,29,seq,begin,0,mono);
bad:Py_XDECREF(mapping);return invalid();
}
static PyObject *sync_row(PyObject *self,PyObject *const *args,Py_ssize_t count){
    if(count!=2){PyErr_SetString(PyExc_TypeError,"sync row requires two arguments");return NULL;}
    PyObject *value=args[0],*mapper=args[1];
    if(!PyDict_CheckExact(value)||PyDict_Size(value)!=4||!PyCallable_Check(mapper))return invalid();
    State *state=PyModule_GetState(self);
    PyObject *result=PyDict_New(),*source=NULL,*rows=NULL;
    if(!result)return NULL;
    const int indices[]={11,0,1};
    for(int j=0;j<3;++j){
        int index=indices[j];PyObject *item=PyDict_GetItemWithError(value,state->status_names[index]);
        if(!item)goto bad;
        if(j==0||item!=Py_None){unsigned char checked[8];if(!tick(item,checked))goto bad;}
        if(PyDict_SetItem(result,state->status_names[index],item)<0)goto bad;
    }
    PyObject *peers=PyDict_GetItemWithError(value,state->status_names[10]);
    if(!peers||!PyDict_CheckExact(peers)||PyDict_Size(peers)>4096)goto bad;
    /* Bounded owned pairs protect references across arbitrary mapper callbacks. */
    source=PyDict_Items(peers);if(!source)goto bad;
    Py_ssize_t size=PyList_GET_SIZE(source);rows=PyTuple_New(size);if(!rows)goto bad;
    for(Py_ssize_t i=0;i<size;++i){
        PyObject *entry=PyList_GET_ITEM(source,i),*values=PyTuple_GET_ITEM(entry,1);
        if((!PyList_CheckExact(values)&&!PyTuple_CheckExact(values))||PySequence_Size(values)!=8)goto bad;
        PyObject *numbers=PyTuple_New(8);if(!numbers)goto bad;
        for(int j=0;j<8;++j){
            PyObject *item=PyList_CheckExact(values)?PyList_GET_ITEM(values,j):PyTuple_GET_ITEM(values,j);
            if(j==4&&PyBool_Check(item)){
                PyObject *n=PyLong_FromLong(item==Py_True);if(!n){Py_DECREF(numbers);goto bad;}
                PyTuple_SET_ITEM(numbers,j,n);
            }else{
                unsigned char checked[8];if(!tick(item,checked)){Py_DECREF(numbers);goto bad;}
                PyTuple_SET_ITEM(numbers,j,Py_NewRef(item));
            }
        }
        PyObject *identity=PyObject_CallOneArg(mapper,PyTuple_GET_ITEM(entry,0));
        if(!identity){Py_DECREF(numbers);goto bad;}
        long long id=PyLong_CheckExact(identity)?PyLong_AsLongLong(identity):0;
        if(PyErr_Occurred()||id<1||id>65536){Py_DECREF(numbers);Py_DECREF(identity);goto bad;}
        PyObject *row=PyTuple_Pack(2,identity,numbers);Py_DECREF(identity);Py_DECREF(numbers);
        if(!row)goto bad;
        PyTuple_SET_ITEM(rows,i,row);
    }
    if(PyDict_SetItem(result,state->status_names[10],rows)<0)goto bad;
    Py_DECREF(rows);Py_DECREF(source);return result;
bad:Py_XDECREF(rows);Py_XDECREF(source);Py_DECREF(result);return invalid();
}
/* Native equivalent of hook.emit/emit_coordinator. Keep owned references across
 * arbitrary observer callbacks; ordinary observer failures cannot break service.
 * BaseException is not swallowed, matching Python's except Exception boundary. */
static PyObject *emit_common(PyObject *self,PyObject *const *args,Py_ssize_t count,int client){
    if(count<2){PyErr_SetString(PyExc_TypeError,"emit requires target and action");return NULL;}
    State *state=PyModule_GetState(self);
    PyObject *co=NULL,*hook=NULL,*handler=NULL,*result=NULL,*rec=NULL;
    co=client?PyObject_GetAttr(args[0],state->emit_names[0]):Py_NewRef(args[0]);
    if(!co)goto failed;
    hook=PyObject_GetAttr(co,state->emit_names[1]);
    if(!hook&&PyErr_ExceptionMatches(PyExc_AttributeError)){PyErr_Clear();goto done;}
    if(!hook)goto failed;
    if(hook==Py_None)goto done;
    handler=PyObject_GetAttr(hook,args[1]);if(!handler)goto failed;
    result=PyObject_Vectorcall(handler,args+2,(size_t)(count-2),NULL);
    if(result)goto done;
failed:
    if(!PyErr_ExceptionMatches(PyExc_Exception))goto done;
    PyErr_Clear();
    if(hook&&hook!=Py_None){
        rec=PyObject_GetAttr(hook,state->emit_names[2]);
        if(rec)result=PyObject_CallMethodObjArgs(rec,state->emit_names[3],state->emit_names[5],NULL);
        if(result){Py_CLEAR(result);result=PyObject_CallMethodObjArgs(hook,state->emit_names[4],NULL);}
        if(PyErr_ExceptionMatches(PyExc_Exception))PyErr_Clear();
    }
done:
    Py_XDECREF(result);Py_XDECREF(handler);Py_XDECREF(rec);Py_XDECREF(hook);Py_XDECREF(co);
    if(PyErr_Occurred())return NULL;
    Py_RETURN_NONE;
}
static PyObject *emit_client(PyObject *self,PyObject *const *args,Py_ssize_t count){return emit_common(self,args,count,1);}
static PyObject *emit_coordinator(PyObject *self,PyObject *const *args,Py_ssize_t count){return emit_common(self,args,count,0);}
static PyObject *queue_stop(CaptureQueue *q,PyObject *unused){(void)unused;q->active=0;Py_RETURN_NONE;}
static PyObject *queue_submitted(CaptureQueue *q,void *unused){(void)unused;return PyLong_FromUnsignedLongLong(q->submitted);}
static PyObject *queue_bytes(CaptureQueue *q,void *unused){(void)unused;return PyLong_FromSsize_t(q->queued);}
static PyObject *queue_peak(CaptureQueue *q,void *unused){(void)unused;return PyLong_FromSsize_t(q->peak);}
#define QUEUE_METHOD(function) (PyCFunction)(void(*)(void))(function)
static PyMethodDef queue_methods[]={
    {"put",QUEUE_METHOD(queue_put),METH_O,"1 accepted,0 stopped,2 byte cap,3 record cap."},
    {"take",QUEUE_METHOD(queue_take),METH_NOARGS,"Atomically detach bounded records in order."},
    {"take_packed",QUEUE_METHOD(queue_take_packed),METH_NOARGS,"Detach concatenated bytes and little-endian uint32 record lengths."},
    {"stop",QUEUE_METHOD(queue_stop),METH_NOARGS,"Disable producer; pending records remain drainable."},
    {NULL,NULL,0,NULL}};
static PyGetSetDef queue_getters[]={
    {"submitted",(getter)queue_submitted,NULL,NULL,NULL},
    {"queued_bytes",(getter)queue_bytes,NULL,NULL,NULL},
    {"peak",(getter)queue_peak,NULL,NULL,NULL},
    {NULL,NULL,NULL,NULL,NULL}};
static PyType_Slot queue_slots[]={
    {Py_tp_new,PyType_GenericNew},{Py_tp_init,queue_init},{Py_tp_dealloc,queue_free},
    {Py_tp_methods,queue_methods},{Py_tp_getset,queue_getters},{0,NULL}};
static PyType_Spec queue_spec={"_mlat_capture_native.Queue",sizeof(CaptureQueue),0,Py_TPFLAGS_DEFAULT,queue_slots};
static PyMethodDef methods[]={
    {"encode_input",(PyCFunction)(void(*)(void))encode_input,METH_FASTCALL,"Bounded ingress encoder; False means deadline, None delegates unsupported input."},
    {"aircraft_row",(PyCFunction)(void(*)(void))aircraft_row,METH_FASTCALL,"Strict sanitized aircraft status row."},
    {"encode_aircraft",(PyCFunction)(void(*)(void))encode_aircraft,METH_FASTCALL,"Bounded binary aircraft status envelope."},
    {"record_input",(PyCFunction)(void(*)(void))record_input,METH_FASTCALL,"Fused active identity, ingress encoding and bounded enqueue."},
    {"input_context",(PyCFunction)(void(*)(void))input_context,METH_FASTCALL,"Bound live recorder context; Python policy for every non-success."},
    {"record_aircraft_batch",(PyCFunction)(void(*)(void))record_aircraft_batch,METH_FASTCALL,"Bounded64-row status batch; same queue caps and individual envelopes."},
    {"random_keys",(PyCFunction)(void(*)(void))random_keys,METH_FASTCALL,"Bounded immutable allowlisted random-decision keys."},
    {"client_row",(PyCFunction)client_row,METH_O,"Allowlisted client status row; private extras never copied."},
    {"encode_random_choice",(PyCFunction)(void(*)(void))encode_random_choice,METH_FASTCALL,"Lossless bounded RNG population plus selection indices."},
    {"sync_row",(PyCFunction)(void(*)(void))sync_row,METH_FASTCALL,"Bounded privacy-sanitized clock peer status row."},
    {"emit",(PyCFunction)(void(*)(void))emit_client,METH_FASTCALL,"Fault-isolated client capture observer dispatch."},
    {"emit_coordinator",(PyCFunction)(void(*)(void))emit_coordinator,METH_FASTCALL,"Fault-isolated coordinator capture observer dispatch."},
    {NULL,NULL,0,NULL}};
static void release(void *module){State *state=PyModule_GetState(module);for(int i=0;i<KEY_COUNT;++i)Py_CLEAR(state->keys[i]);for(int i=0;i<AIR_COUNT;++i)Py_CLEAR(state->air[i]);for(int i=0;i<6;++i)Py_CLEAR(state->emit_names[i]);for(int i=0;i<12;++i)Py_CLEAR(state->status_names[i]);for(int i=0;i<4;++i)Py_CLEAR(state->input_names[i]);Py_CLEAR(state->air_allowed);Py_CLEAR(state->queue_type);Py_CLEAR(state->air_tag);Py_CLEAR(state->pair_tag);}
static struct PyModuleDef module={PyModuleDef_HEAD_INIT,"_mlat_capture_native",NULL,sizeof(State),methods,NULL,NULL,NULL,release};
PyMODINIT_FUNC PyInit__mlat_capture_native(void){
    PyObject *result=PyModule_Create(&module);if(!result)return NULL;
    State *state=PyModule_GetState(result);
    const char *input_names[]={"active","_native_queue","started_mono","deadline"};
    for(int i=0;i<4;++i){state->input_names[i]=PyUnicode_InternFromString(input_names[i]);if(!state->input_names[i]){Py_DECREF(result);return NULL;}}
    state->air_tag=PyUnicode_InternFromString("aircraft");state->pair_tag=PyUnicode_InternFromString("pair");
    if(!state->air_tag||!state->pair_tag){Py_DECREF(result);return NULL;}
    const char *emit_names[]={"coordinator","_mlat_capture","recorder","invalidate","clear","hook_error"};
    for(int i=0;i<6;++i){state->emit_names[i]=PyUnicode_InternFromString(emit_names[i]);if(!state->emit_names[i]){Py_DECREF(result);return NULL;}}
    const char *status_names[]={"lat","lon","alt","message_rate","peer_count","bad_sync_timeout","outlier_percent","privacy","sync_interest","mlat_interest","peers","bad_syncs"};
    for(int i=0;i<12;++i){state->status_names[i]=PyUnicode_InternFromString(status_names[i]);if(!state->status_names[i]){Py_DECREF(result);return NULL;}}
    const char *names[]={"sync","mlat","et","ot","em","om","t","m","seen","lost","input_connected","input_disconnected","clock_reset","clock_jump","heartbeat","rate_report"};
    for(int i=0;i<KEY_COUNT;++i){state->keys[i]=PyUnicode_InternFromString(names[i]);if(!state->keys[i]){Py_DECREF(result);return NULL;}}
    const char *air[]={"icao","elapsed_seen","interesting","allow_mlat","tracking","sync_interest","mlat_interest","adsb_seen","mlat_message_count","mlat_result_count","mlat_kalman_count","sync_count_1min","sync_bad_percent","last_result","lat","lon","alt","heading","speed","tracking_receivers"};
    state->air_allowed=PySet_New(NULL);if(!state->air_allowed){Py_DECREF(result);return NULL;}
    for(int i=0;i<AIR_COUNT;++i){state->air[i]=PyUnicode_InternFromString(air[i]);if(!state->air[i]||PySet_Add(state->air_allowed,state->air[i])<0){Py_DECREF(result);return NULL;}}
    PyObject *type=PyType_FromSpec(&queue_spec);if(!type){Py_DECREF(result);return NULL;}
    state->queue_type=Py_NewRef(type);
    if(PyModule_AddObject(result,"Queue",type)<0){Py_DECREF(type);Py_DECREF(result);return NULL;}
    return result;
}
