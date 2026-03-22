/*
 * _fast.c — C acceleration for Pidgin.
 *
 * Hot paths:
 *   1. ratchet_from_random — derive + transform without Python Instruction objects
 *   2. encode_records — full schema encode with nested objects/arrays
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>

/* ================================================================
 * Dynamic buffer
 * ================================================================ */

typedef struct { unsigned char *d; Py_ssize_t len, cap; } Buf;

static int bi(Buf *b, Py_ssize_t c) {
    b->d = PyMem_Malloc(c); if (!b->d) { PyErr_NoMemory(); return -1; }
    b->len = 0; b->cap = c; return 0;
}
static void bf(Buf *b) { if (b->d) PyMem_Free(b->d); }
static int bg(Buf *b, Py_ssize_t n) {
    if (b->len + n <= b->cap) return 0;
    Py_ssize_t c = b->cap; while (c < b->len + n) c *= 2;
    unsigned char *p = PyMem_Realloc(b->d, c);
    if (!p) { PyErr_NoMemory(); return -1; } b->d = p; b->cap = c; return 0;
}
static int b1(Buf *b, unsigned char v) { if (bg(b,1)) return -1; b->d[b->len++]=v; return 0; }
static int bw(Buf *b, const void *s, Py_ssize_t n) { if (bg(b,n)) return -1; memcpy(b->d+b->len,s,n); b->len+=n; return 0; }
static int b2(Buf *b, unsigned short v) { unsigned char t[2]={(v>>8)&0xFF,v&0xFF}; return bw(b,t,2); }
static int bv(Buf *b, long long val) {
    unsigned long long v = (unsigned long long)((val<<1)^(val>>63));
    do { unsigned char c=v&0x7F; v>>=7; if(v)c|=0x80; if(b1(b,c))return -1; } while(v);
    return 0;
}
static int bd(Buf *b, double val) {
    unsigned char t[8]; unsigned char *p=(unsigned char*)&val;
#if defined(__BYTE_ORDER__)&&__BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    memcpy(t,p,8);
#else
    for(int i=0;i<8;i++)t[i]=p[7-i];
#endif
    return bw(b,t,8);
}

/* ================================================================
 * RATCHET — derive + transform in one C call
 * ================================================================ */
#define OP_XOR 0
#define OP_SWAP 1
#define OP_ROT 2
#define OP_ADD 3
#define OP_INV 4
#define N_OPS 5

static PyObject *
py_ratchet_from_random(PyObject *self, PyObject *args)
{
    Py_buffer db, rb; int opb, fwd;
    if (!PyArg_ParseTuple(args,"y*y*ii",&db,&rb,&opb,&fwd)) return NULL;
    unsigned char *data=db.buf; Py_ssize_t dl=db.len;
    const unsigned char *r=rb.buf; Py_ssize_t rl=rb.len;
    if (dl==0) { PyBuffer_Release(&db); PyBuffer_Release(&rb); Py_RETURN_NONE; }
    int nb=(int)((dl+63)/64); if(nb<1)nb=1;
    int ni=nb*opb; if(ni*4>rl)ni=(int)(rl/4);

    if (fwd) {
        for(int i=0;i<ni;i++){
            int off=i*4, p=r[off]%(int)dl, op=r[off+1]%N_OPS, pb=r[off+2], param;
            switch(op){case OP_SWAP:param=r[off+3]%(int)dl;break;case OP_ROT:param=pb%8;break;default:param=pb;}
            if(p>=(int)dl)continue;
            switch(op){
                case OP_XOR:data[p]^=(unsigned char)param;break;
                case OP_SWAP:if(param<(int)dl){unsigned char t=data[p];data[p]=data[param];data[param]=t;}break;
                case OP_ROT:data[p]=((data[p]<<param)|(data[p]>>(8-param)))&0xFF;break;
                case OP_ADD:data[p]=(data[p]+param)&0xFF;break;
                case OP_INV:data[p]^=0xFF;break;
            }
        }
    } else {
        for(int i=ni-1;i>=0;i--){
            int off=i*4, p=r[off]%(int)dl, op=r[off+1]%N_OPS, pb=r[off+2], param;
            switch(op){case OP_SWAP:param=r[off+3]%(int)dl;break;case OP_ROT:param=pb%8;break;default:param=pb;}
            if(p>=(int)dl)continue;
            switch(op){
                case OP_XOR:data[p]^=(unsigned char)param;break;
                case OP_SWAP:if(param<(int)dl){unsigned char t=data[p];data[p]=data[param];data[param]=t;}break;
                case OP_ROT:data[p]=((data[p]>>param)|(data[p]<<(8-param)))&0xFF;break;
                case OP_ADD:data[p]=(data[p]-param)&0xFF;break;
                case OP_INV:data[p]^=0xFF;break;
            }
        }
    }
    PyBuffer_Release(&db); PyBuffer_Release(&rb); Py_RETURN_NONE;
}

