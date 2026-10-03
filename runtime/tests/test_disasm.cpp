#include "psx_disasm.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
static void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(expr) require((expr), #expr, __LINE__)
static void check(unsigned word, unsigned pc, const char* expected) {
    char out[128];
    int n=psx_disasm_one(word, pc, out, sizeof(out));
    if (std::strcmp(out,expected)) {
        std::fprintf(stderr,"%08X: expected [%s], got [%s]\n",word,expected,out);
        CHECK(false);
    }
    CHECK(n == (int)std::strlen(expected));
}
int main() {
    check(0x012A4020,0,"ADD      $t0, $t1, $t2");
    check(0x012A0018,0,"MULT     $t1, $t2");
    check(0x012A001B,0,"DIVU     $t1, $t2");
    check(0x012A4004,0,"SLLV     $t0, $t2, $t1");
    check(0x0521FFFF,0x80001000,"BGEZ     $t1, 0x80001000");
    check(0x1920FFFE,0x80001000,"BLEZ     $t1, 0x80000FFC");
    check(0x112AFFFE,0x80001000,"BEQ      $t1, $t2, 0x80000FFC");
    check(0x08000000,0x8FFFFFFC,"J        0x90000000");
    check(0x3128FFFF,0,"ANDI     $t0, $t1, 0xFFFF");
    check(0x2528FFFF,0,"ADDIU    $t0, $t1, -1");
    check(0x8D28FFF8,0,"LW       $t0, -8($t1)");
    check(0xC923FFF8,0,"LWC2     $VZ1, -8($t1)");
    check(0x40086000,0,"MFC0     $t0, $12");
    check(0x42000010,0,"RFE");
    check(0x48086800,0,"MFC2     $t0, $SXY1");
    check(0x4848D000,0,"CFC2     $t0, $H");
    check(0x4A080001,0,"RTPS     0x0080001");
    check(0x48200001,0,".word 0x48200001");
    check(0xFC000000,0,".word 0xFC000000");
    char tiny[4]={'x','x','x','x'};
    CHECK(psx_disasm_one(0,0,tiny,4)==3 && tiny[3]==0);
    CHECK(psx_disasm_one(0,0,tiny,1)==0 && tiny[0]==0);
    CHECK(psx_disasm_one(0,0,nullptr,4)==0);
    tiny[0]='x'; CHECK(psx_disasm_one(0,0,tiny,0)==0 && tiny[0]=='x');
}
