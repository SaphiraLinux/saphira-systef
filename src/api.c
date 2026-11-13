/* Local C source lookup. Lexical, deliberately not a C preprocessor: all
 * conditional branches are retained and never presented as active ABI proof. */
#include "core.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define READ_LIMIT (4u * 1024u * 1024u)
#define FILE_LIMIT 30000
#define REPLY_LIMIT 6000
#define MATCH_LIMIT 8

typedef struct {
 const char *symbol;
 result *res;
 int matches, docs, files, limited, oversized;
 size_t readbytes;
 char library[PATH_MAX];
  int docpass;
  int candidates;
  char first[PATH_MAX];
  int first_line;
  /* Header's own parenthesized form and a documented signature line, both
   * captured verbatim for the PARAMETERS comparison. No parsing of types. */
  char declparams[512];
  char docform[512];
} search;
static int ident(int c) { return isalnum((unsigned char)c) || c == '_'; }
static int symbol_ok(const char *s) {
 if (!s || !(isalpha((unsigned char)*s) || *s == '_')) return 0;
 for (; *s; s++) if (!ident(*s)) return 0;
 return 1;
}
static int word(const char *s, size_t n, const char *w) {
 size_t l = strlen(w);
 for (size_t i=0; i+l<=n; i++)
  if (!memcmp(s+i,w,l) && (!i || !ident(s[i-1])) && (i+l==n || !ident(s[i+l]))) return 1;
 return 0;
}
static const char *root(const char *env, const char *fallback) {
 const char *v=getenv(env); return v ? v : fallback;
}
static void ref(gbuf *b, const char *label, const char *path, int line) {
 char num[32]; snprintf(num,sizeof num,":%d\n",line);
 gbuf_adds(b,label); gbuf_adds(b,path); gbuf_adds(b,num);
}
/* argv-only subprocess, bounded output, no shell, no inherited protocol stdin. */
static int command(char *const av[], gbuf *out) {
 int f[2]; if (pipe(f)) return 0;
 pid_t p=fork();
 if (p<0) { close(f[0]); close(f[1]); return 0; }
 if (!p) {
  dup2(f[1],1); close(f[0]); close(f[1]);
  int d=open("/dev/null",O_RDWR); if (d>=0) { dup2(d,0); dup2(d,2); close(d); }
  setenv("LC_ALL","C",1); execvp(av[0],av); _exit(127);
 }
 close(f[1]); char buf[8192]; ssize_t n; int overflow=0;
 while ((n=read(f[0],buf,sizeof buf))>0) {
  if (out->len+(size_t)n<=READ_LIMIT) gbuf_addn(out,buf,(size_t)n); else overflow=1;
 }
 close(f[0]); int st=0; while (waitpid(p,&st,0)<0) { if (errno != EINTR) return 0; }
 return !overflow && WIFEXITED(st) && WEXITSTATUS(st)==0;
}
static int read_file(const char *path, gbuf *b) {
 struct stat st; if (stat(path,&st) || !S_ISREG(st.st_mode) || st.st_size>READ_LIMIT) return 0;
 const char *ext=strrchr(path,'.');
 const char *tool=ext && !strcmp(ext,".gz") ? "gzip" : ext && !strcmp(ext,".xz") ? "xz" : ext && !strcmp(ext,".bz2") ? "bzip2" : NULL;
 if (tool) { char *av[]={(char*)tool,"-dc","--",(char*)path,NULL}; return command(av,b); }
 FILE *f=fopen(path,"rb"); if (!f) return 0;
 char buf[8192]; size_t n;
 while ((n=fread(buf,1,sizeof buf,f))) {
  if (b->len+n>READ_LIMIT) { fclose(f); return 0; }
  gbuf_addn(b,buf,n);
 }
 int ok=!ferror(f) && !b->failed; fclose(f); return ok;
}
/* Mask comments and literals without changing offsets or line numbers. */
static char *mask(const char *s, size_t n) {
 char *m=malloc(n+1); if (!m) return NULL;
 memcpy(m,s,n); m[n]=0;
 int mode=0,escape=0;
 for (size_t i=0;i<n;i++) {
  char c=s[i];
  if (mode) {
   if (c!='\n') m[i]=' ';
   if (mode==1 && c=='*' && i+1<n && s[i+1]=='/') { m[++i]=' '; mode=0; }
   else if (mode==2 && c=='\n' && (i==0 || s[i-1]!='\\')) mode=0;
   else if (mode>=3) { if (!escape && c==(mode==3?'"':'\'')) mode=0; if (c=='\\' && !escape) escape=1; else escape=0; }
  } else if (c=='/' && i+1<n && (s[i+1]=='*' || s[i+1]=='/')) { mode=s[i+1]=='*'?1:2; m[i]=m[i+1]=' '; i++; }
  else if (c=='"' || c=='\'') { mode=c=='"'?3:4; m[i]=' '; }
 }
 return m;
}
/* Reject mere uses in parameters/initializers. This recognises ordinary C
 * declarators, not extension-heavy grammar; uncertainties remain source text. */
