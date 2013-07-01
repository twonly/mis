#define FUSE_USE_VERSION 26
#include "client.h"

int min(int a,int b){
  return a<b?a:b;
}

void sendpacket(int sockfd,ppacket* p){
  int i;
  int len = 0;

  while(p->bytesleft){
    i = write(sockfd,p->startptr,p->bytesleft);
    if(i < 0){
      if(i != -EAGAIN){
        fprintf(stderr,"write error:%d\n",i);
        exit(1);
      }
      continue;
    }
    if(i==0){
      fprintf(stderr,"Connection closed\n");
      exit(1);
    }

    len += i;
    p->startptr += i;
    p->bytesleft -= i;
  }

  printf("%d bytes sent\n",len);
}

ppacket* receivepacket(int sockfd){
  char headbuf[20];
  uint8_t* startptr;
  const uint8_t* ptr;
  int i,hleft;
  ppacket* p;

  hleft = HEADER_LEN;
  startptr = headbuf;
  while(hleft){
    i = read(sockfd,startptr,hleft);
    if(i < 0){
      if(i != -EAGAIN){
        fprintf(stderr,"read error:%d\n",i);
        exit(1);
      }
      continue;
    }
    if(i==0){
      fprintf(stderr,"Connection closed\n");
      exit(1);
    }

    hleft -= i;
    startptr += i;
  }

  int size,cmd;
  ptr = headbuf;
  size = get32bit(&ptr);
  cmd = get32bit(&ptr);

  printf("got packet:size=%d,cmd=%X\n",size,cmd);
  p = createpacket_r(size,cmd,1);

  while(p->bytesleft){
    i = read(sockfd,p->startptr,p->bytesleft);
    if(i < 0){
      if(i != -EAGAIN){
        fprintf(stderr,"read error:%d\n",i);
        exit(1);
      }
      continue;
    }
    if(i==0){
      fprintf(stderr,"Connection closed\n");
      exit(1);
    }

    p->bytesleft -= i;
    p->startptr += i;
  }

  p->startptr = p->buf;

  return p;
}

void print_attr(const attr* a){
  if(a->mode & S_IFDIR){
    fprintf(stderr,"\tdirectory\n");
  } else if(a->mode & S_IFREG){
    fprintf(stderr,"\tregular file\n");
  }

  fprintf(stderr,"\tmode=%o\n",a->mode);

  fprintf(stderr,"\tperm:%d%d%d\n",a->mode / 0100 & 7,
      a->mode / 0010 & 7,
      a->mode & 7);

  fprintf(stderr,"\tuid=%d,gid=%d",a->uid,a->gid);
  fprintf(stderr,"\tatime=%d,ctime=%d,mtime=%d\n",a->atime,a->ctime,a->mtime);
  fprintf(stderr,"\tlink=%d\n",a->link);
  fprintf(stderr,"\tsize=%d\n",a->size);
}

static struct fuse_operations ppfs_oper = {
  .init       = ppfs_fsinit, //connect to mds
  //.statfs		= ppfs_statfs,
  .getattr	= ppfs_getattr,
  //.mknod		= ppfs_mknod,
  .unlink		= ppfs_unlink,
  .mkdir		= ppfs_mkdir,
  .rmdir		= ppfs_rmdir,
  //.symlink	= ppfs_symlink,
  //.readlink	= ppfs_readlink,
  /*.rename		= ppfs_rename,*/
  .chmod		= ppfs_chmod,
  //.link		= ppfs_link,
  //.opendir	= ppfs_opendir,
  .readdir	= ppfs_readdir,
  //.releasedir	= ppfs_releasedir,
  .create		= ppfs_create, //replace mknod and open
  .open		= ppfs_open, //called before read
  .release	= ppfs_release,
  //.flush		= ppfs_flush,
  //.fsync		= ppfs_fsync,
  .read		= ppfs_read,
  .write		= ppfs_write,
  .truncate = ppfs_truncate,
  .access		= ppfs_access,
  .utimens  = ppfs_utimens,
};

typedef struct ppfs_conn_entry{
  int sockfd;
  int peerip;
} ppfs_conn_entry;

int fd;
ppfs_conn_entry local_mds,remote_mds;

int numip = 0x010000FE;
char *ip = "127.0.0.1";
char *port = "8224";

