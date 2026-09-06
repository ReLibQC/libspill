#include <stdio.h>
#include "libspill.h"
int main(void){
    ls_opts o; int e=0; ls_store *s; double v=3.25, b=0;
    ls_opts_default(&o);
    s = ls_open("consumer_c", &o, &e);
    if(!s){ printf("open failed %d\n", e); return 1; }
    if(ls_write(s,"k",0,sizeof v,&v)!=LS_OK) return 1;
    if(ls_read(s,"k",0,sizeof b,&b)!=LS_OK) return 1;
    ls_close(s,0);
    printf("  [%s] C consumer via find_package\n", b==v?"PASS":"FAIL");
    return b!=v;
}