static int declares(const char *m, size_t n, const char *sym, int macro) {
 size_t sl=strlen(sym); int par=0,brace=0;
 for (size_t i=0;i<n;i++) {
  if (i+sl<=n && !memcmp(m+i,sym,sl) && (!i || !ident(m[i-1])) && (i+sl==n || !ident(m[i+sl]))) {
   size_t j=i+sl; while(j<n && isspace((unsigned char)m[j])) j++;
   if (macro) {
    size_t k=0; while(k<n && (isspace((unsigned char)m[k]) || m[k]=='#')) k++;
    if (k+6>n || memcmp(m+k,"define",6)) return 0;
    k+=6; while(k<n && isspace((unsigned char)m[k])) k++;
    return k==i;
   }
   if (!par && (!brace || word(m,i,"enum")) && (j==n || m[j]=='(' || m[j]==';' || m[j]=='[' || m[j]=='{' || m[j]==',' || m[j]=='=')) return 1;
   /* function-pointer typedef */
   if (par==1 && word(m,i,"typedef") && j<n && m[j]==')') return 1;
  }
  if (m[i]=='(') par++; else if (m[i]==')') par--;
  else if (m[i]=='{') brace++; else if (m[i]=='}') brace--;
  else if (m[i]=='=' && !brace && !par) break;
 }
 return 0;
}
static void declaration(search *s, const char *path, const char *raw, const char *m, size_t a, size_t b, int ln, int macro) {
 while (a<b && isspace((unsigned char)m[a])) { if (m[a]=='\n') ln++; a++; }
 if (a==b || !declares(m+a,b-a,s->symbol,macro)) return;
  s->matches++;
  if (!s->first[0]) { snprintf(s->first,sizeof s->first,"%s",path); s->first_line=ln; }
  if (!s->declparams[0]) {
   /* First parenthesized group in the masked span: comments and literals
    * are already blanked there, so this is lexical bracket matching on
    * code text, never a parse of the declarator. */
   for (size_t k=a;k<b;k++) if (m[k]=='(') {
    int depth=0; size_t e=k;
    for (;e<b && e-k<480;e++) {
     if (m[e]=='(') depth++; else if (m[e]==')' && --depth==0) break;
    }
    if (e<b && m[e]==')') { size_t n=e-k+1; memcpy(s->declparams,raw+k,n); s->declparams[n]=0; }
    break;
   }
  }
 if (s->matches>MATCH_LIMIT || s->res->text.len>REPLY_LIMIT) return;
 ref(&s->res->text,"SOURCE: ",path,ln);
 if (b-a>1500) { gbuf_adds(&s->res->text,"DECLARATION: exceeds 1500 bytes; inspect source at this line.\n\n"); s->oversized++; return; }
 gbuf_adds(&s->res->text,"DECLARATION:\n"); gbuf_addn(&s->res->text,raw+a,b-a); gbuf_adds(&s->res->text,"\n\n");
}
static void header(search *s, const char *path) {
 gbuf b; gbuf_init(&b);
 if (!read_file(path,&b)) { s->limited=1; gbuf_free(&b); return; }
 s->readbytes += b.len;
 if (!b.p || !word(b.p,b.len,s->symbol)) { gbuf_free(&b); return; }
 char *m=mask(b.p,b.len); if (!m) { s->limited=1; gbuf_free(&b); return; }
 size_t start=0; int ln=1,begin=1,par=0,brace=0,bol=1;
 for (size_t i=0;i<b.len;i++) {
  char c=m[i];
  if (bol && c=='#') {
   size_t end=i;
   do { while(end<b.len && m[end]!='\n') end++; if (end==b.len || end==0 || m[end-1]!='\\') break; end++; } while(end<b.len);
   declaration(s,path,b.p,m,i,end,ln,1);
   for(size_t k=i;k<end;k++) if(m[k]=='\n') ln++;
   i=end; if(i<b.len) ln++; start=i+1; begin=ln; bol=1; continue;
  }
  if (c=='\n') { ln++; bol=1; } else if (!isspace((unsigned char)c)) bol=0;
  if (c=='(') par++; else if(c==')') { if(par) par--; }
  else if(c=='{') {
   if (!brace && !par && word(m+start,i-start,"extern") && memchr(b.p+start,'"',i-start)) { start=i+1; begin=ln; }
   else brace++;
  } else if(c=='}') { if(brace) brace--; else { start=i+1; begin=ln; } }
  if (c==';' && !par && !brace) { declaration(s,path,b.p,m,start,i+1,begin,0); start=i+1; begin=ln; }
 }
 free(m); gbuf_free(&b);
}
/* Scandir provides deterministic results. Do not follow directory symlinks. */
static void walk(search *s, const char *path, int depth, void (*visit)(search*,const char*)) {
 if (depth>20 || s->files>=FILE_LIMIT || s->readbytes > 128u*1024u*1024u) { s->limited=1; return; }
 struct dirent **names=NULL; int n=scandir(path,&names,NULL,alphasort);
 if(n<0) return;
 for(int i=0;i<n;i++) {
  const char *name=names[i]->d_name;
  if(name[0]!='.') {
   char p[PATH_MAX]; struct stat st;
   if(snprintf(p,sizeof p,"%s/%s",path,name)<(int)sizeof p && !lstat(p,&st)) {
    if(S_ISDIR(st.st_mode)) walk(s,p,depth+1,visit);
    else if(S_ISREG(st.st_mode)) {
     if(s->files++<FILE_LIMIT && s->readbytes <= 128u*1024u*1024u) visit(s,p); else s->limited=1;
    }
   }
  }
  free(names[i]);
 }
 free(names);
}
static void header_visit(search *s,const char *path) {
 const char *e=strrchr(path,'.'); if(e && !strcmp(e,".h")) header(s,path);
}
static int same_owner(const char *header, const char *doc);
static int is_html(const char *path);
static void excerpt_add(gbuf *out, const char *p, size_t n, int html);
static void docs_visit(search *s,const char *path) {
 const char *name=strrchr(path,'/'); name=name?name+1:path;
 /* Man trees contain thousands of compressed files: function-name pages
  * are useful; decompressing unrelated command manuals is not. */
 if(strstr(path,"/man/") && !strstr(name,s->symbol)) return;
 const char *e=strrchr(path,'.');
 if(e && (!strcmp(e,".pdf") || !strcmp(e,".png") || !strcmp(e,".svg"))) return;
 gbuf b; gbuf_init(&b); if(!read_file(path,&b) || !b.p) { gbuf_free(&b); return; }
 s->readbytes += b.len;
 /* Require exact symbol spelling, not just its library prefix. */
 if (!word(b.p,b.len,s->symbol)) { gbuf_free(&b); return; }
 int ln=1;
 for(size_t i=0;i<b.len;) {
  size_t end=i; while(end<b.len && b.p[end]!='\n') end++;
  if(word(b.p+i,end-i,s->symbol)) {
   int matched_owner=same_owner(s->first,path);
   if(s->docpass==0) s->candidates++;
   if ((s->docpass==0 && !matched_owner) || (s->docpass==1 && matched_owner)) break;
   s->docs++;
   if(s->docs<=3 && s->res->text.len<REPLY_LIMIT) {
    ref(&s->res->text,"DOCUMENT: ",path,ln);
    gbuf_adds(&s->res->text,matched_owner
     ? "LOCAL EXCERPT (verbatim; same installed package/version as header):\n"
     : "LOCAL EXCERPT (verbatim; applicability/version not established):\n");
    size_t stop=end; int lines=0;
    while(stop<b.len && stop-i<1100 && lines<12) {
     if(b.p[stop++]=='\n') { lines++; if(stop<b.len && b.p[stop]=='\n') break; }
    }
    if(stop-i>1200) stop=end;
    if(stop-i<=1500) excerpt_add(&s->res->text,b.p+i,stop-i,is_html(path));
    else gbuf_adds(&s->res->text,"Matching line exceeds excerpt budget; inspect source.\n");
    if (!s->docform[0]) {
     /* Documented signature: the symbol word followed by a parenthesized
      * list inside the quoted excerpt (e.g. a texinfo Function line).
      * Captured verbatim; never reconciled by parsing. */
     size_t sl=strlen(s->symbol);
     for (size_t k=i;k<stop && !s->docform[0];k++) {
      if (k+sl<stop && !memcmp(b.p+k,s->symbol,sl) &&
          (k==0 || !ident(b.p[k-1])) && (k+sl==stop || (!ident(b.p[k+sl]) && b.p[k+sl]!='_'))) {
       size_t p=k+sl; while (p<stop && isspace((unsigned char)b.p[p])) p++;
       if (p<stop && b.p[p]=='(') {
        int depth=0; size_t e=p;
        for (;e<stop && e-p<480;e++) {
         if (b.p[e]=='(') depth++; else if (b.p[e]==')' && --depth==0) break;
        }
        if (e<stop && b.p[e]==')') { size_t n=e-k+1; memcpy(s->docform,b.p+k,n); s->docform[n]=0; }
       }
      }
     }
    }
    gbuf_adds(&s->res->text,"\n");
   }
   break;
  }
  if(end==b.len) break;
  i=end+1; ln++;
 }
 gbuf_free(&b);
}