int serv_connect(ppfs_conn_entry* e,int ip,int port){
  int fd = tcpsocket();
  if(fd < 0){
    fprintf(stderr,"cannot create socket\n");
    return -1;
  }

  tcpnodelay(fd);
  if(tcpnumconnect(fd,ip,port) < 0){
    fprintf(stderr,"cannot connect to %X:%d\n",ip,port);
    return -1;
  }

  e->sockfd = fd;
	tcpgetpeer(fd,&(e->peerip),NULL);

  return fd;
}

void* ppfs_fsinit( struct fuse_conn_info* conn ) { //connect to MDS
  fprintf(stderr, "ppfs_fsinit\n");
  if((fd = tcpsocket())<0) {
    fprintf(stderr, "can't create socket\n");
    exit(1);
  } else {
    fprintf(stderr, "fd:%d\n", fd);
  }

  tcpnodelay(fd);
  if(tcpstrconnect(fd, ip, MDS_PORT_STR)<0) {
    fprintf(stderr, "can't connect to MDS (%s:%s)\n", ip, port);
    fd = -1;

    exit(1);
  }

  fprintf(stderr, "ppfs_init create socket: %u\n", fd);

  local_mds.sockfd = fd;
	tcpgetpeer(fd,&(local_mds.peerip),NULL);

  remote_mds.sockfd = -1;
  remote_mds.peerip = -1;
}

int ppfs_getattr(const char* path, struct stat* stbuf){
  fprintf(stderr, "ppfs_getattr path : %s\n", path);

  ppacket *s = createpacket_s(4+strlen(path), CLTOMD_GETATTR,-1);
  fprintf(stderr, "createpacket_s packet size:%u, cmd:%X\n", s->size, s->cmd); //5,4096
  uint8_t* ptr = s->startptr+HEADER_LEN;

  put32bit(&ptr, strlen(path));
  memcpy(ptr, path, strlen(path));
  ptr += strlen(path);

  sendpacket(fd, s);
  free(s);

  s = receivepacket(fd);
  const uint8_t* ptr2 = s->startptr;
  int status = get32bit(&ptr2);
  fprintf(stderr,"status:%d\n",status);

  if(status == 0){
    print_attr((const attr*)ptr2);

    memset(stbuf, 0, sizeof(struct stat));
    const attr* a = (const attr*)ptr2;

    stbuf->st_mode = a->mode; //S_IFREG | 0755;
    stbuf->st_nlink = a->link;
    if(stbuf->st_mode & S_IFDIR )
      stbuf->st_size = 4096;
    else
      stbuf->st_size = a->size;

    stbuf->st_ctime = a->ctime;
    stbuf->st_atime = a->atime;
    stbuf->st_mtime = a->mtime;
  }
  free(s);
  return status;
} //always called before open

int ppfs_mkdir(const char* path, mode_t mt){
  fprintf(stderr, "ppfs_mkdir path : %s\n", path);
  mt |= S_IFDIR;

  ppacket* p = createpacket_s(4+strlen(path)+4,CLTOMD_MKDIR,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;

  put32bit(&ptr,strlen(path));
  memcpy(ptr,path,strlen(path));
  ptr += strlen(path);
  put32bit(&ptr, mt);

  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);

  fprintf(stderr, "mkdir status:%d\n", status);
  free(p);
  return status;
}

int ppfs_open(const char* path, struct fuse_file_info* fi){
  fprintf(stderr, "ppfs_open path : %s\n", path);

  int status;
  ppacket* s = createpacket_s(4+strlen(path),CLTOMD_OPEN,-1);
  uint8_t* ptr = s->startptr + HEADER_LEN;

  put32bit(&ptr,strlen(path));
  memcpy(ptr,path,strlen(path));
  sendpacket(fd,s);
  free(s);

  s = receivepacket(fd);
  const uint8_t* ptr2 = s->startptr;
  status = get32bit(&ptr2);
  free(s);

  return status;
}

int ppfs_release(const char* path,struct fuse_file_info* fi){
  return 0;
}

