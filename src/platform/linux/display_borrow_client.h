#pragma once
#include "packaging/linux/vibepollo-display-borrow.h"
#include "src/display_target_lease.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <cerrno>
#include <cstdio>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace platf::display_power::borrow_protocol {
  using deadline_t=std::chrono::steady_clock::time_point;
  inline bool ready(int fd,short events,deadline_t deadline) {
    while(std::chrono::steady_clock::now()<deadline) {
      const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
      pollfd p{fd,events,0};const int result=poll(&p,1,static_cast<int>(left>100?100:left));
      if(result>0)return (p.revents&events)!=0;
      if(result<0 && errno!=EINTR)return false;
    }
    return false;
  }
  class lease_t {
    int fd_=-1,kernel_fd_=-1,native_fd_=-1;std::uint64_t generation_{};display_borrow_identity token_{};
    std::function<void()> unknown_;bool mutation_=false,acknowledged_=false;int release_status_=0;
    void uncertain() noexcept {
      // SCM_RIGHTS shares the exact locked open-file description. A dead
      // producer or unconfirmed RELEASE must not drop that kernel protection.
      // Deliberately retain the CLOEXEC descriptor until process death. No
      // allocation or lock is safe in this last-owner/destructor callback.
      if(kernel_fd_>=0) kernel_fd_=-1;
      if(native_fd_>=0) native_fd_=-1;
      try{if(unknown_)unknown_();}catch(...){}
    }
    bool send_frame(std::uint16_t type) {
      struct {vibepollo_session_message header;display_borrow_identity token;} frame{};
      frame.header={VIBEPOLLO_SESSION_PROTOCOL_MAGIC,VIBEPOLLO_SESSION_PROTOCOL_VERSION,type,sizeof(token_),0,generation_,release_status_,0};frame.token=token_;
      return send(fd_,&frame,sizeof(frame),MSG_NOSIGNAL|MSG_DONTWAIT)==sizeof(frame);
    }
    // 1 = exact correlated ACK; 0 = old/unsupported producer rejected before
    // admission; -1 = timeout, disconnect, malformed or unconfirmed completion.
    int receive(std::uint16_t type,deadline_t deadline) {
      for(unsigned frames=0;frames<32 && ready(fd_,POLLIN,deadline);++frames) {
        unsigned char buffer[sizeof(vibepollo_session_message)+VIBEPOLLO_SESSION_PROTOCOL_OUTPUT_CHUNK];
        iovec v{buffer,sizeof(buffer)};union {cmsghdr align;unsigned char bytes[CMSG_SPACE(sizeof(int)*8)];} control{};
        msghdr m{};m.msg_iov=&v;m.msg_iovlen=1;m.msg_control=control.bytes;m.msg_controllen=sizeof(control.bytes);
        const auto n=recvmsg(fd_,&m,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
        std::vector<int> descriptors;
        for(auto *c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)) {
          if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
            const auto count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);const auto *fds=reinterpret_cast<const int*>(CMSG_DATA(c));
            descriptors.insert(descriptors.end(),fds,fds+count);
          }
        }
        vibepollo_session_message preview{};
        if(n>=static_cast<ssize_t>(sizeof(preview)))memcpy(&preview,buffer,sizeof(preview));
        if(type==DISPLAY_BORROW_ACK && preview.type==DISPLAY_BORROW_ACK && descriptors.size()==(mutation_?1u:2u) && !(m.msg_flags&(MSG_TRUNC|MSG_CTRUNC))) {
          struct stat st{},native{};
          if(!fstat(descriptors.front(),&st) && S_ISREG(st.st_mode) && st.st_uid==0 && st.st_nlink==1 && (st.st_mode&0777)==0600 &&
             (mutation_ || (!fstat(descriptors[1],&native) && S_ISSOCK(native.st_mode)))) {
            kernel_fd_=descriptors.front();if(!mutation_)native_fd_=descriptors[1];descriptors.clear();
          }
        }
        for(int descriptor:descriptors)close(descriptor);
        if((type==DISPLAY_BORROW_ACK && kernel_fd_<0) && n>=static_cast<ssize_t>(sizeof(vibepollo_session_message))) {
          vibepollo_session_message candidate;memcpy(&candidate,buffer,sizeof(candidate));
          if(candidate.type!=VIBEPOLLO_SESSION_EXIT && candidate.type!=VIBEPOLLO_SESSION_STDERR)return -1;
        }
        if(!descriptors.empty())return -1;
        if(n<static_cast<ssize_t>(sizeof(vibepollo_session_message)) || (m.msg_flags&(MSG_TRUNC|MSG_CTRUNC)))return -1;
        vibepollo_session_message header;memcpy(&header,buffer,sizeof(header));
        const auto length=n-sizeof(header);
        if(header.magic!=VIBEPOLLO_SESSION_PROTOCOL_MAGIC || header.version!=VIBEPOLLO_SESSION_PROTOCOL_VERSION ||
           header.generation!=generation_ || header.reserved || header.argument_count || header.payload_length!=length)return -1;
        if(header.type==VIBEPOLLO_SESSION_STDERR && length>0 && !header.status)continue;
        if(!acknowledged_ && header.type==VIBEPOLLO_SESSION_EXIT && !length && header.status==126)return 0;
        if(header.type!=type || header.status || length!=sizeof(token_))return -1;
        display_borrow_identity token;memcpy(&token,buffer+sizeof(header),sizeof(token));
        return display_borrow_identity_matches(&token,&token_)?1:-1;
      }
      return -1;
    }
  public:
    lease_t(const lease_t &)=delete;lease_t &operator=(const lease_t &)=delete;
    lease_t(int fd,std::uint64_t generation,const char *context,std::function<void()> unknown):fd_(fd),generation_(generation),unknown_(std::move(unknown)) {
      token_.version=DISPLAY_BORROW_VERSION;memcpy(token_.context,context,sizeof(token_.context));
    }
    ~lease_t() {
      if(fd_<0)return;
      if(acknowledged_ && (!send_frame(DISPLAY_BORROW_RELEASE) || receive(DISPLAY_BORROW_RELEASED,std::chrono::steady_clock::now()+std::chrono::seconds(4))!=1))uncertain();
      if(native_fd_>=0)close(native_fd_);
      if(kernel_fd_>=0)close(kernel_fd_);
      close(fd_);
    }
    void completion_unknown() noexcept {release_status_=VIBEPOLLO_SESSION_COMPLETION_UNKNOWN;}
    bool alive() {
      char byte;const auto n=recv(fd_,&byte,1,MSG_PEEK|MSG_DONTWAIT);
      bool live=n<0 && (errno==EAGAIN || errno==EWOULDBLOCK);
      if(live && native_fd_>=0) {
        // The authenticated helper remains the sole GDBus reader and reports
        // semantic owner/policy loss through its health channel. A normal
        // queued reply is not a failure; never consume it in this observer.
        // If that helper/channel is lost, the outer live check already fences
        // admission and retains the raw resource without reconnecting it.
        const auto native=recv(native_fd_,&byte,1,MSG_PEEK|MSG_DONTWAIT);
        live=native>0 || (native<0 && (errno==EAGAIN || errno==EWOULDBLOCK));
      }
      if (!live) {
        uncertain();
      }
      return live;
    }
    // Test seam takes only an already authenticated inert transport. Production
    // always enters through connect() below and validates root peer credentials.
    static std::shared_ptr<lease_t> acquire_fd(int fd,std::uint64_t generation,const char *context,
        std::function<void()> unknown,std::chrono::milliseconds budget=std::chrono::seconds(16),bool mutation=false) {
      if(fd<0 || !generation || !display_borrow_context_valid(context)){if(fd>=0)close(fd);return {};}
      auto result=std::make_shared<lease_t>(fd,generation,context,std::move(unknown));result->mutation_=mutation;
      std::string payload=mutation?"display-mutation-v2":"display-inhibit-v2";payload.push_back('\0');payload+=context;payload.push_back('\0');
      vibepollo_session_message header{VIBEPOLLO_SESSION_PROTOCOL_MAGIC,VIBEPOLLO_SESSION_PROTOCOL_VERSION,VIBEPOLLO_SESSION_REQUEST,static_cast<std::uint32_t>(payload.size()),2,generation,0,0};
      std::string frame(reinterpret_cast<const char*>(&header),sizeof(header));frame+=payload;
      const auto sent=send(fd,frame.data(),frame.size(),MSG_NOSIGNAL|MSG_DONTWAIT);
      if(sent!=static_cast<ssize_t>(frame.size()))return {};
      const int received=result->receive(DISPLAY_BORROW_ACK,std::chrono::steady_clock::now()+budget);
      if(received!=1){if(received<0)result->uncertain();return {};}
      result->acknowledged_=true;return result;
    }
    static std::shared_ptr<lease_t> connect(bool mutation=false) {
      const char *text=getenv("VIBEPOLLO_SESSION_GENERATION");
      std::uint64_t generation=0;
      if(!text || !*text)return {};
      for(const unsigned char *p=reinterpret_cast<const unsigned char*>(text);*p;++p) {
        if(*p<'0'||*p>'9'||generation>(UINT64_MAX-(*p-'0'))/10)return {};
        generation=generation*10+(*p-'0');
      }
      if(!generation)return {};
      unsigned char entropy[16];if(getrandom(entropy,sizeof(entropy),GRND_NONBLOCK)!=sizeof(entropy))return {};
      char context[33];for(unsigned i=0;i<16;++i)snprintf(context+2*i,3,"%02x",entropy[i]);
      int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK,0);if(fd<0)return {};
      sockaddr_un address{};address.sun_family=AF_UNIX;static_assert(sizeof(VIBEPOLLO_SESSION_BROKER_SOCKET)<=sizeof(address.sun_path));
      memcpy(address.sun_path,VIBEPOLLO_SESSION_BROKER_SOCKET,sizeof(VIBEPOLLO_SESSION_BROKER_SOCKET));
      if(::connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address)) &&
         (errno!=EINPROGRESS || !ready(fd,POLLOUT,std::chrono::steady_clock::now()+std::chrono::seconds(2)))) {close(fd);return {};}
      int error=0;socklen_t size=sizeof(error);ucred peer{};socklen_t peer_size=sizeof(peer);
      if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)||error||getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&peer,&peer_size)||peer_size!=sizeof(peer)||peer.pid<=0||peer.uid!=0||peer.gid!=0){close(fd);return {};}
      return acquire_fd(fd,generation,context,[]{display_target::leases().fence_unknown_release();},std::chrono::seconds(16),mutation);
    }
  };
}
