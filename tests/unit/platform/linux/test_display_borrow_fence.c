#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int pwrite_failure;
static int fail_next_ftruncate;
static int fail_next_fdatasync;

static ssize_t fence_test_pwrite(int fd,const void *buffer,size_t length,off_t offset) {
  if(pwrite_failure) {
    const int failure=pwrite_failure;
    pwrite_failure=0;
    if(failure==1 && length==1)return 0; // exercise the short-write path.
    errno=EIO;
    return -1;
  }
  return pwrite(fd,buffer,length,offset);
}

static int fence_test_ftruncate(int fd,off_t length) {
  if(fail_next_ftruncate) {
    --fail_next_ftruncate;
    errno=EIO;
    return -1;
  }
  return ftruncate(fd,length);
}

static int fence_test_fdatasync(int fd) {
  if(fail_next_fdatasync) {
    --fail_next_fdatasync;
    errno=EIO;
    return -1;
  }
  return fdatasync(fd);
}

#define DISPLAY_BORROW_PWRITE fence_test_pwrite
#define DISPLAY_BORROW_FTRUNCATE fence_test_ftruncate
#define DISPLAY_BORROW_FDATASYNC fence_test_fdatasync
#include "packaging/linux/vibepollo-display-borrow.h"

#define CHECK(expression) do { \
  if(!(expression)) { \
    fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#expression); \
    return 1; \
  } \
} while(0)

static int temporary_lock(char path[static 32]) {
  strcpy(path,"/tmp/vibepollo-lock-XXXXXX");
  return mkstemp(path);
}

static bool file_size(int fd,off_t expected) {
  struct stat st;
  return fstat(fd,&st)==0 && st.st_size==expected;
}

int main(void) {
  char path[32];
  int fd=temporary_lock(path);
  CHECK(fd>=0);
  CHECK(display_borrow_marker_clean(fd));
  int native_actions=0;
  if(display_borrow_acquire_lock(-1,false,true))++native_actions; // invalid fd fails before lock admission.
  int path_only=open(path,O_PATH|O_CLOEXEC);
  CHECK(path_only>=0);
  if(display_borrow_acquire_lock(path_only,false,true))++native_actions; // flock failure fails before action admission.
  CHECK(native_actions==0);
  CHECK(display_borrow_marker_clean(fd));
  close(path_only);
  CHECK(display_borrow_acquire_lock(fd,true,false));
  CHECK(file_size(fd,1));
  char marker=0;
  CHECK(pread(fd,&marker,1,0)==1 && marker=='?'); // write-ahead state is durable before borrow ACK.
  int contender=open(path,O_RDWR|O_CLOEXEC);
  CHECK(contender>=0);
  CHECK(!display_borrow_acquire_lock(contender,true,false)); // a second process cannot overlap the lease.
  CHECK(display_borrow_clear_uncertainty(fd)); // only a confirmed clean release clears it.
  CHECK(flock(fd,LOCK_UN)==0);
  CHECK(display_borrow_acquire_lock(contender,false,true));
  display_borrow_note_mutation_unknown(contender); // the prewritten marker remains on every unknown path.
  CHECK(flock(contender,LOCK_UN)==0);
  close(fd); // simulate broker restart: the on-disk marker must still deny admission.
  fd=open(path,O_RDWR|O_CLOEXEC);
  CHECK(fd>=0 && !display_borrow_acquire_lock(fd,false,true));
  close(contender);
  close(fd);
  unlink(path);

  for(int failure=0;failure<4;++failure) {
    fd=temporary_lock(path);
    CHECK(fd>=0);
    int native_actions=0;
    if(failure==0)pwrite_failure=1; // short write
    else if(failure==1)pwrite_failure=2; // write error
    else if(failure==2)fail_next_ftruncate=1;
    else fail_next_fdatasync=1;
    if(display_borrow_acquire_lock(fd,false,true))++native_actions;
    CHECK(native_actions==0); // truncate/write/sync failure aborts before the action.
    CHECK(file_size(fd,failure==2?0:1)); // extending before write leaves failed writes non-empty.
    CHECK(flock(fd,LOCK_EX|LOCK_NB)==0);
    CHECK(display_borrow_marker_clean(fd)==(failure==2));
    CHECK(display_borrow_clear_uncertainty(fd)); // explicit recovery after confirming no action started.
    CHECK(flock(fd,LOCK_UN)==0);
    close(fd);
    unlink(path);
  }

  for(int malformed=0;malformed<2;++malformed) {
    fd=temporary_lock(path);
    CHECK(fd>=0);
    const char marker[2]={'x','!'};
    const size_t marker_length=malformed?sizeof(marker):1;
    CHECK(pwrite(fd,marker,marker_length,0)==(ssize_t)marker_length);
    CHECK(!display_borrow_acquire_lock(fd,false,true)); // every non-empty/corrupt marker state denies admission.
    close(fd);
    unlink(path);
  }

  fd=temporary_lock(path);
  CHECK(fd>=0 && display_borrow_acquire_lock(fd,true,false));
  fail_next_ftruncate=1;
  CHECK(!display_borrow_clear_uncertainty(fd));
  CHECK(file_size(fd,1) && !display_borrow_marker_clean(fd)); // failed clear preserves denial.
  CHECK(display_borrow_clear_uncertainty(fd));
  CHECK(flock(fd,LOCK_UN)==0);
  close(fd);
  unlink(path);

  fd=temporary_lock(path);
  CHECK(fd>=0 && display_borrow_acquire_lock(fd,true,false));
  fail_next_fdatasync=1;
  CHECK(!display_borrow_clear_uncertainty(fd));
  CHECK(file_size(fd,0)); // only a confirmed clean release calls this path; stale disk state is a safe false-positive fence.
  CHECK(flock(fd,LOCK_UN)==0);
  close(fd);
  unlink(path);
  puts("PASS Linux display mutation write-ahead fence fault and restart witnesses");
  return 0;
}