int ppfs_truncate(const char* path,off_t off){
  fprintf(stderr,"\n\n\n+ppfs_truncate\n\n\n");

  ppacket* p = createpacket_s(4+strlen(path)+4+4,CLTOMD_READ_CHUNK_INFO,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;
  int len = strlen(path);
  uint32_t ip;
  int chunks;

  put32bit(&ptr,len);
  memcpy(ptr,path,len);
  ptr += len;
  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);
  fprintf(stderr,"status:%d\n",status);
  if(status == 0){
    ip = get32bit(&ptr2);
    if(ip == -1){
      fprintf(stderr,"local mds\n");
    } else {
      fprintf(stderr,"remote mds:%X\n",ip);
    }

    chunks = get32bit(&ptr2);
    fprintf(stderr,"chunks=%d\n",chunks);
  } else {
    free(p);
    return status;
  }
  free(p);

  ppfs_conn_entry* e = NULL;
  if(ip != -1){
    if(remote_mds.sockfd != -1 && remote_mds.peerip != ip){
      tcpclose(remote_mds.sockfd);
      remote_mds.sockfd = -1;
    }

    if(remote_mds.sockfd == -1){
      if(serv_connect(&remote_mds,numip,MDS_PORT) < 0){
        return -1;
      }
    }

    e = &remote_mds;
  } else {
    e = &local_mds;
  }

  off_t size = chunks * CHUNKSIZE;

  p = createpacket_s(4+len,CLTOMD_POP_CHUNK,-1);
  ptr = p->startptr + HEADER_LEN;
  put32bit(&ptr,len);
  memcpy(ptr,path,len);
  while(size >= off + CHUNKSIZE){
    sendpacket(e->sockfd,p);

    ppacket* rp = receivepacket(e->sockfd);
    ptr2 = rp->startptr;
    status = get32bit(&ptr2);
    printf("status:%d\n",status);
    if(status != 0){
      return status;
    }
    free(rp);

    size -= CHUNKSIZE;
  }
  free(p);

  p = createpacket_s(4+len,CLTOMD_APPEND_CHUNK,-1);
  ptr = p->startptr + HEADER_LEN;
  put32bit(&ptr,len);
  memcpy(ptr,path,len);
  while(size < off){
    sendpacket(e->sockfd,p);

    ppacket* rp = receivepacket(e->sockfd);
    ptr2 = rp->startptr;
    status = get32bit(&ptr2);
    printf("status:%d\n",status);
    if(status != 0){
      return status;
    }
    free(rp);

    size += CHUNKSIZE;
  }
  free(p);

  return 0;
}

int ppfs_access (const char *path, int amode){
  fprintf(stderr, "ppfs_access path : %s\n", path);
  return ppfs_open(path,NULL); //temporary hack
}

int	ppfs_chmod (const char *path, mode_t mt){
  fprintf(stderr, "ppfs_chmod path : %s\n", path);

  ppacket *s = createpacket_s(4+strlen(path)+4, CLTOMD_CHMOD,1);
  uint8_t* ptr = s->startptr+HEADER_LEN;

  put32bit(&ptr, strlen(path));
  memcpy(ptr, path, strlen(path));
  ptr+=strlen(path);
  put32bit(&ptr, mt);

  sendpacket(fd, s);
  free(s);

  s = receivepacket(fd);
  const uint8_t* ptr2 = s->startptr;
  int status = get32bit(&ptr2);

  if(status == 0){
    print_attr((const attr*)ptr2);
  }
  free(s);

  return status;
}

int	ppfs_chown (const char *path, uid_t uid, gid_t gid){
  fprintf(stderr, "ppfs_chown path : %s\n", path);

  ppacket *s = createpacket_s(4+strlen(path)+8, CLTOMD_CHOWN, 1);
  uint8_t* ptr = s->startptr+HEADER_LEN;

  put32bit(&ptr, strlen(path));
  memcpy(ptr, path, strlen(path));
  ptr+=strlen(path);
  put32bit(&ptr, uid);
  put32bit(&ptr, gid);

  sendpacket(fd, s);
  free(s);

  s = receivepacket(fd);
  const uint8_t* ptr2 = s->startptr;
  int status = get32bit(&ptr2);

  if(status == 0){
    print_attr((const attr*)ptr2);
  }
  free(s);

  return status;
}

int	ppfs_create(const char *path, mode_t mt, struct fuse_file_info *fi){
  fprintf(stderr, "\n\n\n\nppfs_create path : %s\n", path);
  fprintf(stderr, "ppfs_create mode : %o\n\n\n\n\n", mt);

  ppacket* p = createpacket_s(4+strlen(path)+4,CLTOMD_CREATE,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;

  mt = (mt & 0777) | S_IFREG;

  put32bit(&ptr,strlen(path));
  memcpy(ptr,path,strlen(path));
  ptr += strlen(path);
  put32bit(&ptr,mt);

  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);

  fprintf(stderr, "create status:%d\n", status);
  free(p);

  return status;
}