/* ================================================================
 * SCHEMA ENCODE — recursive, handles nested objects + arrays
 *
 * Schema args: (names: tuple, types: tuple, nullable: tuple,
 *               enum_maps: dict, sub_schemas: dict)
 *
 * Type codes: 0=bool 1=int 2=float 3=str 4=enum 5=timestamp
 *             6=object 7=array
 * ================================================================ */

#define T_BOOL 0
#define T_INT  1
#define T_FLT  2
#define T_STR  3
#define T_ENUM 4
#define T_TS   5
#define T_OBJ  6
#define T_ARR  7

#define M_NULL    0xFF
#define M_PRESENT 0x01
#define M_ABSENT  0x00

/* Forward declaration */
static int encode_one_record(Buf *b, PyObject *rec,
    PyObject *names, PyObject *types, PyObject *nullable,
    PyObject *emaps, PyObject *subs);

static int enc_val(Buf *b, PyObject *val, int ft, PyObject *fn,
                   PyObject *emaps, PyObject *subs)
{
    switch (ft) {
    case T_BOOL: return b1(b, PyObject_IsTrue(val)?1:0);
    case T_INT:  return bv(b, PyLong_AsLongLong(val));
    case T_FLT:  return bd(b, PyFloat_AsDouble(val));
    case T_STR: {
        Py_ssize_t sl; const char *s=PyUnicode_AsUTF8AndSize(val,&sl);
        if(!s)return -1; if(b2(b,(unsigned short)sl))return -1; return bw(b,s,sl);
    }
    case T_ENUM: {
        PyObject *el=PyDict_GetItem(emaps,fn);
        if(el){
            Py_ssize_t n=PyList_GET_SIZE(el);
            for(Py_ssize_t i=0;i<n;i++){
                if(PyUnicode_Compare(val,PyList_GET_ITEM(el,i))==0) return b1(b,(unsigned char)i);
            }
            if(b1(b,0xFE))return -1;
            Py_ssize_t sl; const char *s=PyUnicode_AsUTF8AndSize(val,&sl);
            if(!s)return -1; if(b2(b,(unsigned short)sl))return -1; return bw(b,s,sl);
        }
        Py_ssize_t sl; const char *s=PyUnicode_AsUTF8AndSize(val,&sl);
        if(!s)return -1; if(b2(b,(unsigned short)sl))return -1; return bw(b,s,sl);
    }
    case T_TS: {
        Py_ssize_t sl; const char *s=PyUnicode_AsUTF8AndSize(val,&sl);
        if(!s)return -1; if(b1(b,(unsigned char)sl))return -1; return bw(b,s,sl);
    }
    case T_OBJ: {
        /* Nested object — get sub-schema and recurse */
        PyObject *sub=PyDict_GetItem(subs,fn);
        if(!sub||!PyDict_Check(val)){if(b2(b,0))return -1; return 0;}
        PyObject *sn=PyTuple_GET_ITEM(sub,0), *st=PyTuple_GET_ITEM(sub,1),
                 *snl=PyTuple_GET_ITEM(sub,2), *sem=PyTuple_GET_ITEM(sub,3),
                 *ss=PyTuple_GET_ITEM(sub,4);
        return encode_one_record(b,val,sn,st,snl,sem,ss);
    }
    case T_ARR: {
        /* Array of objects — get sub-schema, encode count + each item */
        PyObject *sub=PyDict_GetItem(subs,fn);
        if(!sub||!PyList_Check(val)){if(b2(b,0))return -1; return 0;}
        Py_ssize_t cnt=PyList_GET_SIZE(val);
        if(b2(b,(unsigned short)cnt))return -1;
        PyObject *sn=PyTuple_GET_ITEM(sub,0), *st=PyTuple_GET_ITEM(sub,1),
                 *snl=PyTuple_GET_ITEM(sub,2), *sem=PyTuple_GET_ITEM(sub,3),
                 *ss=PyTuple_GET_ITEM(sub,4);
        for(Py_ssize_t i=0;i<cnt;i++){
            PyObject *item=PyList_GET_ITEM(val,i);
            if(PyDict_Check(item)){
                if(encode_one_record(b,item,sn,st,snl,sem,ss))return -1;
            } else {
                /* Primitive array item → JSON fallback */
                PyObject *jm=PyImport_ImportModule("json");
                PyObject *fn2=PyObject_GetAttrString(jm,"dumps"); Py_DECREF(jm);
                PyObject *sep=Py_BuildValue("(ss)",",",":");
                PyObject *js=PyObject_CallFunction(fn2,"OO",item,sep);
                Py_DECREF(fn2);Py_DECREF(sep);
                if(!js)return -1;
                Py_ssize_t sl; const char *s=PyUnicode_AsUTF8AndSize(js,&sl);
                int rc=0;
                if(!s||b2(b,(unsigned short)sl)||bw(b,s,sl))rc=-1;
                Py_DECREF(js); if(rc)return -1;
            }
        }
        return 0;
    }
    default: {
        /* JSON fallback */
        PyObject *jm=PyImport_ImportModule("json");
        PyObject *fn2=PyObject_GetAttrString(jm,"dumps"); Py_DECREF(jm);
        PyObject *sep=Py_BuildValue("(ss)",",",":");
        PyObject *js=PyObject_CallFunction(fn2,"OO",val,sep);
        Py_DECREF(fn2);Py_DECREF(sep);
        if(!js)return -1;
        Py_ssize_t sl; const char *s=PyUnicode_AsUTF8AndSize(js,&sl);
        int rc=0;
        if(!s||b2(b,(unsigned short)sl)||bw(b,s,sl))rc=-1;
        Py_DECREF(js); return rc;
    }
    }
}

