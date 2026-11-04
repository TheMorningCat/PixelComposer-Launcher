#include "pxc_detect.h"
#include <iostream>
#include <filesystem>
#include <string>
#include <map>

namespace fs = std::filesystem;
using namespace pxc;

static const char* fmtStr(Format f){
    switch(f){case Format::JSON:return "JSON";case Format::ZLIB:return "ZLIB";
             case Format::PXCX:return "PXCX";default:return "UNK";}
}
static const char* confStr(Confidence c){
    switch(c){case Confidence::HIGH:return "HIGH";case Confidence::MEDIUM:return "MED";
             case Confidence::LOW:return "LOW";default:return "NONE";}
}

int main(int argc, char** argv){
    if(argc < 2){
        std::cerr << "[usage] detect_test.exe <samples_dir>\n"
                  << "  e.g. detect_test.exe \"..\\samples\"\n"
                  << "  (no arg -> scans current directory '.')\n";
    }
    std::string root = (argc>1) ? argv[1] : ".";
    int high=0, med=0, low=0, none=0, total=0;
    std::map<std::string,int> verCount;
    for(auto& e : fs::recursive_directory_iterator(root)){
        if(e.path().extension()==".pxc"){
            DetectionResult r = detectPxc(e.path().string());
            total++;
            std::string v = r.version.empty() ? "?" : r.version;
            std::cout << fmtStr(r.format) << "\t" << confStr(r.confidence) << "\t"
                      << v << "\t" << r.rawValue << "\t"
                      << e.path().filename().string()
                      << (r.note.empty()?"":"\t["+r.note+"]") << "\n";
            std::string c = confStr(r.confidence);
            if(c=="HIGH") high++; else if(c=="MED") med++;
            else if(c=="LOW") low++; else none++;
            verCount[v]++;
        }
    }
    std::cout << "\nTOTAL=" << total << "  HIGH=" << high
              << "  MED=" << med << "  LOW=" << low << "  NONE=" << none << "\n";
    std::cout << "versions detected:\n";
    for(auto& kv : verCount)
        std::cout << "  " << kv.first << " : " << kv.second << "\n";
    return 0;
}