int	ppfs_readdir (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi){
  fprintf(stderr, "ppfs_readdir path : %s\n", path);

  (void) offset;
  (void) fi;

  ppacket *s = createpacket_s(4+strlen(path)+4, CLTOMD_READDIR,-1);
  uint8_t* ptr = s->startptr+HEADER_LEN;

  put32bit(&ptr, strlen(path));
  memcpy(ptr, path, strlen(path));
  ptr+=strlen(path);
  put32bit(&ptr, offset);

  sendpacket(fd, s);
  free(s);

  s = receivepacket(fd);
  const uint8_t* ptr2 = s->startptr;
  int status = get32bit(&ptr2);

  if(status == 0){
    int nfiles = get32bit(&ptr2);
    int i;

    for(i=0;i<nfiles;++i) {
      int flen = get32bit(&ptr2);
      char * fn = (char*)malloc(flen*sizeof(char)+1);
      memcpy(fn, ptr2, flen);
      fn[flen] = 0;
      ptr2 += flen;

      filler(buf, fn, NULL, 0);
      free(fn);
    }
  } else {
    if(status == -ENOENT){
      fprintf(stderr,"\tENOENT\n");
    }
    if(status == -ENOTDIR){
      fprintf(stderr,"\tENOTDIR\n");
    }
  }
  free(s);
  return 0;
}

int	ppfs_rmdir (const char *path){
  fprintf(stderr, "ppfs_rmdir path : %s\n", path);
  ppacket* p = createpacket_s(4+strlen(path),CLTOMD_RMDIR,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;

  put32bit(&ptr,strlen(path));
  memcpy(ptr,path,strlen(path));
  ptr += strlen(path);

  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);

  fprintf(stderr, "rmdir status:%d\n", status);
  free(p);
  return status;
}

int	ppfs_unlink (const char *path){
  fprintf(stderr, "ppfs_unlink path : %s\n", path);

  ppacket* p = createpacket_s(4+strlen(path),CLTOMD_UNLINK,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;

  put32bit(&ptr,strlen(path));
  memcpy(ptr,path,strlen(path));
  ptr += strlen(path);

  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);

  fprintf(stderr, "unlink status:%d\n", status);

  free(p);
  return status;
}

int ppfs_utimens(const char* path,const struct timespec tv[2]){ //tv[0]: atime, tv[1] mtime
  ppacket* p = createpacket_s(4+strlen(path)+4+4,CLTOMD_UTIMENS,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;
  int len = strlen(path);

  put32bit(&ptr,len);
  memcpy(ptr,path,len);
  ptr += len;
  put32bit(&ptr,tv[0].tv_sec);
  put32bit(&ptr,tv[1].tv_sec);

  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);

  free(p);
  return status;
}