static int encode_one_record(Buf *b, PyObject *rec,
    PyObject *names, PyObject *types, PyObject *nullable,
    PyObject *emaps, PyObject *subs)
{
    Py_ssize_t nf=PyTuple_GET_SIZE(names);
    for(Py_ssize_t f=0;f<nf;f++){
        PyObject *fn=PyTuple_GET_ITEM(names,f);
        int ft=(int)PyLong_AsLong(PyTuple_GET_ITEM(types,f));
        int nl=PyObject_IsTrue(PyTuple_GET_ITEM(nullable,f));

        if(nl){
            int has=PyDict_Contains(rec,fn);
            if(!has){if(b1(b,M_ABSENT))return -1; continue;}
            PyObject *val=PyDict_GetItem(rec,fn);
            if(val==Py_None){if(b1(b,M_NULL))return -1; continue;}
            if(b1(b,M_PRESENT))return -1;
            if(enc_val(b,val,ft,fn,emaps,subs))return -1;
        } else {
            PyObject *val=PyDict_GetItem(rec,fn);
            if(!val){PyErr_Format(PyExc_KeyError,"missing field: %S",fn); return -1;}
            if(enc_val(b,val,ft,fn,emaps,subs))return -1;
        }
    }
    /* Unknown fields fallback */
    PyObject *unk=PyDict_New(); if(!unk)return -1;
    PyObject *k,*v; Py_ssize_t pos=0;
    while(PyDict_Next(rec,&pos,&k,&v)){
        int found=0;
        for(Py_ssize_t f=0;f<nf;f++){
            if(PyUnicode_Compare(k,PyTuple_GET_ITEM(names,f))==0){found=1;break;}
        }
        if(!found)PyDict_SetItem(unk,k,v);
    }
    if(PyDict_GET_SIZE(unk)>0){
        PyObject *jm=PyImport_ImportModule("json");
        PyObject *fn=PyObject_GetAttrString(jm,"dumps"); Py_DECREF(jm);
        PyObject *sep=Py_BuildValue("(ss)",",",":");
        PyObject *kw=Py_BuildValue("{sO}","separators",sep);
        PyObject *a=Py_BuildValue("(O)",unk);
        PyObject *js=PyObject_Call(fn,a,kw);
        Py_DECREF(fn);Py_DECREF(sep);Py_DECREF(kw);Py_DECREF(a);Py_DECREF(unk);
        if(!js)return -1;
        Py_ssize_t sl; const char *s=PyUnicode_AsUTF8AndSize(js,&sl);
        int rc=0;
        if(!s||b2(b,(unsigned short)sl)||bw(b,s,sl))rc=-1;
        Py_DECREF(js); return rc;
    }
    Py_DECREF(unk);
    return b2(b,0);
}

