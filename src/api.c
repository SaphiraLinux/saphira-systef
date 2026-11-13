/* Local C source lookup. Lexical, deliberately not a C preprocessor: all
 * conditional branches are retained and never presented as active ABI proof. */
#include "core.h"
#include "json.h"
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

/* Rank: verified same-package/version first (applicability is proven), then
 * code-context matches (contract-like use, not prose), then excerpts using
 * the symbol next to its co-declared header siblings (same API family;
 * closer wins), then repeated mentions (denser treatment), then concise
 * excerpts (long tables and boilerplate sink). Path and line close the
 * order so the same corpus always yields the same pages. */
static int excerpt_rank(const void *a, const void *b) {
 const sysref_excerpt *x=a, *y=b;
 if (x->owner != y->owner) return y->owner - x->owner;
 if (x->code != y->code) return y->code - x->code;
 { int xn = x->prox < 0, yn = y->prox < 0;
   if (xn != yn) return xn - yn;
   if (!xn && x->prox != y->prox) return x->prox < y->prox ? -1 : 1; }
 if (x->freq != y->freq) return y->freq - x->freq;
 if (x->len != y->len) return x->len < y->len ? -1 : 1;
 { int c=strcmp(x->path,y->path); if (c) return c; }
 return x->ln - y->ln;
}
/* Best-match verdict, or NULL. Named only when the lead excerpt is a
 * uniquely close family co-mention: top hit carries code context and a
 * sibling next to the symbol, strictly closer than any runner-up. Anything
 * less stays silent (today's behavior), so the tool never bluffs. */
static sysref_excerpt *verdict_pick(const char *verb, sysref_excerpt *docs, int ndocs) {
 if (strcmp(verb, "api") != 0 || ndocs < 1) return NULL;
 sysref_excerpt *top = &docs[0];
 if (!top->code || top->prox < 0) return NULL;
 if (ndocs > 1) {
  sysref_excerpt *next = &docs[1];
  if (next->prox >= 0 && next->prox <= top->prox) return NULL;
 }
 return top;
}

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
   * captured verbatim for the PARAMETERS comparison. No type knowledge. */
  char declparams[512];
  char docform[512];
  /* First-match declaration kind: 0 undecided, 1 function, 2 function-like
   * macro, 3 object-like macro. Keyword flags record only words literally
   * present in the declaration. */
  int kind, k_static, k_inline, k_extern;
  /* Innermost preprocessor guards enclosing the first match, verbatim. */
  char curguard[3][96];
  int ncurguard;
  char guard[300];
  /* Installed-package owner of the declaration header, when local
   * metadata establishes it; gates prose-match suppression. */
  char pkg[256];
  int pkg_known;
  /* Whitespace-normalized hashes of emitted excerpts: exact duplicates
   * across files add no information and are not counted. */
  unsigned long seen[32];
  int nseen;
  /* Suppressed low-relevance matches (prose-only from other packages). */
  int skipped;
  /* 1-based result page; windowing is deterministic per (query, page). */
  int page;
  /* Excerpts quoted per page (density knob, default API_PAGE_SIZE). */
  int ppsize;
  /* Co-declared siblings from the first declaring header (macro names;
   * the query symbol itself excluded). Excerpts using the symbol next to
   * its API siblings describe the API family, not an incidental mention. */
  char sib[48][64];
  int nsib;
  /* Ranked excerpt buffer: accepted excerpts are collected during the scan
   * and emitted in rank order afterwards, so excerpts and lookahead share
   * one ordering. s.docs counts every accepted excerpt exactly; the buffer
   * holds the first rankcap (overflow is counted, not quoted). */
  sysref_excerpt *rank;
  int nrank;
  int rankcap;
} search;
#define API_PAGE_SIZE 3
#define API_MAX_PER_PAGE 10
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
  if (!s->first[0]) {
   snprintf(s->first,sizeof s->first,"%s",path); s->first_line=ln;
   if (macro) {
    size_t k=a;
    while (k<b && (isspace((unsigned char)m[k])||m[k]=='#')) k++;
    k+=6; /* "define" verified by declares() */
    while (k<b && isspace((unsigned char)m[k])) k++;
    while (k<b && ident(m[k])) k++;
    s->kind = (k<b && m[k]=='(') ? 2 : 3;
   } else {
    int has_paren=0;
    for (size_t k=a;k<b;k++) if (m[k]=='(') { has_paren=1; break; }
    int typelike = word(m+a,b-a,"typedef") || word(m+a,b-a,"struct") ||
                   word(m+a,b-a,"enum") || word(m+a,b-a,"union");
    int fnptr = 0;
    for (size_t k=a;k+1<b;k++) if (m[k]=='(' && m[k+1]=='*') { fnptr=1; break; }
    if (has_paren && !typelike && !fnptr) {
     s->kind=1;
     s->k_static=word(m+a,b-a,"static");
     s->k_inline=word(m+a,b-a,"inline");
     s->k_extern=word(m+a,b-a,"extern");
    }
   }
   if (s->ncurguard>0) {
    size_t pos=0;
    for (int g=0;g<s->ncurguard && g<3 && pos<sizeof s->guard-4;g++) {
     if (g) { memcpy(s->guard+pos," / ",3); pos+=3; }
     size_t l=strlen(s->curguard[g]);
     if (pos+l>sizeof s->guard-1) l=sizeof s->guard-1-pos;
     memcpy(s->guard+pos,s->curguard[g],l); pos+=l;
    }
    s->guard[pos]=0;
   }
  }
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
 /* Structured sidecar: the same declaration the text path quotes, bounded.
  * Renderers (markdown/json) read this, so formats share one evidence. */
 if (s->res->ndecls < SYSREF_MAX_DECLS) {
  sysref_decl *d=&s->res->decls[s->res->ndecls++];
  snprintf(d->path,sizeof d->path,"%s",path);
  d->ln=ln;
  if (b-a>1500) { d->over=1; d->text[0]=0; }
  else { d->over=0; memcpy(d->text,raw+a,b-a); d->text[b-a]=0; }
 }
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
    { /* Track conditional nesting for GUARDED BY: directive word plus the
       * condition's own text, whitespace-collapsed, never evaluated. */
     size_t d=i+1; while (d<end && isspace((unsigned char)m[d])) d++;
     size_t w=d; while (w<end && isalpha((unsigned char)m[w])) w++;
     size_t wl=w-d;
     if ((wl==2 && !memcmp(m+d,"if",2)) || (wl==5 && !memcmp(m+d,"ifdef",5)) ||
         (wl==6 && !memcmp(m+d,"ifndef",6))) {
      if (s->ncurguard<3) {
       char *g=s->curguard[s->ncurguard]; size_t pos=0;
       int ran=0;
       for (size_t k=i;k<end && pos<92;k++) {
        if (m[k]=='\\' && k+1<end && m[k+1]=='\n') { k++; continue; }
        if (isspace((unsigned char)m[k])) { if (!ran) { g[pos++]=' '; ran=1; } }
        else { g[pos++]=m[k]; ran=0; }
       }
       while (pos>0 && g[pos-1]==' ') pos--;
       g[pos]=0;
       if (pos>0) s->ncurguard++;
      }
     } else if (wl==5 && !memcmp(m+d,"endif",5)) {
      if (s->ncurguard>0) s->ncurguard--;
     } else if (wl==6 && !memcmp(m+d,"define",6)) {
      /* An include guard opened and immediately defined (#ifndef X just
       * above #define X) is boilerplate, not a configuration condition. */
      size_t k=d+6; while (k<end && isspace((unsigned char)m[k])) k++;
      size_t n0=k; while (k<end && ident(m[k])) k++;
      if (k>n0 && s->ncurguard>0) {
       char *top=s->curguard[s->ncurguard-1];
       size_t tn=strlen(top);
       if (tn==8+(k-n0) && !memcmp(top+8,m+n0,k-n0)) s->ncurguard--;
      }
     }
    }
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
static unsigned long excerpt_hash(const char *p, size_t n);
static int code_context(const char *p, size_t pos, size_t n, size_t sl);
static int mentions_feature_test(const char *g);
static int c_keyword(const char *w) {
 static const char *kw[] = {"auto","break","case","char","const","continue","default","do","double","else","enum","extern","float","for","goto","if","inline","int","long","register","restrict","return","short","signed","sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while","_Bool","_Complex","_Imaginary",NULL};
 for (int i=0;kw[i];i++) if (!strcmp(w,kw[i])) return 1;
 return 0;
}
/* Co-declared macro siblings from the first declaring header: the other
 * names its #define lines establish. Exact line match, no declarator
 * parsing; C keywords, one-letter names and the query symbol never
 * qualify. File order is deterministic, count bounded. */
