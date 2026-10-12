#include "snapshot_patch.h"
#include <string.h>

#define MAX_RANGES 64u
#define MAX_GROUPS 64u
#define MAX_STRING 4096u
#define MIN_STRINGS 8u

typedef struct { size_t first, end; } Range;
typedef struct { uint32_t cid, count; unsigned layout, width; } Group;
typedef struct {
    size_t payload, characters, hash_offset;
    uint32_t cid, hash;
    unsigned layout, width, hash_width, hash_shift;
} StringObject;

static uint16_t read16(const uint8_t *p) { uint16_t n; memcpy(&n,p,2); return n; }
static uint32_t read32(const uint8_t *p) { uint32_t n; memcpy(&n,p,4); return n; }
static uint64_t read64(const uint8_t *p) { uint64_t n; memcpy(&n,p,8); return n; }
static int within(uint64_t offset, uint64_t size, size_t length) {
    return offset <= length && size <= length - (size_t)offset;
}

static int read_ranges(const uint8_t *data, size_t length, Range ranges[MAX_RANGES], unsigned *count) {
    *count=0;
    if (length<64 || memcmp(data,"\177ELF",4) || data[4]!=2 || data[5]!=1 ||
        read16(data+18)!=62 || read16(data+52)<64) return 0;
    uint64_t offset=read64(data+32);
    unsigned stride=read16(data+54), total=read16(data+56);
    if (stride<56 || !total || total>MAX_RANGES || !within(offset,(uint64_t)stride*total,length)) return 0;
    for (unsigned i=0;i<total;++i) {
        const uint8_t *p=data+(size_t)offset+(size_t)i*stride;
        uint32_t type=read32(p), flags=read32(p+4);
        uint64_t start=read64(p+8), bytes=read64(p+32);
        if (type!=1 || !bytes) continue;
        if (!within(start,bytes,length)) return 0;
        /* Executable instructions and mutable data are never scanned. */
        if ((flags&7)!=4) continue;
        size_t first=((size_t)start+7)&~(size_t)7;
        if (first>(size_t)start+(size_t)bytes) continue;
        ranges[(*count)++]=(Range){first,(size_t)start+(size_t)bytes};
    }
    for (unsigned i=0;i<*count;++i) for (unsigned j=i+1;j<*count;++j) {
        if (ranges[i].first<ranges[j].end && ranges[j].first<ranges[i].end) return 0;
    }
    return *count!=0;
}

static uint32_t string_hash(const uint8_t *p,size_t length,unsigned width) {
    uint32_t h=0;
    for(size_t i=0;i<length;++i) {
        uint32_t c=width==1?p[i]:read16(p+2*i);
        h+=c; h+=h<<10; h^=h>>6;
    }
    h+=h<<3; h^=h>>11; h+=h<<15;
    h&=0x3fffffffu;
    return h?h:1;
}

static int object_at(const uint8_t *data,size_t at,size_t end,unsigned layout,unsigned width,StringObject *o) {
    size_t header=layout==2?24:16;
    if (end-at<header) return 0;
    uint64_t tags=read64(data+at), raw_length;
    uint64_t stored;
    if (layout==0) { raw_length=read64(data+at+8); stored=tags>>32; }
    else if (layout==1) { raw_length=read32(data+at+12); stored=read32(data+at+8); }
    else { raw_length=read64(data+at+16); stored=read64(data+at+8); }
    if (layout && ((tags>>32)!=0 || (stored&1))) return 0;
    if (!raw_length || (raw_length&1) || raw_length>2*MAX_STRING) return 0;
    size_t characters=(size_t)(raw_length/2), bytes=characters*width;
    if (bytes>end-at-header) return 0;
    size_t object_size=(header+bytes+15)&~(size_t)15;
    if (object_size>end-at) return 0;
    unsigned size_tag=(unsigned)((tags>>8)&15);
    if (size_tag!=(object_size<=240?object_size/16:0)) return 0;
    uint32_t cid=(uint32_t)((tags>>12)&0xfffff);
    if (!cid || cid>4095) return 0;
    uint64_t hash=layout?stored>>1:stored;
    if (hash>0x3fffffffu || hash!=string_hash(data+at+header,characters,width)) return 0;
    *o=(StringObject){at+header,characters,layout?at+8:at+4,cid,(uint32_t)hash,layout,width,layout==2?8:4,layout?1:0};
    return 1;
}

static char lower_ascii(char c) { return c>='A'&&c<='Z'?(char)(c+'a'-'A'):c; }
static int same_word(const char *s,size_t n,const char *word) {
    if (strlen(word)!=n) return 0;
    for(size_t i=0;i<n;++i) if(lower_ascii(s[i])!=word[i]) return 0;
    return 1;
}
static int ad_segment(const char *s,size_t n) {
    return same_word(s,n,"advertisement") || same_word(s,n,"advertisements") ||
           same_word(s,n,"advert") || same_word(s,n,"adverts") ||
           same_word(s,n,"advertising") || same_word(s,n,"ads") || same_word(s,n,"ad");
}
static int begins(const char *s,const char *prefix) { return strncmp(s,prefix,strlen(prefix))==0; }
static int identifier(const char *s,size_t n) {
    if (!n || s[0]<'a' || s[0]>'z') return 0;
    for(size_t i=0;i<n;++i) if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||
        (s[i]>='0'&&s[i]<='9')||s[i]=='_')) return 0;
    return 1;
}