static PyObject *
py_encode_records(PyObject *self, PyObject *args)
{
    PyObject *records, *names, *types, *nullable, *emaps, *subs;
    if (!PyArg_ParseTuple(args,"OOOOOO",
                          &records,&names,&types,&nullable,&emaps,&subs))
        return NULL;

    Py_ssize_t nr=PyList_GET_SIZE(records);
    Buf b; if(bi(&b,nr*64))return NULL;

    for(Py_ssize_t r=0;r<nr;r++){
        if(encode_one_record(&b,PyList_GET_ITEM(records,r),
                             names,types,nullable,emaps,subs)){
            bf(&b); return NULL;
        }
    }
    PyObject *res=PyBytes_FromStringAndSize((char*)b.d,b.len);
    bf(&b); return res;
}

/* ================================================================
 * SCHEMA DECODE — mirror of encode, recursive
 *
 * decode_records(data: bytes, offset: int, count: int,
 *                names, types, nullable, enum_maps, sub_schemas)
 * -> (list[dict], new_offset)
 * ================================================================ */

/* Read helpers */
static long long rd_varint(const unsigned char *d, Py_ssize_t dl, Py_ssize_t *p) {
    unsigned long long r=0; int sh=0;
    while(*p<dl){unsigned char c=d[(*p)++]; r|=(unsigned long long)(c&0x7F)<<sh; if(!(c&0x80))break; sh+=7;}
    return (long long)((r>>1)^-(long long)(r&1));
}
static unsigned short rd_u16be(const unsigned char *d, Py_ssize_t *p) {
    unsigned short v=((unsigned short)d[*p]<<8)|d[*p+1]; *p+=2; return v;
}

/* Forward declaration */
static PyObject *decode_one_record(const unsigned char *d, Py_ssize_t dl, Py_ssize_t *p,
    PyObject *names, PyObject *types, PyObject *nullable,
    PyObject *emaps, PyObject *subs);