int	ppfs_read (const char * path, char * buf, size_t st, off_t off, struct fuse_file_info *fi){
  int nread = 0;
  int ooff = off;
  int ost = st;

  ppacket* p = createpacket_s(4+strlen(path),CLTOMD_READ_CHUNK_INFO,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;
  int plen = strlen(path);
  uint64_t* chunklist = NULL;
  int clen,calloc;
  char* rbuf = buf;

  put32bit(&ptr,plen);
  memcpy(ptr,path,plen);
  ptr += plen;
  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);
  fprintf(stderr,"status:%d\n",status);
  if(status == 0){
    uint32_t ip = get32bit(&ptr2);
    if(ip == -1){
      fprintf(stderr,"local mds\n");
    } else {
      fprintf(stderr,"remote mds:%X\n",ip);
    }

    int chunks = get32bit(&ptr2);
    fprintf(stderr,"chunks=%d\n",chunks);
    int i;

    chunklist = (uint64_t*)malloc(sizeof(uint64_t)*(chunks+20));
    clen = 0;
    calloc = chunks+20;

    for(i=0;i<chunks;i++){
      uint64_t chunkid = get64bit(&ptr2);
      fprintf(stderr,"(%d):id=%lld\n",i,chunkid);

      chunklist[clen++] = chunkid;
    }

    fprintf(stderr,"preparing mds connection\n");

    ppfs_conn_entry* e = NULL;
    if(ip != -1){
      if(remote_mds.sockfd != -1 && remote_mds.peerip != ip){
        tcpclose(remote_mds.sockfd);
        remote_mds.sockfd = -1;
      }

      if(remote_mds.sockfd == -1){
        if(serv_connect(&remote_mds,numip,MDS_PORT) < 0){
          return -1;
        }
      }

      e = &remote_mds;
    } else {
      e = &local_mds;
    }

    fprintf(stderr,"done\n");

    fprintf(stderr,"off=%d,st=%d\n",ooff,ost);

    if(chunks * CHUNKSIZE < off + st){
      return 0;
    }

    fprintf(stderr,"start reading now\n");

    int starti = off/CHUNKSIZE;
    int buflen = min(st,CHUNKSIZE - off % CHUNKSIZE);
    ppfs_conn_entry cs;
    cs.sockfd = -1;

    while(st > 0){
      uint64_t chunkid = chunklist[starti];

      ppacket* p = createpacket_s(8,CLTOMD_LOOKUP_CHUNK,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put64bit(&ptr,chunkid);
      sendpacket(e->sockfd,p);
      free(p);

      p = receivepacket(e->sockfd);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        int csip = get32bit(&ptr2);
        printf("cid:%lld,csip:%X\n",chunkid,csip);

        if(cs.sockfd != -1 && cs.peerip != csip){
          tcpclose(cs.sockfd);
        }

        if(cs.sockfd == -1){
          if(serv_connect(&cs,csip,CS_PORT) < 0){
            return -1;
          }
        }
      } else {
        return -1;
      }

      fprintf(stderr,"chunkid=%lld,off=%d,buflen=%d\n",chunkid,off,buflen);

      p = createpacket_s(8+4+4,CLTOCS_READ_CHUNK,-1);
      ptr = p->startptr + HEADER_LEN;
      put64bit(&ptr,chunkid);
      put32bit(&ptr,off % CHUNKSIZE);
      put32bit(&ptr,buflen);

      sendpacket(cs.sockfd,p);
      free(p);

      p = receivepacket(cs.sockfd);
      ptr2 = p->startptr;
      status = get32bit(&ptr2);
      printf("status=%d\n",status);
      if(status == 0){
        int rlen =  get32bit(&ptr2);
        nread += rlen;
        printf("rlen=%d\n",rlen);
        
        memcpy(rbuf,ptr2,rlen);
        rbuf += rlen;
      } else {
        return -1;
      }

      st -= buflen;
      off += buflen;

      starti = off/CHUNKSIZE;
      buflen = min(st,CHUNKSIZE - off % CHUNKSIZE);
    }
  } else {
    return -1;
  }

  return nread;
}

