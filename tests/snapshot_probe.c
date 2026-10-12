#include "../src/snapshot_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv) {
    if(argc<2||argc>3) return 2;
    FILE *file=fopen(argv[1],"rb"); if(!file) return 3;
    if(fseek(file,0,SEEK_END)) { fclose(file);return 4; }
    long length=ftell(file);
    if(length<0||length>512L*1024*1024) { fclose(file);return 5; }
    rewind(file); unsigned char *data=malloc(length?(size_t)length:1);
    if(!data) { fclose(file);return 6; }
    if(fread(data,1,(size_t)length,file)!=(size_t)length) {free(data);fclose(file);return 7;}
    fclose(file);
    SnapshotReport report; int recognized=snapshot_patch_image(data,(size_t)length,&report,1);
    if(argc==3) {
        file=fopen(argv[2],"wb"); if(!file) {free(data);return 8;}
        int ok=fwrite(data,1,(size_t)length,file)==(size_t)length;
        if(fclose(file)) ok=0;
        if(!ok) {free(data);return 9;}
    }
    printf("{\"recognized\":%s,\"strings\":%u,\"layouts\":%u,\"classId\":%u,\"apiPaths\":%u,\"adFields\":%u,\"adAssets\":%u,\"patched\":%u}\n",
        recognized?"true":"false",report.string_objects,report.layouts,report.class_id,
        report.api_paths,report.ad_fields,report.ad_assets,report.patched_objects);
    free(data);return 0;
}