static PyObject *dec_val(const unsigned char *d, Py_ssize_t dl, Py_ssize_t *p,
                         int ft, PyObject *fn, PyObject *emaps, PyObject *subs)
{
    switch(ft){
    case T_BOOL: { int v=d[(*p)++]; return PyBool_FromLong(v); }
    case T_INT:  { long long v=rd_varint(d,dl,p); return PyLong_FromLongLong(v); }
    case T_FLT:  {
        unsigned char t[8];
#if defined(__BYTE_ORDER__)&&__BYTE_ORDER__==__ORDER_BIG_ENDIAN__
        memcpy(t,d+*p,8);
#else
        for(int i=0;i<8;i++)t[i]=d[*p+7-i];
#endif
        *p+=8;
        double v; memcpy(&v,t,8);
        return PyFloat_FromDouble(v);
    }
    case T_STR: {
        unsigned short sl=rd_u16be(d,p);
        PyObject *s=PyUnicode_DecodeUTF8((const char*)(d+*p),sl,NULL);
        *p+=sl; return s;
    }
    case T_ENUM: {
        unsigned char idx=d[(*p)++];
        if(idx==0xFE){
            unsigned short sl=rd_u16be(d,p);
            PyObject *s=PyUnicode_DecodeUTF8((const char*)(d+*p),sl,NULL);
            *p+=sl; return s;
        }
        PyObject *el=PyDict_GetItem(emaps,fn);
        if(el && idx<PyList_GET_SIZE(el)){
            PyObject *v=PyList_GET_ITEM(el,idx);
            Py_INCREF(v); return v;
        }
        Py_RETURN_NONE;
    }
    case T_TS: {
        unsigned char sl=d[(*p)++];
        PyObject *s=PyUnicode_DecodeUTF8((const char*)(d+*p),sl,NULL);
        *p+=sl; return s;
    }
    case T_OBJ: {
        PyObject *sub=PyDict_GetItem(subs,fn);
        if(!sub){ Py_RETURN_NONE; }
        return decode_one_record(d,dl,p,
            PyTuple_GET_ITEM(sub,0),PyTuple_GET_ITEM(sub,1),
            PyTuple_GET_ITEM(sub,2),PyTuple_GET_ITEM(sub,3),PyTuple_GET_ITEM(sub,4));
    }
    case T_ARR: {
        PyObject *sub=PyDict_GetItem(subs,fn);
        if(!sub){ Py_RETURN_NONE; }
        unsigned short cnt=rd_u16be(d,p);
        PyObject *lst=PyList_New(cnt);
        if(!lst)return NULL;
        PyObject *sn=PyTuple_GET_ITEM(sub,0),*st=PyTuple_GET_ITEM(sub,1),
                 *snl=PyTuple_GET_ITEM(sub,2),*sem=PyTuple_GET_ITEM(sub,3),
                 *ss=PyTuple_GET_ITEM(sub,4);
        for(Py_ssize_t i=0;i<cnt;i++){
            PyObject *item=decode_one_record(d,dl,p,sn,st,snl,sem,ss);
            if(!item){Py_DECREF(lst);return NULL;}
            PyList_SET_ITEM(lst,i,item);
        }
        return lst;
    }
    default: {
        unsigned short sl=rd_u16be(d,p);
        PyObject *raw=PyUnicode_DecodeUTF8((const char*)(d+*p),sl,NULL);
        *p+=sl;
        if(!raw)return NULL;
        PyObject *jm=PyImport_ImportModule("json");
        PyObject *fn2=PyObject_GetAttrString(jm,"loads"); Py_DECREF(jm);
        PyObject *val=PyObject_CallFunctionObjArgs(fn2,raw,NULL);
        Py_DECREF(fn2); Py_DECREF(raw);
        return val;
    }
    }
}

