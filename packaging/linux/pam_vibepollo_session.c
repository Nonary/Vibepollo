#define _GNU_SOURCE

#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include <errno.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char helper_path[] = "/usr/libexec/vibeshine/vibepollo-session-handoff";
static const char token_key[] = "vibepollo.session-token";

static void cleanup_token(pam_handle_t *pamh, void *data, int error_status) {
  (void) pamh;
  (void) error_status;
  if (data != NULL) {
    explicit_bzero(data, strlen((const char *) data));
    free(data);
  }
}

static int valid_account_name(const char *name) {
  const unsigned char *cursor = (const unsigned char *) name;
  if (cursor == NULL || cursor[0] == '\0') {
    return 0;
  }
  for (; *cursor != '\0'; ++cursor) {
    if (*cursor == ':' || *cursor == '\n' || *cursor == '\r') {
      return 0;
    }
  }
  return 1;
}

static int account_identity(pam_handle_t *pamh, const char **user, uid_t *uid) {
  const void *service_item = NULL;
  const char *service;
  struct passwd password;
  struct passwd *result = NULL;
  char buffer[16384];
  int status;

  status = pam_get_item(pamh, PAM_SERVICE, &service_item);
  if (status != PAM_SUCCESS || service_item == NULL) {
    return PAM_SERVICE_ERR;
  }
  service = (const char *) service_item;
  if (strcmp(service, "plasmalogin") != 0) {
    return PAM_IGNORE;
  }

  status = pam_get_user(pamh, user, NULL);
  if (status != PAM_SUCCESS || !valid_account_name(*user)) {
    return PAM_USER_UNKNOWN;
  }
  status = getpwnam_r(*user, &password, buffer, sizeof(buffer), &result);
  if (status != 0 || result == NULL || result->pw_uid == 0) {
    return PAM_USER_UNKNOWN;
  }
  *uid = result->pw_uid;
  return PAM_SUCCESS;
}

static int generate_token(char token[33]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char bytes[16];
  size_t offset = 0;

  while (offset < sizeof(bytes)) {
    ssize_t count = getrandom(bytes + offset, sizeof(bytes) - offset, 0);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    offset += (size_t) count;
  }
  for (size_t index = 0; index < sizeof(bytes); ++index) {
    token[index * 2] = hex[bytes[index] >> 4];
    token[index * 2 + 1] = hex[bytes[index] & 0x0f];
  }
  token[32] = '\0';
  explicit_bzero(bytes, sizeof(bytes));
  return 0;
}

static int invoke_helper(pam_handle_t *pamh, const char *operation, const char *user,
                         uid_t uid, const char *token) {
  char uid_text[32];
  char *const environment[] = {NULL};
  char *const arguments[] = {
    (char *) helper_path,
    (char *) operation,
    (char *) user,
    uid_text,
    (char *) token,
    NULL,
  };
  pid_t child;
  pid_t waited;
  int child_status;

  if (snprintf(uid_text, sizeof(uid_text), "%ju", (uintmax_t) uid) >= (int) sizeof(uid_text)) {
    return PAM_SYSTEM_ERR;
  }
  child = fork();
  if (child < 0) {
    pam_syslog(pamh, LOG_ERR, "could not fork the Vibepollo session helper: %m");
    return PAM_SUCCESS;
  }
  if (child == 0) {
    execve(helper_path, arguments, environment);
    _exit(127);
  }
  do {
    waited = waitpid(child, &child_status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0) {
    pam_syslog(pamh, LOG_ERR, "could not wait for the Vibepollo session helper: %m");
    return PAM_SUCCESS;
  }
  if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    pam_syslog(pamh, LOG_WARNING,
               "Vibepollo session helper %s failed; allowing the desktop login without streaming handoff",
               operation);
    return PAM_SUCCESS;
  }
  return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc,
                                   const char **argv) {
  const char *user = NULL;
  uid_t uid;
  char token[33];
  char *stored_token;
  int status;
  (void) flags;
  (void) argc;
  (void) argv;

  status = account_identity(pamh, &user, &uid);
  if (status != PAM_SUCCESS) {
    return PAM_IGNORE;
  }
  if (generate_token(token) != 0) {
    pam_syslog(pamh, LOG_ERR, "could not create a Vibepollo session token: %m");
    return PAM_SUCCESS;
  }
  stored_token = strdup(token);
  if (stored_token == NULL) {
    explicit_bzero(token, sizeof(token));
    return PAM_SUCCESS;
  }
  status = pam_set_data(pamh, token_key, stored_token, cleanup_token);
  if (status != PAM_SUCCESS) {
    cleanup_token(pamh, stored_token, status);
    explicit_bzero(token, sizeof(token));
    return PAM_SUCCESS;
  }
  status = invoke_helper(pamh, "pam-open", user, uid, token);
  explicit_bzero(token, sizeof(token));
  return status;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc,
                                    const char **argv) {
  const char *user = NULL;
  const void *stored_token = NULL;
  uid_t uid;
  int status;
  (void) flags;
  (void) argc;
  (void) argv;

  status = account_identity(pamh, &user, &uid);
  if (status != PAM_SUCCESS) {
    return status == PAM_IGNORE ? PAM_IGNORE : PAM_SUCCESS;
  }
  if (pam_get_data(pamh, token_key, &stored_token) != PAM_SUCCESS || stored_token == NULL) {
    pam_syslog(pamh, LOG_WARNING, "Vibepollo session token is unavailable during close");
    return PAM_SUCCESS;
  }
  return invoke_helper(pamh, "pam-close", user, uid, (const char *) stored_token);
}
