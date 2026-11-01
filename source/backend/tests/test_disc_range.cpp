#include "disc_range.hpp"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
using namespace disc_range;
static void yes(std::string h,uint64_t n,uint64_t a,uint64_t b){auto r=parse(h,n);assert(r.valid&&r.first==a&&r.last==b);}
static void no(std::string h,uint64_t n){assert(!parse(h,n).valid);}
int main(){
yes("bytes=0-",1152520091ULL,0,1152520090ULL);
yes("bytes=1152519000-",1152520091ULL,1152519000ULL,1152520090ULL);
yes("bytes=-1024",1152520091ULL,1152519067ULL,1152520090ULL);
yes("bytes=10-20",100,10,20);
yes("bytes=10-2000",100,10,99);
yes("bytes=0-0",100,0,0);
yes("bytes=-500",100,0,99);
yes("bytes=0-",1,0,0);
no("bytes=100-",100);no("bytes=20-10",100);no("bytes=0-10,11-20",100);no("bytes=-0",100);
no("bytes=0-",0);no("nobytes=0-",100);no("bytes=123abc-",100);
no("bytes=18446744073709551616-",100);no("bytes=0-18446744073709551616",100);
no("bytes=--",100);no("bytes=-",100);
std::puts("HTTP Range parser tests passed");
}