int	ppfs_write (const char *path, const char *buf, size_t st, off_t off, struct fuse_file_info *fi){
  fprintf(stderr,"\n\n\nppfs_write:%s,size:%d,offset:%d\n\n\n",path,st,off);

  int nwrite = 0;

  int ost,ooff;
  ost = st;
  ooff = off;

  ppacket* p = createpacket_s(4+strlen(path)+4+4,CLTOMD_READ_CHUNK_INFO,-1);
  uint8_t* ptr = p->startptr + HEADER_LEN;
  int plen = strlen(path);
  uint64_t* chunklist = NULL;
  int clen,calloc;
  const char* wbuf = buf;

  put32bit(&ptr,plen);
  memcpy(ptr,path,plen);
  ptr += plen;
  sendpacket(fd,p);
  free(p);

  p = receivepacket(fd);
  const uint8_t* ptr2 = p->startptr;
  int status = get32bit(&ptr2);
  fprintf(stderr,"status:%d\n",status);
  if(status == 0){
    uint32_t ip = get32bit(&ptr2);
    if(ip == -1){
      fprintf(stderr,"local mds\n");
    } else {
      fprintf(stderr,"remote mds:%X\n",ip);
    }

    int chunks = get32bit(&ptr2);
    fprintf(stderr,"chunks=%d\n",chunks);
    int i;

    chunklist = (uint64_t*)malloc(sizeof(uint64_t)*(chunks+20));
    clen = 0;
    calloc = chunks+20;

    for(i=0;i<chunks;i++){
      uint64_t chunkid = get64bit(&ptr2);
      fprintf(stderr,"(%d):id=%lld\n",i,chunkid);

      chunklist[clen++] = chunkid;
    }

    ppfs_conn_entry* e = NULL;
    if(ip != -1){
      if(remote_mds.sockfd != -1 && remote_mds.peerip != ip){
        tcpclose(remote_mds.sockfd);
        remote_mds.sockfd = -1;
      }

      if(remote_mds.sockfd == -1){
        if(serv_connect(&remote_mds,numip,MDS_PORT) < 0){
          return -1;
        }
      }

      e = &remote_mds;
    } else {
      e = &local_mds;
    }

    if(chunks * CHUNKSIZE <= off + st){
      fprintf(stderr,"appending chunk\n");
      while(chunks * CHUNKSIZE <= off + st){
        ppacket* p = createpacket_s(4+plen,CLTOMD_APPEND_CHUNK,-1);
        uint8_t* ptr = p->startptr + HEADER_LEN;
        put32bit(&ptr,plen);
        memcpy(ptr,path,plen);
        sendpacket(e->sockfd,p);
        free(p);

        ppacket* rp = receivepacket(e->sockfd);
        const uint8_t* ptr2 = rp->startptr;
        int status = get32bit(&ptr2);
        printf("status:%d\n",status);
        if(status == 0){
          uint64_t chunkid = get64bit(&ptr2);
          printf("chunkid=%lld\n",chunkid);

          if(clen < calloc){
            chunklist[clen++] = chunkid;
          } else {
            chunklist = (uint64_t*)realloc(chunklist,calloc<<1);
            chunklist[clen++] = chunkid;
          }
        } else {
          free(rp);
          return status;
        }
        free(rp);

        chunks++;
      }
    }

    fprintf(stderr,"chunklist now:\n");
    for(i=0;i<clen;i++){
      fprintf(stderr,"\t(%d):%lld\n",i,chunklist[i]);
    }

    int starti = off/CHUNKSIZE;
    int buflen = min(st,CHUNKSIZE - off % CHUNKSIZE);
    ppfs_conn_entry cs;
    cs.sockfd = -1;

    fprintf(stderr,"off=%d,st=%lld\n",off,st);

    while(st > 0){
      uint64_t chunkid = chunklist[starti];

      ppacket* p = createpacket_s(8,CLTOMD_LOOKUP_CHUNK,-1);
      uint8_t* ptr = p->startptr + HEADER_LEN;
      put64bit(&ptr,chunkid);
      sendpacket(e->sockfd,p);
      free(p);

      p = receivepacket(e->sockfd);
      const uint8_t* ptr2 = p->startptr;
      int status = get32bit(&ptr2);
      printf("status:%d\n",status);
      if(status == 0){
        int csip = get32bit(&ptr2);
        printf("cid:%lld,csip:%X\n",chunkid,csip);

        if(cs.sockfd != -1 && cs.peerip != csip){
          tcpclose(cs.sockfd);
        }

        if(cs.sockfd == -1){
          if(serv_connect(&cs,csip,CS_PORT) < 0){
            return -1;
          }
        }
      } else {
        return -1;
      }

      p = createpacket_s(8+4+4+buflen,CLTOCS_WRITE_CHUNK,-1);
      ptr = p->startptr + HEADER_LEN;
      put64bit(&ptr,chunkid);
      put32bit(&ptr,off % CHUNKSIZE);
      put32bit(&ptr,buflen);
      memcpy(ptr,wbuf,buflen);

      printf("STROKE!!!:%d\n",buflen);
      fprintf(stderr,"starti=%d,chunkid=%lld,off=%d,buflen=%d\n",starti,chunkid,off % CHUNKSIZE,buflen);

      sendpacket(cs.sockfd,p);
      free(p);

      p = receivepacket(cs.sockfd);
      ptr2 = p->startptr;
      status = get32bit(&ptr2);
      printf("status=%d\n",status);
      if(status == 0){
        int wlen =  get32bit(&ptr2);
        nwrite += wlen;
        printf("wlen=%d,nwrite=%d\n",wlen,nwrite);
        wbuf += wlen;
      }

      st -= buflen;
      off += buflen;

      starti = off/CHUNKSIZE;
      buflen = min(st,CHUNKSIZE - off % CHUNKSIZE);
    }
  } else {
    return status;
  }

  fprintf(stderr,"off=%d,st=%d,nwrite=%d\n",ooff,ost,nwrite);
  return nwrite;
}

int main(int argc, char* argv[]) {
  argv[argc++] = "-s";

  return fuse_main(argc, argv, &ppfs_oper, NULL);
}
