#ifndef VIBEPOLLO_DISPLAY_BORROW_H
#define VIBEPOLLO_DISPLAY_BORROW_H

#include "vibepollo-session-protocol.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifndef DISPLAY_BORROW_PWRITE
#define DISPLAY_BORROW_PWRITE pwrite
#endif
#ifndef DISPLAY_BORROW_FTRUNCATE
#define DISPLAY_BORROW_FTRUNCATE ftruncate
#endif
#ifndef DISPLAY_BORROW_FDATASYNC
#define DISPLAY_BORROW_FDATASYNC fdatasync
#endif

#define DISPLAY_BORROW_VERSION 2u
#define DISPLAY_BORROW_ACK 50u
#define DISPLAY_BORROW_RELEASE 51u
#define DISPLAY_BORROW_RELEASED 52u
#define DISPLAY_BORROW_LOCK_FD 198
#define DISPLAY_BORROW_LOCK_ENV "VIBEPOLLO_DISPLAY_MUTATION_LOCK_FD"
#ifndef DISPLAY_BORROW_LOCK_PATH
#define DISPLAY_BORROW_LOCK_PATH "/run/vibepollo/display-mutation.lock"
#endif
struct display_borrow_identity { uint32_t version; uint32_t reserved; char context[32]; };
static inline bool display_borrow_context_valid(const char *context) {
  if (!context || strlen(context) != 32) return false;
  for (unsigned i=0;i<32;++i) if (!((context[i]>='0' && context[i]<='9') || (context[i]>='a' && context[i]<='f'))) return false;
  return true;
}
static inline bool display_borrow_identity_matches(const struct display_borrow_identity *a, const struct display_borrow_identity *b) {
  return a->version == DISPLAY_BORROW_VERSION && b->version == DISPLAY_BORROW_VERSION &&
    !a->reserved && !b->reserved && !memcmp(a->context,b->context,sizeof(a->context));
}
static inline bool display_borrow_request(int argc,char **argv) {
  return argc==3 && !strcmp(argv[1],"display-inhibit-v2") && display_borrow_context_valid(argv[2]);
}
static inline bool display_mutation_request(int argc,char **argv) {
  return argc==3 && !strcmp(argv[1],"display-mutation-v2") && display_borrow_context_valid(argv[2]);
}
static inline int display_borrow_open_lock(void) {
  int fd=open(DISPLAY_BORROW_LOCK_PATH,O_RDWR|O_NOFOLLOW|O_CLOEXEC);
  struct stat st;
  if (fd<0) return -1;
  if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_nlink!=1 || (st.st_mode&0777)!=0600) { close(fd);return -1; }
  return fd;
}
// An empty, correctly opened file is the only clean state. Every non-empty,
// malformed, unreadable, or short-write result is a sticky fail-closed marker.
static inline bool display_borrow_marker_clean(int fd) {
  struct stat st;
  return fd>=0 && fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size==0;
}
// Persist uncertainty before ACK or native mutation. Extending first means a
// short/failed marker write leaves a non-empty file that every later admission
// rejects; a failed sync aborts before the caller can start the operation.
static inline bool display_borrow_mark_uncertain(int fd) {
  if(!display_borrow_marker_clean(fd) || DISPLAY_BORROW_FTRUNCATE(fd,1))return false;
  const char marker='?';
  if(DISPLAY_BORROW_PWRITE(fd,&marker,1,0)!=1)return false;
  return DISPLAY_BORROW_FDATASYNC(fd)==0;
}
// Clearing is only called after a confirmed no-op or clean completion. If
// truncation/sync cannot be confirmed, the caller remains fenced when the
// marker persists; if truncation reached disk, the operation was known clean.
static inline bool display_borrow_clear_uncertainty(int fd) {
  return fd>=0 && DISPLAY_BORROW_FTRUNCATE(fd,0)==0 && DISPLAY_BORROW_FDATASYNC(fd)==0;
}
static inline void display_borrow_note_mutation_unknown(int fd) {
  // The durable write-ahead marker is already present while the exclusive
  // lock is held. Never replace it with an unchecked post-mutation write.
  (void)fd;
}
static inline void display_borrow_poison(int fd) {
  // Upgrade the already-durable '?' to a diagnostic permanent marker. Both
  // values fence; a failed update cannot restore the empty state.
  struct stat st;
  if(fd<0 || fstat(fd,&st) || st.st_size!=1)return;
  const char marker='!';
  if(DISPLAY_BORROW_PWRITE(fd,&marker,1,0)==1)(void)DISPLAY_BORROW_FDATASYNC(fd);
}
static inline bool display_borrow_acquire_lock(int fd,bool borrow,bool mutation) {
  if(fd<0)return false;
  if(!borrow && !mutation)return true;
  // Borrow publication and native mutation both retain exclusive admission
  // until a clean RELEASE/completion. This makes the durable marker unambiguous
  // across restart and avoids admitting a second lease beside an uncertain one.
  if(flock(fd,LOCK_EX|LOCK_NB))return false;
  if(!display_borrow_marker_clean(fd) || !display_borrow_mark_uncertain(fd)) {
    (void)flock(fd,LOCK_UN);return false;
  }
  return true;
}
static inline int display_borrow_inherited_lock(void) {
  const char *value=getenv(DISPLAY_BORROW_LOCK_ENV);
  if (!value) return -1; // standalone legacy helper has no broker-owned lock
  if (strcmp(value,"198")) return -2;
  struct stat st;
  if (fstat(DISPLAY_BORROW_LOCK_FD,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 || st.st_nlink!=1 || (st.st_mode&0777)!=0600) return -2;
  return DISPLAY_BORROW_LOCK_FD;
}
#endif