/* Optional dynamic-symbol evidence, only on an unsuccessful header lookup.
 * An exported symbol establishes a runtime object, never a C declaration. */
static void library_visit(search *s,const char *path) {
 if(s->library[0]) return;
 const char *name=strrchr(path,'/'); name=name?name+1:path;
 if(strncmp(name,"lib",3) || !strstr(name,".so")) return;
 char *av[]={"nm","-D","--defined-only","--format=posix","--",(char*)path,NULL};
 gbuf b; gbuf_init(&b);
 if(command(av,&b) && b.p) {
  size_t sl=strlen(s->symbol);
  for(size_t i=0;i<b.len;) {
   if(i+sl<b.len && !memcmp(b.p+i,s->symbol,sl) && (isspace((unsigned char)b.p[i+sl]) || b.p[i+sl]=='@')) {
    snprintf(s->library,sizeof s->library,"%s",path); break;
   }
   while(i<b.len && b.p[i]!='\n') i++;
   if(i<b.len) i++;
  }
 }
 gbuf_free(&b);
}

/* Separate distro adapters, all read-only and offline. File-search indexes
 * are never refreshed here. Output is evidence, not an inferred package name. */
typedef struct { const char *name; const char *owner[5]; const char *files[5]; } adapter;
static const adapter adapters[]={
 {"apk", {"apk","info","--who-owns",NULL}, {NULL}},
 {"pacman", {"pacman","-Qo",NULL}, {"pacman","-F","--",NULL}},
 {"Portage", {"qfile","-v",NULL}, {NULL}},
 {"dpkg", {"dpkg-query","-S",NULL}, {NULL}},
 {"RPM", {"rpm","-qf","--",NULL}, {"rpm","-q","--whatprovides",NULL}},
};
static int evidence(gbuf *out,const adapter *a,const char *const *prefix,const char *path,const char *label) {
 if(!prefix[0]) return 0;
 char *av[8]; int i=0; for(;prefix[i];i++) av[i]=(char*)prefix[i]; av[i++]=(char*)path; av[i]=NULL;
 gbuf b; gbuf_init(&b); int ok=command(av,&b) && b.len;
 if(ok && prefix==a->files) {
  const char *needle=path[0]=='/'?path+1:path;
  const char *hit=strstr(b.p,needle);
  ok=hit && (hit[strlen(needle)]==0 || isspace((unsigned char)hit[strlen(needle)]));
 }
 if(ok) { gbuf_adds(out,label); gbuf_adds(out,a->name); gbuf_adds(out,": "); gbuf_addn(out,b.p,b.len>800?800:b.len); gbuf_addch(out,'\n'); }
 gbuf_free(&b); return ok;
}
static int same_owner(const char *header,const char *doc) {
 if(!header[0]) return 0;
 for(size_t k=0;k<sizeof adapters/sizeof *adapters;k++) {
  /* dpkg -S and qfile output embeds paths; do not compare that text. */
  if(!strcmp(adapters[k].name,"dpkg") || !strcmp(adapters[k].name,"Portage")) continue;
  gbuf h,d; gbuf_init(&h); gbuf_init(&d);
  char *av[8]; int n=0;
  for(;adapters[k].owner[n];n++) av[n]=(char*)adapters[k].owner[n];
  av[n]=(char*)header; av[n+1]=NULL; int ok=command(av,&h);
  av[n]=(char*)doc; ok=ok && command(av,&d);
  const char *hp=h.p,*dp=d.p;
  if(ok && strcmp(adapters[k].name,"RPM")) {
   hp=hp?strstr(hp," is owned by "):NULL; dp=dp?strstr(dp," is owned by "):NULL;
   if(hp) hp+=13;
   if(dp) dp+=13;
  }
  int same=ok && hp && dp && *hp && !strcmp(hp,dp);
  gbuf_free(&h); gbuf_free(&d); if(same) return 1;
 }
 return 0;
}
/* HTML excerpts are quoted as text: tags are markup, not documentation. */
static int is_html(const char *path) {
 const char *e=strrchr(path,'.');
 return e && (!strcmp(e,".html") || !strcmp(e,".htm"));
}
static void excerpt_add(gbuf *out, const char *p, size_t n, int html) {
 if(!html) { gbuf_addn(out,p,n); return; }
 int tag=0; for(size_t i=0;i<n;i++) {
  if(p[i]=='<') tag=1;
  else if(p[i]=='>') tag=0;
  else if(!tag) gbuf_addch(out,p[i]);
 }
}
/* First owner/version line for the declaration header, for the concise
 * PACKAGE line. Empty output or no working adapter means unknown. */
