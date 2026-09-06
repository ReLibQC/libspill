#include <cstdio>
#include <vector>
#include "libspill.hpp"
int main(){
    std::vector<double> a{1,2,3,4}, b(4);
    { ls::store s{"consumer_cxx"};
      s.write<double>("k",0,a); s.read<double>("k",0,b); }
    bool ok = a==b;
    std::printf("  [%s] C++ consumer via find_package\n", ok?"PASS":"FAIL");
    return ok?0:1;
}