static void collect_siblings(search *s) {
 if (!s->first[0]) return;
 gbuf b; gbuf_init(&b);
 if (!read_file(s->first,&b) || !b.p || b.len > (64u*1024u)) { gbuf_free(&b); return; }
 size_t sl = strlen(s->symbol);
 const char *p = b.p, *end = b.p + b.len;
 while (p < end && s->nsib < 48) {
  const char *e = p;
  while (e < end && *e != '\n') e++;
  const char *q = p;
  while (q < e && (*q == ' ' || *q == '\t')) q++;
  if (q < e && *q == '#') {
   q++;
   while (q < e && (*q == ' ' || *q == '\t')) q++;
   if ((size_t)(e - q) >= 6 && !memcmp(q, "define", 6) &&
       (q + 6 == e || q[6] == ' ' || q[6] == '\t' || q[6] == '(')) {
    q += 6;
    while (q < e && (*q == ' ' || *q == '\t')) q++;
    const char *ns = q;
    while (q < e && (isalnum((unsigned char)*q) || *q == '_')) q++;
    if (q - ns > 1 && (size_t)(q - ns) < sizeof s->sib[0] &&
        ((size_t)(q - ns) != sl || memcmp(ns, s->symbol, sl))) {
     char tmp[64];
     memcpy(tmp, ns, (size_t)(q - ns));
     tmp[q - ns] = 0;
     if (!c_keyword(tmp)) {
      int dup = 0;
      for (int d = 0; d < s->nsib; d++) if (!strcmp(s->sib[d], tmp)) { dup = 1; break; }
      if (!dup) { memcpy(s->sib[s->nsib], tmp, sizeof s->sib[0]); s->nsib++; }
     }
    }
   }
  }
  p = (*e == '\n') ? e + 1 : e;
 }
 gbuf_free(&b);
}
static void docs_visit(search *s,const char *path) {
 const char *name=strrchr(path,'/'); name=name?name+1:path;
 /* Man trees contain thousands of compressed files: function-name pages
  * are useful; decompressing unrelated command manuals is not. */
 if(strstr(path,"/man/") && !strstr(name,s->symbol)) return;
 const char *e=strrchr(path,'.');
 if(e && (!strcmp(e,".pdf") || !strcmp(e,".png") || !strcmp(e,".svg"))) return;
 /* Generated search indexes describe the corpus, not the subject:
  * searchindex*.js blobs and objects.inv match every term by construction
  * and explain nothing. Skip them at index time. */
 { size_t nl=strlen(name);
   if (nl>3 && !strcmp(name+nl-3,".js") && !strncmp(name,"searchindex",11)) return; }
 if (!strcmp(name,"objects.inv")) return;
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
    size_t stop=end; int lines=0;
    while(stop<b.len && stop-i<1100 && lines<12) {
     if(b.p[stop++]=='\n') { lines++; if(stop<b.len && b.p[stop]=='\n') break; }
    }
    if(stop-i>1200) stop=end;
    /* Ranked collection: buffer the accepted excerpt for ordering after the
     * scan (one ranking feeds both excerpts and lookahead). Exact duplicates
     * are kept once and not counted; prose-only matches from a foreign
     * package are counted but not quoted. s.docs counts every accepted
     * excerpt exactly; the buffer holds the first rankcap. */
    { unsigned long h=excerpt_hash(b.p+i, stop-i);
      int dup=0;
      for (int d=0;d<s->nseen;d++) if (s->seen[d]==h) { dup=1; break; }
      if (dup) break;
      if (s->nseen<32) s->seen[s->nseen++]=h;
      size_t sl=strlen(s->symbol);
      int code=0, freq=0;
      size_t sympos[32];
      int nsym=0;
      for (size_t k=i;k+sl<=stop;k++) {
       if (!memcmp(b.p+k,s->symbol,sl) &&
           (k==0 || !ident(b.p[k-1])) && (k+sl==stop || !ident(b.p[k+sl]))) {
        if (freq<999) freq++;
        if (nsym<32) sympos[nsym++]=k;
        if (!code && code_context(b.p,k,stop,sl)) code=1;
       }
      }
      if (!matched_owner && s->pkg_known && !code) { s->skipped++; break; }
      /* Closest whole-word co-mention of a header sibling, in characters:
       * the symbol discussed next to its API family outranks a lone
       * mention. -1 when the window names no sibling. */
      int prox=-1;
      if (s->nsib > 0 && nsym > 0) {
       for (int d=0; d<s->nsib && prox!=0; d++) {
        size_t ll=strlen(s->sib[d]);
        if (ll==0) continue;
        for (size_t m=i; m+ll<=stop; m++) {
         if (!memcmp(b.p+m,s->sib[d],ll) &&
             (m==0 || !ident(b.p[m-1])) && (m+ll==stop || !ident(b.p[m+ll]))) {
          for (int t=0; t<nsym; t++) {
           size_t se=sympos[t]+sl;
           int g = (sympos[t] < m) ? (int)(m-se) : (int)(sympos[t]-(m+ll));
           if (prox<0 || g<prox) prox=g;
          }
          if (prox==0) break;
         }
        }
       }
      }
      if (s->nrank < s->rankcap) {
       sysref_excerpt *er=&s->rank[s->nrank++];
       snprintf(er->path,sizeof er->path,"%s",path);
       er->ln=ln; er->owner=matched_owner; er->code=code; er->prox=prox; er->freq=freq;
       er->len=stop-i; er->over=(stop-i>1500); er->html=is_html(path);
       if (!er->over) { memcpy(er->text,b.p+i,stop-i); er->text[stop-i]=0; }
       else er->text[0]=0;
      }
    }
    s->docs++;
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
/* Whitespace-insensitive excerpt hash for exact-duplicate suppression. */
static unsigned long excerpt_hash(const char *p, size_t n) {
 unsigned long h=5381;
 for (size_t i=0;i<n;i++) {
  unsigned char c=p[i];
  if (isspace(c)) continue;
  h=((h<<5)+h)+c;
 }
 return h;
}
/* Code-adjacent occurrence? A symbol glued to punctuation or parens reads
 * as code; one wrapped in prose words on both sides reads as English.
 * Lexical adjacency only, never a judgment about meaning. */
static int code_context(const char *p, size_t pos, size_t n, size_t sl) {
 static const char codechars[] = "()[],;{}=:#*&<>\"'`/@+-";
 size_t j=pos+sl;
 while (j<n && isspace((unsigned char)p[j])) j++;
 if (j<n && strchr(codechars,p[j])) return 1;
 size_t k=pos;
 while (k>0 && isspace((unsigned char)p[k-1])) k--;
 if (k>0 && strchr(codechars,p[k-1])) return 1;
 return 0;
}
/* Feature-test-macro vocabulary, matched literally against a guard line.
 * Presence only earns a pointer to the manual, never a claim about which
 * macro the declaration needs. */
static int mentions_feature_test(const char *g) {
 static const char *words[] = {
  "_GNU_SOURCE","_USE_","_POSIX_","_XOPEN_","_DEFAULT_SOURCE","_STRICT_ANSI",
  "_BSD_SOURCE","_SVID_SOURCE","_ATFILE_SOURCE","_LARGEFILE","_FILE_OFFSET_BITS",
  "_ISOC","_DYNAMIC","_REENTRANT","_THREAD_SAFE","__STDC_WANT_",NULL};
 for (int i=0;words[i];i++) if (strstr(g,words[i])) return 1;
  return 0;
}
/* Concrete feature-test requirement read off a guard, or 0. Only the
 * simple whole-condition forms qualify: `ifdef MACRO`, `ifndef MACRO`,
 * `if defined(MACRO)`, `if !defined(MACRO)` with MACRO in the
 * feature-test vocabulary above. Anything compound or nested stays a
 * quoted guard plus the manual pointer, because turning an arbitrary
 * expression into a requirement would be inference, not lookup. */
static int guard_requirement(const char *g, char *mac, size_t macsz, int *neg) {
 if (strstr(g, " / ")) return 0; /* nested guards: not simple */
 const char *h = g;
 if (h[0] == '#') h++;
 const char *p = NULL;
 int n = 0;
 if (!strncmp(h, "ifdef ", 6)) { p = h + 6; n = 0; }
 else if (!strncmp(h, "ifndef ", 7)) { p = h + 7; n = 1; }
 else if (!strncmp(h, "if defined(", 11) && h[strlen(h) - 1] == ')') { p = h + 11; n = 0; }
 else if (!strncmp(h, "if !defined(", 12) && h[strlen(h) - 1] == ')') { p = h + 12; n = 1; }
 else return 0;
 size_t ml = strlen(h) - (size_t)(p - h);
 if (p != h + 6 && p != h + 7) {
  if (ml == 0) return 0;
  ml--; /* trailing ')' */
 }
 if (ml == 0 || ml + 1 > macsz) return 0;
 for (size_t i = 0; i < ml; i++)
  if (!isalnum((unsigned char)p[i]) && p[i] != '_') return 0;
 memcpy(mac, p, ml);
 mac[ml] = 0;
 if (!mentions_feature_test(mac)) return 0;
 *neg = n;
 return 1;
}
/* Concrete requirement line, or the honest manual pointer, or nothing.
 * Shared by the text, markdown and json renderers so the three formats
 * cannot disagree. Returns 1 when a line was emitted. */
static int requirement_line(gbuf *o, const char *guard) {
 char mac[96];
 int neg = 0;
 if (guard_requirement(guard, mac, sizeof mac, &neg)) {
  if (!neg) {
   gbuf_adds(o, "Requires ");
   gbuf_adds(o, mac);
   gbuf_adds(o, ": define it before any header (e.g. -D");
   gbuf_adds(o, mac);
   gbuf_adds(o, "); see feature_test_macros(7).\n");
  } else {
   gbuf_adds(o, "Unavailable when ");
   gbuf_adds(o, mac);
   gbuf_adds(o, " is defined; see feature_test_macros(7).\n");
  }
  return 1;
 }
 if (mentions_feature_test(guard)) {
  gbuf_adds(o, "Possible feature-test requirement: see feature_test_macros(7); guards above are quoted, not evaluated.\n");
  return 1;
 }
 return 0;
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
static void parameters_to_gbuf(const char *declparams, const char *docform, gbuf *out);
static void parameters_block(search *s) {
 parameters_to_gbuf(s->declparams, s->docform, &s->res->text);
}
static void parameters_to_gbuf(const char *declparams, const char *docform, gbuf *out) {
 if (!declparams[0]) return;
 if (!docform[0]) {
  gbuf_adds(out,"PARAMETERS: no documented signature in local excerpts; parameter names are exactly as written in the declaration.\n");
  return;
 }
 const char *df=strchr(docform,'(');
 if (!df) {
  gbuf_adds(out,"PARAMETERS: no documented signature in local excerpts; parameter names are exactly as written in the declaration.\n");
  return;
 }
 if (stripped_equal(declparams,df)) {
  gbuf_adds(out,"PARAMETERS: header and documented forms agree.\n");
  return;
 }
 static char dh[12][160], dd[12][160];
 int nh=split_params(declparams,dh,12), nd=split_params(df,dd,12);
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
 gbuf_adds(out,"PARAMETERS:\n  header '"); gbuf_adds(out,declparams);
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
int sysref_api(const char *query, int semantics, int page, int per_page, int format, result *res) {
 char q[SYSREF_MAX_QUERY_BYTES+1]; snprintf(q,sizeof q,"%s",query);
 char *save=NULL,*first=strtok_r(q," \t\n",&save),*second=strtok_r(NULL," \t\n",&save),*extra=strtok_r(NULL," \t\n",&save);
 res->format=format;
 res->query=query;
 snprintf(res->verb,sizeof res->verb,"%s",semantics?"api":"headers");
 if(!first || extra) { gbuf_adds(&res->text,"Use [header.h] symbol (one C identifier).\n"); res->status=1; return 1; }
 const char *sym=second?second:first;
 int named=second || strchr(first,'.') || strchr(first,'/');
 if((second || !named) && !symbol_ok(sym)) { gbuf_adds(&res->text,"Invalid C symbol.\n"); res->status=1; return 1; }
 if(named && (first[0]=='/' || strstr(first,".."))) { gbuf_adds(&res->text,"Header must be relative to an include root.\n"); res->status=1; return 1; }
 if (per_page < 1) per_page = API_PAGE_SIZE;
 if (per_page > API_MAX_PER_PAGE) per_page = API_MAX_PER_PAGE;
 sysref_excerpt *rankbuf = (sysref_excerpt*)calloc(SYSREF_RANK_MAX, sizeof *rankbuf);
 if (!rankbuf) { gbuf_adds(&res->text,"internal: ranking buffer unavailable.\n"); res->status=1; return 1; }
  search s={.symbol=sym,.res=res,.page=(page<1?1:page),.ppsize=per_page,.rank=rankbuf,.nrank=0,.rankcap=SYSREF_RANK_MAX};
  res->page=s.page;
  res->total_pages=1;
  res->per_page=per_page;
  snprintf(res->symbol,sizeof res->symbol,"%s",sym);
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
  if (s.matches && s.kind==2) gbuf_adds(&res->text,"KIND: function-like macro; arguments may evaluate more than once; its address cannot be taken.\n");
  else if (s.matches && s.kind==3) gbuf_adds(&res->text,"KIND: object-like macro; not a function; its address cannot be taken.\n");
  else if (s.matches && s.kind==1) {
   gbuf_adds(&res->text,"KIND: function declaration");
   if (s.k_static || s.k_inline || s.k_extern) {
    gbuf_adds(&res->text," (declared");
    if (s.k_static) gbuf_adds(&res->text," static");
    if (s.k_inline) gbuf_adds(&res->text," inline");
    if (s.k_extern) gbuf_adds(&res->text," extern");
    gbuf_adds(&res->text," in header)");
   }
   gbuf_addch(&res->text,'\n');
  }
   if (s.matches && s.guard[0]) {
    gbuf_adds(&res->text,"GUARDED BY: "); gbuf_adds(&res->text,s.guard); gbuf_addch(&res->text,'\n');
    requirement_line(&res->text, s.guard);
   }
   /* Declared location and owning package first: the agent sees what was
    * found and where before the longer documentation excerpts. Ownership
    * is established once here and reused for prose-match suppression. */
   if(semantics && s.matches) {
    char ln[32];
    snprintf(ln,sizeof ln,":%d\n",s.first_line);
    gbuf_adds(&res->text,"HEADER: "); gbuf_adds(&res->text,s.first); gbuf_adds(&res->text,ln);
    s.pkg_known = header_owner(s.first,s.pkg,sizeof s.pkg);
    if(s.pkg_known) { gbuf_adds(&res->text,"PACKAGE: "); gbuf_adds(&res->text,s.pkg); gbuf_addch(&res->text,'\n'); }
    else gbuf_adds(&res->text,"PACKAGE: unknown from local metadata.\n");
   }
  if(semantics) {
   /* Sibling names are harvested once the declaring header is known, so
    * every excerpt is scored against the same API family. */
   if (s.first[0]) collect_siblings(&s);
   s.files=0; s.readbytes=0;
  const char *dr=root("SYSREF_DOC_PATH","/usr/share/doc:/usr/local/share/doc:/usr/share/info:/usr/share/man");
  rs=strdup(dr); sp=NULL;
  for(r=strtok_r(rs,":",&sp);r;r=strtok_r(NULL,":",&sp)) walk(&s,r,0,docs_visit);
  free(rs);
   /* Same-package matches are appended by a second pass, but only when the
    * first pass left the requested page unfilled: a full flood page stays
    * foreign/unverified, later pages may mix. */
   if(s.docs < s.page*s.ppsize && s.candidates>s.docs) {
    s.docpass=1; s.files=0; s.readbytes=0;
    rs=strdup(dr); sp=NULL;
    for(r=strtok_r(rs,":",&sp);r;r=strtok_r(NULL,":",&sp)) walk(&s,r,0,docs_visit);
    free(rs);
   }
   {
   int total = (s.docs + s.ppsize - 1) / s.ppsize;
   if (total < 1) total = 1;
   res->total_pages = total;
    if (s.page > total) {
     char nb[96];
     snprintf(nb, sizeof nb, "page %d out of range (1-%d).\n", s.page, total);
     gbuf_adds(&res->text, nb);
     res->status = 1;
    } else {
     char nb[96];
     snprintf(nb, sizeof nb, "page=%d\ntotal_pages=%d\n", s.page, total);
     gbuf_adds(&res->text, nb);
     if (s.page < total) {
      snprintf(nb, sizeof nb, "next_page=%d\n", s.page+1);
      gbuf_adds(&res->text, nb);
     }
    }
   }
    if(s.docs > s.page*s.ppsize) { char mb[256]; snprintf(mb, sizeof mb, "Additional documentation matches on later pages; use page=%d for more. For full package context use source=doc with the named package.\n", s.page+1); gbuf_adds(&res->text, mb); }
    else if(s.docs>s.ppsize) gbuf_adds(&res->text,"Additional documentation matches omitted; use source=doc with the named package for more context.\n");
/* Ranked emission: one ordering feeds excerpts and lookahead. The window
 * quotes the current page in rank order; lead paths name the first file
 * of each later page so the agent can jump instead of paging linearly. */
   if (s.nrank > 1) qsort(s.rank, (size_t)s.nrank, sizeof *s.rank, excerpt_rank);
   { sysref_excerpt *vd = verdict_pick(res->verb, s.rank, s.nrank);
     if (vd) {
      gbuf_adds(&res->text, "Best match: ");
      gbuf_adds(&res->text, vd->path);
      gbuf_addch(&res->text, '\n');
     }
   }
   int quoted = 0;
   { int w0=(s.page-1)*s.ppsize, w1=w0+s.ppsize;
     if (w0 < s.nrank) {
      if (w1 > s.nrank) w1 = s.nrank;
      for (int wi=w0; wi<w1; wi++) {
       sysref_excerpt *er=&s.rank[wi];
       ref(&s.res->text,"DOCUMENT: ",er->path,er->ln);
       gbuf_adds(&s.res->text,er->owner
        ? "LOCAL EXCERPT (verbatim; same installed package/version as header):\n"
        : "LOCAL EXCERPT (verbatim; applicability/version not established):\n");
       if(!er->over) excerpt_add(&s.res->text,er->text,er->len,er->html);
       else gbuf_adds(&s.res->text,"Matching line exceeds excerpt budget; inspect source.\n");
       gbuf_adds(&s.res->text,"\n");
       quoted++;
      }
     } else if (s.docs > s.nrank) {
      gbuf_adds(&s.res->text,"(this page lists matches beyond the ranked excerpt buffer; narrow the query or use source=doc.)\n");
     }
   }
   if (s.docs > s.nrank) { char ob[128]; snprintf(ob,sizeof ob,"Omitted %d match(es) beyond the ranked excerpt buffer (counted above); narrow the query.\n",s.docs-s.nrank); gbuf_adds(&s.res->text,ob); }
   { int nlead=0, lastp=0;
     gbuf lb; gbuf_init(&lb);
     for (int li=0; li<s.nrank && nlead<8; li++) {
      int lp=li/s.ppsize+1;
      if (lp<=s.page || lp==lastp) continue;
      lastp=lp;
      char one[1100]; snprintf(one,sizeof one," page %d: %s;",lp,s.rank[li].path);
      gbuf_adds(&lb,one); nlead++;
     }
     if (nlead>0) { gbuf_adds(&s.res->text,"Later pages lead with:"); gbuf_addn(&s.res->text,lb.p,lb.len); gbuf_addch(&s.res->text,'\n'); }
     gbuf_free(&lb);
   }
   /* Lifecycle block: concise evidence-first summary. A signature alone
    * cannot establish argument meaning; the agent must see whether
    * authoritative local documentation exists before inferring behaviour. */
     if(!s.docs) gbuf_adds(&res->text,"DOCUMENTATION: unavailable in searched files (not proof that a documentation package exists).\n");
     else if (s.page > 1 || s.docs > s.ppsize) { char dn[96]; snprintf(dn,sizeof dn,"DOCUMENTATION: %d local excerpt(s) total, %d quoted on this page.\n",s.docs,quoted); gbuf_adds(&res->text,dn); }
     else { char dn[96]; snprintf(dn,sizeof dn,"DOCUMENTATION: %d local excerpt(s) quoted above.\n",s.docs); gbuf_adds(&res->text,dn); }
     if(s.skipped>0) { char sb[160]; snprintf(sb,sizeof sb,"Omitted %d low-relevance match(es) (prose-only from other packages); narrow with source=doc and the package name.\n",s.skipped); gbuf_adds(&res->text,sb); }
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
   if (res->status) gbuf_adds(&res->text,"Not a C declaration? For commands use the man tool; for package docs the doc tool (--source doc).\n");
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
 /* Sidecar handoff: the ranked buffer outlives the search and belongs to
  * the result now; renderers (markdown/json) read the same evidence the
  * text path just quoted. result_free releases it. */
 res->docs=rankbuf;
 res->ndocs=s.nrank;
 res->docs_total=s.docs;
 res->docs_skipped=s.skipped;
 res->kind=s.kind; res->k_static=s.k_static; res->k_inline=s.k_inline; res->k_extern=s.k_extern;
 snprintf(res->guard,sizeof res->guard,"%s",s.guard);
 snprintf(res->header,sizeof res->header,"%s",s.first);
 res->header_line=s.first_line;
 snprintf(res->pkg,sizeof res->pkg,"%s",s.pkg);
 res->pkg_known=s.pkg_known;
 snprintf(res->declparams,sizeof res->declparams,"%s",s.declparams);
 snprintf(res->docform,sizeof res->docform,"%s",s.docform);
 res->matches=s.matches;
 return res->status?1:0;
}

/* ------------------------------------------------------------------ */
/* alternate renderings for api/headers: same sidecar, markdown/json  */
/* ------------------------------------------------------------------ */
/* Both renderers read the sidecar the text path collected (decls, rank
 * buffer, scalars), so text, markdown and json cannot disagree. */
static const char *kind_name(int kind) {
	switch (kind) {
	case 1: return "function";
	case 2: return "function-like macro";
	case 3: return "object-like macro";
	default: return NULL;
	}
}

/* Ranked window actually emitted for the requested page. */
static void api_window(result *res, int *w0, int *w1) {
	int a = (res->page - 1) * res->per_page, b = a + res->per_page;
	if (a < 0) a = 0;
	if (a > res->ndocs) a = res->ndocs;
	if (b > res->ndocs) b = res->ndocs;
	*w0 = a;
	*w1 = b;
}

static void render_api_md(result *res, gbuf *o) {
	char head[320];
	if (res->symbol[0]) {
		snprintf(head, sizeof head, "# `%s`\n\n", res->symbol);
		gbuf_adds(o, head);
	} else {
		gbuf_adds(o, "# ");
		gbuf_adds(o, res->query ? res->query : "sysref");
		gbuf_adds(o, "\n\n");
		gbuf_addn(o, res->text.p ? res->text.p : "", res->text.len);
		return;
	}
	int i;
	if (res->ndecls > 0) {
		gbuf_adds(o, "## Declaration\n\n");
		for (i = 0; i < res->ndecls; i++) {
			sysref_decl *d = &res->decls[i];
			char sl[1100];
			snprintf(sl, sizeof sl, "SOURCE: %s:%d\n", d->path, d->ln);
			gbuf_adds(o, sl);
			if (d->over) {
				gbuf_adds(o, "DECLARATION exceeds 1500 bytes; inspect source at this line.\n\n");
			} else {
				gbuf_adds(o, "```c\n");
				gbuf_adds(o, d->text);
				if (d->text[0] && d->text[strlen(d->text) - 1] != '\n') gbuf_addch(o, '\n');
				gbuf_adds(o, "```\n\n");
			}
		}
	}
	if (res->matches > 0 && kind_name(res->kind)) {
		const char *kn = kind_name(res->kind);
		if (res->kind == 1) {
			gbuf_adds(o, "KIND: function declaration");
			if (res->k_static || res->k_inline || res->k_extern) {
				gbuf_adds(o, " (declared");
				if (res->k_static) gbuf_adds(o, " static");
				if (res->k_inline) gbuf_adds(o, " inline");
				if (res->k_extern) gbuf_adds(o, " extern");
				gbuf_adds(o, " in header)");
			}
			gbuf_addch(o, '\n');
		} else if (res->kind == 2) {
			(void)kn;
			gbuf_adds(o, "KIND: function-like macro; arguments may evaluate more than once; its address cannot be taken.\n");
		} else {
			(void)kn;
			gbuf_adds(o, "KIND: object-like macro; not a function; its address cannot be taken.\n");
		}
	}
	if (res->matches > 0 && res->guard[0]) {
		gbuf_adds(o, "GUARDED BY: ");
		gbuf_adds(o, res->guard);
		gbuf_addch(o, '\n');
		requirement_line(o, res->guard);
	}
	if (!strcmp(res->verb, "api") && res->matches > 0) {
		char hl[32];
		snprintf(hl, sizeof hl, ":%d\n", res->header_line);
		gbuf_adds(o, "HEADER: ");
		gbuf_adds(o, res->header);
		gbuf_adds(o, hl);
		if (res->pkg_known) {
			gbuf_adds(o, "PACKAGE: ");
			gbuf_adds(o, res->pkg);
			gbuf_addch(o, '\n');
		} else {
			gbuf_adds(o, "PACKAGE: unknown from local metadata.\n");
		}
		gbuf_addch(o, '\n');
	}
	if (!strcmp(res->verb, "api")) {
		int w0, w1;
		api_window(res, &w0, &w1);
		gbuf_adds(o, "## Documentation\n\n");
		{ sysref_excerpt *vd = verdict_pick(res->verb, res->docs, res->ndocs);
		  if (vd) {
		   gbuf_adds(o, "**Best match:** ");
		   gbuf_adds(o, vd->path);
		   gbuf_adds(o, "\n\n");
		  }
		}
		for (i = w0; i < w1; i++) {
			sysref_excerpt *er = &res->docs[i];
			char dl[1100];
			snprintf(dl, sizeof dl, "### %s:%d\n\n", er->path, er->ln);
			gbuf_adds(o, dl);
			gbuf_adds(o, er->owner
			         ? "_LOCAL EXCERPT (verbatim; same installed package/version as header)_\n\n"
			         : "_LOCAL EXCERPT (verbatim; applicability/version not established)_\n\n");
			if (er->over) {
				gbuf_adds(o, "Matching line exceeds excerpt budget; inspect source.\n\n");
			} else {
				gbuf_adds(o, "```\n");
				gbuf_addn(o, er->text, er->len);
				if (er->len && er->text[er->len - 1] != '\n') gbuf_addch(o, '\n');
				gbuf_adds(o, "```\n\n");
			}
		}
		render_nav_md(o, res->page, res->total_pages);
		gbuf_addch(o, '\n');
		if (!res->docs_total) {
			gbuf_adds(o, "DOCUMENTATION: unavailable in searched files (not proof that a documentation package exists).\n");
		} else if (res->page > 1 || res->docs_total > res->per_page) {
			char dn[96];
			snprintf(dn, sizeof dn, "DOCUMENTATION: %d local excerpt(s) total, %d quoted on this page.\n",
			         res->docs_total, w1 - w0);
			gbuf_adds(o, dn);
		}
		if (res->docs_total > res->ndocs) {
			char ob[128];
			snprintf(ob, sizeof ob, "Omitted %d match(es) beyond the ranked excerpt buffer (counted above); narrow the query.\n",
			         res->docs_total - res->ndocs);
			gbuf_adds(o, ob);
		}
		if (res->docs_skipped > 0) {
			char sb[192];
			snprintf(sb, sizeof sb, "Omitted %d low-relevance match(es) (prose-only from other packages); narrow with source=doc and the package name.\n",
			         res->docs_skipped);
			gbuf_adds(o, sb);
		}
		if (res->matches > 0) {
			gbuf tmp;
			gbuf_init(&tmp);
			parameters_to_gbuf(res->declparams, res->docform, &tmp);
			gbuf_addn(o, tmp.p ? tmp.p : "", tmp.len);
			gbuf_free(&tmp);
		}
		if (res->docs_total)
			gbuf_adds(o, "ARGUMENTS: described only where quoted above; no separate parameter table inferred from types.\n");
		else
			gbuf_adds(o, "ARGUMENTS: unavailable -- no local documentation names the parameters; do not infer from types.\n");
		if (res->matches > 0)
			gbuf_adds(o, "STATUS: declaration found.\n");
		else
			gbuf_adds(o, "STATUS: no declaration in searched headers.\n");
		if (res->docs_total)
			gbuf_adds(o, "Detailed local manual: quoted above; only quoted text is evidence.\n");
		else
			gbuf_adds(o, "Detailed local manual: unavailable.\n");
	}
}

static void render_api_json(result *res, gbuf *o) {
	sysref_json_head(o, res);
	int i, w0, w1;
	api_window(res, &w0, &w1);
	gbuf_adds(o, "{\"symbol\":");
	json_escape(o, res->symbol, strlen(res->symbol));
	gbuf_adds(o, ",\"header\":");
	if (res->header[0]) {
		char ln[32];
		snprintf(ln, sizeof ln, "%d", res->header_line);
		gbuf_adds(o, "{\"path\":");
		json_escape(o, res->header, strlen(res->header));
		gbuf_adds(o, ",\"line\":");
		gbuf_adds(o, ln);
		gbuf_adds(o, "}");
	} else {
		gbuf_adds(o, "null");
	}
	gbuf_adds(o, ",\"declarations\":[");
	for (i = 0; i < res->ndecls; i++) {
		sysref_decl *d = &res->decls[i];
		char ln[32];
		snprintf(ln, sizeof ln, "%d", d->ln);
		if (i) gbuf_adds(o, ",");
		gbuf_adds(o, "{\"source\":{\"path\":");
		json_escape(o, d->path, strlen(d->path));
		gbuf_adds(o, ",\"line\":");
		gbuf_adds(o, ln);
		if (d->over) {
			gbuf_adds(o, "},\"truncated\":true}");
		} else {
			gbuf_adds(o, "},\"text\":");
			json_escape(o, d->text, strlen(d->text));
			gbuf_adds(o, "}");
		}
	}
	gbuf_adds(o, "],\"kind\":");
	const char *kn = kind_name(res->kind);
	if (res->matches > 0 && kn) {
		gbuf_adds(o, "{\"category\":");
		json_escape(o, kn, strlen(kn));
		char kb[96];
		snprintf(kb, sizeof kb, ",\"static\":%s,\"inline\":%s,\"extern\":%s,\"guard\":",
		         res->k_static ? "true" : "false", res->k_inline ? "true" : "false",
		         res->k_extern ? "true" : "false");
		gbuf_adds(o, kb);
		if (res->guard[0]) json_escape(o, res->guard, strlen(res->guard));
		else gbuf_adds(o, "null");
		gbuf_adds(o, ",\"requirement\":");
		if (res->guard[0]) {
			gbuf rq;
			gbuf_init(&rq);
			if (requirement_line(&rq, res->guard)) json_escape(o, rq.p ? rq.p : "", rq.len);
			else gbuf_adds(o, "null");
			gbuf_free(&rq);
		} else {
			gbuf_adds(o, "null");
		}
		gbuf_adds(o, "}");
	} else {
		gbuf_adds(o, "null");
	}
	gbuf_adds(o, ",\"package\":");
	if (res->pkg_known) {
		gbuf_adds(o, "{\"owner\":");
		json_escape(o, res->pkg, strlen(res->pkg));
		gbuf_adds(o, "}");
	} else {
		gbuf_adds(o, "null");
	}
	gbuf_adds(o, ",\"documentation\":");
	if (!strcmp(res->verb, "api")) {
		char nb[128];
		snprintf(nb, sizeof nb, "{\"total\":%d,\"quoted\":%d,\"omitted_low_relevance\":%d,\"excerpts\":[",
		         res->docs_total, w1 - w0, res->docs_skipped);
		gbuf_adds(o, nb);
		for (i = w0; i < w1; i++) {
			sysref_excerpt *er = &res->docs[i];
			char ln[32];
			snprintf(ln, sizeof ln, "%d", er->ln);
			if (i > w0) gbuf_adds(o, ",");
			gbuf_adds(o, "{\"path\":");
			json_escape(o, er->path, strlen(er->path));
			gbuf_adds(o, ",\"line\":");
			gbuf_adds(o, ln);
			gbuf_adds(o, ",\"applicability\":");
			gbuf_adds(o, er->owner ? "\"same-package\"" : "\"unverified\"");
			if (er->over) {
				gbuf_adds(o, ",\"truncated\":true}");
			} else {
				gbuf_adds(o, ",\"text\":");
				json_escape(o, er->text, er->len);
				gbuf_adds(o, "}");
			}
		}
		gbuf_adds(o, "],\"rank_buffered\":");
		snprintf(nb, sizeof nb, "%d", res->ndocs);
		gbuf_adds(o, nb);
		gbuf_adds(o, "}");
	} else {
		gbuf_adds(o, "null");
	}
	gbuf_adds(o, ",\"parameters\":");
	if (!strcmp(res->verb, "api")) {
		gbuf_adds(o, "{\"header_form\":");
		if (res->declparams[0]) json_escape(o, res->declparams, strlen(res->declparams));
		else gbuf_adds(o, "null");
		gbuf_adds(o, ",\"documented_form\":");
		if (res->docform[0]) json_escape(o, res->docform, strlen(res->docform));
		else gbuf_adds(o, "null");
		gbuf_adds(o, "}");
	} else {
		gbuf_adds(o, "null");
	}
	char nb[128];
	snprintf(nb, sizeof nb, ",\"status\":{\"declaration_found\":%s,\"documentation_found\":%s}",
	         res->matches > 0 ? "true" : "false", res->docs_total > 0 ? "true" : "false");
	gbuf_adds(o, nb);
	gbuf_adds(o, ",\"verdict\":");
	{ sysref_excerpt *vd = verdict_pick(res->verb, res->docs, res->ndocs);
	  if (vd) {
	   char ln[32];
	   snprintf(ln, sizeof ln, "%d", vd->ln);
	   gbuf_adds(o, "{\"path\":");
	   json_escape(o, vd->path, strlen(vd->path));
	   gbuf_adds(o, ",\"line\":");
	   gbuf_adds(o, ln);
	   gbuf_adds(o, "}");
	  } else {
	   gbuf_adds(o, "null");
	  }
	 }
	 gbuf_adds(o, "}}");
}

void sysref_render_api(result *res, gbuf *o) {
	if (res->format == SYSREF_FMT_MARKDOWN) render_api_md(res, o);
	else render_api_json(res, o);
}