static int header_owner(const char *header, char *out, size_t n) {
 if(!header[0]) return 0;
 for(size_t k=0;k<sizeof adapters/sizeof *adapters;k++) {
  if(!adapters[k].owner[0]) continue;
  char *av[8]; int i=0;
  for(;adapters[k].owner[i];i++) av[i]=(char*)adapters[k].owner[i];
  av[i++]=(char*)header; av[i]=NULL;
  gbuf b; gbuf_init(&b);
  if(command(av,&b) && b.len && b.p) {
   size_t l=0; while(b.p[l] && b.p[l]!='\n' && l+1<n) l++;
   memcpy(out,b.p,l); out[l]=0;
   gbuf_free(&b); return 1;
  }
  gbuf_free(&b);
 }
 return 0;
}
/* Compare header vs documented parameter forms without parsing either side:
 * whitespace-insensitive whole-list equality, else a positional table over
 * top-level comma segments, else a verbatim whole-list fallback. Type
 * knowledge is never consulted; names are never invented. */
static int stripped_equal(const char *a, const char *b) {
 for (;;) {
  while (*a && isspace((unsigned char)*a)) a++;
  while (*b && isspace((unsigned char)*b)) b++;
  if (*a != *b) return 0;
  if (!*a) return 1;
  a++; b++;
 }
}
static int split_params(const char *s, char out[][160], int max) {
 size_t n=strlen(s);
 if (n < 2 || s[0] != '(' || max <= 0) return -1;
 int depth=0, seg=0; size_t start=1;
 for (size_t i=0;i<n;i++) {
  if (s[i] == '(') depth++;
  else if (s[i] == ')') {
   if (--depth < 0) return -1;
   if (depth == 0) {
    if (seg >= max) return -1;
    size_t a=start, b=i;
    while (a<b && isspace((unsigned char)s[a])) a++;
    while (b>a && isspace((unsigned char)s[b-1])) b--;
    size_t len=b-a; if (len>159) len=159;
    memcpy(out[seg],s+a,len); out[seg][len]=0; seg++;
    return (seg==1 && out[0][0]==0) ? 0 : seg;
   }
  } else if (s[i] == ',' && depth == 1) {
   if (seg >= max) return -1;
   size_t a=start, b=i;
   while (a<b && isspace((unsigned char)s[a])) a++;
   while (b>a && isspace((unsigned char)s[b-1])) b--;
   size_t len=b-a; if (len>159) len=159;
   memcpy(out[seg],s+a,len); out[seg][len]=0; seg++;
   start=i+1;
  }
 }
 return -1;
}
static void parameters_block(search *s) {
 gbuf *out=&s->res->text;
 if (!s->declparams[0]) return;
 if (!s->docform[0]) {
  gbuf_adds(out,"PARAMETERS: no documented signature in local excerpts; parameter names are exactly as written in the declaration.\n");
  return;
 }
 const char *df=strchr(s->docform,'(');
 if (!df) {
  gbuf_adds(out,"PARAMETERS: no documented signature in local excerpts; parameter names are exactly as written in the declaration.\n");
  return;
 }
 if (stripped_equal(s->declparams,df)) {
  gbuf_adds(out,"PARAMETERS: header and documented forms agree.\n");
  return;
 }
 static char dh[12][160], dd[12][160];
 int nh=split_params(s->declparams,dh,12), nd=split_params(df,dd,12);
 if (nh>0 && nh==nd) {
  gbuf_adds(out,"PARAMETERS:\n");
  for (int i=0;i<nh;i++) {
   char ln[32]; snprintf(ln,sizeof ln,"  %d: header '",i+1); gbuf_adds(out,ln);
   gbuf_adds(out,dh[i]); gbuf_adds(out,"' | documented '"); gbuf_adds(out,dd[i]);
   gbuf_adds(out,"'\n");
  }
  gbuf_adds(out,"  (documented names from quoted manual text, matched positionally; not inferred.)\n");
  return;
 }
 gbuf_adds(out,"PARAMETERS:\n  header '"); gbuf_adds(out,s->declparams);
 gbuf_adds(out,"'\n  documented '"); gbuf_adds(out,df);
 gbuf_adds(out,"'\n  (forms differ structurally; quoted verbatim, not reconciled.)\n");
}
static void packages(search *s, const char *missing) {
 int found=0;
 for(size_t i=0;i<sizeof adapters/sizeof *adapters;i++) {
  if(s->library[0]) found |= evidence(&s->res->text,&adapters[i],adapters[i].owner,s->library,"RUNTIME OWNER (export verified): ");
  if(s->first[0]) found |= evidence(&s->res->text,&adapters[i],adapters[i].owner,s->first,"HEADER OWNER/VERSION (not documentation provider): ");
  if(missing) found |= evidence(&s->res->text,&adapters[i],adapters[i].files,missing,"LOCAL FILE METADATA (candidate provider; installation state unverified): ");
 }
 if(!found) gbuf_adds(&s->res->text,"Package ownership/availability cannot be determined from local metadata.\n");
 if(!s->docs) gbuf_adds(&s->res->text,"Documentation provider unknown; header ownership does not establish a documentation package. No installation command verified.\n");
}
int sysref_api(const char *query, int semantics, result *res) {
 char q[SYSREF_MAX_QUERY_BYTES+1]; snprintf(q,sizeof q,"%s",query);
 char *save=NULL,*first=strtok_r(q," \t\n",&save),*second=strtok_r(NULL," \t\n",&save),*extra=strtok_r(NULL," \t\n",&save);
 if(!first || extra) { gbuf_adds(&res->text,"Use [header.h] symbol (one C identifier).\n"); res->status=1; return 1; }
 const char *sym=second?second:first;
 int named=second || strchr(first,'.') || strchr(first,'/');
 if((second || !named) && !symbol_ok(sym)) { gbuf_adds(&res->text,"Invalid C symbol.\n"); res->status=1; return 1; }
 if(named && (first[0]=='/' || strstr(first,".."))) { gbuf_adds(&res->text,"Header must be relative to an include root.\n"); res->status=1; return 1; }
  search s={.symbol=sym,.res=res};
  if(semantics && (second || !named)) { gbuf_adds(&res->text,"SYMBOL: "); gbuf_adds(&res->text,sym); gbuf_addch(&res->text,'\n'); }
 const char *roots=root("SYSREF_INCLUDE_PATH","/usr/include:/usr/local/include");
 if (!named && !semantics) {
  char *probe=strdup(roots), *ps=NULL;
  for(char *pr=strtok_r(probe,":",&ps);pr;pr=strtok_r(NULL,":",&ps)) {
   char path[PATH_MAX]; struct stat st;
   if(snprintf(path,sizeof path,"%s/%s.h",pr,first)<(int)sizeof path && !stat(path,&st) && S_ISREG(st.st_mode)) named=1;
  }
  free(probe);
 }
 char *rs=strdup(roots),*sp=NULL,*r; int exists=0;
 for(r=strtok_r(rs,":",&sp);r;r=strtok_r(NULL,":",&sp)) {
  if(named) {
   char path[PATH_MAX],canon[PATH_MAX],cr[PATH_MAX];
   if(snprintf(path,sizeof path,"%s/%s",r,first)>=(int)sizeof path) continue;
   if(!realpath(path,canon) && !strchr(first,'.')) { if(snprintf(path,sizeof path,"%s/%s.h",r,first)>=(int)sizeof path) continue; }
   if(!realpath(path,canon) || !realpath(r,cr) || strncmp(canon,cr,strlen(cr)) || canon[strlen(cr)]!='/') continue;
   exists++;
   if(second) header(&s,canon);
   else {
    gbuf b; gbuf_init(&b); if(read_file(canon,&b) && b.p) {
     ref(&res->text,"SOURCE: ",canon,1);
     gbuf_adds(&res->text,"#include <"); gbuf_adds(&res->text,first); gbuf_adds(&res->text,">\n");
     gbuf_adds(&res->text,"Header available. Add a C symbol for its declaration.\n");
     snprintf(s.first,sizeof s.first,"%s",canon);
    } gbuf_free(&b);
   }
  } else walk(&s,r,0,header_visit);
 }
 free(rs);
  if(!s.matches && (second || !named)) {
   if(exists) { gbuf_adds(&res->text,"Symbol not found in installed header "); gbuf_adds(&res->text,first); gbuf_adds(&res->text,"; installed version/conditional availability unresolved.\n"); }
   else gbuf_adds(&res->text,"Symbol/header unavailable in searched include roots; missing development package versus absent library unresolved.\n");
   res->status=exists?2:1;
  }
 if(named && !exists) { gbuf_adds(&res->text,"Missing header: "); gbuf_adds(&res->text,first); gbuf_addch(&res->text,'\n'); res->status=1; }
  if(s.matches>MATCH_LIMIT || res->text.len>REPLY_LIMIT) gbuf_adds(&res->text,"More definitions found; narrow with a header name.\n");
  if(s.matches) gbuf_adds(&res->text,"Source declarations only; preprocessor branches retained, active configuration not evaluated.\n");
  /* Declared location and owning package first: the agent sees what was
   * found and where before the longer documentation excerpts. */
  if(semantics && s.matches) {
   char pkg[256], ln[32];
   snprintf(ln,sizeof ln,":%d\n",s.first_line);
   gbuf_adds(&res->text,"HEADER: "); gbuf_adds(&res->text,s.first); gbuf_adds(&res->text,ln);
   if(header_owner(s.first,pkg,sizeof pkg)) { gbuf_adds(&res->text,"PACKAGE: "); gbuf_adds(&res->text,pkg); gbuf_addch(&res->text,'\n'); }
   else gbuf_adds(&res->text,"PACKAGE: unknown from local metadata.\n");
  }
 if(semantics) {
  s.files=0; s.readbytes=0;
  const char *dr=root("SYSREF_DOC_PATH","/usr/share/doc:/usr/local/share/doc:/usr/share/info:/usr/share/man");
  rs=strdup(dr); sp=NULL;
  for(r=strtok_r(rs,":",&sp);r;r=strtok_r(NULL,":",&sp)) walk(&s,r,0,docs_visit);
  free(rs);
  /* Verified same-package/version documentation gets the first slots.
   * Only repeat the bounded scan if unverified candidates remain. */
  if(s.docs<3 && s.candidates>s.docs) {
   s.docpass=1; s.files=0; s.readbytes=0;
   rs=strdup(dr); sp=NULL;
   for(r=strtok_r(rs,":",&sp);r;r=strtok_r(NULL,":",&sp)) walk(&s,r,0,docs_visit);
   free(rs);
  }
   if(s.docs>3) gbuf_adds(&res->text,"Additional documentation matches omitted; use source=doc with the named package for more context.\n");
   /* Lifecycle block: concise evidence-first summary. A signature alone
    * cannot establish argument meaning; the agent must see whether
    * authoritative local documentation exists before inferring behaviour. */
   if(!s.docs) gbuf_adds(&res->text,"DOCUMENTATION: unavailable in searched files (not proof that a documentation package exists).\n");
   else { char dn[64]; snprintf(dn,sizeof dn,"DOCUMENTATION: %d local excerpt(s) quoted above.\n",s.docs); gbuf_adds(&res->text,dn); }
   if(semantics && s.matches) parameters_block(&s);
   if(s.docs) gbuf_adds(&res->text,"ARGUMENTS: described only where quoted above; no separate parameter table inferred from types.\n");
   else gbuf_adds(&res->text,"ARGUMENTS: unavailable -- no local documentation names the parameters; do not infer from types.\n");
   if(s.matches) gbuf_adds(&res->text,"STATUS: declaration found.\n");
   else gbuf_adds(&res->text,"STATUS: no declaration in searched headers.\n");
   if(s.docs) gbuf_adds(&res->text,"Detailed local manual: quoted above; only quoted text is evidence.\n");
   else gbuf_adds(&res->text,"Detailed local manual: unavailable.\n");
 }
 if(semantics || res->status) {
  char missing[PATH_MAX]; const char *m=NULL;
  if(named && !exists && snprintf(missing,sizeof missing,"/usr/include/%s",first)<(int)sizeof missing) m=missing;
  if(!s.matches && (second || !named)) {
   s.files=0; s.readbytes=0;
   const char *lr=root("SYSREF_LIBRARY_PATH","/usr/lib:/lib:/usr/local/lib");
   rs=strdup(lr); sp=NULL;
   for(r=strtok_r(rs,":",&sp);r;r=strtok_r(NULL,":",&sp)) walk(&s,r,0,library_visit);
   free(rs);
   if(s.library[0]) {
    gbuf_adds(&res->text,"Runtime library exports the symbol: "); gbuf_adds(&res->text,s.library);
    gbuf_adds(&res->text,"\nDevelopment declaration unavailable in searched headers; runtime library is installed.\n");
   }
  }
  packages(&s,m);
  if(semantics && !s.docs && symbol_ok(sym)) {
   char docpath[PATH_MAX];
   const char *suffix[]={".3",".3.gz",NULL};
   for(int j=0;suffix[j];j++) {
    snprintf(docpath,sizeof docpath,"/usr/share/man/man3/%s%s",sym,suffix[j]);
    for(size_t k=0;k<sizeof adapters/sizeof *adapters;k++)
     evidence(&res->text,&adapters[k],adapters[k].files,docpath,"DOCUMENTATION FILE PROVIDER (cached metadata; package installation state unknown): ");
   }
  }
 }
 if(s.limited) gbuf_adds(&res->text,"Search incomplete: file/depth/read limit reached. A no-match is not an exhaustive absence claim.\n");
 snprintf(res->pagename,sizeof res->pagename,"%s",sym);
 return res->status?1:0;
}
