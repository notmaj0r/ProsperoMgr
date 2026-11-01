#pragma once
// Standalone single-range parser. Multi-ranges are not supported.
#include <cstdint>
#include <limits>
#include <string>
namespace disc_range {
struct Result { bool valid=false; uint64_t first=0; uint64_t last=0; };
inline bool uint_decimal(const std::string &s, uint64_t *out) {
    if(s.empty()) return false;
    uint64_t value=0;
    for(unsigned char c:s) {
        if(c<'0'||c>'9')return false;
        unsigned d=static_cast<unsigned>(c-'0');
        if(value>(std::numeric_limits<uint64_t>::max()-d)/10)return false;
        value=value*10+d;
    }
    *out=value;
    return true;
}
inline Result parse(const std::string &header,uint64_t size){
    Result out;
    if(header.compare(0,6,"bytes=")!=0||size==0)return out;
    const std::string span=header.substr(6);
    if(span.find(',')!=std::string::npos)return out;
    auto dash=span.find('-');
    if(dash==std::string::npos)return out;
    const std::string left=span.substr(0,dash),right=span.substr(dash+1);
    uint64_t a=0,b=0;
    if(left.empty()){
        if(!uint_decimal(right,&b)||b==0)return out;
        out.first=b>=size?0:size-b; out.last=size-1;
    }else{
        if(!uint_decimal(left,&a)||a>=size)return out;
        if(right.empty())b=size-1;
        else if(!uint_decimal(right,&b)||b<a)return out;
        if(b>=size)b=size-1;
        out.first=a;out.last=b;
    }
    out.valid=true;return out;
}
}