static PyObject *decode_one_record(const unsigned char *d, Py_ssize_t dl, Py_ssize_t *p,
    PyObject *names, PyObject *types, PyObject *nullable,
    PyObject *emaps, PyObject *subs)
{
    PyObject *rec=PyDict_New();
    if(!rec)return NULL;
    Py_ssize_t nf=PyTuple_GET_SIZE(names);

    for(Py_ssize_t f=0;f<nf;f++){
        PyObject *fn=PyTuple_GET_ITEM(names,f);
        int ft=(int)PyLong_AsLong(PyTuple_GET_ITEM(types,f));
        int nl=PyObject_IsTrue(PyTuple_GET_ITEM(nullable,f));

        if(nl){
            unsigned char marker=d[(*p)++];
            if(marker==M_NULL){ PyDict_SetItem(rec,fn,Py_None); continue; }
            if(marker==M_ABSENT){ continue; }
            /* M_PRESENT — fall through to decode value */
        }
        PyObject *val=dec_val(d,dl,p,ft,fn,emaps,subs);
        if(!val){Py_DECREF(rec);return NULL;}
        PyDict_SetItem(rec,fn,val);
        Py_DECREF(val);
    }
    /* Unknown fields fallback */
    unsigned short fb_len=rd_u16be(d,p);
    if(fb_len>0){
        PyObject *raw=PyUnicode_DecodeUTF8((const char*)(d+*p),fb_len,NULL);
        *p+=fb_len;
        if(raw){
            PyObject *jm=PyImport_ImportModule("json");
            PyObject *fn2=PyObject_GetAttrString(jm,"loads"); Py_DECREF(jm);
            PyObject *fb=PyObject_CallFunctionObjArgs(fn2,raw,NULL);
            Py_DECREF(fn2); Py_DECREF(raw);
            if(fb && PyDict_Check(fb)){
                PyObject *k,*v; Py_ssize_t pos=0;
                while(PyDict_Next(fb,&pos,&k,&v)) PyDict_SetItem(rec,k,v);
                Py_DECREF(fb);
            }
        }
    }
    return rec;
}

static PyObject *
py_decode_records(PyObject *self, PyObject *args)
{
    const unsigned char *data; Py_ssize_t data_len;
    Py_ssize_t offset; int count;
    PyObject *names, *types, *nullable, *emaps, *subs;

    if (!PyArg_ParseTuple(args,"y#niOOOOO",
                          &data,&data_len,&offset,&count,
                          &names,&types,&nullable,&emaps,&subs))
        return NULL;

    PyObject *lst=PyList_New(count);
    if(!lst)return NULL;

    Py_ssize_t p=offset;
    for(int i=0;i<count;i++){
        PyObject *rec=decode_one_record(data,data_len,&p,names,types,nullable,emaps,subs);
        if(!rec){Py_DECREF(lst);return NULL;}
        PyList_SET_ITEM(lst,i,rec);
    }
    return Py_BuildValue("(Nn)",lst,p);
}

/* ================================================================
 * EXPAND KEY — HKDF-like key expansion in C using Python hmac
 *
 * expand_key(seed: bytes, length: int) -> bytes
 * ================================================================ */
