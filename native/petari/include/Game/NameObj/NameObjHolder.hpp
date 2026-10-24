#pragma once

#include "Game/Util/Array.hpp"
#include <revolution.h>

class NameObj;

typedef void (NameObj::*NameObjMethod)(void);
typedef void (NameObj::*NameObjMethodConst)(void) const;

class NameObjHolder {
public:
    NameObjHolder(int num);

    void add(NameObj* pObj);
    void suspendAllObj();
    void resumeAllObj();
    void syncWithFlags();
    void callMethodAllObj(NameObjMethod pMethod);
    void clearArray();
    NameObj* find(const char* pName);
#ifdef TARGET_PC
    // Nebula: each object of the holder in turn (tests).
    void forEach(void (*pFunc)(NameObj*, void*), void* pData);
#endif

private:
    /* 0x00 */ MR::Vector< MR::AssignableArray< NameObj* > > mObjArray1;
    /* 0x0C */ MR::Vector< MR::FixedArray< NameObj*, 16 > > mObjArray2;
};
