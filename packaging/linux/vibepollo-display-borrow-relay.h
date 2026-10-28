#ifndef VIBEPOLLO_DISPLAY_BORROW_RELAY_H
#define VIBEPOLLO_DISPLAY_BORROW_RELAY_H
/* Included after broker primitives. One authenticated connection owns exactly
 * one immutable generation/context. No RELEASE can name another connection. */
static bool display_borrow_send_ack(int socket_fd,uint64_t generation,const struct display_borrow_identity *token,int lock,int native_fd) {
  struct {struct vibepollo_session_message header;struct display_borrow_identity token;} frame={0};
  frame.header.magic=VIBEPOLLO_SESSION_PROTOCOL_MAGIC;frame.header.version=VIBEPOLLO_SESSION_PROTOCOL_VERSION;
  frame.header.type=DISPLAY_BORROW_ACK;frame.header.generation=generation;frame.header.payload_length=sizeof(*token);frame.token=*token;
  union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int)*2)];} control={0};
  struct iovec vector={.iov_base=&frame,.iov_len=sizeof(frame)};
  struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
  const int resources[2]={lock,native_fd};const size_t bytes=native_fd>=0?sizeof(resources):sizeof(lock);
  message.msg_controllen=CMSG_SPACE(bytes);
  struct cmsghdr *cmsg=CMSG_FIRSTHDR(&message);cmsg->cmsg_level=SOL_SOCKET;cmsg->cmsg_type=SCM_RIGHTS;cmsg->cmsg_len=CMSG_LEN(bytes);memcpy(CMSG_DATA(cmsg),resources,bytes);
  return sendmsg(socket_fd,&message,MSG_NOSIGNAL)==(ssize_t)sizeof(frame);
}
static int relay_display_borrow(int socket_fd,pid_t worker,int output_fd[2],
                               const struct session_identity *identity,gid_t service_gid,
                               const char *context,int mutation_lock) {
  struct display_borrow_identity token={.version=DISPLAY_BORROW_VERSION};
  memcpy(token.context,context,sizeof(token.context));
  char expected[37];snprintf(expected,sizeof(expected),"B2:%s\n",context);
  char ready[36];size_t ready_length=0;int native_fd=-1;
  bool acknowledged=worker==0,released=false,worker_exited=false,remote_unknown=false;
  if(acknowledged && (!identity_is_current(identity,service_gid) || !display_borrow_send_ack(socket_fd,identity->generation,&token,mutation_lock,-1))) {display_borrow_note_mutation_unknown(mutation_lock);return VIBEPOLLO_SESSION_COMPLETION_UNKNOWN;}
  int status=126<<8;uint64_t checked=monotonic_milliseconds(),started=checked;
  while (!released) {
    const uint64_t now=monotonic_milliseconds();
    if (!now || (!acknowledged && now-started>15000) ||
        (now-checked>=250 && !identity_is_current(identity,service_gid))) break;
    if (now-checked>=250) checked=now;
    struct pollfd watched[3]={{.fd=socket_fd,.events=POLLIN},{.fd=output_fd[0],.events=POLLIN},{.fd=output_fd[1],.events=POLLIN}};
    int polled=poll(watched,3,100);
    if (polled<0) { if(errno==EINTR)continue;break; }
    if (watched[0].revents&(POLLHUP|POLLERR|POLLNVAL)) break;
    if (watched[0].revents&POLLIN) {
      struct {struct vibepollo_session_message header;struct display_borrow_identity token;} message;
      struct iovec v={.iov_base=&message,.iov_len=sizeof(message)};struct msghdr m={.msg_iov=&v,.msg_iovlen=1};
      const ssize_t got=recvmsg(socket_fd,&m,MSG_DONTWAIT);
      if(got==0)break;
      if(got==(ssize_t)sizeof(message) && !(m.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) &&
         message.header.magic==VIBEPOLLO_SESSION_PROTOCOL_MAGIC && message.header.version==VIBEPOLLO_SESSION_PROTOCOL_VERSION &&
         message.header.type==DISPLAY_BORROW_RELEASE && message.header.generation==identity->generation &&
         !message.header.argument_count && (message.header.status==0 || (worker==0 && message.header.status==VIBEPOLLO_SESSION_COMPLETION_UNKNOWN)) && !message.header.reserved &&
         message.header.payload_length==sizeof(token) && display_borrow_identity_matches(&message.token,&token)) {released=true;remote_unknown=message.header.status!=0;}
      // Stale/foreign context messages never revoke this live context.
    }
    if(output_fd[1]>=0 && watched[2].revents&(POLLIN|POLLHUP|POLLERR)) {
      if(!forward_output(socket_fd,&output_fd[1],VIBEPOLLO_SESSION_STDERR,identity->generation))break;
    }
    if(output_fd[0]>=0 && watched[1].revents&(POLLIN|POLLHUP|POLLERR)) {
      union {struct cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int)*4)];} control={0};
      struct iovec vector={.iov_base=ready,.iov_len=sizeof(ready)};
      struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control.bytes,.msg_controllen=sizeof(control.bytes)};
      const ssize_t n=recvmsg(output_fd[0],&message,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
      unsigned resources=0;
      for(struct cmsghdr *c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
          const unsigned count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);int *fds=(int*)CMSG_DATA(c);
          for(unsigned i=0;i<count;++i){++resources;if(native_fd<0)native_fd=fds[i];else close(fds[i]);}
        }
      }
      struct stat resource={0};struct ucred peer={0};socklen_t peer_size=sizeof(peer);
      if(n!=(ssize_t)sizeof(ready) || acknowledged || resources!=1 || native_fd<0 ||
         (message.msg_flags&(MSG_TRUNC|MSG_CTRUNC)) || fstat(native_fd,&resource) || !S_ISSOCK(resource.st_mode) ||
         getsockopt(native_fd,SOL_SOCKET,SO_PEERCRED,&peer,&peer_size) || peer_size!=sizeof(peer) || peer.uid!=identity->uid ||
         memcmp(ready,expected,sizeof(ready)) || !identity_is_current(identity,service_gid))break;
      ready_length=(size_t)n;
      if(!display_borrow_send_ack(socket_fd,identity->generation,&token,mutation_lock,native_fd))break;
      acknowledged=true;
    }
    if(worker!=0 && waitpid(worker,&status,WNOHANG)==worker) {worker_exited=true;break;}
  }
  if(worker!=0 && !worker_exited && waitpid(worker,&status,WNOHANG)==worker)worker_exited=true;
  if(worker!=0 && !worker_exited) {
    // Only an exact authenticated host RELEASE permits the helper to release
    // its native inhibitor. Unknown completion kills without running cleanup;
    // the host owns the duplicate bus connection through its retained captures.
    const bool authorized=released && acknowledged && write(output_fd[0],"R",1)==1;
    if(authorized) {
      const uint64_t now=monotonic_milliseconds();
      worker_exited=now && reap_child_until(worker,now+3000,&status);
    }
    if(!worker_exited) {
      (void)terminate_worker(worker,&worker_exited,&status,true);
      remote_unknown=true;
    }
    if(!authorized)remote_unknown=true;
  }
  if(native_fd>=0)close(native_fd);
  if(output_fd[0]>=0)close(output_fd[0]);if(output_fd[1]>=0)close(output_fd[1]);
  if(!acknowledged && !ready_length && worker_exited && WIFEXITED(status) && WEXITSTATUS(status)==126) {
    // The producer confirmed it exited before publishing or ACKing a native
    // resource, so the write-ahead marker can be retired safely.
    (void)display_borrow_clear_uncertainty(mutation_lock);
    (void)send_frame(socket_fd,VIBEPOLLO_SESSION_EXIT,identity->generation,126,NULL,0);
    return 126; // Unsupported/invalid old producer never acquired or ACKed.
  }
  bool clean=released && acknowledged && !remote_unknown && (worker==0 || (worker_exited && WIFEXITED(status) && WEXITSTATUS(status)==0));
  if(clean) {
    // Report uncertainty if retiring the write-ahead record cannot be
    // confirmed, even though the native release itself completed cleanly.
    clean=display_borrow_clear_uncertainty(mutation_lock);
  } else if(worker==0) {
    display_borrow_note_mutation_unknown(mutation_lock);
  } else {
    display_borrow_poison(mutation_lock);
  }
  if(released)(void)send_frame(socket_fd,DISPLAY_BORROW_RELEASED,identity->generation,
    clean?0:VIBEPOLLO_SESSION_COMPLETION_UNKNOWN,&token,sizeof(token));
  return clean?0:VIBEPOLLO_SESSION_COMPLETION_UNKNOWN;
}
#endif