static PyObject *
py_expand_key(PyObject *self, PyObject *args)
{
    const unsigned char *seed; Py_ssize_t seed_len;
    int length;
    if(!PyArg_ParseTuple(args,"y#i",&seed,&seed_len,&length)) return NULL;

    PyObject *hmac_mod=PyImport_ImportModule("hmac");
    if(!hmac_mod)return NULL;
    PyObject *hmac_new=PyObject_GetAttrString(hmac_mod,"new");
    Py_DECREF(hmac_mod);
    PyObject *hashlib=PyImport_ImportModule("hashlib");
    PyObject *sha256=PyObject_GetAttrString(hashlib,"sha256");
    Py_DECREF(hashlib);
    PyObject *seed_bytes=PyBytes_FromStringAndSize((const char*)seed,seed_len);

    unsigned char *result=PyMem_Malloc(length);
    if(!result){Py_DECREF(hmac_new);Py_DECREF(sha256);Py_DECREF(seed_bytes);return PyErr_NoMemory();}
    int pos=0;
    PyObject *block=PyBytes_FromStringAndSize("",0);
    int idx=0;

    while(pos<length){
        /* block = hmac.new(seed, block + struct.pack(">I", idx), hashlib.sha256).digest() */
        unsigned char idx_buf[4]={(idx>>24)&0xFF,(idx>>16)&0xFF,(idx>>8)&0xFF,idx&0xFF};
        PyObject *idx_bytes=PyBytes_FromStringAndSize((const char*)idx_buf,4);
        PyObject *msg=PyBytes_FromStringAndSize(NULL,PyBytes_GET_SIZE(block)+4);
        memcpy(PyBytes_AS_STRING(msg),PyBytes_AS_STRING(block),PyBytes_GET_SIZE(block));
        memcpy(PyBytes_AS_STRING(msg)+PyBytes_GET_SIZE(block),idx_buf,4);
        Py_DECREF(idx_bytes);

        PyObject *h=PyObject_CallFunctionObjArgs(hmac_new,seed_bytes,msg,sha256,NULL);
        Py_DECREF(msg); Py_DECREF(block);
        if(!h){PyMem_Free(result);Py_DECREF(hmac_new);Py_DECREF(sha256);Py_DECREF(seed_bytes);return NULL;}
        PyObject *digest=PyObject_CallMethod(h,"digest",NULL);
        Py_DECREF(h);
        if(!digest){PyMem_Free(result);Py_DECREF(hmac_new);Py_DECREF(sha256);Py_DECREF(seed_bytes);return NULL;}

        Py_ssize_t dlen=PyBytes_GET_SIZE(digest);
        Py_ssize_t copy=length-pos; if(copy>dlen)copy=dlen;
        memcpy(result+pos,PyBytes_AS_STRING(digest),copy);
        pos+=(int)copy;
        block=digest; /* takes ownership */
        idx++;
    }
    Py_DECREF(block);Py_DECREF(hmac_new);Py_DECREF(sha256);Py_DECREF(seed_bytes);

    PyObject *out=PyBytes_FromStringAndSize((const char*)result,length);
    PyMem_Free(result);
    return out;
}

/* ================================================================
 * Varint helpers
 * ================================================================ */
static PyObject *py_encode_varint(PyObject *s, PyObject *a) {
    long long v; if(!PyArg_ParseTuple(a,"L",&v))return NULL;
    unsigned long long u=(unsigned long long)((v<<1)^(v>>63));
    unsigned char buf[10]; int n=0;
    do{unsigned char c=u&0x7F;u>>=7;if(u)c|=0x80;buf[n++]=c;}while(u);
    return PyBytes_FromStringAndSize((char*)buf,n);
}
static PyObject *py_decode_varint(PyObject *s, PyObject *a) {
    const unsigned char *d; Py_ssize_t dl,pos;
    if(!PyArg_ParseTuple(a,"y#n",&d,&dl,&pos))return NULL;
    unsigned long long r=0; int sh=0;
    while(pos<dl){unsigned char c=d[pos++];r|=(unsigned long long)(c&0x7F)<<sh;if(!(c&0x80))break;sh+=7;}
    return Py_BuildValue("(Ln)",(long long)((r>>1)^-(long long)(r&1)),pos);
}

/* ================================================================
 * Module
 * ================================================================ */
static PyMethodDef methods[] = {
    {"ratchet_from_random", py_ratchet_from_random, METH_VARARGS, NULL},
    {"encode_records",      py_encode_records,      METH_VARARGS, NULL},
    {"decode_records",      py_decode_records,      METH_VARARGS, NULL},
    {"expand_key",          py_expand_key,          METH_VARARGS, NULL},
    {"encode_varint",       py_encode_varint,       METH_VARARGS, NULL},
    {"decode_varint",       py_decode_varint,       METH_VARARGS, NULL},
    {NULL,NULL,0,NULL}
};
static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,"_fast","C acceleration for Pidgin",-1,methods
};
PyMODINIT_FUNC PyInit__fast(void) { return PyModule_Create(&module); }