/* Match explicit advertising paths and data keys. General banners, free
 * recommendations, business method names and source/debug names are preserved. */
static unsigned ad_rule(const char *s,size_t n,size_t *replace) {
    int asset=begins(s,"http://")||begins(s,"https://")||begins(s,"assets/");
    if (s[0]=='/' || asset) {
        size_t start=0;
        if (begins(s,"http://")||begins(s,"https://")) {
            start=begins(s,"https://")?8:7;
            while(start<n && s[start]!='/') ++start;
        }
        for(size_t i=start;i<n;) {
            while(i<n && (s[i]=='/'||s[i]=='_'||s[i]=='-')) ++i;
            size_t first=i;
            while(i<n && s[i]!='/'&&s[i]!='_'&&s[i]!='-'&&s[i]!='?'&&s[i]!='.'&&s[i]!='#') ++i;
            if(i>first && ad_segment(s+first,i-first)) { *replace=first; return asset?3:1; }
            if(i<n && (s[i]=='?'||s[i]=='#')) break;
            if(i<n) ++i;
        }
        return 0;
    }
    if (!identifier(s,n)) return 0;
    if (begins(s,"advertisement") || begins(s,"advertising") ||
        !strcmp(s,"advs") || !strcmp(s,"hotRecommendAdItem") ||
        begins(s,"ad_banner") || begins(s,"ad_section") || begins(s,"ad_list") ||
        begins(s,"adBanner") || begins(s,"adSection") || begins(s,"adList") ||
        begins(s,"adsBanner") || begins(s,"adsList") || begins(s,"adsSection") ||
        begins(s,"advertBanner") || begins(s,"advertList") || begins(s,"advertSection")) {
        *replace=0; return 2;
    }
    return 0;
}

static int selected(const Group *groups,unsigned count,const StringObject *o) {
    for(unsigned i=0;i<count;++i) if(groups[i].cid==o->cid && groups[i].layout==o->layout &&
        groups[i].width==o->width && groups[i].count>=MIN_STRINGS) {
        for(unsigned j=0;j<count;++j) if(groups[j].cid==groups[i].cid &&
            groups[j].layout==groups[i].layout && groups[j].count>groups[i].count) return 0;
        return 1;
    }
    return 0;
}

int snapshot_patch_image(uint8_t *data,size_t length,SnapshotReport *report,int apply) {
    Range ranges[MAX_RANGES]; Group groups[MAX_GROUPS]={0}; unsigned range_count=0,group_count=0;
    memset(report,0,sizeof(*report));
    if (!read_ranges(data,length,ranges,&range_count)) return 0;
    for(unsigned r=0;r<range_count;++r) for(size_t at=ranges[r].first;at+24<=ranges[r].end;at+=8) {
        for(unsigned layout=0;layout<3;++layout) for(unsigned width=1;width<=2;++width) {
            StringObject o;
            if(!object_at(data,at,ranges[r].end,layout,width,&o)) continue;
            unsigned g=0;
            for(;g<group_count;++g) if(groups[g].cid==o.cid && groups[g].layout==layout && groups[g].width==width) break;
            if(g==group_count) {
                if(group_count==MAX_GROUPS) return 0;
                groups[group_count++]=(Group){o.cid,0,layout,width};
            }
            ++groups[g].count;
        }
    }
    uint32_t largest=0;
    for(unsigned g=0;g<group_count;++g) {
        StringObject probe={0}; probe.cid=groups[g].cid; probe.layout=groups[g].layout; probe.width=groups[g].width;
        if(!selected(groups,group_count,&probe)) continue;
        report->string_objects+=groups[g].count; ++report->layouts;
        if(groups[g].count>largest) { largest=groups[g].count; report->class_id=groups[g].cid; }
    }
    if(!report->layouts) return 0;
    for(unsigned r=0;r<range_count;++r) for(size_t at=ranges[r].first;at+24<=ranges[r].end;at+=8) {
        for(unsigned layout=0;layout<3;++layout) for(unsigned width=1;width<=2;++width) {
            StringObject o;
            if(!object_at(data,at,ranges[r].end,layout,width,&o)||!selected(groups,group_count,&o)) continue;
            char text[MAX_STRING+1]; int ascii=1;
            for(size_t i=0;i<o.characters;++i) {
                unsigned c=o.width==1?data[o.payload+i]:read16(data+o.payload+2*i);
                if(c<32 || c>126) { ascii=0;break; }
                text[i]=(char)c;
            }
            if(!ascii) continue;
            text[o.characters]=0; size_t replace=0;
            unsigned rule=ad_rule(text,o.characters,&replace);
            if(!rule) continue;
            if(rule==1) ++report->api_paths;
            else if(rule==2) ++report->ad_fields;
            else ++report->ad_assets;
            ++report->patched_objects;
            if(apply) {
                /* Preserve payload length and class/size metadata; recompute
                 * the Dart string hash in whichever verified layout was found. */
                uint16_t replacement=text[replace]>='A'&&text[replace]<='Z'?'Z':'z';
                if(o.width==1) data[o.payload+replace]=(uint8_t)replacement;
                else memcpy(data+o.payload+replace*2,&replacement,2);
                uint64_t hash=(uint64_t)string_hash(data+o.payload,o.characters,o.width)<<o.hash_shift;
                memcpy(data+o.hash_offset,&hash,o.hash_width);
            }
        }
    }
    return 1;
}
